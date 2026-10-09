#include "control_center_panel.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QComboBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QVBoxLayout>

#include "client_controller.h"
#include "theme.h"

namespace pxc::gui {
using namespace theme;
namespace {

// 分段选择按钮（画质/帧率共用）：选中态品牌蓝
QPushButton* make_segment_button(const QString& text, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedHeight(34);
    button->setStyleSheet(QStringLiteral(
        "QPushButton { background: #f2f3f5; color: %1; border: none;"
        " border-radius: 8px; font-size: 13px; padding: 0 18px; }"
        "QPushButton:hover { background: #e8eaf0; }"
        "QPushButton:checked { background: %2; color: white; font-weight: 600; }")
        .arg(kTextPrimary, kAccent));
    return button;
}

}  // namespace

ControlCenterPanel::QualityPreset ControlCenterPanel::preset(Quality quality) {
    switch (quality) {
        case Quality::P1080: return {1920, 1080, 8000,  QStringLiteral("1080P")};
        case Quality::P2K:   return {2560, 1440, 14000, QStringLiteral("2K")};
        case Quality::P4K:   return {3840, 2160, 24000, QStringLiteral("4K")};
    }
    return {1920, 1080, 8000, QStringLiteral("1080P")};
}

ControlCenterPanel::ControlCenterPanel(ClientController* controller, QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), controller_(controller) {
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedWidth(320);
    buildUi();

    if (controller_) {
        connect(controller_, &ClientController::remoteScreensUpdated, this,
                &ControlCenterPanel::setScreens);
        connect(controller_, &ClientController::remoteVideoStateChanged, this,
                &ControlCenterPanel::setVideoState);
    }
}

QWidget* ControlCenterPanel::make_section_title(const QString& text) {
    auto* title = new QLabel(text, this);
    title->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 12px; font-weight: 600; padding: 2px 0;")
        .arg(kTextSecondary));
    return title;
}

