#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include "napi/native_api.h"

#include "hos_controller.h"
#include "hos_video_receiver.h"

namespace {

// ---------------------------------------------------------------------------
// Phase 1 冒烟工具：用原生 POSIX socket 发一个明文 HTTP GET。
// 目的：验证 HarmonyOS 网络策略不拦截 cleartext，且应用沙箱可出网。
// 全程带超时（连接 3s + 读 2s），不会长时间阻塞调用线程。
// ---------------------------------------------------------------------------
std::string raw_http_get(const std::string& host, int port, const std::string& path) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        return "DNS 解析失败: " + host;
    }

    const int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return "socket() 失败: " + std::string(strerror(errno));
    }

    // 非阻塞连接 + poll 超时
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) {
        ::close(fd);
        return "connect() 失败: " + std::string(strerror(errno));
    }
    if (rc != 0) {
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        const int pr = ::poll(&pfd, 1, 3000);
        if (pr <= 0) {
            ::close(fd);
            return "连接超时（3s）: " + host + ":" + port_str;
        }
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
        if (soerr != 0) {
            ::close(fd);
            return "连接失败: " + std::string(strerror(soerr));
        }
    }

    // 恢复阻塞读写，并设置读超时
    fcntl(fd, F_SETFL, flags);
    struct timeval tv {};
    tv.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host +
                            "\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < req.size()) {
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) {
            ::close(fd);
            return "send() 失败: " + std::string(strerror(errno));
        }
        sent += static_cast<size_t>(n);
    }

    std::string resp;
    char buf[2048];
    for (int i = 0; i < 8; ++i) {  // 最多读 8 块，冒烟足够
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (resp.size() > 4096) break;
    }
    ::close(fd);

    if (resp.empty()) return "无响应（服务器未回数据）";
    const size_t line_end = resp.find("\r\n");
    return "收到响应: " + resp.substr(0, line_end == std::string::npos ? resp.size() : line_end);
}

// ---------------------------------------------------------------------------
// Phase 3：HosController <-> JS 事件桥（threadsafe function）
// ---------------------------------------------------------------------------

using pxc::hos::HosController;
using pxc::hos::HosVideoReceiver;

struct EventPayload {
    std::string event;
    std::string json;
};

// holder 用于 finalize 时判定「被销毁的是否仍是当前 listener」
struct TsfnHolder {
    napi_threadsafe_function tsfn;
};

std::mutex                        g_listener_mtx;
napi_threadsafe_function          g_event_tsfn = nullptr;
std::unique_ptr<HosController>    g_controller;
std::unique_ptr<HosVideoReceiver> g_video_receiver;

void EmitToJs(const std::string& event, const std::string& json) {
    std::lock_guard<std::mutex> lock(g_listener_mtx);
    if (g_event_tsfn == nullptr) return;
    auto* payload  = new EventPayload();
    payload->event = event;
    payload->json  = json;
    // 队列不限长，napi_tsfn_blocking 实际不会阻塞
    if (napi_call_threadsafe_function(g_event_tsfn, payload, napi_tsfn_blocking) != napi_ok) {
        delete payload;
    }
}

void EventTsfnCallJs(napi_env env, napi_value jsCallback, void* /*context*/, void* data) {
    auto* payload = static_cast<EventPayload*>(data);
    if (payload == nullptr) return;
    napi_value argv[2] = {nullptr, nullptr};
    napi_create_string_utf8(env, payload->event.c_str(), NAPI_AUTO_LENGTH, &argv[0]);
    napi_create_string_utf8(env, payload->json.c_str(), NAPI_AUTO_LENGTH, &argv[1]);
    if (jsCallback != nullptr) {
        napi_value recv = nullptr;
        napi_get_undefined(env, &recv);
        napi_value result = nullptr;
        napi_call_function(env, recv, jsCallback, 2, argv, &result);
    }
    delete payload;
}

void EventTsfnFinalize(napi_env /*env*/, void* data, void* /*hint*/) {
    auto* holder = static_cast<TsfnHolder*>(data);
    {
        std::lock_guard<std::mutex> lock(g_listener_mtx);
        if (g_event_tsfn == holder->tsfn) {
            g_event_tsfn = nullptr;
        }
    }
    delete holder;
}

HosController* Controller() {
    if (!g_controller) {
        g_controller = std::make_unique<HosController>(&EmitToJs);
    }
    return g_controller.get();
}

