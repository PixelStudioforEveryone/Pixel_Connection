#include "login_page.h"
#include "server_addresses.h"
#include "server_status.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>
#include <QTimer>

#include "theme.h"

namespace pxc::gui {
using namespace theme;
namespace {

// 注册对话框：新账号通过「注册新账号」按钮进入。
class RegisterDialog : public QDialog {
public:
    explicit RegisterDialog(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle(QStringLiteral("注册账号"));
        setFixedSize(420, 260);
        setStyleSheet(QStringLiteral("QDialog { background: white; border-radius: 4px; }"));

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(32, 28, 32, 22);
        layout->setSpacing(12);

        auto* title = new QLabel(QStringLiteral("注册新账号"), this);
        QFont title_font = title->font();
        title_font.setPointSize(14);
        title_font.setBold(true);
        title->setFont(title_font);
        title->setStyleSheet(QStringLiteral("color: %1;").arg(kTextPrimary));
        layout->addWidget(title);

        username_ = new QLineEdit(this);
        email_    = new QLineEdit(this);
        password_ = new QLineEdit(this);

        username_->setPlaceholderText(QStringLiteral("用户名（3 个字符以上，登录用）"));
        email_->setPlaceholderText(QStringLiteral("邮箱"));
        password_->setPlaceholderText(QStringLiteral("密码"));
        password_->setEchoMode(QLineEdit::Password);
        connect(email_, &QLineEdit::textChanged, this, [this](const QString& text) {
            // 用户名留空时用邮箱前缀自动填充
            if (username_->text().trimmed().isEmpty() && text.contains('@')) {
                username_->setText(text.section('@', 0, 0));
            }
        });

        layout->addWidget(username_);
        layout->addWidget(email_);
        layout->addWidget(password_);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                             this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("注册"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        layout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }

    QString username() const { return username_->text().trimmed(); }
    QString email() const { return email_->text().trimmed(); }
    QString password() const { return password_->text(); }

private:
    QLineEdit* username_ = nullptr;
    QLineEdit* email_    = nullptr;
    QLineEdit* password_ = nullptr;
};

}  // namespace

LoginPage::LoginPage(QWidget* parent) : QWidget(parent) {
    setObjectName("loginPage");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setAlignment(Qt::AlignHCenter);

    root->addStretch(3);

    // ---- 品牌区 ----
    auto* brand_host = new QWidget(this);
    auto* brand_row = new QHBoxLayout(brand_host);
    brand_row->setContentsMargins(0, 0, 0, 0);
    brand_row->setSpacing(10);
    brand_row->setAlignment(Qt::AlignCenter);
    auto* logo = new QLabel(brand_host);
    logo->setPixmap(QPixmap(QStringLiteral(":/icon.png")).scaled(
        32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    auto* brand = new QLabel(QStringLiteral("PixelConnection 远程"), brand_host);
    QFont brand_font = brand->font();
    brand_font.setPointSize(19);
    brand_font.setBold(true);
    brand->setFont(brand_font);
    brand->setAlignment(Qt::AlignCenter);
    brand->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                             .arg(kTextPrimary));
    brand_row->addWidget(logo);
    brand_row->addWidget(brand);

    auto* subtitle = new QLabel(QStringLiteral("登录后即可远程控制你的设备"), this);
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12.5px; background: transparent;")
            .arg(kTextSecondary));

    auto* mode_title = new QLabel(QStringLiteral("账号登录"), this);
    QFont mode_font = mode_title->font();
    mode_font.setPointSize(13);
    mode_font.setBold(true);
    mode_title->setFont(mode_font);
    mode_title->setAlignment(Qt::AlignCenter);
    mode_title->setStyleSheet(
        QStringLiteral("color: %1; background: transparent;").arg(kTextPrimary));

    root->addWidget(brand_host);
    root->addSpacing(6);
    root->addWidget(subtitle);
    root->addSpacing(22);
    root->addWidget(mode_title);
    root->addSpacing(14);

    // ---- 表单列（固定宽度，居中）----
    auto* form_host = new QWidget(this);
    form_host->setFixedWidth(340);
    form_host->setStyleSheet("background: transparent;");
    auto* form = new QVBoxLayout(form_host);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(16);

    // 下划线风格输入框（模仿参考图的短信登录面板）
    const QString underline_qss = QStringLiteral(
        "QLineEdit { background: transparent; border: none;"
        " border-bottom: 1px solid %1; padding: 8px 2px; font-size: 14px; color: %2; }"
        "QLineEdit:focus { border-bottom: 2px solid %3; }")
        .arg(kInputBorder, kTextPrimary, kAccent);

    emailField_ = new QLineEdit(form_host);
    emailField_->setPlaceholderText(QStringLiteral("邮箱"));
    emailField_->setStyleSheet(underline_qss);
    form->addWidget(emailField_);

    passwordField_ = new QLineEdit(form_host);
    passwordField_->setPlaceholderText(QStringLiteral("密码"));
    passwordField_->setEchoMode(QLineEdit::Password);
    passwordField_->setStyleSheet(underline_qss);
    form->addWidget(passwordField_);

    loginButton_ = new QPushButton(QStringLiteral("登　录"), form_host);
    loginButton_->setObjectName("accentButton");
    loginButton_->setFixedHeight(46);
    loginButton_->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: white; border: none;"
        " border-radius: 8px; font-size: 15px; font-weight: 600; }"
        "QPushButton:hover { background: %2; }"
        "QPushButton:disabled { background: #b9c6ef; }")
        .arg(kAccent, kAccentDeep));
    form->addWidget(loginButton_);