void ControlCenterPanel::buildUi() {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);

    // 圆角白底卡片
    auto* card = new QFrame(this);
    card->setObjectName("panelCard");
    card->setStyleSheet(QStringLiteral(
        "#panelCard { background: white; border-radius: 14px;"
        " border: 1px solid %1; }").arg(kDivider));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("控制中心"), card);
    QFont title_font = title->font();
    title_font.setPointSize(11);
    title_font.setBold(true);
    title->setFont(title_font);
    title->setStyleSheet(QStringLiteral("color: %1;").arg(kTextPrimary));
    layout->addWidget(title);

    // ---- 屏幕切换 ----
    layout->addWidget(make_section_title(QStringLiteral("屏幕切换")));
    screen_rows_host_ = new QWidget(card);
    screen_rows_ = new QVBoxLayout(screen_rows_host_);
    screen_rows_->setContentsMargins(0, 0, 0, 0);
    screen_rows_->setSpacing(4);

    auto* scroll = new QScrollArea(card);
    scroll->setWidgetResizable(true);
    scroll->setWidget(screen_rows_host_);
    scroll->setFrameShape(QFrame::NoFrame);
    screen_rows_host_->setStyleSheet(QStringLiteral("background: transparent;"));
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; }"
        "QScrollBar:vertical { background: transparent; width: 6px; }"
        "QScrollBar::handle:vertical { background: #d4d8de; border-radius: 3px; }"
        "QScrollBar::add-line, QScrollBar::sub-line { height: 0; }"));
    scroll->setMinimumHeight(58);
    scroll->setMaximumHeight(150);
    layout->addWidget(scroll);

    auto* refresh_row = new QHBoxLayout;
    auto* refresh = new QPushButton(QStringLiteral("刷新屏幕列表"), card);
    refresh->setObjectName("flatTool");
    refresh->setCursor(Qt::PointingHandCursor);
    connect(refresh, &QPushButton::clicked, this, [this] {
        if (controller_) controller_->requestRemoteScreens();
    });
    refresh_row->addWidget(refresh);
    refresh_row->addStretch();
    layout->addLayout(refresh_row);

    // ---- 画质 ----
    layout->addWidget(make_section_title(QStringLiteral("画质")));
    auto* quality_row = new QHBoxLayout;
    quality_row->setSpacing(8);
    const std::initializer_list<Quality> qualities = {
        Quality::P1080, Quality::P2K, Quality::P4K};
    int index = 0;
    for (const Quality q : qualities) {
        auto* button = make_segment_button(preset(q).label, card);
        button->setChecked(q == quality_);
        connect(button, &QPushButton::clicked, this, [this, q, button] {
            quality_ = q;
            for (auto* other : quality_buttons_) {
                if (other && other != button) {
                    other->blockSignals(true);
                    qobject_cast<QPushButton*>(other)->setChecked(false);
                    other->blockSignals(false);
                }
            }
            applyConfig();
        });
        quality_buttons_[index++] = button;
        quality_row->addWidget(button);
    }
    quality_row->addStretch();
    layout->addLayout(quality_row);

    // ---- 帧率 ----
    layout->addWidget(make_section_title(QStringLiteral("帧率")));
    auto* fps_row = new QHBoxLayout;
    fps_row->setSpacing(8);
    const std::initializer_list<int> fps_choices = {30, 60};
    index = 0;
    for (const int f : fps_choices) {
        auto* button = make_segment_button(QStringLiteral("%1 帧").arg(f), card);
        button->setChecked(f == fps_);
        connect(button, &QPushButton::clicked, this, [this, f, button] {
            fps_ = f;
            for (auto* other : fps_buttons_) {
                if (other && other != button) {
                    other->blockSignals(true);
                    qobject_cast<QPushButton*>(other)->setChecked(false);
                    other->blockSignals(false);
                }
            }
            applyConfig();
        });
        fps_buttons_[index++] = button;
        fps_row->addWidget(button);
    }
    fps_row->addStretch();
    layout->addLayout(fps_row);

    // ---- 解码加速 ----
    layout->addWidget(make_section_title(QStringLiteral("解码加速")));
    auto* acceleration_row = new QHBoxLayout;
    auto* acceleration_hint = new QLabel(QStringLiteral("解码方式"), card);
    acceleration_hint->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(kTextSecondary));
    acceleration_row->addWidget(acceleration_hint);
    acceleration_mode_ = new QComboBox(card);
    acceleration_mode_->addItem(QStringLiteral("智能选择"), 0);
    acceleration_mode_->addItem(QStringLiteral("硬件加速"), 1);
    acceleration_mode_->addItem(QStringLiteral("软件加速"), 2);
    const QString saved_acceleration = QSettings().value(QStringLiteral("video/acceleration"),
                                                          QStringLiteral("smart")).toString();
    acceleration_mode_->setCurrentIndex(saved_acceleration == QStringLiteral("hardware") ? 1
        : saved_acceleration == QStringLiteral("software") ? 2 : 0);
    acceleration_row->addWidget(acceleration_mode_, 1);
    layout->addLayout(acceleration_row);
    auto* acceleration_note = new QLabel(
        QStringLiteral("智能选择优先尝试硬件解码；设备不兼容或解码失败时自动切换到软件解码。"), card);
    acceleration_note->setWordWrap(true);
    acceleration_note->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(kTextSecondary));
    layout->addWidget(acceleration_note);
    connect(acceleration_mode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (controller_) controller_->setVideoAccelerationMode(
                    static_cast<pxc::VideoAccelerationMode>(index));
            });

    // ---- 状态回显 ----
    state_label_ = new QLabel(QStringLiteral("正在获取被控端状态…"), card);
    state_label_->setWordWrap(true);
    state_label_->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 11px; background: #f7f8fa;"
        " border-radius: 8px; padding: 8px 10px;").arg(kTextSecondary));
    layout->addWidget(state_label_);

    // ---- 全屏 / 断开 ----
    auto* action_row = new QHBoxLayout;
    action_row->setSpacing(8);

    fullscreen_button_ = new QPushButton(QStringLiteral("全屏"), card);
    fullscreen_button_->setCursor(Qt::PointingHandCursor);
    fullscreen_button_->setFixedHeight(34);
    fullscreen_button_->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: white; border: none;"
        " border-radius: 8px; font-size: 13px; font-weight: 600; }"
        "QPushButton:hover { background: #6a93f0; }").arg(kAccent));
    connect(fullscreen_button_, &QPushButton::clicked, this, [this] {
        emit fullscreenToggleRequested();
        syncFullscreenButton();
    });
    action_row->addWidget(fullscreen_button_, 1);

    disconnect_button_ = new QPushButton(QStringLiteral("断开"), card);
    disconnect_button_->setCursor(Qt::PointingHandCursor);
    disconnect_button_->setFixedHeight(34);
    disconnect_button_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #e5484d; color: white; border: none;"
        " border-radius: 8px; font-size: 13px; font-weight: 600; }"
        "QPushButton:hover { background: #ec5d61; }"));
    connect(disconnect_button_, &QPushButton::clicked, this,
            [this] { emit disconnectRequested(); });
    action_row->addWidget(disconnect_button_, 1);

    layout->addLayout(action_row);

    root_layout->addWidget(card);

    // 初始占位
    auto* placeholder = new QLabel(QStringLiteral("正在获取屏幕列表…"), screen_rows_host_);
    placeholder->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                                   .arg(kTextSecondary));
    screen_rows_->addWidget(placeholder);
}