// ------------------------------------------------------------------ 参数工具

std::string GetStringArg(napi_env env, napi_value value) {
    if (value == nullptr) return std::string();
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, value, &type) != napi_ok || type != napi_string) {
        return std::string();
    }
    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) {
        return std::string();
    }
    std::string out(len, '\0');
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, value, &out[0], len + 1, &copied) != napi_ok) {
        return std::string();
    }
    out.resize(copied);
    return out;
}

int32_t GetIntArg(napi_env env, napi_value value, int32_t fallback) {
    int32_t out = fallback;
    if (value != nullptr) {
        napi_get_value_int32(env, value, &out);
    }
    return out;
}

napi_value MakeLocalIdentity(napi_env env, HosController* controller) {
    napi_value obj = nullptr;
    napi_create_object(env, &obj);

    napi_value deviceId = nullptr;
    const std::string device_id = controller->localDeviceId();
    napi_create_string_utf8(env, device_id.c_str(), NAPI_AUTO_LENGTH, &deviceId);
    napi_set_named_property(env, obj, "deviceId", deviceId);

    napi_value connectionKey = nullptr;
    const std::string key = controller->localConnectionKey();
    napi_create_string_utf8(env, key.c_str(), NAPI_AUTO_LENGTH, &connectionKey);
    napi_set_named_property(env, obj, "connectionKey", connectionKey);

    napi_value enrolled = nullptr;
    napi_get_boolean(env, controller->isEnrolled(), &enrolled);
    napi_set_named_property(env, obj, "enrolled", enrolled);

    return obj;
}

napi_value Undefined(napi_env env) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
}

// ------------------------------------------------------------------ 导出方法

napi_value Version(napi_env env, napi_callback_info /*info*/) {
    napi_value result = nullptr;
    napi_create_string_utf8(env, "pxc_hos 0.3.0 (phase3)", NAPI_AUTO_LENGTH, &result);
    return result;
}

napi_value HttpSmokeTest(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    char host[256] = {0};
    size_t host_len = 0;
    if (argc >= 1 && args[0] != nullptr) {
        napi_get_value_string_utf8(env, args[0], host, sizeof(host), &host_len);
    }
    double port = 0;
    if (argc >= 2 && args[1] != nullptr) {
        napi_get_value_double(env, args[1], &port);
    }
    char path[512] = {0};
    size_t path_len = 0;
    if (argc >= 3 && args[2] != nullptr) {
        napi_get_value_string_utf8(env, args[2], path, sizeof(path), &path_len);
    }
    if (host_len == 0) {
        napi_throw_error(env, nullptr, "host 不能为空");
        return nullptr;
    }
    if (path_len == 0) {
        strncpy(path, "/", sizeof(path) - 1);
    }

    const std::string result = raw_http_get(host, static_cast<int>(port), path);
    napi_value out = nullptr;
    napi_create_string_utf8(env, result.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

// setEventListener(callback: (event: string, json: string) => void): void
napi_value SetEventListener(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1 || args[0] == nullptr) {
        napi_throw_error(env, nullptr, "需要回调参数");
        return nullptr;
    }
    napi_valuetype type = napi_undefined;
    napi_typeof(env, args[0], &type);
    if (type != napi_function) {
        napi_throw_error(env, nullptr, "参数必须是函数");
        return nullptr;
    }

    // 先建控制器，保证 sink 就绪后再挂 listener
    (void)Controller();

    auto* holder = new TsfnHolder{nullptr};
    napi_value async_name = nullptr;
    napi_create_string_utf8(env, "pxc_event", NAPI_AUTO_LENGTH, &async_name);

    napi_threadsafe_function tsfn = nullptr;
    const napi_status st = napi_create_threadsafe_function(
        env, args[0], nullptr, async_name,
        0 /* 不限队列 */, 1, holder, EventTsfnFinalize,
        nullptr, EventTsfnCallJs, &tsfn);
    if (st != napi_ok || tsfn == nullptr) {
        delete holder;
        napi_throw_error(env, nullptr, "创建事件线程函数失败");
        return nullptr;
    }
    holder->tsfn = tsfn;

    napi_threadsafe_function old = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_listener_mtx);
        old          = g_event_tsfn;
        g_event_tsfn = tsfn;
    }
    if (old != nullptr) {
        napi_release_threadsafe_function(old, napi_tsfn_release);
    }
    return Undefined(env);
}

