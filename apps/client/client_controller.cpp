#include "client_controller.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QBuffer>
#include <QClipboard>
#include <QMimeData>
#include <QGuiApplication>
#include <QImageReader>
#include <QScreen>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QMetaObject>
#include <QHostInfo>
#include <QSettings>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtGlobal>

#include <algorithm>
#include <cstring>
#include <future>
#include <thread>

#include <nlohmann/json.hpp>

#include "pxc/crypto.h"
#include "pxc/session_auth.h"
#include "pxc/video_protocol.h"
#include "input_activity.h"
#include "desktop_wallpaper.h"
#include "session_wire.h"
#include "video_sender.h"
#include "clipboard_reader.h"
#include <utility>

namespace pxc::gui {
namespace {

QString to_qstring(const std::string& value) {
    return QString::fromStdString(value);
}

std::string to_std(const QString& value) {
    return value.toStdString();
}

// 设备显示名：主机名（信令注册时上报，服务器存为设备名）
std::string device_display_name() {
    QString name = QHostInfo::localHostName();
    if (name.isEmpty()) name = QSysInfo::machineHostName();
    if (name.isEmpty()) name = QStringLiteral("pixelconnection-client");
    return name.toStdString();
}

QJsonArray to_qjson(const nlohmann::json& j) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(j.dump())).array();
}

nlohmann::json to_nlohmann(const QJsonObject& obj) {
    return nlohmann::json::parse(QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString());
}

// 被控端视频的默认档位：1080P / 30 帧 / 8 Mbps（进入会话后的初始值）
pxc::VideoEncoderConfig default_video_config() {
    pxc::VideoEncoderConfig cfg;
    cfg.width        = 1920;
    cfg.height       = 1080;
    cfg.fps          = 30;
    cfg.bitrate_kbps = 8000;
    return cfg;
}

}  // namespace

ClientController::ClientController(QObject* parent) : QObject(parent) {
    if (auto* clipboard = QGuiApplication::clipboard()) {
        connect(clipboard, &QClipboard::dataChanged, this, &ClientController::syncClipboard);
    }
    // 主控端接收链路：解码在 rtc 回调线程，信号 queued 回 UI
    video_receiver_ = std::make_unique<VideoReceiver>(this);
    file_send_timer_ = new QTimer(this);
    file_send_timer_->setInterval(2);
    connect(file_send_timer_, &QTimer::timeout, this, &ClientController::pumpOutgoingFile);
    connect(video_receiver_.get(), &VideoReceiver::frameDecoded, this,
            [this](quint32 frameId, const QImage& image) {
                emit videoFrameReady(frameId, image);
            });
    connect(video_receiver_.get(), &VideoReceiver::videoNotice, this,
            [this](const QString& message, bool isError) {
                emit videoNotice(message, isError);
            });
    // 丢帧/解码器重建 → 立即请求发送端补发关键帧（PLI 等价物）。
    // 接收端检测到流空洞时若不请求，脏画面要等到发送端下一个空闲
    // 关键帧（实测 2-4 秒）才能恢复。
    connect(video_receiver_.get(), &VideoReceiver::keyframeRequested, this,
            [this] {
                if (session_side_ == SessionSide::Controller &&
                    sessionAuthenticated_) {
                    requestRemoteKeyframe();
                }
            },
            Qt::QueuedConnection);

    // 被控端：周期性回显视频实际状态（实际编码尺寸要等第一帧才有）
    auto* state_timer = new QTimer(this);
    state_timer->setInterval(2000);
    connect(state_timer, &QTimer::timeout, this, [this] {
        // 链路路径/RTT：每 10 秒（5 个周期）记录一次实际选中候选对
        if (session_ && sessionAuthenticated_ && ++pair_log_tick_ >= 5) {
            pair_log_tick_ = 0;
            const auto rtt = session_->rtt();
            emit logMessage(QStringLiteral("[P2P] pair=%1 rtt=%2ms")
                                .arg(to_qstring(session_->selected_pair_string()))
                                .arg(rtt ? QString::number(rtt->count())
                                         : QStringLiteral("n/a")));
        }
        if (session_side_ == SessionSide::Controlled && video_sender_) {
            sendVideoStateReply();
            // 采集/编码错误经日志透出一次（重试期间持续可见）
            const QString err = to_qstring(video_sender_->last_error());
            if (!err.isEmpty() && err != last_sender_error_) {
                last_sender_error_ = err;
                emit logMessage(QStringLiteral("[视频] ") + err);
            } else if (err.isEmpty()) {
                last_sender_error_.clear();
            }
        }
    });
    state_timer->start();

    // P2P 建连超时：进入 authenticating 后 20 秒仍未认证成功则给出明确失败，
    // 避免界面无限卡在「正在校验密钥」（ICE 被网络策略拦截时会出现这种挂起）
    auth_timeout_timer_ = new QTimer(this);
    auth_timeout_timer_->setSingleShot(true);
    auth_timeout_timer_->setInterval(20000);
    connect(auth_timeout_timer_, &QTimer::timeout, this, [this] {
        if (sessionAuthenticated_ || !session_) return;
        const QString device = targetDeviceId_;
        // 先改状态再给原因（状态会覆盖状态栏文本，原因要留在最后）
        emit sessionStateChanged(device, QStringLiteral("p2p_timeout"));
        emit logMessage(QStringLiteral("P2P 建连超时（可能被网络策略拦截 UDP），"
                                       "会话已取消；可在 设置-网络 配置 TURN 中继后重试"));
        disconnectSession();
    });
}

ClientController::~ClientController() {
    // 先禁止 worker 再投递 UI 回调，再停止网络对象，最后等待所有任务退出。
    destroyed_.store(true);

    stopControlledStream();
    if (session_) session_->close();
    if (signaling_) signaling_->stop();
    joinWorkers();
}

bool ClientController::isDeviceEnrolled() const {
    return !localConnectionKey_.isEmpty();
}

QString ClientController::localDeviceId() const {
    return localDeviceId_;
}

QString ClientController::localConnectionKey() const {
    return localConnectionKey_;
}

// ------------------------------------------------------------------ 通用工具

void ClientController::runAsync(std::function<void()>&& fn) {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (destroyed_.load()) return;

    workers_.emplace_back([this, task = std::move(fn)]() mutable {
        try {
            task();
        } catch (const std::exception& e) {
            // invokeMethod 只能在对象还活着时调用。析构先置 destroyed_，
            // 并在 joinWorkers() 等待本线程结束，保证 this 仍有效。
            if (!destroyed_.load()) {
                const QString text = QString::fromUtf8(e.what());
                QMetaObject::invokeMethod(
                    this,
                    [this, text] {
                        if (!destroyed_.load())
                            emit logMessage(QStringLiteral("后台任务异常: ") + text);
                    },
                    Qt::QueuedConnection);
            }
        }
    });
}

void ClientController::joinWorkers() {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        workers.swap(workers_);
    }
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}

void ClientController::configure(const QString& apiUrl, const QString& wsUrl,
                                 const QString& identityPath) {
    ++device_list_revision_;
    local_device_removed_ = false;
    apiUrl_       = apiUrl;
    wsUrl_        = wsUrl;
    identityPath_ = identityPath;

    api_ = std::make_unique<pxc::AccountApiClient>(to_std(apiUrl));

    // 若本机已加入账号，先把设备 ID 和连接密钥读出来供界面显示
    pxc::Identity identity;
    if (pxc::Identity::load(to_std(identityPath_), identity)) {
        localDeviceId_ = to_qstring(identity.device_id());
        if (!identity.connection_key().empty()) {
            localConnectionKey_ = to_qstring(identity.connection_key());
        }
    }

    signaling_ = std::make_unique<pxc::SignalingClient>(to_std(wsUrl), device_display_name());

    pxc::SignalingClient::Callbacks cb;
    cb.on_open = [this] {
        if (destroyed_.load()) return;
        QMetaObject::invokeMethod(
            this, [this] { emit logMessage(QStringLiteral("信令已连接")); },
            Qt::QueuedConnection);
    };
    cb.on_close = [this](const std::string& reason) {
        if (destroyed_.load()) return;
        const QString text = to_qstring(reason);
        QMetaObject::invokeMethod(
            this,
            [this, text] {
                emit logMessage(QStringLiteral("信令断开: ") + text);
                const QString target = targetDeviceId_;
                disconnectSession();
                emit sessionStateChanged(target, QStringLiteral("closed"));
            },
            Qt::QueuedConnection);
    };
    cb.on_error = [this](const std::string& reason) {
        if (destroyed_.load()) return;
        const QString text = to_qstring(reason);
        QMetaObject::invokeMethod(
            this, [this, text] { emit logMessage(QStringLiteral("信令错误: ") + text); },
            Qt::QueuedConnection);
    };
    cb.on_authenticated = [this](const pxc::Message& msg) {
        if (destroyed_.load()) return;
        const QString account = to_qstring(msg.account);
        QMetaObject::invokeMethod(
            this,
            [this, account] {
                emit logMessage(QStringLiteral("设备身份认证通过（") + account +
                                QStringLiteral("），正在签名上线"));
            },
            Qt::QueuedConnection);
    };
    cb.on_registered = [this](const pxc::Message& msg) {
        if (destroyed_.load()) return;
        const QString deviceId = to_qstring(msg.device_id);
        QMetaObject::invokeMethod(
            this,
            [this, deviceId] {
                emit logMessage(QStringLiteral("本机已上线: ") + deviceId);
                refreshDevices();
            },
            Qt::QueuedConnection);
    };
    cb.on_message = [this](const pxc::Message& msg) {
        if (destroyed_.load()) return;
        // 信令回调在 I/O 线程，必须切回 UI 线程再动界面
        const pxc::Message copy = msg;
        QMetaObject::invokeMethod(
            this, [this, copy] { handleSignalingMessage(copy); }, Qt::QueuedConnection);
    };
    signaling_->set_callbacks(std::move(cb));
}

