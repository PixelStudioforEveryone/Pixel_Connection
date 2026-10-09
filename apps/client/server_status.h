#pragma once
#include <functional>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QUrl>
#include <QRegularExpression>
#include <QHostAddress>
#include <QNetworkInterface>
#include "server_addresses.h"

namespace pxc::gui {
struct ServerStatus {
    bool healthy = false;
    bool identified = false;
    QString id, name, platform;
    int signalingPort = 0;
    int apiPort = 0;
    QString apiUrl, signalingUrl, listenV4, listenV6;
    QString description() const {
        if (!healthy) return QStringLiteral("服务器暂时不可达");
        if (!identified) return QStringLiteral("服务可达；旧版服务器未提供来源信息");
        return QStringLiteral("服务器：%1 · %2\n服务器标识：%3").arg(name, platform, id);
    }
};

struct ServerEndpoint { QString api, signaling; };
inline bool shareableUrl(const QString& text, bool signaling) {
    const QUrl url(text);
    const bool scheme = signaling ? url.scheme() == "ws" || url.scheme() == "wss"
                                   : url.scheme() == "http" || url.scheme() == "https";
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || !scheme ||
        url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) return false;
    const QHostAddress address(url.host());
    return address.isNull() || (!address.isLoopback() && !address.isLinkLocal() &&
        address != QHostAddress::Any && address != QHostAddress::AnyIPv4 && address != QHostAddress::AnyIPv6);
}

inline QList<ServerEndpoint> serverEndpoints(const ServerStatus& status, const QString& connectedApi,
                                            const QString& connectedSignaling) {
    QList<ServerEndpoint> endpoints;
    auto add = [&](const QString& api, const QString& ws) {
        if (!shareableUrl(api, false) || !shareableUrl(ws, true)) return;
        for (const auto& row : endpoints) if (row.api == api && row.signaling == ws) return;
        endpoints.append({api, ws});
    };
    if (!status.identified) return endpoints;
    add(status.apiUrl, status.signalingUrl);
    if (!endpoints.isEmpty()) return endpoints;
    // A remote connection is already an externally usable alias. Never use the
    // controller's own network interfaces as a remote server's address.
    if (shareableUrl(connectedApi, false)) {
        add(connectedApi, connectedSignaling);
        return endpoints;
    }
    const QHostAddress localHost(QUrl(connectedApi).host());
    if (!localHost.isLoopback() && QUrl(connectedApi).host() != "localhost") return endpoints;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp) || !(iface.flags() & QNetworkInterface::IsRunning) ||
            (iface.flags() & QNetworkInterface::IsLoopBack)) continue;
        for (const auto& entry : iface.addressEntries()) {
            const auto address = entry.ip();
            const bool v4 = address.protocol() == QAbstractSocket::IPv4Protocol;
            const QString binding = v4 ? status.listenV4 : status.listenV6;
            if (binding.isEmpty()) continue;
            const QHostAddress bound(binding);
            const bool any = bound == QHostAddress::Any || bound == QHostAddress::AnyIPv4 || bound == QHostAddress::AnyIPv6;
            if (!any && bound != address) continue;
            if (status.apiPort < 1 || status.signalingPort < 1) continue;
            add(serverUrl("http", address.toString(), status.apiPort),
                serverUrl("ws", address.toString(), status.signalingPort));
        }
    }
    return endpoints;
}

inline void probeServer(QObject* owner, const QString& apiUrl,
                        std::function<void(ServerStatus)> completed) {
    auto* network = new QNetworkAccessManager(owner);
    QUrl url(apiUrl);
    url.setPath(url.path().replace(QRegularExpression(QStringLiteral("/+$")), QString()) + QStringLiteral("/api/v1/health"));
    url.setQuery(QString()); url.setFragment(QString());
    QNetworkRequest request(url);
    request.setTransferTimeout(3000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto* reply = network->get(request);
    QObject::connect(reply, &QNetworkReply::finished, owner, [network, reply, completed] {
        ServerStatus status;
        if (reply->error() == QNetworkReply::NoError &&
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
            const auto body = reply->readAll();
            const auto root = body.size() <= 16384 ? QJsonDocument::fromJson(body).object() : QJsonObject();
            status.healthy = root.value(QStringLiteral("status")).toString() == QStringLiteral("ok") &&
                             root.value(QStringLiteral("api")).toString() == QStringLiteral("v1");
            const auto server = root.value(QStringLiteral("server")).toObject();
            status.identified = status.healthy && server.value(QStringLiteral("product")).toString() == QStringLiteral("PixelConnection") &&
                server.value(QStringLiteral("ready")).toBool() && !server.value(QStringLiteral("id")).toString().isEmpty();
            status.id = server.value(QStringLiteral("id")).toString();
            status.name = server.value(QStringLiteral("name")).toString();
            status.platform = server.value(QStringLiteral("platform")).toString();
            status.signalingPort = server.value(QStringLiteral("signaling_port")).toInt();
            status.apiPort = server.value(QStringLiteral("api_port")).toInt();
            status.apiUrl = server.value(QStringLiteral("api_url")).toString();
            status.signalingUrl = server.value(QStringLiteral("signaling_url")).toString();
            status.listenV4 = server.value(QStringLiteral("listen_v4")).toString();
            status.listenV6 = server.value(QStringLiteral("listen_v6")).toString();
        }
        network->deleteLater();
        completed(status);
    });
}
} // namespace pxc::gui