    registerButton_ = new QPushButton(QStringLiteral("注册新账号"), form_host);
    registerButton_->setFixedHeight(42);
    registerButton_->setStyleSheet(QStringLiteral(
        "QPushButton { background: white; color: %1; border: 1px solid %2;"
        " border-radius: 8px; font-size: 13.5px; }"
        "QPushButton:hover { border-color: %1; color: %3; }")
        .arg(kAccent, kInputBorder, kAccentDeep));
    form->addWidget(registerButton_);

    // 记住账号/记住密码
    {
        QSettings settings;
        rememberAccount_ = new QCheckBox(QStringLiteral("记住账号"), form_host);
        rememberPassword_ = new QCheckBox(QStringLiteral("保持登录状态"), form_host);
        rememberAccount_->setChecked(
            settings.value(QStringLiteral("login/remember_account"), true).toBool());
        rememberPassword_->setChecked(
            settings.value(QStringLiteral("login/remember_password"), true).toBool());
        rememberPassword_->setToolTip(
            QStringLiteral("凭据会保存在本机设置中（未加密），下次启动时自动登录；公共电脑请关闭"));
        const QString saved_account =
            settings.value(QStringLiteral("login/account")).toString();
        const QString saved_password =
            settings.value(QStringLiteral("login/password")).toString();
        if (!saved_account.isEmpty()) emailField_->setText(saved_account);
        if (rememberPassword_->isChecked() && !saved_password.isEmpty()) {
            passwordField_->setText(saved_password);
        }
        auto* remember_row = new QHBoxLayout;
        remember_row->setSpacing(24);
        remember_row->addStretch();
        remember_row->addWidget(rememberAccount_);
        remember_row->addWidget(rememberPassword_);
        remember_row->addStretch();
        for (auto* box : {rememberAccount_, rememberPassword_}) {
            box->setStyleSheet(QStringLiteral(
                "QCheckBox { color: %1; font-size: 11.5px; background: transparent; }"
                "QCheckBox::indicator { width: 14px; height: 14px; }")
                .arg(kTextSecondary));
        }
        form->addLayout(remember_row);
    }

    // 协议勾选
    agreement_ = new QCheckBox(form_host);
    agreement_->setText(QStringLiteral("我已阅读并同意《用户协议》与《隐私政策》"));
    agreement_->setChecked(QSettings().value(QStringLiteral("login/agreement_accepted"), false).toBool());
    agreement_->setStyleSheet(QStringLiteral(
        "QCheckBox { color: %1; font-size: 11.5px; background: transparent; }"
        "QCheckBox::indicator { width: 14px; height: 14px; }")
        .arg(kTextSecondary));
    form->addWidget(agreement_, 0, Qt::AlignHCenter);

