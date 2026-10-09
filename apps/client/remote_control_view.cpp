#include "remote_control_view.h"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QWheelEvent>

#include <functional>

#include "client_controller.h"
#include "control_center_panel.h"
#include "floating_control_button.h"
#include "rx_log.h"
#include "theme.h"

namespace pxc::gui {
using namespace theme;
namespace {

constexpr int kMoveCoalesceMs = 16;  // 鼠标移动合并发送间隔

QJsonObject mouse_move_event(int screen, double x, double y) {
    QJsonObject ev;
    ev["type"]   = "mouse_move";
    ev["screen"] = screen;
    ev["x"]      = x;
    ev["y"]      = y;
    return ev;
}

}  // namespace

// ==================================================================
// RemoteControlView::VideoCanvas
// ==================================================================
RemoteControlView::VideoCanvas::VideoCanvas(RemoteControlView* owner)
    : QWidget(owner), owner_(owner) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    // 用本地十字光标标注位置：光标所在点即映射到远端的坐标点
    setCursor(Qt::ArrowCursor);
    setStyleSheet("background: black;");

    move_timer_ = new QTimer(this);
    move_timer_->setInterval(kMoveCoalesceMs);
    connect(move_timer_, &QTimer::timeout, this, &VideoCanvas::flushMouseMove);

    // 显示层统计（S1）：每秒一行 [vui]
    stats_timer_ = new QTimer(this);
    stats_timer_->setInterval(1000);
    connect(stats_timer_, &QTimer::timeout, this, &VideoCanvas::emitStats);
    stats_timer_->start();
}

void RemoteControlView::VideoCanvas::emitStats() {
    if (total_set_ == 0) return;  // 尚无画面，不刷屏
    const auto age_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_set_at_)
            .count();
    RxLog::instance().write(
        QString("[vui] set=%1 present=%2 img_age=%3ms fid=%4 total=%5")
            .arg(win_set_).arg(win_present_)
            .arg(static_cast<qint64>(age_ms)).arg(last_fid_).arg(total_set_));
    win_set_     = 0;
    win_present_ = 0;
}

void RemoteControlView::VideoCanvas::setFrame(quint32 frameId, const QImage& image) {
    frame_ = image;
    last_fid_ = frameId;
    ++win_set_;
    ++total_set_;
    last_set_at_ = std::chrono::steady_clock::now();
    frame_painted_ = false;

    // 内容探针（延迟定位用）：采样 5 点判断本帧是否大面积纯红。
    // 红屏闪烁测试时，这里的时间戳与 [vrx]/[vui] 同钟，可直接对出
    // “内容到达视图”的时刻，消除外部像素轮询的窗口遮挡/移动误差。
    {
        bool red = false;
        if (!frame_.isNull()) {
            const int w = frame_.width();
            const int h = frame_.height();
            const QPoint probes[5] = {
                QPoint(w / 2, h / 2), QPoint(w / 4, h / 2),
                QPoint(3 * w / 4, h / 2), QPoint(w / 2, h / 4),
                QPoint(w / 2, 3 * h / 4)};
            int hits = 0;
            for (const auto& pt : probes) {
                const QColor c(frame_.pixel(pt));
                if (c.red() > 180 && c.green() < 90 && c.blue() < 90) ++hits;
            }
            red = hits >= 4;
        }
        if (total_set_ == 1 || red != last_frame_red_) {
            RxLog::instance().write(
                QString("[vui] CONTENT red=%1 total=%2").arg(red ? 1 : 0)
                    .arg(total_set_));
            last_frame_red_ = red;
        }
    }
    update();
}

QRectF RemoteControlView::VideoCanvas::display_rect() const {
    if (frame_.isNull()) return QRectF(rect());
    const QSizeF frame_size(frame_.size());
    const QSizeF widget_size(size());
    const qreal scale = qMin(widget_size.width() / frame_size.width(),
                             widget_size.height() / frame_size.height());
    const qreal w = frame_size.width() * scale;
    const qreal h = frame_size.height() * scale;
    return QRectF((widget_size.width() - w) / 2, (widget_size.height() - h) / 2, w, h);
}

