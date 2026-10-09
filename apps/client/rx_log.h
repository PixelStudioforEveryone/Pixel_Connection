#pragma once

// 轻量文件日志（%TEMP%/pxc-video-rx.log）：黑屏/卡顿排查用。
// 打不开文件时静默降级；调用方任意线程，内部无锁但每行一次性写入。

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTextStream>

namespace pxc::gui {

class RxLog {
public:
    static RxLog& instance() {
        static RxLog log;
        return log;
    }
    void write(const QString& line) {
        if (!open()) return;
        QTextStream ts(&file_);
        ts << QDateTime::currentDateTime().toString("HH:mm:ss.zzz ")
           << line << "\n";
        ts.flush();
    }
private:
    RxLog() = default;
    ~RxLog() {
        if (file_.isOpen()) file_.close();
    }
    bool open() {
        if (tried_) return file_.isOpen();
        tried_ = true;
        file_.setFileName(QDir::temp().filePath("pxc-video-rx.log"));
        file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        return file_.isOpen();
    }
    bool  tried_ = false;
    QFile file_;
};

}  // namespace pxc::gui