    // 状态行
    statusLabel_ = new QLabel(form_host);
    statusLabel_->setAlignment(Qt::AlignCenter);
    statusLabel_->setWordWrap(true);
    statusLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; background: transparent;")
            .arg(kTextSecondary));
    form->addWidget(statusLabel_);

    root->addWidget(form_host, 0, Qt::AlignHCenter);

    root->addStretch(4);

    // ---- 高级区（服务器设置，默认收起）----
    advancedToggle_ = new QPushButton(QStringLiteral("服务器设置 ▾"), this);
    advancedToggle_->setCursor(Qt::PointingHandCursor);
    advancedToggle_->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; color: %1;"
        " font-size: 12px; padding: 6px 14px; }"
        "QPushButton:hover { color: %2; }").arg(kTextSecondary, kAccent));
    connect(advancedToggle_, &QPushButton::clicked, this, [this] {
        const bool show = advancedHost_->isHidden();
        advancedHost_->setVisible(show);
        advancedToggle_->setText(show ? QStringLiteral("服务器设置 ▴")
                                      : QStringLiteral("服务器设置 ▾"));
    });
    root->addWidget(advancedToggle_, 0, Qt::AlignHCenter);
    serverSummary_ = new QLabel(this);
    serverSummary_->setObjectName(QStringLiteral("currentServerSummary"));
    serverSummary_->setFixedWidth(420);
    serverSummary_->setWordWrap(true);
    serverSummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    serverSummary_->setAlignment(Qt::AlignCenter);
    serverSummary_->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 12px; background: transparent; padding: 4px;").arg(kTextSecondary));
    root->addWidget(serverSummary_, 0, Qt::AlignHCenter);
    serverSource_ = new QLabel(this);
    serverSource_->setObjectName(QStringLiteral("pxc-server-source"));
    serverSource_->setWordWrap(true);
    serverSource_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    serverSource_->setAlignment(Qt::AlignCenter);
    serverSource_->setStyleSheet(serverSummary_->styleSheet());
    root->addWidget(serverSource_, 0, Qt::AlignHCenter);

    advancedHost_ = new QWidget(this);
    advancedHost_->setFixedWidth(420);
    advancedHost_->setStyleSheet(QStringLiteral(
        "QWidget { background: rgba(255,255,255,190); border-radius: 10px; }"));
    auto* advanced_layout = new QVBoxLayout(advancedHost_);
    advanced_layout->setContentsMargins(18, 14, 18, 14);
    advanced_layout->setSpacing(8);

    auto* advanced_form = new QFormLayout;
    advanced_form->setLabelAlignment(Qt::AlignRight);
    serverMode_ = new QComboBox(advancedHost_);
    serverMode_->addItem(QStringLiteral("连接已有服务器（局域网 / 公网）"), false);
    serverMode_->addItem(QStringLiteral("使用本机服务器（复用 / 启动）"), true);
    apiUrlField_ = new QLineEdit(advancedHost_);
    wsUrlField_  = new QLineEdit(advancedHost_);
    // 记住上次使用的服务器地址；首次运行用本机测试默认值
    {
        QSettings settings;
        const QString saved_api =
            settings.value(QStringLiteral("net/api_url")).toString();
        const QString saved_ws =
            settings.value(QStringLiteral("net/ws_url")).toString();
        apiUrlField_->setText(
            saved_api.isEmpty() ? QStringLiteral("http://127.0.0.1:29910") : saved_api);
        wsUrlField_->setText(
            saved_ws.isEmpty() ? QStringLiteral("ws://127.0.0.1:9910") : saved_ws);
        const int saved_mode = settings.value(QStringLiteral("login/server_mode"), 0).toInt();
        serverMode_->setCurrentIndex(saved_mode == 1 ? 1 : 0);
        current_mode_local_ = saved_mode == 1;
        if (saved_mode == 1) {
            const QStringList local = settings.value(QStringLiteral("login/fields_local")).toStringList();
            apiUrlField_->setText(local.size() == 2 ? local[0] : QStringLiteral("http://127.0.0.1:29910"));
            wsUrlField_->setText(local.size() == 2 ? local[1] : QStringLiteral("ws://127.0.0.1:9910"));
        }
    }
    apiUrlField_->setStyleSheet(QStringLiteral("QLineEdit { border: 1px solid %1; border-radius: 6px; padding: 6px 8px; background: white; }").arg(kInputBorder));
    wsUrlField_->setStyleSheet(apiUrlField_->styleSheet());
    advanced_form->addRow(QStringLiteral("服务器模式"), serverMode_);
    advanced_form->addRow(QStringLiteral("账号 API"), apiUrlField_);
    advanced_form->addRow(QStringLiteral("信令服务器"), wsUrlField_);

    localForm_ = new QFormLayout;
    const QSettings localSettings;
    localBindField_ = new QLineEdit(localSettings.value(QStringLiteral("login/local_bind"),
                                                       QStringLiteral("0.0.0.0")).toString(), advancedHost_);
    localSignalPort_ = new QSpinBox(advancedHost_);
    localSignalPort_->setRange(1, 65535);
    localSignalPort_->setValue(localSettings.value(QStringLiteral("login/local_signal_port"), 9910).toInt());
    localApiPort_ = new QSpinBox(advancedHost_);
    localApiPort_->setRange(1, 65535);
    localApiPort_->setValue(localSettings.value(QStringLiteral("login/local_api_port"), 29910).toInt());
    localServerButton_ = new QPushButton(QStringLiteral("启动本地服务器"), advancedHost_);
    localForm_->addRow(QStringLiteral("本地绑定地址"), localBindField_);
    localForm_->addRow(QStringLiteral("本地信令端口"), localSignalPort_);
    localForm_->addRow(QStringLiteral("本地 API 端口"), localApiPort_);
    localForm_->addRow(QString(), localServerButton_);
    localForm_->setEnabled(current_mode_local_);

    advanced_layout->addLayout(advanced_form);
    serverModeHint_ = new QLabel(advancedHost_);
    serverModeHint_->setWordWrap(true);
    serverModeHint_->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(kTextSecondary));
    advanced_layout->addWidget(serverModeHint_);
    advanced_layout->addLayout(localForm_);
    advancedHost_->hide();
    root->addWidget(advancedHost_, 0, Qt::AlignHCenter);

    // 页脚
    auto* footer = new QLabel(QStringLiteral("PixelConnection © 2026"), this);
    footer->setAlignment(Qt::AlignCenter);
    footer->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px; background: transparent;")
            .arg(kTextSecondary));
    root->addWidget(footer);
    root->addSpacing(10);

    connect(loginButton_, &QPushButton::clicked, this, &LoginPage::onLoginClicked);
    connect(registerButton_, &QPushButton::clicked, this, &LoginPage::onRegisterClicked);
    connect(passwordField_, &QLineEdit::returnPressed, this, &LoginPage::onLoginClicked);
    connect(emailField_, &QLineEdit::returnPressed, this, &LoginPage::onLoginClicked);
    connect(serverMode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                const bool local = serverMode_->itemData(index).toBool();
                localForm_->setEnabled(local);
                // 两种模式各自记忆一套地址，切换互不覆盖：
                // 离开当前模式前保存字段，进入新模式时恢复其保存值
                QSettings settings;
                const QString save_key =
                    current_mode_local_ ? QStringLiteral("login/fields_local")
                                        : QStringLiteral("login/fields_remote");
                settings.setValue(save_key,
                                  QStringList{apiUrlField_->text().trimmed(),
                                              wsUrlField_->text().trimmed()});
                current_mode_local_ = local;
                settings.setValue(QStringLiteral("login/server_mode"), local ? 1 : 0);

                const QString load_key = local ? QStringLiteral("login/fields_local")
                                               : QStringLiteral("login/fields_remote");
                const QStringList saved =
                    settings.value(load_key).toStringList();
                if (local) {
                    const QString bind = localBindField_->text().trimmed();
                    const QString host = localConnectHost(bind);
                    apiUrlField_->setText(serverUrl(QStringLiteral("http"), host, localApiPort_->value()));
                    wsUrlField_->setText(serverUrl(QStringLiteral("ws"), host, localSignalPort_->value()));
                } else if (saved.size() == 2) {
                    apiUrlField_->setText(saved[0]);
                    wsUrlField_->setText(saved[1]);
                }
                // 首次切回远程服务且没有保存值时，保留当前预置地址
                updateServerSummary();
            });
    connect(localServerButton_, &QPushButton::clicked, this, [this] {
        if (localOwned_) {
            emit stopLocalServerRequested();
        } else if (localExisting_) {
            const QString host = localConnectHost(localBindAddress());
            apiUrlField_->setText(serverUrl(QStringLiteral("http"), host, localApiPort()));
            wsUrlField_->setText(serverUrl(QStringLiteral("ws"), host, localSignalingPort()));
            showInfo(QStringLiteral("已使用本机后台服务器；由后台独立管理，客户端不会重复启动或停止它。"));
        } else {
            emit startLocalServerRequested(localBindAddress(), localSignalingPort(),
                                           localApiPort());
        }
    });
    auto updateLocalUrls = [this] {
        if (!current_mode_local_) return;
        const QString bind = localBindField_->text().trimmed();
        const QString host = localConnectHost(bind);
        apiUrlField_->setText(serverUrl(QStringLiteral("http"), host, localApiPort_->value()));
        wsUrlField_->setText(serverUrl(QStringLiteral("ws"), host, localSignalPort_->value()));
        QSettings settings;
        settings.setValue(QStringLiteral("login/local_bind"), bind);
        settings.setValue(QStringLiteral("login/local_api_port"), localApiPort_->value());
        settings.setValue(QStringLiteral("login/local_signal_port"), localSignalPort_->value());
    };
    connect(localApiPort_, qOverload<int>(&QSpinBox::valueChanged), this,
            [updateLocalUrls](int) { updateLocalUrls(); });
    connect(localSignalPort_, qOverload<int>(&QSpinBox::valueChanged), this,
            [updateLocalUrls](int) { updateLocalUrls(); });
    connect(localBindField_, &QLineEdit::editingFinished, this, updateLocalUrls);
    connect(apiUrlField_, &QLineEdit::textChanged, this, [this] { updateServerSummary(); });
    connect(wsUrlField_, &QLineEdit::textChanged, this, [this] { updateServerSummary(); });
    updateLocalUrls();
    updateServerSummary();
    auto* statusTimer = new QTimer(this);
    statusTimer->setInterval(5000);
    connect(statusTimer, &QTimer::timeout, this, [this] { if (isVisible()) refreshServerStatus(); });
    statusTimer->start();
    QTimer::singleShot(0, this, &LoginPage::refreshServerStatus);
}