void RemoteControlView::VideoCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (frame_.isNull()) {
        p.setPen(QColor(QString(kTextSecondary)));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("等待远端画面…\n\n若长时间无画面，请在悬浮球控制中心"
                                  "确认画质档位，或检查被控端采集权限"));
        return;
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(display_rect(), frame_);
    if (!frame_painted_) {
        frame_painted_ = true;
        ++win_present_;
    }
}

QPointF RemoteControlView::VideoCanvas::toNormalized(const QPointF& widget_pos) const {
    if (frame_.isNull()) return {0, 0};
    const QRectF area = display_rect();
    if (area.width() <= 0 || area.height() <= 0) return {0, 0};
    return {(widget_pos.x() - area.left()) / area.width(),
            (widget_pos.y() - area.top()) / area.height()};
}

void RemoteControlView::VideoCanvas::queueMouseMove(const QPointF& normalized) {
    pending_move_ = normalized;
    if (!move_pending_) {
        move_pending_ = true;
        move_timer_->start();
    }
}

void RemoteControlView::VideoCanvas::flushMouseMove() {
    move_timer_->stop();
    if (!move_pending_) return;
    move_pending_ = false;
    if (owner_->controller_) {
        owner_->controller_->sendInputEvent(
            mouse_move_event(owner_->active_screen_, pending_move_.x(), pending_move_.y()));
    }
}

void RemoteControlView::VideoCanvas::mousePressEvent(QMouseEvent* event) {
    setFocus();
    if (owner_->send_input_) {
        // 点击前先把待发的移动立即发出：保证按下时远端光标就在指针位置，
        // 否则点击会落在最多 16ms 之前的旧位置上
        flushMouseMove();
        pressed_buttons_ |= event->button();
        if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton ||
            event->button() == Qt::MiddleButton) {
            sendMouse(event->button(), true);
        }
    }
    QWidget::mousePressEvent(event);
}

void RemoteControlView::VideoCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (owner_->send_input_) {
        flushMouseMove();  // 抬起前同样先同步位置
        pressed_buttons_ &= ~event->button();
        sendMouse(event->button(), false);
    }
    QWidget::mouseReleaseEvent(event);
}

void RemoteControlView::VideoCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (owner_->send_input_) {
        queueMouseMove(toNormalized(event->position()));
    }
    QWidget::mouseMoveEvent(event);
}

void RemoteControlView::VideoCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    // 什么都不做：press/release 已按原样转发，远端系统按自己的
    // 双击间隔判定，避免注入出多余的第三次点击。
    QWidget::mouseDoubleClickEvent(event);
}

void RemoteControlView::VideoCanvas::wheelEvent(QWheelEvent* event) {
    const QPoint delta = event->angleDelta();
    if (!delta.isNull() && owner_->send_input_ && owner_->controller_) {
        QJsonObject ev;
        ev["type"] = "wheel";
        ev["dy"]   = delta.y() / 120;  // 滚动格数
        owner_->controller_->sendInputEvent(ev);
    }
    event->accept();
}

void RemoteControlView::VideoCanvas::keyPressEvent(QKeyEvent* event) {
    // 功能快捷键在本地响应（设置-键盘 可配置），不会发送到被控端
    if (owner_->handleShortcut(event)) {
        event->accept();
        return;
    }
    if (!owner_->send_input_) {
        // 已解锁鼠标：输入不转发
        event->accept();
        return;
    }
    if (owner_->controller_) {
        QJsonObject ev;
        ev["type"]    = "key";
        ev["key"]     = event->key();
        ev["text"]    = event->text();
        ev["pressed"] = true;
        ev["mods"]    = static_cast<int>(event->modifiers());
        owner_->controller_->sendInputEvent(ev);
    }
    event->accept();
}

