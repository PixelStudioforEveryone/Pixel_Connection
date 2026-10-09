#pragma once

// 远程控制界面：进入桌面后打开的独立窗口。
//
// 结构：
//   ┌────────────────────────────────────────┐
//   │           远端画面（等比铺满）            │
//   │                            （悬浮球）   │  ← 可拖动/贴边，弹出控制中心
//   └────────────────────────────────────────┘     （全屏/断开在控制中心里）
//
// 键鼠转发：本地鼠标/键盘事件转成归一化坐标 + Qt::Key 的 JSON 事件，
// 经 ch-control 发给被控端注入（认证未通过的会话，控制器侧会直接忽略）。

#include <QImage>
#include <QJsonArray>
#include <QPoint>
#include <QWidget>

#include <chrono>

class QLabel;
class QTimer;

namespace pxc::gui {

class ClientController;
class ControlCenterPanel;
class FloatingControlButton;

class RemoteControlView : public QWidget {
    Q_OBJECT

public:
    RemoteControlView(ClientController* controller,
                      const QString& deviceId, const QString& deviceName,
                      QWidget* parent = nullptr);
    ~RemoteControlView() override;

signals:
    void disconnectRequested();

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // 视频画布：等比绘制远端帧 + 键鼠事件采集
    class VideoCanvas : public QWidget {
    public:
        explicit VideoCanvas(RemoteControlView* owner);
        void setFrame(quint32 frameId, const QImage& image);
        // 当前帧在画布内实际显示的区域（含黑边居中）
        QRectF display_rect() const;

    protected:
        void paintEvent(QPaintEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseDoubleClickEvent(QMouseEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void keyReleaseEvent(QKeyEvent* event) override;

    private:
        void sendMouse(Qt::MouseButton button, bool pressed);
        void queueMouseMove(const QPointF& normalized);
        void flushMouseMove();
        QPointF toNormalized(const QPointF& widget_pos) const;
        void emitStats();

        RemoteControlView* owner_ = nullptr;
        QImage frame_;
        // 鼠标移动合并发送（最高 ~60/s，防止事件风暴）
        QTimer*  move_timer_   = nullptr;
        QPointF  pending_move_;
        bool     move_pending_ = false;
        Qt::MouseButtons pressed_buttons_ = Qt::NoButton;
        // 显示层统计（S1）：GUI 消费帧数与实际绘制提交数
        QTimer*  stats_timer_ = nullptr;
        uint64_t win_set_      = 0;   // 本窗口 setFrame 次数
        uint64_t win_present_  = 0;   // 本窗口携带新帧的 paint 次数
        uint64_t total_set_    = 0;
        quint32  last_fid_     = 0;   // 最近到达显示层的帧号（延迟定位）
        std::chrono::steady_clock::time_point last_set_at_{};
        bool     frame_painted_ = false;  // 当前帧已被绘制提交
        bool     last_frame_red_ = false;  // 内容探针：上一帧是否大面积纯红
    };

    friend class VideoCanvas;

    void buildUi();

    ClientController*    controller_ = nullptr;
    VideoCanvas*         canvas_     = nullptr;
    FloatingControlButton* fab_      = nullptr;
    ControlCenterPanel*  panel_      = nullptr;
    QLabel*              toast_      = nullptr;
    QTimer*              toast_timer_   = nullptr;
    QString              device_name_;

    QJsonArray screens_;
    int        active_screen_ = 0;
    bool       send_input_    = true;   // 「解锁鼠标」时暂停转发输入

    // 键盘映射：功能快捷键（设置-键盘 可配置）本地拦截，不发送到被控端
    bool handleShortcut(QKeyEvent* event);
    void showToast(const QString& text);
    void toggleMouseCapture();
    void switchScreenRelative(int delta);
    void applyMouseCaptureUi();
};

}  // namespace pxc::gui
