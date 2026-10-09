#include "desktop_wallpaper.h"

#include <QFile>
#include <QImageReader>
#include <QLinearGradient>
#include <QPainter>
#include <QProcess>
#include <QUrl>
#include <QXmlStreamReader>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#endif

namespace pxc::gui {
namespace {

QString desktop_setting(const QString& schema, const QString& key) {
    QProcess process;
    process.start(QStringLiteral("gsettings"), {QStringLiteral("get"), schema, key});
    if (!process.waitForFinished(1500)) {
        process.kill();
        process.waitForFinished();
        return {};
    }
    if (process.exitCode() != 0) return {};
    QString value = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (value.startsWith('\'') && value.endsWith('\'')) value = value.mid(1, value.size() - 2);
    return value;
}

QString slideshow_file(const QString& path) {
    if (!path.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)) return path;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QXmlStreamReader xml(&file);
    bool in_file = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QStringLiteral("file")) in_file = true;
        if (in_file && xml.isCharacters() && !xml.isWhitespace()) {
            const QString candidate = xml.text().toString().trimmed();
            if (QFile::exists(candidate)) return candidate;
        }
        if (xml.isEndElement() && xml.name() == QStringLiteral("file")) in_file = false;
    }
    return {};
}

}  // namespace

QImage read_desktop_wallpaper(QSize displaySize, QString* error) {
    if (error) error->clear();
    QString path;
    QString mode = QStringLiteral("zoom");
    QColor background(Qt::black), secondary(Qt::black);
    QString shading;
#if defined(_WIN32)
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IDesktopWallpaper* wallpaper = nullptr;
    bool configured = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wallpaper)))) {
        MONITORINFO primary = {};
        primary.cbSize = sizeof(primary);
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &primary);
        UINT count = 0;
        wallpaper->GetMonitorDevicePathCount(&count);
        for (UINT i = 0; i < count; ++i) {
            LPWSTR monitor = nullptr;
            if (FAILED(wallpaper->GetMonitorDevicePathAt(i, &monitor))) continue;
            RECT rect = {};
            if (wallpaper->GetMonitorRECT(monitor, &rect) == S_OK && EqualRect(&rect, &primary.rcMonitor)) {
                LPWSTR filename = nullptr;
                if (SUCCEEDED(wallpaper->GetWallpaper(monitor, &filename))) {
                    path = QString::fromWCharArray(filename ? filename : L"");
                    configured = true;
                    displaySize = QSize(rect.right - rect.left, rect.bottom - rect.top);
                }
                CoTaskMemFree(filename);
            }
            CoTaskMemFree(monitor);
            if (configured) break;
        }
        COLORREF color = 0;
        if (SUCCEEDED(wallpaper->GetBackgroundColor(&color)))
            background = QColor(GetRValue(color), GetGValue(color), GetBValue(color));
        DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL;
        wallpaper->GetPosition(&position);
        if (position == DWPOS_FIT) mode = QStringLiteral("scaled");
        else if (position == DWPOS_STRETCH) mode = QStringLiteral("stretched");
        else if (position == DWPOS_CENTER) mode = QStringLiteral("centered");
        else if (position == DWPOS_TILE) mode = QStringLiteral("wallpaper");
        wallpaper->Release();
    }
    if (!configured) {
        wchar_t filename[32768] = {};
        if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, 32768, filename, 0)) {
            if (error) *error = QStringLiteral("无法读取系统桌面壁纸设置");
            if (SUCCEEDED(initialized)) CoUninitialize();
            return {};
        }
        path = QString::fromWCharArray(filename);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
#elif defined(__linux__)
    const QString schema = QStringLiteral("org.gnome.desktop.background");
    QString uri;
    if (desktop_setting(QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("color-scheme"))
        == QStringLiteral("prefer-dark"))
        uri = desktop_setting(schema, QStringLiteral("picture-uri-dark"));
    if (uri.isEmpty()) uri = desktop_setting(schema, QStringLiteral("picture-uri"));
    mode = desktop_setting(schema, QStringLiteral("picture-options"));
    if (mode.isEmpty()) {
        if (error) *error = QStringLiteral("当前桌面未提供 GNOME 壁纸设置");
        return {};
    }
    path = QUrl(uri).toLocalFile();
    if (mode == QStringLiteral("none")) path.clear();
    background = QColor(desktop_setting(schema, QStringLiteral("primary-color")));
    secondary = QColor(desktop_setting(schema, QStringLiteral("secondary-color")));
    shading = desktop_setting(schema, QStringLiteral("color-shading-type"));
#else
    if (error) *error = QStringLiteral("当前平台暂不支持读取桌面壁纸");
    return {};
#endif
    if (displaySize.isEmpty()) displaySize = QSize(1920, 1080);
    const QSize size = displaySize.scaled(QSize(960, 540), Qt::KeepAspectRatio);
    QImage result(size, QImage::Format_RGB32);
    result.fill(background.isValid() ? background : QColor(Qt::black));
    QPainter painter(&result);
    if (secondary.isValid() && (shading == QStringLiteral("horizontal") || shading == QStringLiteral("vertical"))) {
        QLinearGradient gradient(QPointF(0, 0), shading == QStringLiteral("horizontal")
            ? QPointF(size.width(), 0) : QPointF(0, size.height()));
        gradient.setColorAt(0, background.isValid() ? background : QColor(Qt::black));
        gradient.setColorAt(1, secondary);
        painter.fillRect(result.rect(), gradient);
    }
    if (path.isEmpty()) return result;
    QImageReader reader(slideshow_file(path));
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    if (original.isEmpty()) {
        if (error) *error = QStringLiteral("无法读取系统配置的壁纸图片: ") + reader.errorString();
        return {};
    }
    const bool naturalSize = mode == QStringLiteral("centered") || mode == QStringLiteral("wallpaper");
    const QSize target = naturalSize
        ? QSize(qMax(1, original.width() * size.width() / displaySize.width()),
                qMax(1, original.height() * size.height() / displaySize.height()))
        : mode == QStringLiteral("stretched") ? size
        : original.scaled(size, mode == QStringLiteral("scaled") ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding);
    reader.setScaledSize(target);
    const QImage image = reader.read();
    if (image.isNull()) {
        if (error) *error = QStringLiteral("系统壁纸图片解码失败");
        return {};
    }
    if (mode == QStringLiteral("wallpaper")) {
        for (int y = 0; y < size.height(); y += image.height())
            for (int x = 0; x < size.width(); x += image.width()) painter.drawImage(x, y, image);
    } else {
        painter.drawImage((size.width() - image.width()) / 2, (size.height() - image.height()) / 2, image);
    }
    return result;
}

}  // namespace pxc::gui