// ------------------------------------------------------------------ 密钥管理

bool ClientController::setConnectionKey(const QString& new_key) {
    const QString key = new_key.trimmed();
    if (key.size() < 5) return false;

    pxc::Identity identity;
    if (!pxc::Identity::load(to_std(identityPath_), identity)) return false;

    identity.set_connection_key(to_std(key));
    if (!identity.save(to_std(identityPath_))) return false;

    localConnectionKey_ = key;
    // 重新上线信令：注册消息里的 connection_verifier 由新密钥派生，
    // 服务器需要拿到新的派生值。
    if (signaling_) signaling_->stop();
    startSignalingIfPossible();

    emit connectionKeyChanged(key);
    emit logMessage(QStringLiteral("本机连接密钥已更新为自定义密码"));
    return true;
}

QString ClientController::savedKeyFor(const QString& deviceId) const {
    QSettings settings;
    return settings.value(QStringLiteral("saved_keys/") + deviceId).toString();
}

void ClientController::forgetSavedKey(const QString& deviceId) {
    QSettings settings;
    settings.remove(QStringLiteral("saved_keys/") + deviceId);
}

QStringList ClientController::savedKeyDevices() const {
    QSettings settings;
    settings.beginGroup(QStringLiteral("saved_keys"));
    const QStringList devices = settings.childKeys();
    settings.endGroup();
    return devices;
}

// ------------------------------------------------------------------ 账号

void ClientController::login(const QString& identifier, const QString& password) {
    if (!api_) {
        emit loginFailed(QStringLiteral("尚未配置服务器地址"));
        return;
    }
    auto_email_ = identifier;
    auto_pass_ = password;

    const std::string user = to_std(identifier);
    const std::string pass = to_std(password);

    runAsync([this, user, pass] {
        std::string token, name, error;
        const bool  ok = api_->login(user, pass, token, name, error);

        if (destroyed_.load()) return;

        if (!ok) {
            const QString reason = to_qstring(error);
            QMetaObject::invokeMethod(
                this, [this, reason] { emit loginFailed(reason); }, Qt::QueuedConnection);
            return;
        }

        accessToken_ = token;
        username_    = to_qstring(name.empty() ? user : name);

        QMetaObject::invokeMethod(
            this,
            [this] {
                emit loginSucceeded(username_);
                startSignalingIfPossible();
                refreshDevices();
            },
            Qt::QueuedConnection);
    });
}

void ClientController::registerAccount(const QString& username, const QString& email,
                                       const QString& password) {
    if (!api_) {
        emit registerFinished(false, QStringLiteral("尚未配置服务器地址"));
        return;
    }

    const std::string user  = to_std(username);
    const std::string mail  = to_std(email);
    const std::string pass  = to_std(password);

    runAsync([this, user, mail, pass] {
        std::string error;
        const bool  ok = api_->register_account(user, mail, pass, error);
        if (destroyed_.load()) return;

        const bool    success = ok;
        const QString message = ok ? QStringLiteral("注册成功，请登录")
                                   : to_qstring(error);
        QMetaObject::invokeMethod(
            this,
            [this, success, message] { emit registerFinished(success, message); },
            Qt::QueuedConnection);
    });
}

void ClientController::logout() {
    ++device_list_revision_;
    if (!accessToken_.empty() && api_) {
        const std::string token = accessToken_;
        runAsync([this, token] {
            std::string error;
            api_->logout(token, error);  // 失败也让本地状态失效
        });
    }

    accessToken_.clear();
    username_.clear();
    auto_email_.clear();
    auto_pass_.clear();
    if (signaling_) signaling_->stop();
    disconnectSession();
    emit sessionStateChanged(QString(), QStringLiteral("closed"));
}

// ------------------------------------------------------------------ 设备

void ClientController::addThisDevice() {
    if (accessToken_.empty()) {
        emit deviceEnrollFailed(QStringLiteral("请先登录账号"));
        return;
    }

    local_device_removed_ = false;

    // 已是本账号设备就不重复生成密钥
    pxc::Identity existing;
    if (pxc::Identity::load(to_std(identityPath_), existing) &&
        !existing.connection_key().empty()) {
        localDeviceId_        = to_qstring(existing.device_id());
        localConnectionKey_   = to_qstring(existing.connection_key());
        startSignalingIfPossible();
        emit deviceEnrolled(localDeviceId_, localConnectionKey_);
        emit logMessage(QStringLiteral("本机已是设备，无需重复添加"));
        return;
    }

    // 用户明确点了「加入设备列表」，此时才生成设备私钥、设备 ID 和连接密钥。
    // 私钥只写入本机文件（0600），永不上传；服务器只保存公钥用于验证签名。
    try {
        pxc::Identity identity = pxc::Identity::generate();
        identity.set_connection_key(pxc::crypto::generate_connection_key());

        if (!identity.save(to_std(identityPath_))) {
            emit deviceEnrollFailed(QStringLiteral("无法保存设备身份文件: ") + identityPath_);
            return;
        }

        localDeviceId_      = to_qstring(identity.device_id());
        localConnectionKey_ = to_qstring(identity.connection_key());
    } catch (const std::exception& e) {
        emit deviceEnrollFailed(QStringLiteral("生成设备身份失败: ") +
                                QString::fromUtf8(e.what()));
        return;
    }

    startSignalingIfPossible();
    emit deviceEnrolled(localDeviceId_, localConnectionKey_);
}

void ClientController::startSignalingIfPossible() {
    if (local_device_removed_) return;
    if (!signaling_ || accessToken_.empty()) return;

    pxc::Identity identity;
    if (!pxc::Identity::load(to_std(identityPath_), identity)) return;
    if (identity.connection_key().empty()) return;  // 还没加入账号

    signaling_->set_credentials(accessToken_, identity, "");
    if (!signaling_->connected()) signaling_->start();
    // 已连接时 set_credentials 会主动补发一次认证
}

void ClientController::refreshDevices() {
    if (!api_ || accessToken_.empty()) return;

    const std::string token = accessToken_;
    const quint64 revision = device_list_revision_;
    runAsync([this, token, revision] {
        std::vector<pxc::PeerInfo> devices;
        std::string                error;
        const bool                 ok = api_->list_devices(token, devices, error);

        if (destroyed_.load()) return;

        if (!ok) {
            const QString reason = to_qstring(error);
            QMetaObject::invokeMethod(
                this, [this, reason] { emit deviceListFailed(reason); }, Qt::QueuedConnection);
            return;
        }

        QVector<DeviceRow> rows;
        rows.reserve(static_cast<int>(devices.size()));
        for (const auto& d : devices) {
            DeviceRow row;
            row.deviceId = to_qstring(d.device_id);
            row.name     = to_qstring(d.name);
            row.publicIp = to_qstring(d.public_ip);
            row.lastSeen = to_qstring(d.last_seen);
            row.platform = to_qstring(d.platform);
            row.online   = d.online;
            rows.push_back(row);
        }

        QMetaObject::invokeMethod(
            this, [this, rows, token, revision] {
                if (token == accessToken_ && revision == device_list_revision_) emit devicesUpdated(rows);
            }, Qt::QueuedConnection);
    });
}

void ClientController::removeDevice(const QString& deviceId) {
    const QString id = to_qstring(pxc::Identity::normalize_device_id(to_std(deviceId)));
    if (!api_ || accessToken_.empty() || id.isEmpty()) {
        emit deviceRemovalFinished(deviceId, false, QStringLiteral("请先登录并选择要删除的设备"));
        return;
    }
    if (removing_devices_.contains(id)) return;
    removing_devices_.insert(id);
    const std::string token = accessToken_;
    const std::string apiUrl = to_std(apiUrl_);
    runAsync([this, id, token, apiUrl] {
        pxc::AccountApiClient api(apiUrl);
        std::string error;
        const bool success = api.remove_device(token, to_std(id), error);
        if (destroyed_.load()) return;
        QMetaObject::invokeMethod(this, [this, id, token, success, error] {
            removing_devices_.remove(id);
            if (token != accessToken_) {
                emit deviceRemovalFinished(id, false, QStringLiteral("账号已切换，请刷新设备列表"));
                return;
            }
            if (!success) {
                emit deviceRemovalFinished(id, false, QStringLiteral("删除设备失败：") + to_qstring(error));
                return;
            }
            ++device_list_revision_;
            const auto normalized = [](const QString& value) {
                return to_qstring(pxc::Identity::normalize_device_id(to_std(value)));
            };
            const bool local = !localDeviceId_.isEmpty() && normalized(localDeviceId_) == id;
            if (local || normalized(targetDeviceId_) == id) disconnectSession();
            if (local) {
                local_device_removed_ = true;
                if (signaling_) signaling_->stop();
                localDeviceId_.clear();
                localConnectionKey_.clear();
            }
            for (const QString& saved : savedKeyDevices())
                if (normalized(saved) == id) forgetSavedKey(saved);
            QSettings settings;
            QStringList recent = settings.value(QStringLiteral("assist/recent_devices")).toStringList();
            recent.erase(std::remove_if(recent.begin(), recent.end(),
                [&](const QString& value) { return normalized(value) == id; }), recent.end());
            settings.setValue(QStringLiteral("assist/recent_devices"), recent);
            emit deviceRemovalFinished(id, true, QStringLiteral("设备已从当前账号删除"));
            refreshDevices();
        }, Qt::QueuedConnection);
    });
}

// ------------------------------------------------------------------ 会话