// configure(apiUrl, wsUrl, filesDir, deviceName): LocalIdentity
napi_value Configure(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    const std::string api_url  = GetStringArg(env, argc > 0 ? args[0] : nullptr);
    const std::string ws_url   = GetStringArg(env, argc > 1 ? args[1] : nullptr);
    const std::string files_dir = GetStringArg(env, argc > 2 ? args[2] : nullptr);
    const std::string device_name = GetStringArg(env, argc > 3 ? args[3] : nullptr);
    if (api_url.empty() || ws_url.empty() || files_dir.empty()) {
        napi_throw_error(env, nullptr, "apiUrl / wsUrl / filesDir 不能为空");
        return nullptr;
    }

    const std::string ca_path = files_dir + "/pxc-ca-bundle.pem";
    setenv("SSL_CERT_FILE", ca_path.c_str(), 1);
    HosController* controller = Controller();
    controller->configure(api_url, ws_url, files_dir, device_name);
    return MakeLocalIdentity(env, controller);
}

// addThisDevice(): LocalIdentity
napi_value AddThisDevice(napi_env env, napi_callback_info /*info*/) {
    HosController* controller = Controller();
    controller->addThisDevice();
    return MakeLocalIdentity(env, controller);
}

napi_value GetLocalIdentity(napi_env env, napi_callback_info /*info*/) {
    return MakeLocalIdentity(env, Controller());
}

napi_value SetConnectionKey(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const bool ok = Controller()->setConnectionKey(GetStringArg(env, argc > 0 ? args[0] : nullptr));
    napi_value result = nullptr;
    napi_get_boolean(env, ok, &result);
    return result;
}

napi_value Login(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string identifier = GetStringArg(env, argc > 0 ? args[0] : nullptr);
    const std::string password   = GetStringArg(env, argc > 1 ? args[1] : nullptr);
    if (identifier.empty() || password.empty()) {
        napi_throw_error(env, nullptr, "账号和密码不能为空");
        return nullptr;
    }
    Controller()->login(identifier, password);
    return Undefined(env);
}

napi_value RestoreLogin(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    Controller()->restoreLogin(GetStringArg(env, argc > 0 ? args[0] : nullptr),
                               GetStringArg(env, argc > 1 ? args[1] : nullptr));
    return Undefined(env);
}

napi_value GetLoginSession(napi_env env, napi_callback_info /*info*/) {
    const auto session = Controller()->getLoginSession();
    napi_value result = nullptr;
    napi_create_object(env, &result);
    for (const auto& key : {"token", "username"}) {
        napi_value value = nullptr;
        const auto text = session.at(key).get<std::string>();
        napi_create_string_utf8(env, text.c_str(), text.size(), &value);
        napi_set_named_property(env, result, key, value);
    }
    napi_value generation = nullptr;
    napi_create_double(env, session.at("generation").get<double>(), &generation);
    napi_set_named_property(env, result, "generation", generation);
    return result;
}

napi_value CancelLoginAttempt(napi_env env, napi_callback_info /*info*/) {
    Controller()->cancelLoginAttempt();
    return Undefined(env);
}

napi_value RegisterAccount(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string username = GetStringArg(env, argc > 0 ? args[0] : nullptr);
    const std::string email    = GetStringArg(env, argc > 1 ? args[1] : nullptr);
    const std::string password = GetStringArg(env, argc > 2 ? args[2] : nullptr);
    if (username.empty() || password.empty()) {
        napi_throw_error(env, nullptr, "用户名和密码不能为空");
        return nullptr;
    }
    Controller()->registerAccount(username, email, password);
    return Undefined(env);
}

napi_value Logout(napi_env env, napi_callback_info /*info*/) {
    Controller()->logout();
    return Undefined(env);
}

napi_value RefreshDevices(napi_env env, napi_callback_info /*info*/) {
    Controller()->refreshDevices();
    return Undefined(env);
}

napi_value SetIceServers(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string stun_url  = GetStringArg(env, argc > 0 ? args[0] : nullptr);
    const std::string turn_url  = GetStringArg(env, argc > 1 ? args[1] : nullptr);
    const std::string turn_user = GetStringArg(env, argc > 2 ? args[2] : nullptr);
    const std::string turn_pass = GetStringArg(env, argc > 3 ? args[3] : nullptr);
    Controller()->setIceServers(stun_url, turn_url, turn_user, turn_pass);
    return Undefined(env);
}

