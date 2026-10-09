#pragma once
#include <QUrl>
#include <QHostAddress>

namespace pxc::gui {
inline QString serverUrl(const QString& scheme, const QString& host, quint16 port) {
    QUrl url;
    url.setScheme(scheme); url.setHost(host); url.setPort(port);
    return url.toString(); // QUrl brackets IPv6 literals and escapes scope identifiers.
}
inline QString localConnectHost(const QString& bind) {
    if (bind.isEmpty() || bind == QStringLiteral("0.0.0.0")) return QStringLiteral("127.0.0.1");
    if (bind == QStringLiteral("::")) return QStringLiteral("::1");
    return bind;
}
}