void ClientController::connectToDevice(const QString& deviceId, const QString& connectionKey) {
    if (session_side_ == SessionSide::Controlled) {
        // A blocked outbound action must not change or close the inbound session.
        emit logMessage(QStringLiteral("本机正在被远控，暂不能连接其他设备"));
        return;
    }
    if (accessToken_.empty()) {
        emit sessionStateChanged(deviceId, QStringLiteral("rejected"));
        emit logMessage(QStringLiteral("请先登录账号"));
        return;
    }

    // 记住密钥后连接不再询问：显式传入的密钥优先，其次用记住的。
    QString key = connectionKey.trimmed();
    if (key.isEmpty()) key = savedKeyFor(deviceId);
    if (key.isEmpty()) {
        emit sessionStateChanged(deviceId, QStringLiteral("rejected"));
        emit logMessage(QStringLiteral("必须输入目标设备的连接密钥"));
        return;
    }
    if (!signaling_ || !signaling_->online()) {
        emit sessionStateChanged(deviceId, QStringLiteral("rejected"));
        emit logMessage(QStringLiteral("本机尚未上线，无法发起连接"));
        return;
    }

    disconnectSession();
    session_side_         = SessionSide::Controller;
    targetDeviceId_       = deviceId;
    targetConnectionKey_  = key;
    sessionAuthenticated_ = false;

    pxc::Message msg;
    msg.type   = pxc::kConnectRequest;
    msg.target = to_std(deviceId);
    signaling_->send(msg);

    emit sessionStateChanged(deviceId, QStringLiteral("connecting"));
    emit logMessage(QStringLiteral("已请求连接 ") + deviceId + QStringLiteral("，等待对方确认"));
}

void ClientController::disconnectSession() {
    const bool had_session = session_ || sessionAuthenticated_ ||
        session_side_ != SessionSide::None;
    const QString target = targetDeviceId_;
    clipboard_has_text_ = false;
    paste_read_pending_ = false;
    paste_input_queue_.clear();
    ++session_generation_; // invalidate all queued callbacks before closing rtc
    auth_timeout_timer_->stop();
    finishOutgoingFile(QStringLiteral("会话已断开"));
    if (incoming_file_) {
        incoming_file_->cancelWriting();
        delete incoming_file_;
        incoming_file_ = nullptr;
    }
    incoming_file_id_.clear();
    incoming_file_name_.clear();
    incoming_total_ = incoming_transferred_ = 0;
    pending_upload_id_.clear();
    pending_upload_path_.clear();
    pending_download_id_.clear();
    pending_download_path_.clear();
    stopControlledStream();
    if (video_receiver_) video_receiver_->reset();
    sessionAuthenticated_ = false;
    session_side_         = SessionSide::None;
    sessionNonce_.clear();
    targetConnectionKey_.clear();
    auto session = std::move(session_);
    if (session) session->close();
    // Closing invalidates rtc callbacks above. Publish the terminal UI state
    // here instead of waiting for a callback that will correctly be ignored.
    if (had_session) emit sessionStateChanged(target, QStringLiteral("closed"));
}

void ClientController::handleSignalingMessage(const pxc::Message& msg) {
    if (msg.type == pxc::kIncoming) {
        // glare 处理：本端已有活动会话时拒绝新请求（双方同时互连会死锁）
        if (session_ || sessionAuthenticated_) {
            pxc::Message deny;
            deny.type   = pxc::kConnectResponse;
            deny.target = msg.from;
            deny.accept = false;
            signaling_->send(deny);
            emit logMessage(QStringLiteral("收到来自 ") + to_qstring(msg.from) +
                            QStringLiteral(" 的连接请求，但本端已有活动会话，已拒绝"));
            return;
        }
        // 安全设置：允许同账号控制本设备（UU 远程安全页第一项）。
        QSettings settings;
        if (!settings.value(QStringLiteral("security/allow_remote"), true).toBool()) {
            pxc::Message deny;
            deny.type = pxc::kConnectResponse;
            deny.target = msg.from;
            deny.accept = false;
            signaling_->send(deny);
            emit logMessage(QStringLiteral("已拒绝远程连接（本机关闭了远程协助）"));
            return;
        }
        if (!settings.value(QStringLiteral("security/allow_same_account"), true).toBool()) {
            pxc::Message deny;
            deny.type   = pxc::kConnectResponse;
            deny.target = msg.from;
            deny.accept = false;
            signaling_->send(deny);
            emit logMessage(QStringLiteral("已拒绝来自 ") + to_qstring(msg.from) +
                            QStringLiteral(" 的连接（安全设置不允许被控）"));
            return;
        }
        // 本机被请求连接。真正的授权在连接密钥校验：没有密钥的主控端无法通过。
        emit logMessage(QStringLiteral("收到来自 ") + to_qstring(msg.from) +
                        QStringLiteral(" 的连接请求"));
        pxc::Message reply;
        reply.type   = pxc::kConnectResponse;
        reply.target = msg.from;
        reply.accept = true;
        targetDeviceId_ = to_qstring(msg.from);
        // 被控端必须先创建 Answerer；它不能自己创建 DataChannel/offer。
        session_side_ = SessionSide::Controlled;
        ensureSession(pxc::PeerSession::Role::Answerer);
        signaling_->send(reply);
        return;
    }

    if (msg.type == pxc::kAnswered) {
        if (session_side_ != SessionSide::Controller ||
            to_qstring(msg.from) != targetDeviceId_) return;
        if (!msg.accept) {
            disconnectSession();
            emit sessionStateChanged(targetDeviceId_, QStringLiteral("rejected"));
            emit logMessage(QStringLiteral("对方拒绝了连接"));
            return;
        }
        // 主控端收到接受后创建 Offerer，创建 DataChannel 并生成 SDP offer。
        ensureSession(pxc::PeerSession::Role::Offerer);
        auth_timeout_timer_->start();
        emit sessionStateChanged(targetDeviceId_, QStringLiteral("authenticating"));
        return;
    }

    if (msg.type == pxc::kSdp) {
        if (to_qstring(msg.from) != targetDeviceId_ || session_side_ == SessionSide::None) return;
        if (session_) session_->set_remote_description(msg.sdp, msg.sdp_type);
        return;
    }

    if (msg.type == pxc::kCandidate) {
        if (to_qstring(msg.from) != targetDeviceId_ || session_side_ == SessionSide::None) return;
        if (session_) session_->add_remote_candidate(msg.candidate, msg.mid);
        return;
    }

    if (msg.type == pxc::kError) {
        emit logMessage(QStringLiteral("服务器错误: ") + to_qstring(msg.code) + " " +
                        to_qstring(msg.detail));
        if ((msg.code == pxc::kErrNoSuchPeer || msg.code == pxc::kErrPeerOffline || msg.code == pxc::kErrBusy) &&
            session_side_ == SessionSide::Controller && !sessionAuthenticated_) {
            const QString target = targetDeviceId_;
            disconnectSession();
            emit sessionStateChanged(target,QStringLiteral("rejected"));
        }
        // token 过期自动重登（无人值守被控端必须持续在线）
        if (msg.code == pxc::kErrUnauthorized && !auto_email_.isEmpty() &&
            !username_.isEmpty()) {
            emit logMessage(QStringLiteral("登录凭证已过期，正在自动重新登录..."));
            login(auto_email_, auto_pass_);
        }
        return;
    }

    if (msg.type == pxc::kPeerOnline || msg.type == pxc::kPeerOffline) {
        if (msg.type == pxc::kPeerOffline &&
            pxc::Identity::normalize_device_id(msg.device_id) ==
                pxc::Identity::normalize_device_id(to_std(targetDeviceId_))) {
            const QString target = targetDeviceId_;
            disconnectSession();
            emit sessionStateChanged(target, QStringLiteral("closed"));
        }
        refreshDevices();
        return;
    }
}

