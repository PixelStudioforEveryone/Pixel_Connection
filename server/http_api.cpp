#include "http_api.h"
#include "proxy_client_ip.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>

#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXHttpServer.h>
#include <nlohmann/json.hpp>

#include "pxc/identity.h"

namespace pxc::server {
namespace {

// 请求体上限。
//
// 注意：IXWebSocket 会把整个请求体读进内存之后才回调到这里，所以这个检查
// **不能**防止大包体打爆内存——它只是避免对明显异常的输入做 JSON 解析。
// 真正的请求体大小限制必须放在反向代理（Nginx client_max_body_size 等）。
// 账号接口的正常请求只有几百字节。
constexpr size_t kMaxBodyBytes = 64 * 1024;

ix::HttpResponsePtr json_response(int status, const std::string& description,
                                  const nlohmann::json& body) {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"]  = "application/json; charset=utf-8";
    // 账号接口的响应含令牌，禁止任何中间层缓存
    headers["Cache-Control"] = "no-store";
    headers["X-Content-Type-Options"] = "nosniff";

    return std::make_shared<ix::HttpResponse>(
        status, description, ix::HttpErrorCode::Ok, headers, body.dump());
}

ix::HttpResponsePtr error_response(int status, const std::string& description,
                                   const std::string& code, const std::string& detail = "") {
    nlohmann::json body{{"error", code}};
    if (!detail.empty()) body["detail"] = detail;
    return json_response(status, description, body);
}

// AuthService 的结果映射到 HTTP 语义
int status_for(AuthStatus status) {
    switch (status) {
        case AuthStatus::Ok:                return 200;
        case AuthStatus::InvalidArgument:   return 400;
        case AuthStatus::DuplicateAccount:  return 409;
        case AuthStatus::BadCredentials:    return 401;
        case AuthStatus::Unauthorized:      return 401;
        case AuthStatus::Forbidden:         return 403;
        case AuthStatus::RegistrationClosed:return 403;
        case AuthStatus::NotFound:          return 404;
        case AuthStatus::RateLimited:       return 429;
        case AuthStatus::Internal:          return 500;
    }
    return 500;
}

std::string code_for(AuthStatus status) {
    switch (status) {
        case AuthStatus::Ok:                return "ok";
        case AuthStatus::InvalidArgument:   return "invalid_argument";
        case AuthStatus::DuplicateAccount:  return "duplicate_account";
        case AuthStatus::BadCredentials:    return "bad_credentials";
        case AuthStatus::Unauthorized:      return "unauthorized";
        case AuthStatus::Forbidden:         return "forbidden";
        case AuthStatus::RegistrationClosed:return "registration_closed";
        case AuthStatus::NotFound:          return "not_found";
        case AuthStatus::RateLimited:       return "rate_limited";
        case AuthStatus::Internal:          return "internal_error";
    }
    return "internal_error";
}

// 去掉 query string，只保留路径
std::string path_of(const std::string& uri) {
    const size_t q = uri.find('?');
    return q == std::string::npos ? uri : uri.substr(0, q);
}

// 取 Authorization: Bearer <token>
std::string bearer_token(const ix::HttpRequestPtr& request) {
    auto it = request->headers.find("Authorization");
    if (it == request->headers.end()) return {};

    const std::string& value = it->second;
    const std::string  prefix = "Bearer ";
    if (value.size() <= prefix.size()) return {};
    if (value.compare(0, prefix.size(), prefix) != 0) return {};

    std::string token = value.substr(prefix.size());
    // 容忍尾部空白
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.pop_back();
    return token;
}

nlohmann::json parse_body(const std::string& body, bool& ok) {
    ok = false;
    if (body.empty()) return nlohmann::json::object();
    if (body.size() > kMaxBodyBytes) return nlohmann::json::object();
    try {
        auto parsed = nlohmann::json::parse(body);
        if (!parsed.is_object()) return nlohmann::json::object();
        ok = true;
        return parsed;
    } catch (const std::exception&) {
        return nlohmann::json::object();
    }
}

std::string json_string(const nlohmann::json& body, const char* key) {
    if (!body.contains(key) || !body[key].is_string()) return {};
    return body[key].get<std::string>();
}

nlohmann::json account_json(const store::Account& account) {
    return nlohmann::json{
        {"id", account.id}, {"username", account.username}, {"email", account.email}};
}

nlohmann::json device_json(const store::Device& device) {
    return nlohmann::json{
        {"device_id", device.device_id},
        {"name", device.name},
        {"public_ip", device.last_public_ip},
        {"last_seen", device.last_seen},
        {"platform", device.platform},
        {"online", device.online},
    };
}

}  // namespace

AccountApi::AccountApi(AuthService& auth, ApiOptions options)
    : auth_(auth), options_(options) {}

AccountApi::~AccountApi() {
    stop();
}

bool AccountApi::start() {
    auto launch = [this](const std::string& host, int family, const std::string& label) {
        try {
            auto server = std::make_shared<ix::HttpServer>(
                options_.port, host,
                ix::SocketServer::kDefaultTcpBacklog,
                ix::SocketServer::kDefaultMaxConnections,
                family);

            server->setOnConnectionCallback(
                [this](const ix::HttpRequestPtr&              request,
                       const std::shared_ptr<ix::ConnectionState>& state) {
                    return route(request, state);
                });

            const auto [ok, err] = server->listen();
            if (!ok) {
                std::cerr << "[api] " << label << " 监听 " << host << ":" << options_.port
                          << " 失败: " << err << "\n";
                return false;
            }
            server->start();
            servers_.push_back(server);

            std::ostringstream desc;
            desc << label << " " << host << ":" << options_.port;
            listeners_.push_back(desc.str());
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[api] " << label << " 启动异常: " << e.what() << "\n";
            return false;
        }
    };

    bool any = false;
    if (options_.enable_v4) any |= launch(options_.host_v4, AF_INET, "IPv4");
    if (options_.enable_v6) any |= launch(options_.host_v6, AF_INET6, "IPv6");
    return any;
}

void AccountApi::stop() {
    for (auto& server : servers_) {
        if (server) server->stop();
    }
    servers_.clear();
}

ix::HttpResponsePtr AccountApi::route(const ix::HttpRequestPtr&                   request,
                                      const std::shared_ptr<ix::ConnectionState>& state) {
    const std::string path   = path_of(request->uri);
    const std::string method = request->method;
    const std::string client_ip = proxy_client_ip(state ? state->getRemoteIp() : std::string(),
        request->headers, options_.trust_loopback_proxy);

    // ------------------------------------------------------------------ 健康检查
    if (path == "/api/v1/health" && method == "GET") {
        return json_response(200, "OK", nlohmann::json{{"status", "ok"}, {"api", "v1"},
            {"server", {{"product", "PixelConnection"}, {"id", options_.server_id},
                        {"name", options_.server_name}, {"platform", options_.server_platform},
                        {"signaling_port", options_.signaling_port}, {"api_port", options_.port},
                        {"api_url", options_.advertised_api_url},
                        {"signaling_url", options_.advertised_signaling_url},
                        {"listen_v4", options_.enable_v4 ? options_.host_v4 : ""},
                        {"listen_v6", options_.enable_v6 ? options_.host_v6 : ""},
                        {"ready", ready_.load()}}}});
    }

    // ------------------------------------------------------------------ 注册
    if (path == "/api/v1/auth/register" && method == "POST") {
        if (!auth_.register_allowed(client_ip)) {
            return error_response(429, "Too Many Requests", "rate_limited",
                                  "注册尝试过于频繁，请稍后再试");
        }
        auth_.note_register_attempt(client_ip);

        bool ok = false;
        const auto body = parse_body(request->body, ok);
        if (!ok) return error_response(400, "Bad Request", "invalid_json");

        store::Account account;
        const auto status = auth_.register_account(json_string(body, "username"),
                                                   json_string(body, "email"),
                                                   json_string(body, "password"),
                                                   account);
        if (status != AuthStatus::Ok) {
            // 注册失败不回显输入，也不区分「用户名已占用」和「邮箱已占用」
            return error_response(status_for(status), "Register Failed", code_for(status));
        }
        return json_response(200, "OK", nlohmann::json{{"account", account_json(account)}});
    }

    // ------------------------------------------------------------------ 登录
    if (path == "/api/v1/auth/login" && method == "POST") {
        bool ok = false;
        const auto body = parse_body(request->body, ok);
        if (!ok) return error_response(400, "Bad Request", "invalid_json");

        SessionInfo session;
        const auto  status = auth_.login(json_string(body, "identifier"),
                                        json_string(body, "password"),
                                        client_ip, session);

        if (status == AuthStatus::RateLimited) {
            return error_response(429, "Too Many Requests", "rate_limited",
                                  "尝试过于频繁，请稍后再试");
        }
        if (status != AuthStatus::Ok) {
            // 统一措辞，避免通过错误信息区分「账号不存在」和「密码错误」
            return error_response(status_for(status), "Login Failed", code_for(status));
        }

        return json_response(200, "OK",
                             nlohmann::json{
                                 {"access_token", session.token},
                                 {"expires_in", auth_.config().token_ttl_seconds},
                                 {"token_type", "Bearer"},
                                 {"account",
                                  nlohmann::json{{"id", session.account_id},
                                                 {"username", session.username}}},
                             });
    }

    // ------------------------------------------------------------------ 注销
    if (path == "/api/v1/auth/logout" && method == "POST") {
        const std::string token = bearer_token(request);
        if (token.empty()) return error_response(401, "Unauthorized", "unauthorized");

        const auto status = auth_.logout(token);
        if (status != AuthStatus::Ok) {
            return error_response(status_for(status), "Logout Failed", code_for(status));
        }
        return json_response(200, "OK", nlohmann::json{{"ok", true}});
    }

    // -------------------------------------------------- 设备列表 / 单设备操作
    const std::string devices_prefix = "/api/v1/devices";
    if (path.rfind(devices_prefix, 0) == 0) {
        const std::string token = bearer_token(request);
        if (token.empty()) return error_response(401, "Unauthorized", "unauthorized");

        store::Account account;
        if (auth_.validate_token(token, account) != AuthStatus::Ok) {
            return error_response(401, "Unauthorized", "unauthorized", "令牌无效或已过期");
        }

        // GET /api/v1/devices
        if (path == devices_prefix && method == "GET") {
            std::vector<store::Device> devices;
            const auto status = auth_.list_devices(account.id, devices);
            if (status != AuthStatus::Ok) {
                return error_response(status_for(status), "Failed", code_for(status));
            }

            nlohmann::json array = nlohmann::json::array();
            for (const auto& device : devices) array.push_back(device_json(device));
            return json_response(200, "OK", nlohmann::json{{"devices", array}});
        }

        // /api/v1/devices/<id> 及子路径
        if (path.size() > devices_prefix.size() + 1 && path[devices_prefix.size()] == '/') {
            std::string rest = path.substr(devices_prefix.size() + 1);

            bool revoke = false;
            const std::string revoke_suffix = "/revoke";
            if (rest.size() > revoke_suffix.size() &&
                rest.compare(rest.size() - revoke_suffix.size(), revoke_suffix.size(),
                             revoke_suffix) == 0) {
                rest   = rest.substr(0, rest.size() - revoke_suffix.size());
                revoke = true;
            }

            if (rest.empty() || rest.find('/') != std::string::npos) {
                return error_response(404, "Not Found", "not_found");
            }

            // /revoke 只接受 POST。
            // 如果让它落到下面的 DELETE/PATCH 分支，DELETE /devices/<id>/revoke
            // 会变成「删除设备」——一个撤销请求把设备删掉，语义完全相反。
            if (revoke) {
                if (method != "POST") {
                    return error_response(405, "Method Not Allowed", "method_not_allowed");
                }
                const auto status = auth_.revoke_device(account.id, rest);
                if (status != AuthStatus::Ok) {
                    return error_response(status_for(status), "Failed", code_for(status));
                }
                return json_response(200, "OK", nlohmann::json{{"ok", true}});
            }

            if (method == "DELETE") {
                const auto status = auth_.remove_device(account.id, rest);
                if (status != AuthStatus::Ok) {
                    return error_response(status_for(status), "Failed", code_for(status));
                }
                return json_response(200, "OK", nlohmann::json{{"ok", true}});
            }

            if (method == "PATCH") {
                bool ok = false;
                const auto body = parse_body(request->body, ok);
                if (!ok) return error_response(400, "Bad Request", "invalid_json");

                const auto status = auth_.rename_device(account.id, rest,
                                                        json_string(body, "name"));
                if (status != AuthStatus::Ok) {
                    return error_response(status_for(status), "Failed", code_for(status));
                }
                return json_response(200, "OK", nlohmann::json{{"ok", true}});
            }

            return error_response(405, "Method Not Allowed", "method_not_allowed");
        }

        return error_response(404, "Not Found", "not_found");
    }

    return error_response(404, "Not Found", "not_found");
}

}  // namespace pxc::server
