// PixelConnection Qt 客户端入口。
//
// 界面结构：主窗口里一个 QStackedWidget 在登录页和设备页之间切换；
// 「进入桌面」后打开独立的 RemoteControlView 控制窗口（含悬浮控制球）。
// 所有网络、信令和密钥逻辑都在 ClientController 里，界面只发信号、接信号。

#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QLockFile>
#include <QMessageBox>
#include <QStackedWidget>
#include <QScrollArea>
#include <QScreen>
#include <QCloseEvent>
#include <QDateTime>
#include <QComboBox>
#include <QClipboard>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QNetworkInterface>
#include "server_addresses.h"
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QPushButton>
#include <QStringList>
#include <QDir>
#include <QVBoxLayout>
#include <QWidget>

// IXNetSystem.h 在 Windows 上会引入 windows.h，先锁住 min/max 宏
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ixwebsocket/IXNetSystem.h>

#include <memory>
#include <algorithm>

#include "client_controller.h"
#include "device_page.h"
#include "file_transfer_dialog.h"
#include "local_server_manager.h"
#include "login_page.h"
#include "remote_control_view.h"
#include "theme.h"
#include "server_runtime.h"
#include "clipboard_helper.h"
#include <vector>

namespace {

// 文件日志：所有状态/错误写入 %TEMP%\pxc-client.log（远程排障用）。
void file_message_handler(QtMsgType, const QMessageLogContext&, const QString& msg) {
    static QFile log(QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                     QStringLiteral("/pxc-client.log"));
    if (!log.isOpen()) {
        log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }
    if (log.isOpen()) {
        log.write(QStringLiteral("[%1] %2\n")
                      .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                           msg)
                      .toUtf8());
        log.flush();
    }
}

// 把协议里的会话状态翻译成中文提示
QString describeState(const QString& deviceId, const QString& state) {
    if (state == QStringLiteral("connecting"))   return QStringLiteral("正在建立 P2P 连接...");
    if (state == QStringLiteral("authenticating")) return QStringLiteral("正在校验连接密钥...");
    if (state == QStringLiteral("authenticated")) return QStringLiteral("已连接并通过认证");
    if (state == QStringLiteral("connected"))    return QStringLiteral("通道已建立");
    if (state == QStringLiteral("rejected"))     return QStringLiteral("连接失败或密钥错误");
    if (state == QStringLiteral("p2p_timeout"))  return QStringLiteral("P2P 建连超时（网络拦截 UDP），"
                                                                    "可在 设置-网络 配置 TURN 中继后重试");
    if (state == QStringLiteral("closed"))       return QStringLiteral("会话已断开");
    return state;
}

class MainWindow : public QWidget {
public:
    // 无人值守自动登录（被控端部署用）：configure + login，
    // 登录成功后由 loginSucceeded 的既有处理接管（上线 + 刷新设备列表）。
    // 凭据留在内存里：token 过期被服务器拒绝时自动重登，保证被控端持续在线。
    void startAutoLogin(const QString& email, const QString& password) {
        controller_->configure(loginPage_->apiUrl(), loginPage_->wsUrl(), identityPath_);
        loginPage_->showInfo(QStringLiteral("自动登录中: ") + email);
        // 未入网时自动把本机加入该账号（无人值守被控端部署）
        QObject::connect(controller_.get(), &pxc::gui::ClientController::loginSucceeded,
                         this, [this] { controller_->addThisDevice(); },
                         Qt::SingleShotConnection);
        controller_->setAutoLoginCredentials(email, password);
        controller_->login(email, password);
    }

    void startRememberedLogin() {
        const QSettings settings;
        if (!settings.value(QStringLiteral("login/remember_password"), true).toBool() ||
            !settings.value(QStringLiteral("login/agreement_accepted"), false).toBool()) return;
        const QString email = settings.value(QStringLiteral("login/account")).toString();
        const QString password = settings.value(QStringLiteral("login/password")).toString();
        if (email.isEmpty() || password.isEmpty()) return;
        controller_->configure(loginPage_->apiUrl(), loginPage_->wsUrl(), identityPath_);
        controller_->setAutoLoginCredentials(email, password);
        loginPage_->showInfo(QStringLiteral("正在自动登录…"));
        controller_->login(email, password);
    }