napi_value ConnectToDevice(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string device_id      = GetStringArg(env, argc > 0 ? args[0] : nullptr);
    const std::string connection_key = GetStringArg(env, argc > 1 ? args[1] : nullptr);
    if (device_id.empty() || connection_key.empty()) {
        napi_throw_error(env, nullptr, "deviceId 和 connectionKey 不能为空");
        return nullptr;
    }
    Controller()->connectToDevice(device_id, connection_key);
    return Undefined(env);
}

napi_value Disconnect(napi_env env, napi_callback_info /*info*/) {
    Controller()->disconnect();
    return Undefined(env);
}

napi_value SessionAuthenticated(napi_env env, napi_callback_info /*info*/) {
    napi_value result = nullptr;
    napi_get_boolean(env, Controller()->sessionAuthenticated(), &result);
    return result;
}

napi_value RequestRemoteScreens(napi_env env, napi_callback_info /*info*/) {
    Controller()->requestRemoteScreens();
    return Undefined(env);
}

napi_value RequestRemoteWallpaper(napi_env env, napi_callback_info /*info*/) {
    Controller()->requestRemoteWallpaper();
    return Undefined(env);
}

napi_value FileReady(napi_env env, napi_callback_info) {
    napi_value result; napi_get_boolean(env,Controller()->fileChannelReady(),&result); return result;
}
napi_value FileCommand(napi_env env, napi_callback_info info) {
    size_t argc=1; napi_value args[1]; napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);
    napi_value result; napi_get_boolean(env,Controller()->fileCommand(GetStringArg(env,argc?args[0]:nullptr)),&result); return result;
}
napi_value UploadFile(napi_env env, napi_callback_info info) {
    size_t argc=3; napi_value args[3]; napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);
    const auto id=Controller()->uploadFile(GetIntArg(env,argc?args[0]:nullptr,-1),
        GetStringArg(env,argc>1?args[1]:nullptr),GetStringArg(env,argc>2?args[2]:nullptr));
    napi_value result; napi_create_string_utf8(env,id.c_str(),id.size(),&result); return result;
}
napi_value DownloadFile(napi_env env, napi_callback_info info) {
    size_t argc=3; napi_value args[3]; napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);
    const auto id=Controller()->downloadFile(GetIntArg(env,argc?args[0]:nullptr,-1),
        GetStringArg(env,argc>1?args[1]:nullptr),GetStringArg(env,argc>2?args[2]:nullptr));
    napi_value result; napi_create_string_utf8(env,id.c_str(),id.size(),&result); return result;
}
napi_value CancelFile(napi_env env, napi_callback_info info) {
    size_t argc=1; napi_value args[1]; napi_get_cb_info(env,info,&argc,args,nullptr,nullptr);
    Controller()->cancelFileTransfer(GetStringArg(env,argc?args[0]:nullptr)); return Undefined(env);
}

napi_value CachedWallpaper(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const auto path = Controller()->cachedWallpaper(GetStringArg(env, argc ? args[0] : nullptr));
    napi_value result = nullptr;
    napi_create_string_utf8(env, path.c_str(), path.size(), &result);
    return result;
}

napi_value SwitchRemoteScreen(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    Controller()->switchRemoteScreen(GetIntArg(env, argc > 0 ? args[0] : nullptr, 0));
    return Undefined(env);
}

napi_value SetRemoteVideoConfig(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    Controller()->setRemoteVideoConfig(
        GetIntArg(env, argc > 0 ? args[0] : nullptr, 0),
        GetIntArg(env, argc > 1 ? args[1] : nullptr, 0),
        GetIntArg(env, argc > 2 ? args[2] : nullptr, 0),
        GetIntArg(env, argc > 3 ? args[3] : nullptr, 0));
    return Undefined(env);
}

napi_value RequestRemoteKeyframe(napi_env env, napi_callback_info /*info*/) {
    Controller()->requestRemoteKeyframe();
    return Undefined(env);
}

napi_value SendClipboardText(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const bool sent = Controller()->sendClipboardText(GetStringArg(env, argc ? args[0] : nullptr));
    napi_value result; napi_get_boolean(env, sent, &result); return result;
}

