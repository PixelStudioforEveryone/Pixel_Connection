#pragma once

// Qt 客户端控制器：把账号 API、信令连接和 P2P 会话收在一处，
// 只对界面暴露信号/槽。
//
// 线程模型：AccountApiClient 和 SignalingClient 都是阻塞式的，
// 所有网络调用都丢到工作线程执行，结果通过 Qt::QueuedConnection 回到 UI 线程。
// 界面控件永远只在 UI 线程被更新。
//
// 密钥处理：设备私钥和连接密钥只保存在本机身份文件里（0600），
// 不上传服务器、不写日志、不进 SDP。主控端每次连接输入的是目标设备的连接密钥。
//
// 会话双角色：本机既可能作为主控端发起连接（收画面、发输入），
// 也可能作为被控端接受连接（采屏幕、发画面、收输入注入）。
// 只有连接密钥挑战通过后，才会启动视频流 / 接受任何键鼠命令。

#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "pxc/account_api.h"
#include "pxc/identity.h"
#include "pxc/input_injector.h"
#include "pxc/peer_session.h"
#include "pxc/protocol.h"
#include "pxc/screen_capturer.h"
#include "pxc/signaling_client.h"
#include "video_receiver.h"

class QFile;
class QSaveFile;
class QTimer;
class QMimeData;

namespace pxc::gui {

class VideoSender;

// 设备列表的一行。放在这里而不是直接用 pxc::PeerInfo，
// 是为了让界面层不依赖协议头。
struct DeviceRow {
    QString deviceId;
    QString name;
    QString publicIp;
    QString lastSeen;
    QString platform;
    bool    online = false;

    bool supportsRemoteDesktop() const {
        QString os = platform.toLower();
        os.remove(QLatin1Char(' ')).remove(QLatin1Char('-')).remove(QLatin1Char('_'));
        return !os.contains(QStringLiteral("harmonyos")) &&
               !os.startsWith(QStringLiteral("openharmony")) && os != QStringLiteral("ohos");
    }
};

class ClientController : public QObject {
    Q_OBJECT

public:
    // 本次会话中本机的角色
    enum class SessionSide { None, Controller, Controlled };

    explicit ClientController(QObject* parent = nullptr);
    ~ClientController() override;

    // 必须在 login 之前调用一次
    void configure(const QString& apiUrl, const QString& wsUrl, const QString& identityPath);

    // 本机是否已经「加入账号」（即已有设备身份和连接密钥）
    bool isDeviceEnrolled() const;
    // 本机设备 ID（未加入账号时为空）
    QString localDeviceId() const;
    // 本机连接密钥（未加入账号时为空）
    QString localConnectionKey() const;

    // 当前会话中本机的角色（无会话时 None）
    SessionSide sessionSide() const { return session_side_; }

    // ---- 密钥管理 ----
    // 修改本机连接密钥为用户自定义密码（≥6 位）。成功后重新上线信令，
    // 主控端此后必须用新密码连接本机。
    bool setConnectionKey(const QString& new_key);
    // 某设备的密钥是否已在本机记住（记住后连接不再询问）
    QString savedKeyFor(const QString& deviceId) const;
    void forgetSavedKey(const QString& deviceId);
    QStringList savedKeyDevices() const;

    void login(const QString& identifier, const QString& password);
    void registerAccount(const QString& username, const QString& email, const QString& password);
    void logout();

    // 用户点击「把本机加入设备列表」时调用。
    // 只有到这一步才生成设备私钥、设备 ID 和连接密钥。
    void addThisDevice();

    void refreshDevices();
    void removeDevice(const QString& deviceId);

    // 连接目标设备：必须提供该设备的连接密钥
    void connectToDevice(const QString& deviceId, const QString& connectionKey);
    void disconnectSession();

    // 无人值守凭据（仅内存）：token 过期被服务器拒绝时自动重登
    void setAutoLoginCredentials(const QString& email, const QString& password) {
        auto_email_ = email;
        auto_pass_  = password;
    }

    // ---- 主控端控制面（RemoteControlView 调用；经 ch-control 发给被控端）----
    void requestRemoteScreens();
    void switchRemoteScreen(int index);
    // 画质档位（1080P/2K/4K）与帧率（30/60）以「上限分辨率 + 帧率 + 码率」下发
    void setRemoteVideoConfig(int width, int height, int fps, int bitrate_kbps);
    void setVideoAccelerationMode(pxc::VideoAccelerationMode mode);
    void requestRemoteKeyframe();
    void requestRemoteWallpaper();
    void requestLocalWallpaper();
    // 触控板/键鼠事件（payload 为 input 事件字段，不含 kind 封装）
    void sendInputEvent(const QJsonObject& event);

    // P2P 文件传输（需先完成连接密钥认证）。
    void requestRemoteFileList(const QString& path);
    void createRemoteDirectory(const QString& parentPath, const QString& name);
    QString requestRemoteFileDownload(const QString& remotePath, const QString& localPath);
    QString offerFileUpload(const QString& localPath);
    void acceptIncomingFile(const QString& transferId, const QString& destinationPath);
    void rejectIncomingFile(const QString& transferId);

signals:
    void deviceRemovalFinished(const QString& deviceId, bool success, const QString& message);
    void loginSucceeded(const QString& username);
    void loginFailed(const QString& reason);
    void registerFinished(bool ok, const QString& message);

    void devicesUpdated(const QVector<pxc::gui::DeviceRow>& devices);
    void deviceListFailed(const QString& reason);

