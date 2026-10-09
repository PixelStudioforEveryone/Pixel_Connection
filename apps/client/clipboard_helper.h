#pragma once
#include <QByteArray>
#include <QString>
#include <cstdio>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#endif

namespace pxc::gui {
inline int runClipboardTextHelper(int argc, char** argv) {
    QByteArray bytes;
#ifdef Q_OS_WIN
    (void)argc; (void)argv;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return 2;
    if (!OpenClipboard(nullptr)) return 3;
    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    const SIZE_T size = handle ? GlobalSize(handle) : 0;
    const auto* text = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
    bool valid = false;
    if (text && size <= 256 * 1024) {
        const size_t limit = size / sizeof(wchar_t);
        size_t count = 0;
        while (count < limit && text[count]) ++count;
        if (count < limit) {
            bytes = QString::fromWCharArray(text, static_cast<int>(count)).toUtf8();
            valid = bytes.size() <= 64 * 1024;
        }
    }
    if (text) GlobalUnlock(handle);
    CloseClipboard();
    if (!valid) return 3;
#else
    QGuiApplication app(argc, argv);
    auto* clipboard = app.clipboard();
    if (!clipboard || !clipboard->mimeData() || !clipboard->mimeData()->hasText()) return 2;
    bytes = clipboard->text().toUtf8();
    if (bytes.size() > 64 * 1024) return 3;
#endif
#ifdef Q_OS_WIN
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    return WriteFile(output, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        && written == bytes.size() ? 0 : 3;
#else
    return std::fwrite(bytes.constData(), 1, bytes.size(), stdout) == static_cast<size_t>(bytes.size()) ? 0 : 3;
#endif
}
} // namespace pxc::gui