napi_value SendInputEvent(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string payload = GetStringArg(env, argc > 0 ? args[0] : nullptr);
    if (payload.empty()) {
        napi_throw_error(env, nullptr, "事件 JSON 不能为空");
        return nullptr;
    }
    Controller()->sendInputEvent(payload);
    return Undefined(env);
}

// ------------------------------------------------------------------ 视频链路

// setVideoSurface(surfaceId: string, width: number, height: number): boolean
napi_value SetVideoSurface(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const std::string surface_str = GetStringArg(env, argc > 0 ? args[0] : nullptr);
    const int32_t width  = GetIntArg(env, argc > 1 ? args[1] : nullptr, 0);
    const int32_t height = GetIntArg(env, argc > 2 ? args[2] : nullptr, 0);
    if (surface_str.empty()) {
        napi_throw_error(env, nullptr, "surfaceId 不能为空");
        return nullptr;
    }
    uint64_t surface_id = 0;
    try {
        surface_id = std::stoull(surface_str);
    } catch (const std::exception&) {
        napi_throw_error(env, nullptr, "surfaceId 格式无效");
        return nullptr;
    }

    HosController* controller = Controller();
    // 旧接收器先停，再按新 surface 重建
    controller->resetFrameSink();
    g_video_receiver.reset();

    auto receiver = std::make_unique<HosVideoReceiver>(
        &EmitToJs, [] {
            if (g_controller) g_controller->requestRemoteKeyframe();
        });
    if (!receiver->start(surface_id, width, height)) {
        g_video_receiver = std::move(receiver);  // 保留对象以便 stop 清理
        napi_value failed = nullptr;
        napi_get_boolean(env, false, &failed);
        return failed;
    }
    controller->setFrameSink([](const uint8_t* data, size_t size, uint32_t frame_id,
                                bool keyframe, uint64_t ts_us) {
        if (g_video_receiver) g_video_receiver->onFrame(data, size, frame_id, keyframe, ts_us);
    });
    g_video_receiver = std::move(receiver);

    napi_value ok = nullptr;
    napi_get_boolean(env, true, &ok);
    return ok;
}

napi_value ClearVideoSurface(napi_env env, napi_callback_info /*info*/) {
    if (g_controller) g_controller->resetFrameSink();
    g_video_receiver.reset();
    return Undefined(env);
}

}  // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"sendClipboardText",nullptr,SendClipboardText,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"fileChannelReady",nullptr,FileReady,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"fileCommand",nullptr,FileCommand,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"uploadFile",nullptr,UploadFile,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"downloadFile",nullptr,DownloadFile,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"cancelFileTransfer",nullptr,CancelFile,nullptr,nullptr,nullptr,napi_default,nullptr},
        {"version", nullptr, Version, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"httpSmokeTest", nullptr, HttpSmokeTest, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setEventListener", nullptr, SetEventListener, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"configure", nullptr, Configure, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"addThisDevice", nullptr, AddThisDevice, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getLocalIdentity", nullptr, GetLocalIdentity, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setConnectionKey", nullptr, SetConnectionKey, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"login", nullptr, Login, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"restoreLogin", nullptr, RestoreLogin, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getLoginSession", nullptr, GetLoginSession, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cancelLoginAttempt", nullptr, CancelLoginAttempt, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerAccount", nullptr, RegisterAccount, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"logout", nullptr, Logout, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"refreshDevices", nullptr, RefreshDevices, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setIceServers", nullptr, SetIceServers, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"connectToDevice", nullptr, ConnectToDevice, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"disconnect", nullptr, Disconnect, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sessionAuthenticated", nullptr, SessionAuthenticated, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"requestRemoteScreens", nullptr, RequestRemoteScreens, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"requestRemoteWallpaper", nullptr, RequestRemoteWallpaper, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cachedWallpaper", nullptr, CachedWallpaper, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"switchRemoteScreen", nullptr, SwitchRemoteScreen, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setRemoteVideoConfig", nullptr, SetRemoteVideoConfig, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"requestRemoteKeyframe", nullptr, RequestRemoteKeyframe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendInputEvent", nullptr, SendInputEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setVideoSurface", nullptr, SetVideoSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"clearVideoSurface", nullptr, ClearVideoSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module pxcHosModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "pxc_hos",
    .nm_priv = ((void*)0),
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterPxcHosModule(void) {
    napi_module_register(&pxcHosModule);
}