void ClientController::ensureSession(pxc::PeerSession::Role role) {
    if (session_) return;

    pxc::PeerSession::Config config;
    // STUN 默认关闭：不可达的 STUN 会把候选收集卡死（本机内网直连
    // 根本用不到它）。跨 NAT 部署时在 设置-网络 配置 STUN/TURN。
    config.enable_ice_udp_mux = true;

    // STUN（可选）：设置-网络 配置后启用
    QSettings settings;
    const QString stun_url = settings.value(QStringLiteral("net/stun_url")).toString();
    if (!stun_url.isEmpty()) {
        config.stun_servers.push_back(stun_url.toStdString());
    }

    // TURN 中继：直连失败时 ICE 自动退回到中继；地址/账号在 设置-网络 配置。
    // 直连成功时视频数据不会经过中继服务器。
    const QString turn_url = settings.value(QStringLiteral("net/turn_url")).toString();
    if (!turn_url.isEmpty()) {
        pxc::PeerSession::IceServer turn;
        turn.url        = turn_url.toStdString();
        turn.username   = settings.value(QStringLiteral("net/turn_user")).toString().toStdString();
        turn.credential = settings.value(QStringLiteral("net/turn_pass")).toString().toStdString();
        config.ice_servers.push_back(turn);
    }

    session_ = pxc::PeerSession::create(role, std::move(config));
    const uint64_t generation = ++session_generation_;
    const std::string target = to_std(targetDeviceId_);
    auth_timeout_timer_->start(); // also bounds an abandoned inbound auth attempt

    pxc::PeerSession::Callbacks cb;
    cb.on_log = [this](const std::string& text) {
        if (destroyed_.load()) return;
        const QString line = to_qstring(text);
        QMetaObject::invokeMethod(
            this, [this, line] { emit logMessage(QStringLiteral("[P2P] ") + line); },
            Qt::QueuedConnection);
    };
    cb.on_state = [this, generation](rtc::PeerConnection::State state) {
        if (destroyed_.load()) return;
        QString name;
        using S = rtc::PeerConnection::State;
        switch (state) {
            case S::Connected:    name = QStringLiteral("connected"); break;
            case S::Connecting:   name = QStringLiteral("connecting"); break;
            case S::Failed:       name = QStringLiteral("rejected"); break;
            case S::Disconnected: name = QStringLiteral("closed"); break;
            case S::Closed:       name = QStringLiteral("closed"); break;
            default:              name = QStringLiteral("connecting"); break;
        }
        // 会话终结时清理视频链路（在 UI 线程做，避免在 rtc 回调线程 join 线程）
        const bool ended = (state == S::Failed || state == S::Disconnected ||
                            state == S::Closed);
        QMetaObject::invokeMethod(
            this,
            [this, generation, name, ended] {
                if (generation != session_generation_) return;
                if (ended) {
                    // 会话终结必须完整清理（与 disconnectSession 一致）：
                    // 只停流不清 session_ 的话，被控端的 glare 检查
                    // （session_ || sessionAuthenticated_）会永远拒绝新的
                    // 连接请求——实测断开后重连提示“对方拒绝了连接”，
                    // 只有被控端重新登录才恢复。
                    disconnectSession();
                }
                emit sessionStateChanged(targetDeviceId_, name);
            },
            Qt::QueuedConnection);
    };
    cb.on_local_description = [this, generation, target](const std::string& sdp, const std::string& type) {
        if (destroyed_.load()) return;
        QMetaObject::invokeMethod(this, [this, generation, target, sdp, type] {
        if (generation != session_generation_ || !signaling_) return;
        pxc::Message out;
        out.type     = pxc::kSdp;
        out.target   = target;
        out.sdp      = sdp;
        out.sdp_type = type;
        signaling_->send(out);
        }, Qt::QueuedConnection);
    };
    cb.on_local_candidate = [this, generation, target](const std::string& candidate, const std::string& mid) {
        if (destroyed_.load()) return;
        QMetaObject::invokeMethod(this, [this, generation, target, candidate, mid] {
        if (generation != session_generation_ || !signaling_) return;
        pxc::Message out;
        out.type      = pxc::kCandidate;
        out.target    = target;
        out.candidate = candidate;
        out.mid       = mid;
        signaling_->send(out);
        }, Qt::QueuedConnection);
    };
    cb.on_data_channel = [this, generation](std::shared_ptr<rtc::DataChannel> channel) {
        if (destroyed_.load()) return;
        QMetaObject::invokeMethod(this, [this, generation, channel] {
            if (generation == session_generation_) wireChannel(channel);
        }, Qt::QueuedConnection);
    };
    session_->set_callbacks(std::move(cb));
    session_->start();

    // Offerer 本地创建通道；Answerer 通过 on_data_channel 收到它们。
    if (role == pxc::PeerSession::Role::Offerer) {
        wireChannel(session_->channel(pxc::kChControl));
        wireChannel(session_->channel(pxc::kChVideo));
        wireChannel(session_->channel(pxc::kChClip));
        wireChannel(session_->channel(pxc::kChFile));
    }
}

void ClientController::wireChannel(const std::shared_ptr<rtc::DataChannel>& channel) {
    if (!channel) return;

    const std::string label = channel->label();
    const uint64_t generation = session_generation_.load();

    if (label == pxc::kChVideo) {
        // ch-video：二进制分片。主控端收画面；被控端只发不收。
        channel->onOpen([this, generation, channel] {
            if (destroyed_.load() || generation != session_generation_) return;
            if (destroyed_.load()) return;
            QMetaObject::invokeMethod(
                this,
                [this, generation] {
            if (destroyed_.load() || generation != session_generation_) return; emit logMessage(QStringLiteral("视频通道已打开")); },
                Qt::QueuedConnection);
        });
        channel->onMessage([this, generation, channel](rtc::message_variant data) {
            if (destroyed_.load() || generation != session_generation_) return;
            if (session_side_ != SessionSide::Controller) return;
            if (!std::holds_alternative<rtc::binary>(data)) return;
            if (destroyed_.load()) return;
            auto& bytes = std::get<rtc::binary>(data);
            video_receiver_->push(bytes);
        });
        channel->onClosed([this, generation, channel] {
            if (destroyed_.load() || generation != session_generation_) return;
            QMetaObject::invokeMethod(
                this,
                [this, generation] {
            if (destroyed_.load() || generation != session_generation_) return; emit logMessage(QStringLiteral("视频通道已关闭")); },
                Qt::QueuedConnection);
        });
        return;
    }

    if (label == pxc::kChFile) {
        channel->onOpen([this, generation] {
            if (destroyed_.load() || generation != session_generation_) return;
            if (destroyed_.load()) return;
            QMetaObject::invokeMethod(this, [this, generation] {
            if (destroyed_.load() || generation != session_generation_) return;
                emit logMessage(QStringLiteral("文件传输通道已打开"));
                QJsonObject ready;
                ready.insert(QStringLiteral("op"), QStringLiteral("ready"));
                emit fileTransferMessageReceived(ready);
            }, Qt::QueuedConnection);
        });
        channel->onMessage([this, generation](rtc::message_variant data) {
            if (destroyed_.load() || generation != session_generation_) return;
            if (destroyed_.load()) return;
            if (std::holds_alternative<std::string>(data)) {
                const QByteArray raw = QByteArray::fromStdString(std::get<std::string>(data));
                QMetaObject::invokeMethod(this, [this, generation, raw] {
            if (destroyed_.load() || generation != session_generation_) return;
                    const QJsonDocument doc = QJsonDocument::fromJson(raw);
                    if (doc.isObject()) handleFileTransferMessage(doc.object());
                }, Qt::QueuedConnection);
            } else if (std::holds_alternative<rtc::binary>(data)) {
                const auto& bytes = std::get<rtc::binary>(data);
                const QByteArray chunk(reinterpret_cast<const char*>(bytes.data()),
                                       static_cast<qsizetype>(bytes.size()));
                QMetaObject::invokeMethod(this, [this, generation, chunk] {
            if (destroyed_.load() || generation != session_generation_) return;
                    handleFileTransferChunk(chunk);
                }, Qt::QueuedConnection);
            }
        });
        return;
    }

    if (label == pxc::kChClip) {
        channel->onOpen([this, generation] {
            QMetaObject::invokeMethod(this, [this, generation] {
                if (generation == session_generation_ && session_side_ == SessionSide::Controller)
                    syncClipboard();
            }, Qt::QueuedConnection);
        });
        channel->onMessage([this, generation](rtc::message_variant data) {
            if (destroyed_.load() || generation != session_generation_ ||
                !std::holds_alternative<std::string>(data)) return;
            const auto& raw = std::get<std::string>(data);
            if (raw.size() > 400 * 1024) return;
            const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(raw));
            if (!doc.isObject()) return;
            const auto obj = doc.object();
            if (obj.value(QStringLiteral("type")).toString() != QStringLiteral("text") ||
                !obj.value(QStringLiteral("text")).isString()) return;
            const auto text = obj.value(QStringLiteral("text")).toString();
            if (text.toUtf8().size() > 64 * 1024) return;
            QMetaObject::invokeMethod(this, [this, generation, text] {
                if (generation != session_generation_ || !sessionAuthenticated_) return;
                if (auto* clipboard = QGuiApplication::clipboard()) {
                    clipboard_last_text_ = text;
                    clipboard_has_text_ = true;
                    clipboard_applying_ = true;
                    clipboard->setText(text);
                    clipboard_applying_ = false;
                }
            }, Qt::QueuedConnection);
        });
        return;
    }
    if (label != pxc::kChControl) return;
    channel->onClosed([this, generation] {
        if (destroyed_.load()) return;
        QMetaObject::invokeMethod(this, [this, generation] {
            if (generation != session_generation_) return;
            const QString target = targetDeviceId_;
            disconnectSession();
            emit sessionStateChanged(target, QStringLiteral("closed"));
        }, Qt::QueuedConnection);
    });

    // ch-control：可靠有序。认证挑战/应答 + JSON 控制面都走这里。
    channel->onOpen([this, generation, channel] {
            if (destroyed_.load() || generation != session_generation_) return;
        if (destroyed_.load()) return;
        QMetaObject::invokeMethod(
            this,
            [this, generation, channel] {
            if (destroyed_.load() || generation != session_generation_) return;
                if (session_side_ == SessionSide::Controlled) {
                    // 被控端主动下发挑战；主控端等挑战到达后出示证明
                    if (localConnectionKey_.isEmpty()) {
                        emit logMessage(QStringLiteral("本机连接密钥不可用，拒绝接受控制"));
                        channel->send(pxc::kSessionAuthFail);
                        return;
                    }
                    sessionNonce_ = pxc::make_session_nonce();
                    channel->send(std::string(pxc::kSessionAuthChallenge) + " " +
                                  sessionNonce_);
                    emit logMessage(QStringLiteral("已下发连接认证挑战，等待主控端证明"));
                } else {
                    emit logMessage(QStringLiteral("控制通道已打开，等待认证挑战"));
                }
            },
            Qt::QueuedConnection);
    });

    channel->onMessage([this, generation, channel](rtc::message_variant data) {
            if (destroyed_.load() || generation != session_generation_) return;
        if (!std::holds_alternative<std::string>(data)) return;
        const std::string text = std::get<std::string>(std::move(data));
        if (destroyed_.load()) return;

        QMetaObject::invokeMethod(
            this, [this, generation, text] {
            if (destroyed_.load() || generation != session_generation_) return; onControlMessage(QString::fromStdString(text)); },
            Qt::QueuedConnection);
    });
}

