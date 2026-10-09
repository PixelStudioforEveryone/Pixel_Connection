#pragma once

// 控制中心面板：悬浮按钮弹出的控制面板（Qt::Popup，点击外部自动关闭）。
//
// 内容（跨平台一致，Win/Linux 都可用）：
//   - 屏幕切换：列出被控端 enumerate_screens 的所有屏幕，点选即切换；
//   - 画质：1080P / 2K / 4K 三档（对应上限分辨率与码率，只缩不放）；
//   - 帧率：30 帧 / 60 帧；
//   - 底部显示被控端回报的实际发送状态。

#include <QJsonArray>
#include <QWidget>

class QLabel;
class QPushButton;
class QComboBox;

namespace pxc::gui {

class ClientController;

class ControlCenterPanel : public QWidget {
    Q_OBJECT

public:
    // 画质档位
    enum class Quality { P1080 = 0, P2K = 1, P4K = 2 };

    struct QualityPreset {
        int      width;
        int      height;
        int      bitrate_kbps;
        QString  label;
    };

    static QualityPreset preset(Quality quality);

    explicit ControlCenterPanel(ClientController* controller, QWidget* parent = nullptr);

    // 打开面板（自动拉取屏幕列表）；anchor 为悬浮球在父窗口中的全局位置
    void popup_at(const QPoint& global_anchor);

    // 被控端回报
    void setScreens(const QJsonArray& screens);
    void setVideoState(int width, int height, int fps, int screen,
                       const QString& encoder_name, const QString& error);

signals:
    // 供 RemoteControlView 同步鼠标坐标映射用的当前屏幕
    void activeScreenChanged(int index);
    // 全屏/退出全屏切换（由 RemoteControlView 执行）
    void fullscreenToggleRequested();
    // 断开远程会话
    void disconnectRequested();

private slots:
    void onScreenClicked(int index);
    void onQualityClicked();
    void onFpsClicked();

private:
    void buildUi();
    void rebuildScreenRows();
    void applyConfig();
    QWidget* make_section_title(const QString& text);
    // 按主窗口（RemoteControlView）当前是否全屏刷新全屏按钮文案
    void syncFullscreenButton();

    ClientController* controller_ = nullptr;

    QWidget*  screen_rows_host_ = nullptr;
    QLayout*  screen_rows_      = nullptr;
    QJsonArray screens_;
    int       selected_screen_  = 0;

    QWidget*  quality_row_ = nullptr;
    QWidget*  fps_row_     = nullptr;
    Quality   quality_     = Quality::P1080;
    int       fps_         = 30;

    QWidget*  quality_buttons_[3]  = {nullptr, nullptr, nullptr};
    QWidget*  fps_buttons_[2]      = {nullptr, nullptr};
    class QLabel*   state_label_   = nullptr;
    QPushButton* fullscreen_button_ = nullptr;
    QPushButton* disconnect_button_ = nullptr;
    QComboBox* acceleration_mode_ = nullptr;
};

}  // namespace pxc::gui