void LoginPage::updateServerSummary() {
    serverSummary_->setText(QStringLiteral("当前账号服务：%1\n当前信令服务：%2").arg(apiUrl(), wsUrl()));
    serverModeHint_->setText(localServerMode()
        ? (localExisting_ ? QStringLiteral("本机服务器已在后台运行，直接复用；由后台管理，客户端不重复启动。其他设备使用这台电脑的局域网地址。")
                          : QStringLiteral("先检测本机已有服务，没有运行时才启动。其他电脑选择「连接已有服务器」，填写本机可分享的地址。"))
        : QStringLiteral("连接已经运行的服务器，包括 Ubuntu 的局域网服务。127.0.0.1 始终指当前这台电脑，不代表另一台设备。"));
    serverSource_->setText(checkedApi_ == apiUrl() ? sourceText_ : QStringLiteral("正在确认服务器来源…"));
    updateLocalControls();
    QTimer::singleShot(350, this, &LoginPage::refreshServerStatus);
}

void LoginPage::refreshServerStatus() {
    const auto generation = ++probeGeneration_;
    const QString currentApi = apiUrl();
    const QString localApi = serverUrl(QStringLiteral("http"), localConnectHost(localBindAddress()), localApiPort());
    const auto expectedSignal = localSignalingPort();
    auto applyLocal = [this, generation, expectedSignal, localApi](const ServerStatus& status) {
        if (generation != probeGeneration_) return;
        localExisting_ = status.identified && status.signalingPort == expectedSignal;
        if (!localModeDetected_ && localExisting_ && apiUrl() == localApi &&
            wsUrl() == serverUrl(QStringLiteral("ws"), localConnectHost(localBindAddress()), expectedSignal)) {
            localModeDetected_ = true;
            serverMode_->setCurrentIndex(1);
        }
        updateLocalControls();
        if (localServerMode()) serverModeHint_->setText(localExisting_
            ? QStringLiteral("本机服务器已在后台运行，直接复用；后台服务不会随客户端退出。")
            : QStringLiteral("本机未检测到可复用服务；点击启动前会再次确认，避免重复启动。"));
    };
    probeServer(this, currentApi, [this, generation, currentApi, localApi, applyLocal](const ServerStatus& status) {
        if (generation != probeGeneration_ || currentApi != apiUrl()) return;
        checkedApi_ = currentApi; sourceText_ = status.description();
        serverSource_->setText(sourceText_);
        if (currentApi == localApi) applyLocal(status);
    });
    if (localApi != currentApi) probeServer(this, localApi, applyLocal);
}

