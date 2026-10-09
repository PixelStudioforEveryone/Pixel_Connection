#pragma once

// 设置页：模仿 UU 远程设置（常规 / 安全 / 键盘 / 网络 四个标签）。
//
// 与文档参考图对应：
//   安全.png  — 允许同账号控制本设备 / 自定义验证码（修改连接密钥）
//   键盘*.png — 仅控制端响应的快捷键 + 功能快捷键（可在控制窗口本地触发）
//   常规.png  — 开机自启 / 防止休眠 等
//
// 持久化统一走 QSettings(organization=PixelConnection, application=PixelConnection)。

#include <QWidget>

class QFrame;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;

namespace pxc::gui {

class ClientController;

class SettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPage(ClientController* controller, QWidget* parent = nullptr);

    // 进入页面时刷新动态内容（本机信息 / 服务器 / 已保存密钥）
    void refresh();

signals:
    void backRequested();
    void logoutRequested();

private:
    void buildTabs();
    QWidget* buildGeneralTab();
    QWidget* buildSecurityTab();
    QWidget* buildKeyboardTab();
    QWidget* buildNetworkTab();
    QWidget* buildSavedKeysCard();

    // 卡片/行构造工具（UU 白色圆角卡片 + 分隔线行）
    QFrame*  make_card();
    QWidget* make_row(const QPixmap& icon, const QString& title,
                      const QString& subtitle = QString());
    QWidget* make_divider();

    void rebuildSavedKeys();

    ClientController* controller_ = nullptr;

    // 标签
    QPushButton* tabs_[4]  = {nullptr, nullptr, nullptr, nullptr};
    QWidget* pages_[4]     = {nullptr, nullptr, nullptr, nullptr};
    QStackedWidget* tab_content_ = nullptr;
    QWidget* tab_underlines_[4] = {nullptr, nullptr, nullptr, nullptr};
    int      active_tab_   = 0;

    // 动态区域
    QWidget* saved_keys_host_  = nullptr;
    QWidget* local_id_value_   = nullptr;   // 验证码连接说明里的本机 ID
    QWidget* custom_key_value_ = nullptr;   // 自定义验证码状态
    QWidget* api_value_        = nullptr;
    QWidget* ws_value_         = nullptr;
    QComboBox* server_endpoints_ = nullptr;
    QPushButton* copy_server_ = nullptr;
    quint64 server_probe_generation_ = 0;
};

}  // namespace pxc::gui
