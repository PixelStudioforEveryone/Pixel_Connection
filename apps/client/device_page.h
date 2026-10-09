#pragma once

// 设备页：模仿「网易UU远程」的布局与外观。
//
//   ┌────────────┬──────────────────────────────┐
//   │ 我的设备    │  ●在线  DAILY            ⋮   │
//   │  ■ DAILY   │  ┌──────────────────────────┐│
//   │  ■ ROG     │  │   （深色设备卡）          ││
//   │ 全部设备    │  │      进入桌面  →          ││
//   │ 远程协助    │  ├──────────────────────────┤│
//   │  开始协助   │  │ 文件传输|观看模式|终端|…  ││
//   │  收藏设备   │  └──────────────────────────┘│
//   │            │  快速启动                     │
//   │ 设置        │  ┌ ─ ─ ─ 添加 ─ ─ ─ ┐        │
//   └────────────┴──────────────────────────────┘
//
// 旧功能的入口全部保留：刷新 / 把本机加入设备列表 / 断开 / 退出登录
// 收纳在「设置」面板与标题栏的 ⋮ 菜单里。

#include <QJsonArray>
#include <QImage>
#include <QSet>
#include <QVector>
#include <QWidget>

#include "client_controller.h"

class QDialog;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

namespace pxc::gui {

class ClientController;
class SettingsPage;
class ToggleSwitch;

class DevicePage : public QWidget {
    Q_OBJECT

public:
    explicit DevicePage(ClientController* controller, QWidget* parent = nullptr);

    void setDevices(const QVector<DeviceRow>& devices);
    void setLocalDevice(const QString& deviceId, const QString& connectionKey);
    void setStatus(const QString& text, bool error = false);
    void setServerInfo(const QString& apiUrl, const QString& wsUrl);
    // 本机正在被控制时显示红色「被控中」徽标（被控端可见状态提示）
    void setControlledActive(bool active);
    // 本端有活动会话时显示「断开」按钮
    void setSessionActive(bool active);
    void beginDesktopPreviewCapture(const QString& deviceId);
    void setDesktopPreviewImage(const QString& deviceId, const QImage& image);
    QImage desktopPreviewImage(const QString& deviceId) const;
    QString selectedDeviceId() const;
    void finishDeviceRemoval(const QString& deviceId, bool success, const QString& message);

signals:
    void refreshRequested();
    void addThisDeviceRequested();
    // 进入桌面：与旧「连接」一致，携带目标设备与连接密钥
    void connectRequested(const QString& deviceId, const QString& connectionKey,
                          bool fileTransferOnly = false);
    void logoutRequested();
    void removeDeviceRequested(const QString& deviceId);

private slots:
    void onConnectClicked();
    void deleteSelectedDevice();

private:
    class SidebarItem;
    class DeviceCard;
    class QuickLaunchBox;

    void buildSidebar();
    void connectSelectedDevice(bool fileTransferOnly);
    void buildContentPane();
    void rebuildDeviceList();
    void selectDevice(int row);
    void updateHeader();
    void showSelectedDesktopPreview();
    QString desktopPreviewPath(const QString& deviceId) const;
    void openSettingsPage();
    void showDeviceMenu();
    void onCardAction(const QString& action);
    void openAllDevicesPage();
    void openAssistPage();
    void rebuildAllDevicesPage();
    void rebuildAssistRecentList();
    void setNavSelection(SidebarItem* selected);
    QWidget* new_divider_v();
    QString selectedDeviceName() const;

    QVector<DeviceRow> devices_;
    int     selected_        = -1;
    bool    device_visible_  = true;
    bool    controlled_active_ = false;
    bool    session_active_    = false;
    QString api_url_;
    QString ws_url_;

    // 侧栏
    QWidget*              sidebar_          = nullptr;
    QLabel*               brand_label_      = nullptr;
    SidebarItem*          my_devices_header_ = nullptr;
    QVBoxLayout*          device_rows_      = nullptr;
    QWidget*              device_rows_host_ = nullptr;
    SidebarItem*          all_devices_row_  = nullptr;
    SidebarItem*          assist_row_       = nullptr;
    SidebarItem*          favorites_row_    = nullptr;
    SidebarItem*          settings_row_     = nullptr;

    // 内容区
    QLabel*       status_pill_      = nullptr;
    QLabel*       device_name_      = nullptr;
    QLabel*       controlled_badge_ = nullptr;
    QToolButton*  more_button_      = nullptr;
    DeviceCard*   card_             = nullptr;
    QuickLaunchBox* quick_launch_   = nullptr;
    QLabel*       status_label_     = nullptr;

    QString       local_device_id_;
    QString       local_connection_key_;
    QSet<QString> preview_capture_devices_;
    QSet<QString> removing_devices_;

    // 右栏页面切换：设备详情 / 设置
    QStackedWidget* content_stack_ = nullptr;
    QWidget*        device_pane_   = nullptr;
    QWidget*        all_devices_page_ = nullptr;
    QWidget*        assist_page_ = nullptr;
    QVBoxLayout*    all_devices_layout_ = nullptr;
    QVBoxLayout*    assist_recent_layout_ = nullptr;
    QLabel*         local_id_label_ = nullptr;
    QLabel*         local_key_label_ = nullptr;
    QLineEdit*      partner_id_field_ = nullptr;
    QPushButton*   partner_connect_button_ = nullptr;
    ToggleSwitch*   assist_allow_toggle_ = nullptr;
    SettingsPage*   settings_page_ = nullptr;
    ClientController* controller_  = nullptr;
};

}  // namespace pxc::gui
