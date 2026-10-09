#pragma once

// 登录页：模仿 UU 远程登录窗风格（浅蓝渐变背景 + 居中表单）。
// 只暴露邮箱 + 密码；注册走独立对话框；服务器参数收在「服务器设置」高级区。

#include <QWidget>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace pxc::gui {

class LoginPage : public QWidget {
    Q_OBJECT

public:
    explicit LoginPage(QWidget* parent = nullptr);

    // 请求进行中时禁用按钮，避免重复提交
    void setBusy(bool busy);
    void showError(const QString& message);
    void showInfo(const QString& message);

    QString apiUrl() const;
    QString wsUrl() const;
    QString localBindAddress() const;
    quint16 localSignalingPort() const;
    quint16 localApiPort() const;
    bool localServerMode() const;
    void disableAutoLogin();

    void setLocalServerRunning(bool running, const QString& message = {});

signals:
    void startLocalServerRequested(const QString& bindAddress, quint16 signalingPort,
                                   quint16 apiPort);
    void stopLocalServerRequested();
    void loginRequested(const QString& identifier, const QString& password);
    void registerRequested(const QString& username, const QString& email,
                           const QString& password);

private slots:
    void onLoginClicked();
    void onRegisterClicked();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void updateServerSummary();
    void refreshServerStatus();
    void updateLocalControls();
    // 表单
    QLineEdit*   emailField_     = nullptr;
    QLineEdit*   passwordField_  = nullptr;
    QPushButton* loginButton_    = nullptr;
    QPushButton* registerButton_ = nullptr;
    QCheckBox*   agreement_      = nullptr;
    QCheckBox*   rememberAccount_ = nullptr;
    QCheckBox*   rememberPassword_ = nullptr;
    QLabel*      statusLabel_    = nullptr;
    QLabel*      serverSummary_  = nullptr;
    QLabel*      serverModeHint_ = nullptr;
    QLabel*      serverSource_ = nullptr;
    QString checkedApi_;
    QString sourceText_;
    bool localOwned_ = false;
    bool localExisting_ = false;
    bool localModeDetected_ = false;
    quint64 probeGeneration_ = 0;

    // 高级区（服务器设置）
    QPushButton* advancedToggle_ = nullptr;
    QWidget*     advancedHost_   = nullptr;
    QComboBox*   serverMode_     = nullptr;
    QLineEdit*   apiUrlField_    = nullptr;
    QLineEdit*   wsUrlField_     = nullptr;
    QLineEdit*   localBindField_ = nullptr;
    QSpinBox*    localSignalPort_ = nullptr;
    QSpinBox*    localApiPort_   = nullptr;
    QPushButton* localServerButton_ = nullptr;
    QFormLayout* localForm_      = nullptr;
    bool         current_mode_local_ = false;  // 当前服务器模式（双模式地址记忆）
};

}  // namespace pxc::gui