void LoginPage::updateLocalControls() {
    localServerButton_->setText(localOwned_ ? QStringLiteral("停止本地服务器") :
        localExisting_ ? QStringLiteral("使用已运行的本机服务器") : QStringLiteral("启动本地服务器"));
    localServerButton_->setEnabled(localServerMode());
    localBindField_->setEnabled(localServerMode() && !localOwned_);
    localSignalPort_->setEnabled(localServerMode() && !localOwned_);
    localApiPort_->setEnabled(localServerMode() && !localOwned_);
}

void LoginPage::paintEvent(QPaintEvent*) {
    // 浅蓝渐变背景（模仿 UU 登录窗的柔和渐变）
    QPainter p(this);
    QLinearGradient gradient(0, 0, 0, height());
    gradient.setColorAt(0.0, QColor("#d9e9ff"));
    gradient.setColorAt(0.45, QColor("#eef5ff"));
    gradient.setColorAt(1.0, QColor("#fbfdff"));
    p.fillRect(rect(), gradient);
}

QString LoginPage::apiUrl() const { return apiUrlField_->text().trimmed(); }
QString LoginPage::wsUrl() const { return wsUrlField_->text().trimmed(); }
QString LoginPage::localBindAddress() const { return localBindField_->text().trimmed(); }
quint16 LoginPage::localSignalingPort() const {
    return static_cast<quint16>(localSignalPort_->value());
}
quint16 LoginPage::localApiPort() const {
    return static_cast<quint16>(localApiPort_->value());
}
bool LoginPage::localServerMode() const { return serverMode_->currentData().toBool(); }