void RemoteControlView::VideoCanvas::keyReleaseEvent(QKeyEvent* event) {
    if (!owner_->send_input_) {
        event->accept();
        return;
    }
    if (owner_->controller_) {
        QJsonObject ev;
        ev["type"]    = "key";
        ev["key"]     = event->key();
        ev["text"]    = event->text();
        ev["pressed"] = false;
        ev["mods"]    = static_cast<int>(event->modifiers());
        owner_->controller_->sendInputEvent(ev);
    }
    event->accept();
}

void RemoteControlView::VideoCanvas::sendMouse(Qt::MouseButton button, bool pressed) {
    if (!owner_->controller_) return;
    const char* name = button == Qt::LeftButton  ? "left"
                       : button == Qt::RightButton ? "right"
                                                   : "middle";
    QJsonObject ev;
    ev["type"]    = "mouse_button";
    ev["button"]  = QString(name);
    ev["pressed"] = pressed;
    owner_->controller_->sendInputEvent(ev);
}

// ==================================================================
// RemoteControlView
// ==================================================================
RemoteControlView::RemoteControlView(ClientController* controller,
                                     const QString& deviceId, const QString& deviceName,
                                     QWidget* parent)
    : QWidget(parent), controller_(controller), device_name_(deviceName) {
    setWindowTitle(QStringLiteral("PixelConnection — 正在控制 %1")
                       .arg(deviceName.isEmpty() ? deviceId : deviceName));
    setStyleSheet(QStringLiteral("RemoteControlView { background: black; }"));
    resize(1280, 800);

    buildUi();

    if (controller_) {
        connect(controller_, &ClientController::videoFrameReady, this,
                [this](quint32 frameId, const QImage& image) {
                    canvas_->setFrame(frameId, image);
                });
        connect(controller_, &ClientController::remoteScreensUpdated, this,
                [this](const QJsonArray& screens) {
                    screens_ = screens;
                });
        connect(controller_, &ClientController::remoteVideoStateChanged, this,
                [this](int, int, int, int screen, const QString&, const QString&) {
                    active_screen_ = screen;
                });
    }

    // 进入会话立刻拉一次屏幕列表 + 请求关键帧（尽快出画面）
    QTimer::singleShot(200, this, [this] {
        if (controller_) {
            controller_->requestRemoteScreens();
            controller_->requestRemoteKeyframe();
        }
    });
}

RemoteControlView::~RemoteControlView() = default;

void RemoteControlView::buildUi() {
    canvas_ = new VideoCanvas(this);

    // 悬浮控制球（全屏/断开等操作在弹出的控制中心里）
    fab_ = new FloatingControlButton(this);
    fab_->move(width() - fab_->width() - 48, 96);
    panel_ = new ControlCenterPanel(controller_, this);
    connect(fab_, &FloatingControlButton::activated, this, [this] {
        panel_->popup_at(fab_->mapToGlobal(QPoint(fab_->width() / 2, fab_->height() / 2)));
    });
    connect(panel_, &ControlCenterPanel::activeScreenChanged, this,
            [this](int index) {
                active_screen_ = index;
            });
    connect(panel_, &ControlCenterPanel::fullscreenToggleRequested, this, [this] {
        if (isFullScreen()) showNormal(); else showFullScreen();
    });
    connect(panel_, &ControlCenterPanel::disconnectRequested, this,
            &RemoteControlView::disconnectRequested);

    // 本地提示气泡（快捷键动作反馈）
    toast_ = new QLabel(this);
    toast_->setStyleSheet(QStringLiteral(
        "background: rgba(15,20,32,200); color: white; border-radius: 8px;"
        " padding: 8px 16px; font-size: 12.5px;"));
    toast_->setAlignment(Qt::AlignCenter);
    toast_->hide();
    toast_timer_ = new QTimer(this);
    toast_timer_->setSingleShot(true);
    connect(toast_timer_, &QTimer::timeout, toast_, &QLabel::hide);
}

void RemoteControlView::showToast(const QString& text) {
    if (!toast_) return;
    toast_->setText(text);
    toast_->adjustSize();
    toast_->move((width() - toast_->width()) / 2, height() - 90);
    toast_->show();
    toast_->raise();
    toast_timer_->start(2200);
}