void ClientController::onControlMessage(const QString& text) {
    const std::string message = to_std(text);
    const auto reject = [this] {
        if (auto channel = session_ ? session_->channel(pxc::kChControl) : nullptr)
            channel->send(pxc::kSessionAuthFail);
        const auto generation = session_generation_.load();
        const QString target = targetDeviceId_;
        emit sessionStateChanged(target, QStringLiteral("rejected"));
        QTimer::singleShot(100, this, [this, generation] {
            if (generation == session_generation_) disconnectSession();
        });
    };

    // ---- JSON 控制面（必须在认证通过后才处理内容）----
    if (!message.empty() && message.front() == '{') {
        nlohmann::json j;
        if (parse_session_message(message, j)) {
            handleSessionJson(j);
        }
        return;
    }

    // ---- 被控端：校验主控端出示的证明 ----
    if (session_side_ == SessionSide::Controlled) {
        const auto result = pxc::parse_session_auth_response(message);
        if (!result.parsed) return;

        if (result.nonce_hex != sessionNonce_) {
            reject();
            emit logMessage(QStringLiteral("认证失败：挑战不匹配"));
            return;
        }

        const std::string fingerprint =
            session_ ? session_->local_dtls_fingerprint() : std::string();
        if (fingerprint.empty()) {
            reject();
            emit logMessage(QStringLiteral("认证失败：本端 DTLS 指纹不可用"));
            return;
        }

        if (!pxc::verify_session_proof(to_std(localConnectionKey_), result.nonce_hex,
                                       fingerprint, result.proof_hex)) {
            reject();
            sessionAuthenticated_ = false;
            emit logMessage(QStringLiteral("认证失败：连接密钥不正确"));
            return;
        }

        sessionAuthenticated_ = true;
        auth_timeout_timer_->stop();
        if (auto channel = session_ ? session_->channel(pxc::kChControl) : nullptr) {
            channel->send(pxc::kSessionAuthOk);
        }
        emit sessionStateChanged(targetDeviceId_, QStringLiteral("authenticated"));
        emit logMessage(QStringLiteral("主控端连接密钥验证通过，开始共享屏幕"));
        startControlledStream();
        return;
    }

    // ---- 主控端：收到挑战后出示证明 ----
    std::string nonce;
    if (pxc::parse_session_auth_challenge(message, nonce)) {
        const std::string fingerprint =
            session_ ? session_->remote_dtls_fingerprint() : std::string();
        if (fingerprint.empty()) {
            emit logMessage(QStringLiteral("对端 DTLS 指纹不可用，拒绝发送证明"));
            return;
        }

        const std::string proof =
            pxc::compute_session_proof(to_std(targetConnectionKey_), nonce, fingerprint);
        if (proof.empty()) {
            emit logMessage(QStringLiteral("连接密钥格式无效"));
            if (auto channel = session_ ? session_->channel(pxc::kChControl) : nullptr) {
                channel->send(pxc::kSessionAuthFail);
            }
            return;
        }

        if (auto channel = session_ ? session_->channel(pxc::kChControl) : nullptr) {
            channel->send(std::string(pxc::kSessionAuthResponse) + " " + nonce + " " + proof);
        }
        emit logMessage(QStringLiteral("已出示连接密钥证明，等待被控端确认"));
        return;
    }

    if (message == pxc::kSessionAuthOk) {
        sessionAuthenticated_ = true;
        clipboard_has_text_ = false;
        syncClipboard();
        auth_timeout_timer_->stop();
        // 主控端首次认证成功后记住该设备的连接密钥，之后连接不再询问。
        // （与 UU 远程的「记住验证码」一致；可在设置-安全里清除）
        if (!targetDeviceId_.isEmpty() && !targetConnectionKey_.isEmpty()) {
            QSettings settings;
            settings.setValue(QStringLiteral("saved_keys/") + targetDeviceId_,
                              targetConnectionKey_);
        }
        emit sessionStateChanged(targetDeviceId_, QStringLiteral("authenticated"));
        emit logMessage(QStringLiteral("✅ 连接密钥验证通过，可以控制该设备"));
        return;
    }

    if (message == pxc::kSessionAuthFail) {
        sessionAuthenticated_ = false;
        emit sessionStateChanged(targetDeviceId_, QStringLiteral("rejected"));
        emit logMessage(QStringLiteral("❌ 认证失败：连接密钥错误"));
        disconnectSession();
        return;
    }

    if (!sessionAuthenticated_) {
        emit logMessage(QStringLiteral("认证未完成，忽略控制消息"));
        return;
    }
    emit logMessage(QStringLiteral("[控制] ") + text);
}

// ------------------------------------------------------------------ 控制面

void ClientController::handleSessionJson(const nlohmann::json& j) {
    const std::string kind = j.value("kind", "");
    if (kind == "wallpaper") {
        if (session_side_ != SessionSide::Controller || !sessionAuthenticated_) return;
        const std::string encoded = j.value("image", "");
        if (encoded.empty() || encoded.size() > 64 * 1024) return;
        QByteArray bytes = QByteArray::fromBase64(QByteArray::fromStdString(encoded),
                                                QByteArray::AbortOnBase64DecodingErrors);
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "JPEG");
        const QSize size = reader.size();
        if (size.isEmpty() || size.width() > 960 || size.height() > 960) return;
        const QImage image = reader.read();
        if (!image.isNull()) emit desktopWallpaperReady(targetDeviceId_, image);
        return;
    }

    if (kind == "screens") {
        if (session_side_ == SessionSide::Controller) {
            emit remoteScreensUpdated(to_qjson(j.value("screens", nlohmann::json::array())));
        }
        return;
    }
    if (kind == "video_state") {
        if (session_side_ == SessionSide::Controller) {
            emit remoteVideoStateChanged(j.value("width", 0), j.value("height", 0),
                                         j.value("fps", 0), j.value("screen", 0),
                                         to_qstring(j.value("encoder", "")),
                                         to_qstring(j.value("error", "")));
        }
        return;
    }
    if (kind == "cmd") {
        if (session_side_ == SessionSide::Controlled && sessionAuthenticated_) {
            handleControlledCommand(j);
        }
        return;
    }
    if (kind == "input") {
        if (session_side_ == SessionSide::Controlled && sessionAuthenticated_) {
            handleInputEvent(j);
        }
        return;
    }
}