void LoginPage::disableAutoLogin() {
    if (rememberPassword_) rememberPassword_->setChecked(false);
    QSettings settings;
    settings.setValue(QStringLiteral("login/remember_password"), false);
    settings.remove(QStringLiteral("login/password"));
}

void LoginPage::setLocalServerRunning(bool running, const QString& message) {
    localOwned_ = running;
    updateLocalControls();
    refreshServerStatus();
    if (!message.isEmpty()) showInfo(message);
}

void LoginPage::setBusy(bool busy) {
    loginButton_->setEnabled(!busy);
    registerButton_->setEnabled(!busy);
}

void LoginPage::showError(const QString& message) {
    statusLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; background: transparent;")
            .arg(kDangerRed));
    statusLabel_->setText(message);
    setBusy(false);
}

void LoginPage::showInfo(const QString& message) {
    statusLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; background: transparent;")
            .arg(kOnlineGreen));
    statusLabel_->setText(message);
    setBusy(false);
}

void LoginPage::onLoginClicked() {
    const QString email = emailField_->text().trimmed();
    const QString password = passwordField_->text();

    if (apiUrl().isEmpty() || wsUrl().isEmpty()) {
        showError(QStringLiteral("请先在「服务器设置」中填写服务器地址"));
        return;
    }
    if (email.isEmpty() || password.isEmpty()) {
        showError(QStringLiteral("请填写邮箱和密码"));
        return;
    }
    if (!agreement_->isChecked()) {
        showError(QStringLiteral("请先阅读并勾选同意《用户协议》与《隐私政策》"));
        return;
    }
    QSettings().setValue(QStringLiteral("login/agreement_accepted"), true);

    // 记住账号/密码（勾选才存，取消勾选即清除）
    {
        QSettings settings;
        settings.setValue(QStringLiteral("login/remember_account"),
                          rememberAccount_->isChecked());
        settings.setValue(QStringLiteral("login/remember_password"),
                          rememberPassword_->isChecked());
        if (rememberAccount_->isChecked() || rememberPassword_->isChecked()) {
            settings.setValue(QStringLiteral("login/account"), email);
        } else {
            settings.remove(QStringLiteral("login/account"));
        }
        if (rememberPassword_->isChecked()) {
            settings.setValue(QStringLiteral("login/password"), password);
        } else {
            settings.remove(QStringLiteral("login/password"));
        }
        settings.setValue(QStringLiteral("login/server_mode"), localServerMode() ? 1 : 0);
        settings.setValue(localServerMode() ? QStringLiteral("login/fields_local")
                                            : QStringLiteral("login/fields_remote"),
                          QStringList{apiUrl(), wsUrl()});
        settings.setValue(QStringLiteral("net/api_url"), apiUrl());
        settings.setValue(QStringLiteral("net/ws_url"), wsUrl());
    }

    statusLabel_->clear();
    setBusy(true);
    emit loginRequested(email, password);
}

void LoginPage::onRegisterClicked() {
    if (apiUrl().isEmpty()) {
        showError(QStringLiteral("请先在「服务器设置」中填写账号 API 地址"));
        return;
    }

    RegisterDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;

    if (dialog.username().isEmpty() || dialog.email().isEmpty() ||
        dialog.password().isEmpty()) {
        showError(QStringLiteral("注册信息不完整"));
        return;
    }

    setBusy(true);
    emit registerRequested(dialog.username(), dialog.email(), dialog.password());
}

}  // namespace pxc::gui