void RemoteControlView::applyMouseCaptureUi() {
    // Keep the normal pointer while input capture is active; a crosshair obscures the remote pointer.
    canvas_->setCursor(Qt::ArrowCursor);
    showToast(send_input_ ? QStringLiteral("已接管鼠标（输入将发送到被控端）")
                          : QStringLiteral("已解锁鼠标（在本机操作，输入不发送）"));
}

void RemoteControlView::toggleMouseCapture() {
    send_input_ = !send_input_;
    applyMouseCaptureUi();
}

void RemoteControlView::switchScreenRelative(int delta) {
    int count = screens_.isEmpty() ? 1 : screens_.size();
    int next = active_screen_ + delta;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    if (controller_) controller_->switchRemoteScreen(next);
    showToast(QStringLiteral("已切换到屏幕 %1").arg(next + 1));
}

bool RemoteControlView::handleShortcut(QKeyEvent* event) {
    QSettings settings;

    // 组合键 = 修饰符 + 主键；修饰键自身的按下事件不会命中任何组合
    const int key = event->key();
    switch (key) {
        case Qt::Key_Control:
        case Qt::Key_Shift:
        case Qt::Key_Alt:
        case Qt::Key_Meta:
            return false;
        default:
            break;
    }
    const QKeySequence sequence(static_cast<int>(event->modifiers() | key));

    // ---- 功能快捷键（设置-键盘-功能快捷键）----
    const std::initializer_list<std::pair<const char*, std::function<void()>>> actions = {
        {"fullscreen", [this] {
             if (isFullScreen()) showNormal(); else showFullScreen();
         }},
        {"screen_prev",  [this] { switchScreenRelative(-1); }},
        {"screen_next",  [this] { switchScreenRelative(1); }},
        {"fab_toggle",   [this] {
             fab_->setVisible(!fab_->isVisible());
             showToast(fab_->isVisible() ? QStringLiteral("已显示悬浮球")
                                         : QStringLiteral("已隐藏悬浮球（Ctrl+Alt+G 恢复）"));
         }},
        {"unlock_mouse", [this] { toggleMouseCapture(); }},
        {"exit_remote",  [this] { emit disconnectRequested(); }},
        {"send_ctrl_alt_del", [this] {
             showToast(QStringLiteral("发送 Ctrl+Alt+Delete 需要被控端系统服务支持"));
         }},
        {"privacy_off",  [this] {
             showToast(QStringLiteral("防窥模式需要被控端支持（开发中）"));
         }},
        {"mute",         [this] { showToast(QStringLiteral("音频功能开发中")); }},
        {"boss_key",     [this] {
             showToast(QStringLiteral("老板键需要全局热键支持（开发中）"));
         }},
        {"open_file_transfer", [this] {
             showToast(QStringLiteral("文件传输功能开发中"));
         }},
        {"privacy_screen", [this] {
             showToast(QStringLiteral("隐私屏保需要被控端支持（开发中）"));
         }},
    };

    for (const auto& [name, action] : actions) {
        const QString stored = settings.value(
            QStringLiteral("kb/") + QString::fromUtf8(name)).toString();
        if (stored.isEmpty()) continue;
        if (QKeySequence(stored) == sequence) {
            action();
            return true;
        }
    }

    // ---- 仅控制端响应的快捷键（自定义，吞掉不转发）----
    const QStringList custom =
        settings.value(QStringLiteral("kb/custom")).toStringList();
    for (const QString& stored : custom) {
        if (!stored.isEmpty() && QKeySequence(stored) == sequence) {
            showToast(QStringLiteral("已在本地响应（未发送到被控端）：") + stored);
            return true;
        }
    }
    return false;
}

void RemoteControlView::closeEvent(QCloseEvent* event) {
    emit disconnectRequested();
    QWidget::closeEvent(event);
}

void RemoteControlView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (canvas_) canvas_->setGeometry(rect());
}

}  // namespace pxc::gui