void ClientController::handleControlledCommand(const nlohmann::json& j) {
    const std::string cmd = j.value("cmd", "");
    if (cmd == "wallpaper") {
        const auto activeSession = session_;
        const QSize displaySize = QGuiApplication::primaryScreen()
            ? QGuiApplication::primaryScreen()->size() : QSize(1920, 1080);
        runAsync([this, activeSession, displaySize] {
            QString error;
            QImage image = read_desktop_wallpaper(displaySize, &error);
            QByteArray bytes;
            // Keep the reliable control message below 64 KiB, including base64.
            for (int quality : {80, 65, 50, 35}) {
                bytes.clear();
                QBuffer buffer(&bytes);
                buffer.open(QIODevice::WriteOnly);
                if (image.isNull() || !image.save(&buffer, "JPEG", quality)) break;
                if (bytes.size() <= 40 * 1024) break;
                image = image.scaled(image.size() * 0.8, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            if (destroyed_.load()) return;
            QMetaObject::invokeMethod(this, [this, activeSession, image, bytes, error] {
                if (session_ != activeSession || !sessionAuthenticated_ || session_side_ != SessionSide::Controlled) return;
                if (!error.isEmpty() || bytes.isEmpty() || bytes.size() > 40 * 1024) {
                    emit logMessage(QStringLiteral("无法提供桌面壁纸: ") + error);
                    return;
                }
                sendSessionJson({{"kind", "wallpaper"}, {"image", bytes.toBase64().toStdString()}});
                emit desktopWallpaperReady(localDeviceId_, image);
            }, Qt::QueuedConnection);
        });
        return;
    }

    if (cmd == "video_cfg") {
        if (!video_sender_) {
            sendVideoStateReply(QStringLiteral("视频未就绪"));
            return;
        }
        auto cfg = video_sender_->config();
        // Mobile controls change one setting at a time; zero means preserve that field.
        if (j.value("width", 0) > 0) cfg.width = std::max(j.value("width", 0), 320);
        if (j.value("height", 0) > 0) cfg.height = std::max(j.value("height", 0), 240);
        if (j.value("fps", 0) > 0) cfg.fps = std::clamp(j.value("fps", 0), 1, 120);
        if (j.value("bitrate_kbps", 0) > 0)
            cfg.bitrate_kbps = static_cast<uint32_t>(std::clamp(j.value("bitrate_kbps", 0), 500, 80000));
        video_sender_->set_config(cfg);
        sendVideoStateReply();
        return;
    }

    if (cmd == "screen_list") {
        nlohmann::json reply;
        reply["kind"] = "screens";
        auto screens  = nlohmann::json::array();
        std::string error;
        for (const auto& screen : pxc::enumerate_screens(&error)) {
            nlohmann::json item;
            item["index"]      = screen.index;
            item["name"]       = screen.name;
            item["gdi_device"] = screen.gdi_device;
            item["width"]      = screen.width;
            item["height"]     = screen.height;
            item["primary"]    = screen.primary;
            screens.push_back(item);
        }
        reply["screens"] = screens;
        sendSessionJson(reply);
        return;
    }

    if (cmd == "screen_switch") {
        // 切屏是异步的（新采集器的 D-Bus/PipeWire 建立耗时数秒且
        // 必须离开 UI 线程），结果随 2 秒一次的 video_state 上报。
        const int index = j.value("index", 0);
        if (video_sender_) {
            video_sender_->switch_screen(index);
            sendVideoStateReply();
        }
        return;
    }

    if (cmd == "keyframe") {
        if (video_sender_) video_sender_->force_keyframe();
        return;
    }
}

void ClientController::handleInputEvent(const nlohmann::json& j) {
    // 输入活动打点：视频发送线程用它区分「画面静止」与「采集停摆」
    note_remote_input();

    if (!injector_) {
        std::string error;
        injector_ = pxc::create_input_injector(&error);
        if (!injector_) {
            emit logMessage(QStringLiteral("键鼠注入不可用: ") + to_qstring(error));
            return;
        }
    }

    const std::string type = j.value("type", "");
    if (type == "text") {
        // IME commits are Unicode text, not Qt key codes. Paste once after composition finishes.
        const QString text = to_qstring(j.value("text", ""));
        if (text.isEmpty() || text.toUtf8().size() > 16 * 1024) return;
        auto* clipboard = QGuiApplication::clipboard();
        if (!clipboard) return;
        if (!text_clipboard_backup_ || clipboard->text() != text_clipboard_value_) {
            text_clipboard_backup_ = std::make_shared<QMimeData>();
            if (const auto* previous = clipboard->mimeData()) {
                for (const auto& format : previous->formats())
                    text_clipboard_backup_->setData(format, previous->data(format));
            }
        }
        text_clipboard_value_ = text;
        const auto generation = ++text_clipboard_generation_;
        clipboard_applying_ = true;
        clipboard->setText(text);
        clipboard_applying_ = false;
        injector_->key_event(Qt::Key_Control, "", true, 0);
        injector_->key_event(Qt::Key_V, "v", true, Qt::ControlModifier);
        injector_->key_event(Qt::Key_V, "v", false, Qt::ControlModifier);
        injector_->key_event(Qt::Key_Control, "", false, 0);
        QTimer::singleShot(800, this, [this, clipboard, text, generation] {
            if (generation != text_clipboard_generation_) return;
            const auto saved = std::move(text_clipboard_backup_);
            // Do not overwrite clipboard contents that the user changed after this paste.
            if (!saved || clipboard->text() != text) return;
            auto* restored = new QMimeData;
            for (const auto& format : saved->formats()) restored->setData(format, saved->data(format));
            clipboard_applying_ = true;
            clipboard->setMimeData(restored);
            clipboard_applying_ = false;
        });
        return;
    }
    if (type == "mouse_move") {
        injector_->mouse_move(j.value("screen", 0),
                              static_cast<float>(j.value("x", 0.0)),
                              static_cast<float>(j.value("y", 0.0)));
        return;
    }
    if (type == "mouse_button") {
        injector_->mouse_button(j.value("button", "left"), j.value("pressed", false));
        return;
    }
    if (type == "wheel") {
        injector_->mouse_wheel(j.value("dy", 0));
        return;
    }
    if (type == "key") {
        // Clipboard and keyboard use distinct SCTP streams. Carry the paste snapshot
        // on the ordered key stream so Ctrl+V cannot overtake clipboard delivery.
        if (j.value("key", 0) == Qt::Key_V && j.value("pressed", false) &&
            (j.value("mods", 0) & Qt::ControlModifier) && j.contains("clipboard") && j["clipboard"].is_string()) {
            const auto text = to_qstring(j["clipboard"].get<std::string>());
            if (text.toUtf8().size() <= 64 * 1024) {
                if (auto* clipboard = QGuiApplication::clipboard()) {
                    clipboard_last_text_ = text; clipboard_has_text_ = true;
                    clipboard_applying_ = true; clipboard->setText(text); clipboard_applying_ = false;
                }
            }
        }
        injector_->key_event(j.value("key", 0), j.value("text", ""),
                             j.value("pressed", false), j.value("mods", 0));
        return;
    }
}

void ClientController::sendSessionJson(const nlohmann::json& j) {
    auto channel = session_ ? session_->channel(pxc::kChControl) : nullptr;
    if (!channel || !channel->isOpen()) return;
    channel->send(encode_session_message(j));
}

void ClientController::sendVideoStateReply(const QString& error) {
    if (!video_sender_) return;
    nlohmann::json reply;
    reply["kind"]   = "video_state";
    reply["width"]  = video_sender_->actual_width();
    reply["height"] = video_sender_->actual_height();
    reply["fps"]    = video_sender_->actual_fps();
    reply["screen"] = video_sender_->screen_index();
    reply["encoder"] = video_sender_->encoder_name();
    if (!error.isEmpty()) reply["error"] = to_std(error);
    sendSessionJson(reply);
}

// 主控端控制面入口（RemoteControlView 调用）
void ClientController::requestRemoteScreens() {
    sendSessionJson(make_screen_list_cmd());
}

void ClientController::requestRemoteWallpaper() {
    if (session_side_ == SessionSide::Controller && sessionAuthenticated_)
        sendSessionJson({{"kind", "cmd"}, {"cmd", "wallpaper"}});
}

void ClientController::requestLocalWallpaper() {
    const QString deviceId = localDeviceId_;
    if (deviceId.isEmpty()) return;
    const QSize displaySize = QGuiApplication::primaryScreen()
        ? QGuiApplication::primaryScreen()->size() : QSize(1920, 1080);
    runAsync([this, deviceId, displaySize] {
        QString error;
        const QImage image = read_desktop_wallpaper(displaySize, &error);
        if (destroyed_.load()) return;
        QMetaObject::invokeMethod(this, [this, deviceId, image, error] {
            if (deviceId != localDeviceId_) return;
            if (!image.isNull()) emit desktopWallpaperReady(deviceId, image);
            else emit logMessage(QStringLiteral("无法读取本机桌面壁纸: ") + error);
        }, Qt::QueuedConnection);
    });
}

void ClientController::switchRemoteScreen(int index) {
    sendSessionJson(make_screen_switch_cmd(index));
}

void ClientController::setRemoteVideoConfig(int width, int height, int fps,
                                            int bitrate_kbps) {
    sendSessionJson(make_video_cfg_cmd(width, height, fps, bitrate_kbps));
}

void ClientController::setVideoAccelerationMode(pxc::VideoAccelerationMode mode) {
    if (video_receiver_) video_receiver_->setAccelerationMode(mode);
    QSettings settings;
    const QString value = mode == pxc::VideoAccelerationMode::Hardware ? QStringLiteral("hardware")
                        : mode == pxc::VideoAccelerationMode::Software ? QStringLiteral("software")
                        : QStringLiteral("smart");
    settings.setValue(QStringLiteral("video/acceleration"), value);
}

void ClientController::requestRemoteKeyframe() {
    sendSessionJson(make_keyframe_cmd());
}

void ClientController::syncClipboard() {
    if (clipboard_applying_ || !sessionAuthenticated_ || !session_) return;
    auto channel = session_->channel(pxc::kChClip);
    if (!channel || !channel->isOpen()) return;
    if (clipboard_read_pending_) { clipboard_read_again_ = true; return; }
    clipboard_read_pending_ = true;
    const auto generation = session_generation_.load();
    readClipboardText(this, [this, generation](ClipboardText result) {
        clipboard_read_pending_ = false;
        const bool retry = std::exchange(clipboard_read_again_, false);
        if (generation == session_generation_ && sessionAuthenticated_ && result.available &&
            !(text_clipboard_backup_ && result.text == text_clipboard_value_) &&
            !(clipboard_has_text_ && result.text == clipboard_last_text_)) {
            const QJsonObject message{{QStringLiteral("type"), QStringLiteral("text")},
                                     {QStringLiteral("text"), result.text}};
            try {
                auto channel = session_ ? session_->channel(pxc::kChClip) : nullptr;
                if (channel && channel->isOpen()) {
                    // false means queued successfully in libdatachannel.
                    channel->send(QJsonDocument(message).toJson(QJsonDocument::Compact).toStdString());
                    clipboard_last_text_ = result.text;
                    clipboard_has_text_ = true;
                }
            } catch (const std::exception&) { /* closed during clipboard read */ }
        }
        if (retry) syncClipboard();
    });
}

void ClientController::sendInputEvent(const QJsonObject& event) {
    try {
        auto payload = to_nlohmann(event);
        if (paste_read_pending_) {
            paste_input_queue_.push_back(std::move(payload));
            return;
        }
        if (payload.value("type", "") == "key" && payload.value("key", 0) == Qt::Key_V &&
            payload.value("pressed", false) && (payload.value("mods", 0) & Qt::ControlModifier)) {
            paste_read_pending_ = true;
            paste_input_queue_.push_back(std::move(payload));
            const auto generation = session_generation_.load();
            readClipboardText(this, [this, generation](ClipboardText result) {
                if (generation != session_generation_) return;
                paste_read_pending_ = false;
                auto events = std::move(paste_input_queue_);
                paste_input_queue_.clear();
                if (!sessionAuthenticated_ || events.empty()) return;
                if (result.available) events.front()["clipboard"] = to_std(result.text);
                // Preserve press/release order while waiting, so V is never
                // delivered after the user's Ctrl release. Timeout uses the
                // remote clipboard without injecting a stale cached value.
                try {
                    for (const auto& input : events) sendSessionJson(make_input_event(input));
                } catch (const std::exception&) { /* channel closed while reading */ }
            });
            return;
        }
        sendSessionJson(make_input_event(payload));
    } catch (const std::exception&) {
        // 事件序列化失败直接丢弃，键鼠事件不值得打断会话
    }
}

void ClientController::sendFileTransferJson(const QJsonObject& message) {
    if (!sessionAuthenticated_) return;
    auto channel = session_ ? session_->channel(pxc::kChFile) : nullptr;
    if (!channel || !channel->isOpen()) {
        emit fileTransferProgress(message.value(QStringLiteral("id")).toString(), {}, 0, 0,
                                  QStringLiteral("文件通道未就绪"));
        return;
    }
    channel->send(QJsonDocument(message).toJson(QJsonDocument::Compact).toStdString());
}

void ClientController::requestRemoteFileList(const QString& path) {
    QJsonObject message;
    message.insert(QStringLiteral("op"), QStringLiteral("list"));
    message.insert(QStringLiteral("path"), path);
    sendFileTransferJson(message);
}

void ClientController::createRemoteDirectory(const QString& parentPath, const QString& name) {
    QJsonObject message;
    message.insert(QStringLiteral("op"), QStringLiteral("mkdir"));
    message.insert(QStringLiteral("path"), parentPath);
    message.insert(QStringLiteral("name"), QFileInfo(name).fileName());
    sendFileTransferJson(message);
}

QString ClientController::requestRemoteFileDownload(const QString& remotePath,
                                                    const QString& localPath) {
    if (incoming_file_ || !pending_download_id_.isEmpty()) {
        emit fileTransferProgress({}, {}, 0, 0, QStringLiteral("当前已有接收任务"));
        return {};
    }
    pending_download_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending_download_path_ = localPath;
    QJsonObject message;
    message.insert(QStringLiteral("op"), QStringLiteral("download_request"));
    message.insert(QStringLiteral("id"), pending_download_id_);
    message.insert(QStringLiteral("path"), remotePath);
    sendFileTransferJson(message);
    return pending_download_id_;
}

QString ClientController::offerFileUpload(const QString& localPath) {
    if (outgoing_file_ || !pending_upload_id_.isEmpty()) {
        emit fileTransferProgress({}, QFileInfo(localPath).fileName(), 0, 0,
                                  QStringLiteral("当前已有发送任务"));
        return {};
    }
    const QFileInfo info(localPath);
    if (!info.exists() || !info.isFile() || !info.isReadable()) {
        emit fileTransferProgress({}, info.fileName(), 0, 0, QStringLiteral("无法读取所选文件"));
        return {};
    }
    pending_upload_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending_upload_path_ = info.absoluteFilePath();
    QJsonObject message;
    message.insert(QStringLiteral("op"), QStringLiteral("upload_offer"));
    message.insert(QStringLiteral("id"), pending_upload_id_);
    message.insert(QStringLiteral("name"), info.fileName());
    message.insert(QStringLiteral("size"), static_cast<double>(info.size()));
    sendFileTransferJson(message);
    emit fileTransferProgress(pending_upload_id_, info.fileName(), 0, info.size(),
                              QStringLiteral("等待远端接受"));
    return pending_upload_id_;
}

void ClientController::acceptIncomingFile(const QString& transferId,
                                          const QString& destinationPath) {
    if (transferId.isEmpty() || transferId != incoming_file_id_ || destinationPath.isEmpty()) return;
    if (incoming_file_) {
        emit fileTransferProgress(transferId, incoming_file_name_, 0, incoming_total_,
                                  QStringLiteral("当前已有接收任务"));
        return;
    }
    incoming_file_ = new QSaveFile(destinationPath, this);
    if (!incoming_file_->open(QIODevice::WriteOnly)) {
        const QString reason = incoming_file_->errorString();
        delete incoming_file_;
        incoming_file_ = nullptr;
        rejectIncomingFile(transferId);
        emit fileTransferProgress(transferId, incoming_file_name_, 0, incoming_total_, reason);
        return;
    }
    incoming_transferred_ = 0;
    QJsonObject message;
    message.insert(QStringLiteral("op"), QStringLiteral("upload_accept"));
    message.insert(QStringLiteral("id"), transferId);
    sendFileTransferJson(message);
    emit fileTransferProgress(transferId, incoming_file_name_, 0, incoming_total_,
                              QStringLiteral("接收中"));
}

void ClientController::rejectIncomingFile(const QString& transferId) {
    QJsonObject message;
    message.insert(QStringLiteral("op"), QStringLiteral("upload_reject"));
    message.insert(QStringLiteral("id"), transferId);
    sendFileTransferJson(message);
    if (incoming_file_) {
        incoming_file_->cancelWriting();
        delete incoming_file_;
        incoming_file_ = nullptr;
    }
    incoming_file_id_.clear();
}

void ClientController::handleFileTransferMessage(const QJsonObject& message) {
    if (!sessionAuthenticated_) return;
    const QString op = message.value(QStringLiteral("op")).toString();
    const QString id = message.value(QStringLiteral("id")).toString();

    if (op == QStringLiteral("list")) {
        QString path = message.value(QStringLiteral("path")).toString().trimmed();
        if (path.isEmpty() || path == QStringLiteral("~")) path = QDir::homePath();
        QDir dir(QDir::cleanPath(path));
        QJsonObject reply;
        reply.insert(QStringLiteral("op"), QStringLiteral("list_result"));
        reply.insert(QStringLiteral("path"), dir.absolutePath());
        if (!dir.exists()) {
            reply.insert(QStringLiteral("error"), QStringLiteral("目录不存在或不可访问"));
        } else {
            QJsonArray entries;
            const QFileInfoList infos = dir.entryInfoList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Readable,
                QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
            for (const QFileInfo& info : infos) {
                if (info.isSymLink()) continue;
                QJsonObject entry;
                entry.insert(QStringLiteral("name"), info.fileName());
                entry.insert(QStringLiteral("path"), info.absoluteFilePath());
                entry.insert(QStringLiteral("directory"), info.isDir());
                entry.insert(QStringLiteral("size"), static_cast<double>(info.isDir() ? 0 : info.size()));
                entry.insert(QStringLiteral("modified"), info.lastModified().toString(Qt::ISODate));
                entries.append(entry);
                if (entries.size() >= 1000) break;
            }
            reply.insert(QStringLiteral("entries"), entries);
        }
        sendFileTransferJson(reply);
        emit fileTransferMessageReceived(reply);
        return;
    }
    if (op == QStringLiteral("mkdir")) {
        const QString parent = message.value(QStringLiteral("path")).toString();
        const QString name = QFileInfo(message.value(QStringLiteral("name")).toString()).fileName();
        const QDir dir(parent.isEmpty() || parent == QStringLiteral("~")
                           ? QDir::homePath() : QDir::cleanPath(parent));
        const bool ok = !name.isEmpty() && name != QStringLiteral(".") &&
                        name != QStringLiteral("..") && dir.mkdir(name);
        QJsonObject reply;
        reply.insert(QStringLiteral("op"), QStringLiteral("mkdir_result"));
        reply.insert(QStringLiteral("path"), dir.absolutePath());
        if (!ok) reply.insert(QStringLiteral("error"), QStringLiteral("无法创建文件夹"));
        sendFileTransferJson(reply);
        emit fileTransferMessageReceived(reply);
        if (ok) requestRemoteFileList(dir.absolutePath());
        return;
    }
    if (op == QStringLiteral("upload_request")) {
        const QString name = QFileInfo(message.value(QStringLiteral("name")).toString()).fileName();
        const QDir parent(message.value(QStringLiteral("path")).toString());
        const qint64 size = static_cast<qint64>(message.value(QStringLiteral("size")).toDouble(-1));
        const QString destination = parent.filePath(name);
        if (incoming_file_ || !incoming_file_id_.isEmpty() || id.isEmpty() || name.isEmpty() ||
            name == QStringLiteral(".") || name == QStringLiteral("..") || size < 0 ||
            !parent.isAbsolute() || !parent.exists() || QFileInfo::exists(destination)) {
            QJsonObject error;
            error.insert(QStringLiteral("op"), QStringLiteral("transfer_error"));
            error.insert(QStringLiteral("id"), id);
            error.insert(QStringLiteral("message"), QStringLiteral("目标不可写、文件已存在或已有接收任务"));
            sendFileTransferJson(error);
            return;
        }
        incoming_file_id_ = id;
        incoming_file_name_ = name;
        incoming_total_ = size;
        acceptIncomingFile(id, destination);
    } else if (op == QStringLiteral("upload_offer")) {
        if (incoming_file_ || !incoming_file_id_.isEmpty()) {
            QJsonObject deny; deny.insert(QStringLiteral("op"), QStringLiteral("upload_reject"));
            deny.insert(QStringLiteral("id"), id); sendFileTransferJson(deny); return;
        }
        incoming_file_id_ = id;
        incoming_file_name_ = QFileInfo(message.value(QStringLiteral("name")).toString()).fileName();
        incoming_total_ = static_cast<qint64>(message.value(QStringLiteral("size")).toDouble());
        incoming_transferred_ = 0;
        emit incomingFileOffer(id, incoming_file_name_, incoming_total_);
    } else if (op == QStringLiteral("upload_accept")) {
        if (id == pending_upload_id_) {
            const QString source = pending_upload_path_;
            pending_upload_id_.clear();
            pending_upload_path_.clear();
            beginOutgoingFile(id, source, QStringLiteral("upload"));
        }
    } else if (op == QStringLiteral("upload_reject")) {
        if (id == pending_upload_id_) {
            emit fileTransferProgress(id, QFileInfo(pending_upload_path_).fileName(), 0, 0,
                                      QStringLiteral("远端拒绝接收"));
            pending_upload_id_.clear();
            pending_upload_path_.clear();
        }
    } else if (op == QStringLiteral("download_request")) {
        beginOutgoingFile(id, message.value(QStringLiteral("path")).toString(),
                          QStringLiteral("download"));
    } else if (op == QStringLiteral("download_start")) {
        if (id == pending_download_id_ && !pending_download_path_.isEmpty()) {
            incoming_file_id_ = id;
            incoming_file_name_ = QFileInfo(message.value(QStringLiteral("name")).toString()).fileName();
            incoming_total_ = static_cast<qint64>(message.value(QStringLiteral("size")).toDouble());
            incoming_transferred_ = 0;
            incoming_file_ = new QSaveFile(pending_download_path_, this);
            if (!incoming_file_->open(QIODevice::WriteOnly)) {
                const QString reason = incoming_file_->errorString();
                delete incoming_file_;
                incoming_file_ = nullptr;
                QJsonObject error;
                error.insert(QStringLiteral("op"), QStringLiteral("transfer_error"));
                error.insert(QStringLiteral("id"), id);
                error.insert(QStringLiteral("message"), reason);
                sendFileTransferJson(error);
                emit fileTransferProgress(id, incoming_file_name_, 0, incoming_total_, reason);
                pending_download_id_.clear();
                pending_download_path_.clear();
            } else {
                emit fileTransferProgress(id, incoming_file_name_, 0, incoming_total_,
                                          QStringLiteral("下载中"));
            }
        }
    } else if (op == QStringLiteral("transfer_end")) {
        if (incoming_file_ && id == incoming_file_id_) {
            const QString name = incoming_file_name_;
            const qint64 total = incoming_total_;
            const bool complete = incoming_transferred_ == incoming_total_;
            const bool committed = complete && incoming_file_->commit();
            const QString reason = complete ? incoming_file_->errorString() : QStringLiteral("接收文件大小不匹配");
            if (!complete) incoming_file_->cancelWriting();
            delete incoming_file_;
            incoming_file_ = nullptr;
            if (committed) {
                QJsonObject reply;
                reply.insert(QStringLiteral("op"), QStringLiteral("transfer_complete"));
                reply.insert(QStringLiteral("id"), id);
                sendFileTransferJson(reply);
                emit fileTransferProgress(id, name, total, total, QStringLiteral("已完成"));
            } else {
                QJsonObject error; error.insert(QStringLiteral("op"), QStringLiteral("transfer_error"));
                error.insert(QStringLiteral("id"), id); error.insert(QStringLiteral("message"), reason);
                sendFileTransferJson(error);
                emit fileTransferProgress(id, name, incoming_transferred_, total, reason);
            }
            incoming_file_id_.clear();
            pending_download_id_.clear();
            pending_download_path_.clear();
        }
    } else if (op == QStringLiteral("transfer_complete")) {
        if (id == outgoing_file_id_) finishOutgoingFile(QStringLiteral("已完成"));
    } else if (op == QStringLiteral("transfer_error") || op == QStringLiteral("transfer_cancel")) {
        emit fileTransferProgress(id, message.value(QStringLiteral("name")).toString(), 0, 0,
                                  message.value(QStringLiteral("message")).toString());
        if (id == outgoing_file_id_) finishOutgoingFile(QStringLiteral("传输失败"));
        if (id == pending_upload_id_) {
            pending_upload_id_.clear();
            pending_upload_path_.clear();
        }
        if (id == pending_download_id_) {
            pending_download_id_.clear();
            pending_download_path_.clear();
        }
        if (id == incoming_file_id_) {
            if (incoming_file_) { incoming_file_->cancelWriting(); delete incoming_file_; incoming_file_ = nullptr; }
            incoming_file_id_.clear(); incoming_file_name_.clear(); incoming_total_ = incoming_transferred_ = 0;
        }
    }

    emit fileTransferMessageReceived(message);
}

void ClientController::handleFileTransferChunk(const QByteArray& chunk) {
    if (!sessionAuthenticated_ || !incoming_file_ || chunk.isEmpty()) return;
    const qint64 written = chunk.size() <= incoming_total_ - incoming_transferred_
        ? incoming_file_->write(chunk) : -1;
    if (written != chunk.size()) {
        const QString reason = incoming_file_->errorString();
        const QString id = incoming_file_id_;
        incoming_file_->cancelWriting();
        delete incoming_file_;
        incoming_file_ = nullptr;
        incoming_file_id_.clear();
        QJsonObject error;
        error.insert(QStringLiteral("op"), QStringLiteral("transfer_error"));
        error.insert(QStringLiteral("id"), id);
        error.insert(QStringLiteral("message"), reason);
        sendFileTransferJson(error);
        emit fileTransferProgress(id, incoming_file_name_, incoming_transferred_, incoming_total_, reason);
        return;
    }
    incoming_transferred_ += written;
    emit fileTransferProgress(incoming_file_id_, incoming_file_name_, incoming_transferred_,
                              incoming_total_, QStringLiteral("传输中"));
}

void ClientController::beginOutgoingFile(const QString& transferId, const QString& path,
                                         const QString& direction) {
    if (outgoing_file_) {
        QJsonObject error;
        error.insert(QStringLiteral("op"), QStringLiteral("transfer_error"));
        error.insert(QStringLiteral("id"), transferId);
        error.insert(QStringLiteral("message"), QStringLiteral("当前已有发送任务"));
        sendFileTransferJson(error);
        return;
    }
    auto* file = new QFile(path, this);
    if (!file->open(QIODevice::ReadOnly)) {
        const QString reason = file->errorString();
        delete file;
        QJsonObject error;
        error.insert(QStringLiteral("op"), QStringLiteral("transfer_error"));
        error.insert(QStringLiteral("id"), transferId);
        error.insert(QStringLiteral("name"), QFileInfo(path).fileName());
        error.insert(QStringLiteral("message"), reason);
        sendFileTransferJson(error);
        emit fileTransferProgress(transferId, QFileInfo(path).fileName(), 0, 0, reason);
        return;
    }
    outgoing_file_ = file;
    outgoing_file_id_ = transferId;
    outgoing_file_name_ = QFileInfo(path).fileName();
    outgoing_direction_ = direction;
    outgoing_total_ = file->size();
    outgoing_transferred_ = 0;
    if (direction == QStringLiteral("download")) {
        QJsonObject start;
        start.insert(QStringLiteral("op"), QStringLiteral("download_start"));
        start.insert(QStringLiteral("id"), transferId);
        start.insert(QStringLiteral("name"), outgoing_file_name_);
        start.insert(QStringLiteral("size"), static_cast<double>(outgoing_total_));
        sendFileTransferJson(start);
    }
    emit fileTransferProgress(transferId, outgoing_file_name_, 0, outgoing_total_,
                              direction == QStringLiteral("download")
                                  ? QStringLiteral("上传中") : QStringLiteral("发送中"));
    file_send_timer_->start();
}

void ClientController::pumpOutgoingFile() {
    if (!outgoing_file_) {
        file_send_timer_->stop();
        return;
    }
    auto channel = session_ ? session_->channel(pxc::kChFile) : nullptr;
    if (!sessionAuthenticated_ || !channel || !channel->isOpen()) {
        finishOutgoingFile(QStringLiteral("连接已断开"));
        return;
    }
    if (channel->bufferedAmount() > 512 * 1024) return;
    const QByteArray chunk = outgoing_file_->read(48 * 1024);
    if (chunk.isEmpty()) {
        if (outgoing_file_->error() != QFile::NoError) {
            const QString reason = outgoing_file_->errorString();
            QJsonObject error;
            error.insert(QStringLiteral("op"), QStringLiteral("transfer_error"));
            error.insert(QStringLiteral("id"), outgoing_file_id_);
            error.insert(QStringLiteral("message"), reason);
            sendFileTransferJson(error);
            finishOutgoingFile(reason);
            return;
        }
        if (outgoing_file_->atEnd()) {
            file_send_timer_->stop();
            outgoing_file_->close();
            delete outgoing_file_;
            outgoing_file_ = nullptr;
            QJsonObject end;
            end.insert(QStringLiteral("op"), QStringLiteral("transfer_end"));
            end.insert(QStringLiteral("id"), outgoing_file_id_);
            sendFileTransferJson(end);
        }
        return;
    }
    rtc::binary payload(chunk.size());
    std::memcpy(payload.data(), chunk.constData(), static_cast<size_t>(chunk.size()));
    channel->send(std::move(payload));
    outgoing_transferred_ += chunk.size();
    emit fileTransferProgress(outgoing_file_id_, outgoing_file_name_, outgoing_transferred_,
                              outgoing_total_, QStringLiteral("传输中"));
}

void ClientController::finishOutgoingFile(const QString& state) {
    if (file_send_timer_) file_send_timer_->stop();
    if (outgoing_file_) {
        outgoing_file_->close();
        delete outgoing_file_;
        outgoing_file_ = nullptr;
    }
    if (!outgoing_file_id_.isEmpty()) {
        emit fileTransferProgress(outgoing_file_id_, outgoing_file_name_, outgoing_transferred_,
                                  outgoing_total_, state);
    }
    outgoing_file_id_.clear();
    outgoing_file_name_.clear();
    outgoing_direction_.clear();
    outgoing_total_ = outgoing_transferred_ = 0;
}

// ------------------------------------------------------------------ 被控端视频流

void ClientController::startControlledStream() {
    stopControlledStream();

    std::string error;
    auto capturer = pxc::create_screen_capturer(&error);
    if (!capturer) {
        emit logMessage(QStringLiteral("屏幕采集不可用: ") + to_qstring(error));
        return;
    }

    auto sender = std::make_unique<VideoSender>(std::move(capturer));
    auto video_channel = session_ ? session_->channel(pxc::kChVideo) : nullptr;
    if (!sender->start(video_channel, default_video_config())) {
        emit logMessage(QStringLiteral("视频发送启动失败: ") +
                        to_qstring(sender->last_error()));
        return;
    }
    video_sender_ = std::move(sender);
    emit controlledSessionStarted();
    // 采集器建立已移入发送线程（PipeWire/D-Bus 最坏数十秒），
    // 这里立即返回，避免 UI 线程被阻塞导致「未响应」。
    emit logMessage(QStringLiteral("屏幕共享启动中（1080P/30帧，等待主控端调整画质）"));
}

void ClientController::stopControlledStream() {
    if (video_sender_) {
        video_sender_->stop();
        video_sender_.reset();
    }
    injector_.reset();
}

}  // namespace pxc::gui