    void deviceEnrolled(const QString& deviceId, const QString& connectionKey);
    void deviceEnrollFailed(const QString& reason);

    // 会话状态：connecting / authenticating / authenticated / rejected / closed
    void sessionStateChanged(const QString& deviceId, const QString& state);
    // 面向用户的日志行
    void logMessage(const QString& message);

    // 主控端：远端画面与控制面应答
    void videoFrameReady(quint32 frameId, const QImage& image);
    void desktopWallpaperReady(const QString& deviceId, const QImage& image);
    void remoteScreensUpdated(const QJsonArray& screens);
    void remoteVideoStateChanged(int width, int height, int fps, int screenIndex,
                                 const QString& encoderName, const QString& error);
    void videoNotice(const QString& message, bool isError);
    // 被控端：会话已建立（界面显示「被控中」提示）
    void controlledSessionStarted();
    // 本机连接密钥被用户修改
    void connectionKeyChanged(const QString& newKey);
    void fileTransferMessageReceived(const QJsonObject& message);
    void incomingFileOffer(const QString& transferId, const QString& fileName, qint64 size);
    void fileTransferProgress(const QString& transferId, const QString& fileName,
                              qint64 transferred, qint64 total, const QString& state);

private:
    void startSignalingIfPossible();
    void handleSignalingMessage(const pxc::Message& msg);
    void ensureSession(pxc::PeerSession::Role role);
    void wireChannel(const std::shared_ptr<rtc::DataChannel>& channel);
    void syncClipboard();
    void onControlMessage(const QString& text);
    // ch-control 的 JSON 控制面（命令/输入/应答分发）
    void handleSessionJson(const nlohmann::json& j);
    void handleControlledCommand(const nlohmann::json& j);
    void handleInputEvent(const nlohmann::json& j);
    void sendSessionJson(const nlohmann::json& j);
    void sendVideoStateReply(const QString& error = QString());
    void handleFileTransferMessage(const QJsonObject& message);
    void handleFileTransferChunk(const QByteArray& chunk);
    void sendFileTransferJson(const QJsonObject& message);
    void beginOutgoingFile(const QString& transferId, const QString& path,
                           const QString& direction);
    void pumpOutgoingFile();
    void finishOutgoingFile(const QString& state);

    // 被控端视频流生命周期（认证通过后启动）
    void startControlledStream();
    void stopControlledStream();

    // 在后台线程执行，结果回到 UI 线程；析构时会等待这些任务结束。
    void runAsync(std::function<void()>&& fn);
    void joinWorkers();

    QString apiUrl_;
    QString wsUrl_;
    QString identityPath_;

    std::unique_ptr<pxc::AccountApiClient> api_;
    std::unique_ptr<pxc::SignalingClient>  signaling_;
    std::shared_ptr<pxc::PeerSession>      session_;
    std::atomic<uint64_t> session_generation_{0};

    std::string accessToken_;   // 只在内存中，不落盘
    QString     username_;
    QString     localDeviceId_;
    QString     localConnectionKey_;
    QSet<QString> removing_devices_;
    quint64 device_list_revision_ = 0;
    bool local_device_removed_ = false;

    // 当前会话（连接目标设备）
    QString      targetDeviceId_;
    QString      targetConnectionKey_;
    std::string  sessionNonce_;
    bool         sessionAuthenticated_ = false;
    SessionSide  session_side_ = SessionSide::None;

    // 视频链路
    std::unique_ptr<VideoSender>      video_sender_;    // 被控端
    std::unique_ptr<VideoReceiver>    video_receiver_;  // 主控端
    std::unique_ptr<pxc::InputInjector> injector_;      // 被控端
    std::shared_ptr<QMimeData> text_clipboard_backup_;
    QString text_clipboard_value_;
    uint64_t text_clipboard_generation_ = 0;
    QString clipboard_last_text_;
    bool clipboard_applying_ = false;
    bool clipboard_has_text_ = false;
    bool clipboard_read_pending_ = false;
    bool clipboard_read_again_ = false;
    bool paste_read_pending_ = false;
    std::vector<nlohmann::json> paste_input_queue_;
    QString                            last_sender_error_;  // 去重后的采集错误日志
    int                                 pair_log_tick_ = 0;  // 链路日志节流

    // P2P 建连超时（authenticating 后 20 秒未认证成功则取消会话）
    QTimer* auth_timeout_timer_ = nullptr;
    QString auto_email_;  // 无人值守自动重登凭据（仅内存）
    QString auto_pass_;

    QFile* outgoing_file_ = nullptr;
    QSaveFile* incoming_file_ = nullptr;
    QTimer* file_send_timer_ = nullptr;
    QString outgoing_file_id_;
    QString outgoing_file_name_;
    QString outgoing_direction_;
    qint64 outgoing_total_ = 0;
    qint64 outgoing_transferred_ = 0;
    QString incoming_file_id_;
    QString incoming_file_name_;
    qint64 incoming_total_ = 0;
    qint64 incoming_transferred_ = 0;
    QString pending_upload_id_;
    QString pending_upload_path_;
    QString pending_download_id_;
    QString pending_download_path_;

    std::atomic<bool> destroyed_{false};
    std::mutex              workers_mtx_;
    std::vector<std::thread> workers_;
};

}  // namespace pxc::gui

// DeviceRow 会通过 Qt::QueuedConnection 跨线程传递，
// 必须注册为元类型，否则运行时连接会失败并只打印一句警告。
Q_DECLARE_METATYPE(pxc::gui::DeviceRow)
Q_DECLARE_METATYPE(QVector<pxc::gui::DeviceRow>)
