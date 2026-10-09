#include "pxc/account_api.h"

#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <nlohmann/json.hpp>

namespace pxc {
namespace {

// 从 JSON 响应里取错误码；取不到就返回状态码文本
std::string extract_error(const std::string& body, int status) {
    try {
        auto j = nlohmann::json::parse(body);
        if (j.is_object()) {
            if (j.contains("error") && j["error"].is_string()) {
                std::string code = j["error"].get<std::string>();
                if (j.contains("detail") && j["detail"].is_string()) {
                    code += ": " + j["detail"].get<std::string>();
                }
                return code;
            }
        }
    } catch (const std::exception&) {
        // 非 JSON 响应，落到下面的通用描述
    }
    return "HTTP " + std::to_string(status);
}

}  // namespace

AccountApiClient::AccountApiClient(std::string base_url, int timeout_seconds)
    : base_url_(std::move(base_url)), timeout_seconds_(timeout_seconds) {
    // 去掉尾部斜杠，拼接路径时不会出现双斜杠
    while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
}

HttpResult AccountApiClient::request(const std::string& method,
                                     const std::string& path,
                                     const std::string& body,
                                     const std::string& token) {
    HttpResult result;

    ix::HttpClient client;
    client.setForceBody(true);

    if (!verify_tls_) {
        // 仅用于自签证书的本机测试。生产必须开启校验，
        // 否则任何能劫持 DNS/路由的人都可冒充账号服务器。
        ix::SocketTLSOptions tls;
        tls.disable_hostname_validation = true;
        client.setTLSOptions(tls);
    }

    auto args = client.createRequest(base_url_ + path, method);
    args->connectTimeout  = timeout_seconds_;
    args->transferTimeout = timeout_seconds_;
    args->followRedirects = false;  // 账号接口不应出现重定向，避免令牌被转发到别处
    args->extraHeaders["Content-Type"] = "application/json";
    if (!token.empty()) {
        args->extraHeaders["Authorization"] = "Bearer " + token;
    }

    ix::HttpResponsePtr response;
    try {
        response = client.request(base_url_ + path, method, body, args);
    } catch (const std::exception& e) {
        result.error = std::string("请求异常: ") + e.what();
        return result;
    }

    if (!response) {
        result.error = "无响应";
        return result;
    }

    result.status = response->statusCode;
    result.body   = response->body;

    if (response->errorCode != ix::HttpErrorCode::Ok) {
        result.error = response->errorMsg.empty() ? "传输失败" : response->errorMsg;
        return result;
    }

    result.ok = result.status >= 200 && result.status < 300;
    if (!result.ok) result.error = extract_error(result.body, result.status);
    return result;
}

// ---------------------------------------------------------------------- 账号

bool AccountApiClient::register_account(const std::string& username,
                                        const std::string& email,
                                        const std::string& password,
                                        std::string&       error) {
    nlohmann::json body{
        {"username", username}, {"email", email}, {"password", password}};

    const auto result = request("POST", "/api/v1/auth/register", body.dump(), "");
    if (!result.ok) {
        error = result.error;
        return false;
    }
    return true;
}

bool AccountApiClient::login(const std::string& identifier,
                             const std::string& password,
                             std::string&       out_token,
                             std::string&       out_username,
                             std::string&       error) {
    nlohmann::json body{{"identifier", identifier}, {"password", password}};

    const auto result = request("POST", "/api/v1/auth/login", body.dump(), "");
    if (!result.ok) {
        error = result.error;
        return false;
    }

    try {
        const auto parsed = nlohmann::json::parse(result.body);
        out_token = parsed.at("access_token").get<std::string>();
        if (parsed.contains("account") && parsed["account"].is_object()) {
            out_username = parsed["account"].value("username", "");
        }
    } catch (const std::exception&) {
        error = "登录响应格式异常";
        return false;
    }

    if (out_token.empty()) {
        error = "服务器未返回访问令牌";
        return false;
    }
    return true;
}

bool AccountApiClient::logout(const std::string& token, std::string& error) {
    const auto result = request("POST", "/api/v1/auth/logout", "{}", token);
    if (!result.ok) {
        error = result.error;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------- 设备

bool AccountApiClient::list_devices(const std::string&     token,
                                    std::vector<PeerInfo>& out_devices,
                                    std::string&           error) {
    out_devices.clear();

    const auto result = request("GET", "/api/v1/devices", "", token);
    if (!result.ok) {
        error = result.error;
        return false;
    }

    try {
        const auto parsed = nlohmann::json::parse(result.body);
        if (parsed.contains("devices") && parsed["devices"].is_array()) {
            for (const auto& item : parsed["devices"]) {
                PeerInfo info;
                info.device_id = item.value("device_id", "");
                info.name      = item.value("name", "");
                info.public_ip = item.value("public_ip", "");
                info.last_seen = item.value("last_seen", "");
                info.platform  = item.value("platform", "");
                info.online    = item.value("online", false);
                if (!info.device_id.empty()) out_devices.push_back(std::move(info));
            }
        }
    } catch (const std::exception&) {
        error = "设备列表响应格式异常";
        return false;
    }
    return true;
}

bool AccountApiClient::rename_device(const std::string& token,
                                     const std::string& device_id,
                                     const std::string& new_name,
                                     std::string&       error) {
    nlohmann::json body{{"name", new_name}};
    const auto result = request("PATCH", "/api/v1/devices/" + device_id, body.dump(), token);
    if (!result.ok) {
        error = result.error;
        return false;
    }
    return true;
}

bool AccountApiClient::remove_device(const std::string& token,
                                     const std::string& device_id,
                                     std::string&       error) {
    const auto result = request("DELETE", "/api/v1/devices/" + device_id, "", token);
    if (!result.ok) {
        error = result.error;
        return false;
    }
    return true;
}

bool AccountApiClient::revoke_device(const std::string& token,
                                     const std::string& device_id,
                                     std::string&       error) {
    const auto result = request("POST", "/api/v1/devices/" + device_id + "/revoke", "{}", token);
    if (!result.ok) {
        error = result.error;
        return false;
    }
    return true;
}

}  // namespace pxc