    MainWindow() {
        setWindowTitle(QStringLiteral("PixelConnection"));
        const QSize available = screen()->availableGeometry().size() - QSize(32, 48);
        setMinimumSize(QSize(640, 420).boundedTo(available));
        resize(QSize(1024, 680).boundedTo(available));
        setStyleSheet(pxc::gui::theme::app_stylesheet());

        // 身份文件放在用户数据目录，不放工作目录：
        // 避免把私钥提交进代码仓库，也避免多用户共用。
        QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (dataDir.isEmpty()) dataDir = QDir::homePath() + QStringLiteral("/.pixelconnection");
        QDir().mkpath(dataDir);
        identityPath_ = dataDir + QStringLiteral("/identity.json");

        controller_ = std::make_unique<pxc::gui::ClientController>();
        localServer_ = std::make_unique<pxc::gui::LocalServerManager>(this);

        loginPage_  = new pxc::gui::LoginPage;
        devicePage_ = new pxc::gui::DevicePage(controller_.get());

        stack_ = new QStackedWidget;
        auto* login_viewport = new QScrollArea;
        login_viewport->setFrameShape(QFrame::NoFrame);
        login_viewport->setWidgetResizable(true);
        login_viewport->setWidget(loginPage_);
        login_container_ = login_viewport;
        stack_->addWidget(login_container_);
        stack_->addWidget(devicePage_);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* viewport = new QScrollArea(this);
        viewport->setFrameShape(QFrame::NoFrame);
        viewport->setWidgetResizable(true);
        viewport->setWidget(stack_);
        layout->addWidget(viewport);

        connectSignals();
        stack_->setCurrentWidget(login_container_);
        setupTrayIcon();
    }

    // 托盘：图标 + 菜单（显示主窗口 / 退出）
    void setupTrayIcon() {
        auto* tray_menu = new QMenu(this);
        tray_menu->addAction(QStringLiteral("显示主窗口"), this, [this] {
            show();
            setWindowState(windowState() & ~Qt::WindowMinimized);
            raise();
            activateWindow();
        });
        tray_menu->addSeparator();
        tray_menu->addAction(QStringLiteral("退出"), this, [this] {
            force_quit_ = true;
            close();
        });

        tray_ = new QSystemTrayIcon(QIcon(QStringLiteral(":/icon.png")), this);
        tray_->setToolTip(QStringLiteral("PixelConnection 远程"));
        tray_->setContextMenu(tray_menu);
        tray_->show();
        connect(tray_, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::Trigger ||
                        reason == QSystemTrayIcon::DoubleClick) {
                        show();
                        setWindowState(windowState() & ~Qt::WindowMinimized);
                        raise();
                        activateWindow();
                    }
                });
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        // 关闭行为由 设置-常规-「关闭窗口时」决定：0=退出程序，1=最小化到托盘
        if (!force_quit_ &&
            QSettings().value(QStringLiteral("general/close_mode"), 0).toInt() == 1) {
            hide();
            if (tray_) {
                tray_->showMessage(QStringLiteral("PixelConnection"),
                                   QStringLiteral("已最小化到托盘，点击图标恢复主窗口"),
                                   QSystemTrayIcon::Information, 2000);
            }
            event->ignore();
            return;
        }
        force_quit_ = true;
        closeControlView();
        controller_->disconnectSession();
        QWidget::closeEvent(event);
    }