void ControlCenterPanel::rebuildScreenRows() {
    while (screen_rows_->count() > 0) {
        QLayoutItem* item = screen_rows_->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    if (screens_.isEmpty()) {
        auto* hint = new QLabel(QStringLiteral("未获取到屏幕信息，请点「刷新屏幕列表」"),
                                screen_rows_host_);
        hint->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                                .arg(kTextSecondary));
        hint->setWordWrap(true);
        screen_rows_->addWidget(hint);
        return;
    }

    for (int i = 0; i < screens_.size(); ++i) {
        const QJsonObject screen = screens_.at(i).toObject();
        const QString name = screen.value("name").toString(
            QStringLiteral("屏幕 %1").arg(i + 1));
        QString label = QStringLiteral("%1　%2×%3")
                            .arg(name)
                            .arg(screen.value("width").toInt())
                            .arg(screen.value("height").toInt());
        if (screen.value("primary").toBool()) label += QStringLiteral("（主屏）");
        if (i == selected_screen_) label += QStringLiteral("　✓");

        auto* row = new QPushButton(label, screen_rows_host_);
        row->setCursor(Qt::PointingHandCursor);
        row->setFixedHeight(34);
        row->setStyleSheet(QStringLiteral(
            "QPushButton { text-align: left; padding: 0 12px; border-radius: 8px;"
            " border: 1px solid %1; background: white; color: %2; font-size: 12px; }"
            "QPushButton:hover { background: %3; }")
            .arg(kDivider, kTextPrimary, kHoverBg));
        if (i == selected_screen_) {
            row->setStyleSheet(row->styleSheet() + QStringLiteral(
                "QPushButton { border-color: %1; background: #eef3ff; }").arg(kAccent));
        }
        connect(row, &QPushButton::clicked, this, [this, i] { onScreenClicked(i); });
        screen_rows_->addWidget(row);
    }
}

void ControlCenterPanel::popup_at(const QPoint& global_anchor) {
    adjustSize();
    if (controller_) controller_->requestRemoteScreens();
    syncFullscreenButton();

    // 优先显示在悬浮球左侧/下方，并保证不超出屏幕
    QScreen* screen = QGuiApplication::screenAt(global_anchor);
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);

    QPoint pos = global_anchor - QPoint(width() + 24, height() / 2);
    if (pos.x() < available.left() + 8) {
        pos.setX(qMin(global_anchor.x() + 24, available.right() - width() - 8));
    }
    pos.setY(qBound(available.top() + 8, pos.y(),
                    available.bottom() - height() - 8));
    move(pos);
    show();
    raise();
    activateWindow();
}

void ControlCenterPanel::setScreens(const QJsonArray& screens) {
    screens_ = screens;
    bool found = false;
    for (int i = 0; i < screens_.size(); ++i) {
        if (screens_.at(i).toObject().value("index").toInt() == selected_screen_) {
            found = true;
            break;
        }
    }
    if (!found && !screens_.isEmpty()) {
        // 默认选中主屏
        for (int i = 0; i < screens_.size(); ++i) {
            if (screens_.at(i).toObject().value("primary").toBool()) {
                selected_screen_ = screens_.at(i).toObject().value("index").toInt(i);
                break;
            }
        }
        emit activeScreenChanged(selected_screen_);
    }
    rebuildScreenRows();
}

void ControlCenterPanel::setVideoState(int width, int height, int fps, int screen,
                                       const QString& encoder_name, const QString& error) {
    selected_screen_ = screen;
    rebuildScreenRows();
    emit activeScreenChanged(screen);

    if (!error.isEmpty()) {
        state_label_->setText(QStringLiteral("被控端：") + error);
        return;
    }
    QString text = QStringLiteral("实际发送 %1×%2 @%3 帧 · 屏幕 %4")
                       .arg(width).arg(height).arg(fps).arg(screen + 1);
    if (!encoder_name.isEmpty()) text += QStringLiteral(" · ") + encoder_name;
    state_label_->setText(text);
}

void ControlCenterPanel::onScreenClicked(int index) {
    selected_screen_ = index;
    rebuildScreenRows();
    emit activeScreenChanged(index);
    if (controller_) controller_->switchRemoteScreen(index);
}

void ControlCenterPanel::applyConfig() {
    const QualityPreset p = preset(quality_);
    if (controller_) {
        controller_->setRemoteVideoConfig(p.width, p.height, fps_, p.bitrate_kbps);
    }
}

void ControlCenterPanel::onQualityClicked() {}
void ControlCenterPanel::onFpsClicked() {}

void ControlCenterPanel::syncFullscreenButton() {
    if (!fullscreen_button_) return;
    // 面板父窗口即 RemoteControlView（远程控制主窗口）
    const bool full = parentWidget() && parentWidget()->isFullScreen();
    fullscreen_button_->setText(full ? QStringLiteral("退出全屏")
                                     : QStringLiteral("全屏"));
}

}  // namespace pxc::gui
