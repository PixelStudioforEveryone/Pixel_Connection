#pragma once
#include <QCoreApplication>
#include <QProcess>
#include <QTimer>
#include <QString>
#include <functional>
#include <memory>

namespace pxc::gui {
struct ClipboardText { bool available = false; QString text; };

// Clipboard owners may render text lazily and stop answering. Isolate that
// potentially blocking OS/Qt read in the same executable's helper mode.
inline void readClipboardText(QObject* owner, std::function<void(ClipboardText)> callback) {
    auto* process = new QProcess(owner);
    auto* timeout = new QTimer(process);
    timeout->setSingleShot(true);
    auto delivered = std::make_shared<bool>(false);
    auto complete = [process, timeout, delivered, callback](ClipboardText result) {
        if (*delivered) return;
        *delivered = true;
        timeout->stop();
        callback(std::move(result));
        if (process->state() == QProcess::NotRunning) process->deleteLater();
        else {
            // Reap asynchronously after kill; QProcess destruction must not
            // wait for an unresponsive helper in the GUI thread.
            QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                             process, [process] { process->deleteLater(); });
            process->kill();
        }
    };
    QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), owner,
        [process, complete](int code, QProcess::ExitStatus status) {
            const QByteArray bytes = process->readAllStandardOutput();
            if (code == 0 && status == QProcess::NormalExit && bytes.size() <= 64 * 1024)
                complete({true, QString::fromUtf8(bytes)});
            else complete({});
        });
    QObject::connect(process, &QProcess::errorOccurred, owner,
        [complete](QProcess::ProcessError) { complete({}); });
    QObject::connect(timeout, &QTimer::timeout, owner, [process, complete] {
        process->kill();
        complete({});
    });
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= 0x08000000; // CREATE_NO_WINDOW, including console test runners
    });
#endif
    const QString testExecutable = qEnvironmentVariable("PXC_CLIPBOARD_HELPER_EXE");
    process->start(testExecutable.isEmpty() ? QCoreApplication::applicationFilePath() : testExecutable,
                   {QStringLiteral("--clipboard-read-text")});
    timeout->start(1500);
}
} // namespace pxc::gui