private:
    bool targetSupportsRemoteDesktop(const QString& deviceId) const {
        const auto normalized = [](QString id) { return id.remove(QLatin1Char('-')).toUpper(); };
        for (const auto& row : last_devices_)
            if (normalized(row.deviceId) == normalized(deviceId)) return row.supportsRemoteDesktop();
        return true;
    }

    void openControlView() {
        if (!targetSupportsRemoteDesktop(controlled_device_id_)) {
            devicePage_->setStatus(QStringLiteral("Harmony OS 设备无法被远控，请使用文件传输"), true);
            return;
        }
        if (controlView_) return;
        controlView_ = new pxc::gui::RemoteControlView(
            controller_.get(), controlled_device_id_, controlled_device_name_);
        QObject::connect(controlView_, &pxc::gui::RemoteControlView::disconnectRequested,
                         this, [this] { controller_->disconnectSession(); });
        controlView_->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(controlView_, &QObject::destroyed, this,
                         [this] { controlView_ = nullptr; });
        controlView_->show();
        controlView_->activateWindow();
    }

    void closeControlView() {
        if (!controlView_) return;
        auto* view = controlView_;
        controlView_ = nullptr;
        // Closing a page programmatically must not disconnect the shared file session.
        QObject::disconnect(view, nullptr, this, nullptr);
        view->close();
    }

    void openFileTransferDialog() {
        if (fileTransferDialog_) {
            fileTransferDialog_->show();
            fileTransferDialog_->raise();
            fileTransferDialog_->activateWindow();
            return;
        }
        const QString remote_name = controlled_device_name_.isEmpty()
            ? controlled_device_id_ : controlled_device_name_;
        fileTransferDialog_ = new pxc::gui::FileTransferDialog(controller_.get(), remote_name, this);
        fileTransferDialog_->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(fileTransferDialog_, &QObject::destroyed, this,
                         [this] { fileTransferDialog_ = nullptr; });
        fileTransferDialog_->show();
        fileTransferDialog_->raise();
        fileTransferDialog_->activateWindow();
    }

    void showLocalServerAddresses(const QString& bindAddress, quint16 signalPort, quint16 apiPort) {
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("本地服务器连接地址"));
        dialog.setMinimumWidth(520);
        auto* layout = new QVBoxLayout(&dialog);
        auto* intro = new QLabel(QStringLiteral(
            "选择本机可互通的 IPv4 / IPv6 地址，把下面两条地址复制到其他电脑的「远程服务」设置中。"
            "请确保两台电脑处于可互通的网络，并允许对应端口通过防火墙。"), &dialog);
        intro->setWordWrap(true);
        layout->addWidget(intro);
        auto* form = new QFormLayout;
        auto* interfaces = new QComboBox(&dialog);
        auto* api = new QLineEdit(&dialog);
        auto* ws = new QLineEdit(&dialog);
        api->setReadOnly(true);
        ws->setReadOnly(true);
        form->addRow(QStringLiteral("网卡与地址"), interfaces);
        form->addRow(QStringLiteral("账号 API"), api);
        form->addRow(QStringLiteral("信令服务"), ws);
        layout->addLayout(form);

        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
            if (!(iface.flags() & QNetworkInterface::IsUp) ||
                !(iface.flags() & QNetworkInterface::IsRunning) ||
                (iface.flags() & QNetworkInterface::IsLoopBack)) continue;
            for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
                const QHostAddress address = entry.ip();
                if (address.isLoopback() || address.isNull() ||
                    address.toString().startsWith(QStringLiteral("169.254.")) ||
                    address.toString().startsWith(QStringLiteral("fe80:"), Qt::CaseInsensitive)) continue;
                const bool dual = bindAddress == QStringLiteral("0.0.0.0");
                const bool any6 = bindAddress == QStringLiteral("::") &&
                    address.protocol() == QAbstractSocket::IPv6Protocol;
                if (!dual && !any6 && address != QHostAddress(bindAddress)) continue;
                interfaces->addItem(QStringLiteral("%1 — %2").arg(iface.humanReadableName(), address.toString()),
                                    address.toString());
            }
        }
        if (interfaces->count() == 0) {
            interfaces->addItem(QStringLiteral("仅本机可用（无可分享的监听地址）"), pxc::gui::localConnectHost(bindAddress));
            intro->setText(QStringLiteral("当前监听地址没有可分享的局域网地址。"
                "请绑定 0.0.0.0（双栈）、::（IPv6）或可互通网卡地址后重新启动服务器。"));
        }
        auto updateUrls = [&] {
            const QString host = interfaces->currentData().toString();
            api->setText(pxc::gui::serverUrl(QStringLiteral("http"), host, apiPort));
            ws->setText(pxc::gui::serverUrl(QStringLiteral("ws"), host, signalPort));
        };
        QObject::connect(interfaces, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                         [updateUrls] { updateUrls(); });
        updateUrls();

        auto* buttons = new QHBoxLayout;
        auto* copy = new QPushButton(QStringLiteral("复制两条地址"), &dialog);
        auto* done = new QPushButton(QStringLiteral("完成"), &dialog);
        buttons->addStretch();
        buttons->addWidget(copy);
        buttons->addWidget(done);
        layout->addLayout(buttons);
        QObject::connect(copy, &QPushButton::clicked, &dialog, [api, ws, copy, this] {
            QApplication::clipboard()->setText(api->text() + QLatin1Char('\n') + ws->text());
            devicePage_->setStatus(QStringLiteral("已复制本地服务器地址，可粘贴到另一台电脑"));
            copy->setText(QStringLiteral("已复制"));
        });
        QObject::connect(done, &QPushButton::clicked, &dialog, &QDialog::accept);
        dialog.exec();
    }

    void connectSignals() {
        // ---------------------------------------------------------- 登录页
        QObject::connect(loginPage_, &pxc::gui::LoginPage::loginRequested, this,
                         [this](const QString& identifier, const QString& password) {
                             controller_->configure(loginPage_->apiUrl(), loginPage_->wsUrl(),
                                                    identityPath_);
                             controller_->login(identifier, password);
                         });

        QObject::connect(loginPage_, &pxc::gui::LoginPage::registerRequested, this,
                         [this](const QString& username, const QString& email,
                                const QString& password) {
                             controller_->configure(loginPage_->apiUrl(), loginPage_->wsUrl(),
                                                    identityPath_);
                             controller_->registerAccount(username, email, password);
                         });

        QObject::connect(loginPage_, &pxc::gui::LoginPage::startLocalServerRequested, this,
                         [this](const QString& bindAddress, quint16 signalPort, quint16 apiPort) {
                             const QString exe = QCoreApplication::applicationFilePath();
                             const QString dataDir = QStandardPaths::writableLocation(
                                 QStandardPaths::AppDataLocation);
                             QDir().mkpath(dataDir);
                             const QString dbPath = dataDir + QStringLiteral("/local-server.db");
                             localServer_->start(exe, dbPath, bindAddress, signalPort, apiPort);
                         });

        QObject::connect(loginPage_, &pxc::gui::LoginPage::stopLocalServerRequested, this,
                         [this] { localServer_->stop(); });
        QObject::connect(localServer_.get(), &pxc::gui::LocalServerManager::existingServerDetected, this,
                         [this](const QString&, quint16, quint16) {
                             loginPage_->setLocalServerRunning(false,
                                 QStringLiteral("本机后台服务器已经运行，已直接复用，无需重复启动。"));
                         });

        QObject::connect(localServer_.get(), &pxc::gui::LocalServerManager::started, this,
                         [this](const QString& address, quint16 signalPort, quint16 apiPort) {
                             loginPage_->setLocalServerRunning(true,
                                 QStringLiteral("本地服务已启动；可复制下方局域网地址到其他电脑。"));
                             showLocalServerAddresses(address, signalPort, apiPort);
                         });

        QObject::connect(localServer_.get(), &pxc::gui::LocalServerManager::stopped, this,
                         [this] { loginPage_->setLocalServerRunning(false,
                                                QStringLiteral("本地服务器已停止")); });

        QObject::connect(localServer_.get(), &pxc::gui::LocalServerManager::failed, this,
                         [this](const QString& reason) { loginPage_->showError(reason); });

        QObject::connect(localServer_.get(), &pxc::gui::LocalServerManager::output, this,
                         [this](const QString& line) { loginPage_->showInfo(line); });

        QObject::connect(controller_.get(), &pxc::gui::ClientController::loginSucceeded, this,
                         [this](const QString& username) {
                             loginPage_->showInfo(QStringLiteral("登录成功: ") + username);
                             devicePage_->setServerInfo(loginPage_->apiUrl(), loginPage_->wsUrl());
                             devicePage_->setStatus(QStringLiteral("已登录: ") + username);
                             devicePage_->setLocalDevice(controller_->localDeviceId(),
                                                         controller_->localConnectionKey());
                             devicePage_->beginDesktopPreviewCapture(controller_->localDeviceId());
                             controller_->requestLocalWallpaper();
                             stack_->setCurrentWidget(devicePage_);
                             controller_->refreshDevices();
                         });

        QObject::connect(controller_.get(), &pxc::gui::ClientController::loginFailed, this,
                         [this](const QString& reason) {
                             loginPage_->showError(QStringLiteral("登录失败: ") + reason);
                         });

        QObject::connect(controller_.get(), &pxc::gui::ClientController::incomingFileOffer,
                         this, [this](const QString& transferId, const QString& fileName,
                                      qint64 size) {
                             const QString size_text = size < 1024 * 1024
                                 ? QStringLiteral("%1 KB").arg(size / 1024.0, 0, 'f', 1)
                                 : QStringLiteral("%1 MB").arg(size / (1024.0 * 1024.0), 0, 'f', 1);
                             const auto answer = QMessageBox::question(
                                 this, QStringLiteral("接收文件"),
                                 QStringLiteral("对端希望向本机发送文件：\n\n%1\n大小：%2\n\n是否选择保存位置并接收？")
                                     .arg(fileName, size_text),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                             if (answer != QMessageBox::Yes) {
                                 controller_->rejectIncomingFile(transferId);
                                 return;
                             }
                             QString directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
                             if (directory.isEmpty()) directory = QDir::homePath();
                             const QString destination = QFileDialog::getSaveFileName(
                                 this, QStringLiteral("保存接收文件"), QDir(directory).filePath(fileName));
                             if (destination.isEmpty()) controller_->rejectIncomingFile(transferId);
                             else controller_->acceptIncomingFile(transferId, destination);
                         });

        QObject::connect(controller_.get(), &pxc::gui::ClientController::registerFinished, this,
                         [this](bool ok, const QString& message) {
                             if (ok) loginPage_->showInfo(message);
                             else    loginPage_->showError(message);
                         });

        // ---------------------------------------------------------- 设备页
        QObject::connect(controller_.get(), &pxc::gui::ClientController::devicesUpdated, this,
                         [this](const QVector<pxc::gui::DeviceRow>& devices) {
                             devicePage_->setDevices(devices);
                         });

        QObject::connect(controller_.get(), &pxc::gui::ClientController::desktopWallpaperReady,
                         devicePage_, [this](const QString& deviceId, const QImage& image) {
                             devicePage_->setDesktopPreviewImage(deviceId, image);
                         });

        QObject::connect(controller_.get(), &pxc::gui::ClientController::deviceListFailed, this,
                         [this](const QString& reason) {
                             devicePage_->setStatus(QStringLiteral("获取设备列表失败: ") + reason,
                                                    true);
                         });

        QObject::connect(controller_.get(), &pxc::gui::ClientController::deviceEnrolled, this,
                         [this](const QString& deviceId, const QString& key) {
                             devicePage_->setLocalDevice(deviceId, key);
                             devicePage_->beginDesktopPreviewCapture(deviceId);
                             controller_->requestLocalWallpaper();
                             controller_->refreshDevices();  // 入网成功立即刷新列表
                             QMessageBox::information(
                                 this, QStringLiteral("已加入设备列表"),
                                 QStringLiteral("本机已加入账号。\n\n设备 ID: %1\n连接密钥: %2\n\n"
                                                "连接密钥用于主控端连接本机，请妥善保管；\n"
                                                "设备私钥保存在本机，不会上传。")
                                     .arg(deviceId, key));
                         });

        QObject::connect(controller_.get(), &pxc::gui::ClientController::deviceEnrollFailed, this,
                         [this](const QString& reason) {
                             devicePage_->setStatus(QStringLiteral("加入设备失败: ") + reason, true);
                         });

        // 会话状态驱动：主控端认证通过 → 打开控制窗口；
        // 被控端认证通过 → 显示「被控中」提示。
        QObject::connect(controller_.get(), &pxc::gui::ClientController::sessionStateChanged,
                         this, [this](const QString& deviceId, const QString& state) {
                             devicePage_->setStatus(deviceId.isEmpty()
                                                        ? describeState(deviceId, state)
                                                        : deviceId + QStringLiteral(": ") +
                                                              describeState(deviceId, state),
                                                    state == QStringLiteral("rejected"));

                             if (state == QStringLiteral("authenticated")) {
                                 if (controller_->sessionSide() ==
                                     pxc::gui::ClientController::SessionSide::Controller) {
                                     controller_session_active_ = true;
                                     controlled_device_id_ = deviceId;
                                     devicePage_->beginDesktopPreviewCapture(deviceId);
                                     controller_->requestRemoteWallpaper();
                                     for (const auto& row : last_devices_) {
                                         if (row.deviceId == deviceId) {
                                             controlled_device_name_ = row.name;
                                             break;
                                         }
                                     }
                                     devicePage_->setSessionActive(true);
                                     if (file_transfer_only_) {
                                         openFileTransferDialog();
                                     } else {
                                         openControlView();
                                     }
                                 } else {
                                     controller_session_active_ = false;
                                     devicePage_->setControlledActive(true);
                                     devicePage_->beginDesktopPreviewCapture(controller_->localDeviceId());
                                 }
                             } else if (state == QStringLiteral("connected")) {
                                 devicePage_->setSessionActive(true);
                             } else if (state == QStringLiteral("closed") ||
                                        state == QStringLiteral("rejected")) {
                                 controller_session_active_ = false;
                                 file_transfer_only_ = false;
                                 closeControlView();
                                 if (fileTransferDialog_) fileTransferDialog_->close();
                                 devicePage_->setControlledActive(false);
                                 devicePage_->setSessionActive(false);
                             }
                         });

        QObject::connect(controller_.get(), &pxc::gui::ClientController::devicesUpdated, this,
                         [this](const QVector<pxc::gui::DeviceRow>& devices) {
                             last_devices_ = devices;
                         });

        // 视频链路日志（编码器错误等）透出
        QObject::connect(controller_.get(), &pxc::gui::ClientController::videoNotice, this,
                         [this](const QString& message, bool isError) {
                             devicePage_->setStatus(message, isError);
                         });

        // 恢复丢失的接线：所有信令/P2P 诊断信息显示到状态栏并写入日志文件
        QObject::connect(controller_.get(), &pxc::gui::ClientController::logMessage, this,
                         [this](const QString& message) {
                             qDebug().noquote() << message;
                             devicePage_->setStatus(message);
                         });

        // ---------------------------------------------------------- 用户操作
        QObject::connect(devicePage_, &pxc::gui::DevicePage::removeDeviceRequested,
                         controller_.get(), &pxc::gui::ClientController::removeDevice);
        QObject::connect(controller_.get(), &pxc::gui::ClientController::deviceRemovalFinished, this,
                         [this](const QString& id, bool success, const QString& message) {
                             if (success) {
                                 const auto normalized = [](QString value) { return value.remove(QLatin1Char('-')).toUpper(); };
                                 last_devices_.erase(std::remove_if(last_devices_.begin(), last_devices_.end(),
                                     [&](const pxc::gui::DeviceRow& row) { return normalized(row.deviceId) == normalized(id); }), last_devices_.end());
                             }
                             devicePage_->finishDeviceRemoval(id, success, message);
                             if (success)
                                 devicePage_->setLocalDevice(controller_->localDeviceId(), controller_->localConnectionKey());
                         });
        QObject::connect(devicePage_, &pxc::gui::DevicePage::refreshRequested, this,
                         [this] { controller_->refreshDevices(); });

        QObject::connect(devicePage_, &pxc::gui::DevicePage::addThisDeviceRequested, this,
                         [this] { controller_->addThisDevice(); });

        QObject::connect(devicePage_, &pxc::gui::DevicePage::connectRequested, this,
                         [this](const QString& deviceId, const QString& key, bool fileTransferOnly) {
                             if (!fileTransferOnly && !targetSupportsRemoteDesktop(deviceId)) {
                                 devicePage_->setStatus(QStringLiteral("Harmony OS 设备无法被远控，请使用文件传输"), true);
                                 return;
                             }
                             if (controller_session_active_ && controlled_device_id_ == deviceId) {
                                 file_transfer_only_ = fileTransferOnly;
                                 if (fileTransferOnly) {
                                     closeControlView();
                                     openFileTransferDialog();
                                 } else {
                                     openControlView();
                                 }
                                 return;
                             }
                             if (controller_session_active_) controller_->disconnectSession();
                             // Store the requested page before connecting: rejection may be synchronous.
                             file_transfer_only_ = fileTransferOnly;
                             controlled_device_id_   = deviceId;
                             controlled_device_name_.clear();
                             controller_->connectToDevice(deviceId, key);
                         });


        QObject::connect(devicePage_, &pxc::gui::DevicePage::logoutRequested, this, [this] {
            controller_->logout();
            loginPage_->disableAutoLogin();
            devicePage_->setLocalDevice(QString(), QString());
            closeControlView();
            if (fileTransferDialog_) fileTransferDialog_->close();
            controller_session_active_ = false;
            file_transfer_only_ = false;
            devicePage_->setControlledActive(false);
            devicePage_->setSessionActive(false);
            stack_->setCurrentWidget(login_container_);
        });
    }

    std::unique_ptr<pxc::gui::ClientController> controller_;
    std::unique_ptr<pxc::gui::LocalServerManager> localServer_;
    QSystemTrayIcon* tray_ = nullptr;
    bool force_quit_ = false;
    pxc::gui::LoginPage*  loginPage_  = nullptr;
    QWidget* login_container_ = nullptr;
    pxc::gui::DevicePage* devicePage_ = nullptr;
    QStackedWidget*       stack_      = nullptr;
    pxc::gui::RemoteControlView* controlView_ = nullptr;
    pxc::gui::FileTransferDialog* fileTransferDialog_ = nullptr;
    QString               identityPath_;
    QString               controlled_device_id_;
    QString               controlled_device_name_;
    QVector<pxc::gui::DeviceRow> last_devices_;
    bool controller_session_active_ = false;
    bool file_transfer_only_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && QByteArray(argv[1]) == "--clipboard-read-text")
        return pxc::gui::runClipboardTextHelper(argc, argv);
    // 服务模式必须先于 QApplication、GUI 单实例锁和记住的登录逻辑分流。
    // QCoreApplication 提供 Unicode 参数转换，并可在无桌面的 Ubuntu 上运行。
    if (argc > 1 && QByteArray(argv[1]) == "--server") {
        QCoreApplication serverApp(argc, argv);
        QStringList arguments = serverApp.arguments();
        arguments.removeAt(1);
        std::vector<QByteArray> utf8;
        for (const QString& value : arguments) utf8.push_back(value.toUtf8());
        std::vector<char*> serverArgs;
        for (auto& value : utf8) serverArgs.push_back(value.data());
        return pxc::server::run_server(static_cast<int>(serverArgs.size()), serverArgs.data());
    }
    // Windows 上 ixwebsocket 要求应用显式初始化 WinSock（WSAStartup）；
    // Linux 上是空操作。不调用会导致所有连接报 WSANOTINITIALISED。
    const bool net_inited = ix::initNetSystem();

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("PixelConnection"));
    app.setOrganizationName(QStringLiteral("PixelConnection"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icon.png")));
    app.setStyleSheet(pxc::gui::theme::app_stylesheet());
    qInstallMessageHandler(file_message_handler);
    qInfo() << "=== 客户端启动 ===";

    // 自定义类型要经 queued 信号跨线程传递，必须显式注册
    qRegisterMetaType<pxc::gui::DeviceRow>("pxc::gui::DeviceRow");
    qRegisterMetaType<QVector<pxc::gui::DeviceRow>>("QVector<pxc::gui::DeviceRow>");

    // 单实例锁：两个实例会竞争同一份设备身份文件，导致注册身份错乱
    // （服务端以 ID+公钥 校验设备，重复生成会让旧实例永远注册失败）
    const QString data_dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(data_dir);
    QLockFile instance_lock(data_dir + QStringLiteral("/client.lock"));
    instance_lock.setStaleLockTime(0);
    if (!instance_lock.tryLock(0)) {
        QMessageBox::warning(nullptr, QStringLiteral("PixelConnection"),
                             QStringLiteral("PixelConnection 已经在运行，请使用已打开的窗口。"));
        if (net_inited) ix::uninitNetSystem();
        return 1;
    }

    MainWindow window;
    window.show();

    // 无人值守自动登录：pxc-client --login <邮箱> <密码>
    // （被控端部署用；登录成功后自动上线为受控设备）
    const QStringList cli_args = app.arguments();
    const int login_idx = cli_args.indexOf(QStringLiteral("--login"));
    if (login_idx > 0 && login_idx + 2 < cli_args.size()) {
        window.startAutoLogin(cli_args[login_idx + 1], cli_args[login_idx + 2]);
    } else {
        QTimer::singleShot(250, &window, [&window] { window.startRememberedLogin(); });
    }

    const int ret = app.exec();
    if (net_inited) ix::uninitNetSystem();
    return ret;
}
