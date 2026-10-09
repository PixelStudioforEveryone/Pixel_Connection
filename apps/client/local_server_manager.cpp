#include "local_server_manager.h"
#include "server_status.h"
#include "server_addresses.h"

#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QTcpServer>

namespace pxc::gui {

LocalServerManager::LocalServerManager(QObject* parent) : QObject(parent) {}

LocalServerManager::~LocalServerManager() {
    stop();
}

bool LocalServerManager::running() const {
    return process_ && process_->state() != QProcess::NotRunning;
}

void LocalServerManager::start(const QString& executable, const QString& databasePath,
                               const QString& bindAddress, quint16 signalingPort,
                               quint16 apiPort) {
    if (running()) { emit failed(QStringLiteral("本地服务器已经在运行")); return; }
    const auto generation = ++startGeneration_;
    probeServer(this, serverUrl(QStringLiteral("http"), localConnectHost(bindAddress), apiPort),
        [this, executable, databasePath, bindAddress, signalingPort, apiPort, generation](const ServerStatus& status) {
            if (generation != startGeneration_) return;
            if (status.healthy) {
                if (!status.identified || status.signalingPort != signalingPort) {
                    emit failed(QStringLiteral("本机 API 端口已有服务，但无法确认对应信令端口。请核对已有服务地址，未启动第二个服务器。"));
                    return;
                }
                emit existingServerDetected(bindAddress, signalingPort, apiPort);
                return;
            }
            launch(executable, databasePath, bindAddress, signalingPort, apiPort);
        });
}

void LocalServerManager::launch(const QString& executable, const QString& databasePath,
                              const QString& bindAddress, quint16 signalingPort, quint16 apiPort) {
    if (running()) {
        emit failed(QStringLiteral("本地服务器已经在运行"));
        return;
    }
    if (executable.isEmpty() || databasePath.isEmpty() || bindAddress.isEmpty()) {
        emit failed(QStringLiteral("本地服务器配置不完整"));
        return;
    }
    if (signalingPort == 0 || apiPort == 0 || signalingPort == apiPort) {
        emit failed(QStringLiteral("端口配置无效或端口重复"));
        return;
    }
    const QHostAddress address(bindAddress);
    if (address.isNull()) { emit failed(QStringLiteral("监听地址应为 IPv4 或 IPv6")); return; }
    for (const auto port : {signalingPort, apiPort}) {
        QTcpServer check;
        if (!check.listen(address, port)) {
            emit failed(QStringLiteral("本机端口 %1 已被占用或无法监听。请核对已有服务地址；未启动第二个服务器。").arg(port));
            return;
        }
        check.close();
    }

    if (!QFile::exists(executable)) {
        emit failed(QStringLiteral(
            "找不到 PixelConnection 程序，请保留完整程序包，"
            "或使用「远程服务」连接已有服务器。"));
        return;
    }

    auto* process = new QProcess(this);
    process_ = process;
    address_ = bindAddress;
    signalingPort_ = signalingPort;
    apiPort_ = apiPort;
    stopping_ = false;
    ready_ = false;
    outputBuffer_.clear();

    connect(process, &QProcess::readyReadStandardOutput,
            this, &LocalServerManager::onReadyRead);
    connect(process, &QProcess::readyReadStandardError,
            this, &LocalServerManager::onReadyRead);
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &LocalServerManager::onFinished);
    connect(process, &QProcess::errorOccurred,
            this, &LocalServerManager::onErrorOccurred);

    QStringList args;
    args << QStringLiteral("--server") << QStringLiteral("--control-stdin")
         << QStringLiteral("--port") << QString::number(signalingPort_)
         << QStringLiteral("--api-port") << QString::number(apiPort_)
         << QStringLiteral("--db") << databasePath;
    const QHostAddress bind(bindAddress);
    if (bind.protocol() == QAbstractSocket::IPv6Protocol) {
        args << QStringLiteral("--no-v4") << QStringLiteral("--v6") << bindAddress;
    } else if (bind.protocol() == QAbstractSocket::IPv4Protocol) {
        args << QStringLiteral("--v4") << bindAddress;
        if (bind.isLoopback()) args << QStringLiteral("--v6") << QStringLiteral("::1");
        else if (bind == QHostAddress::AnyIPv4) args << QStringLiteral("--v6") << QStringLiteral("::");
        else args << QStringLiteral("--no-v6"); // a specific IPv4 binding remains interface scoped
    } else {
        process->deleteLater(); process_ = nullptr;
        emit failed(QStringLiteral("监听地址应为 IPv4 或 IPv6；双栈监听请填写 0.0.0.0"));
        return;
    }

    process->setProgram(executable);
    process->setArguments(args);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->start();

    if (!process->waitForStarted(3000)) {
        const QString reason = process->errorString();
        process->deleteLater();
        process_ = nullptr;
        emit failed(QStringLiteral("本地服务器启动失败: ") + reason);
        return;
    }

    emit output(QStringLiteral("本地服务器进程已启动，等待监听端口就绪"));
    QTimer::singleShot(10000, process, [this, process] {
        if (process_ != process || ready_) return;
        stop();
        emit failed(QStringLiteral("本地服务器启动超时，请检查监听地址和端口是否被占用"));
    });
}

void LocalServerManager::stop() {
    ++startGeneration_;
    if (!process_) return;
    stopping_ = true;
    QProcess* process = process_;
    process_ = nullptr;
    if (process->state() != QProcess::NotRunning) {
        process->write("STOP\n");
        process->closeWriteChannel();
        if (!process->waitForFinished(3000)) {
            process->terminate();
            if (!process->waitForFinished(1000)) {
            // 只强制结束自己创建的这个子进程，不会影响同名/其它服务。
                process->kill();
                process->waitForFinished(1000);
            }
        }
    }
    process->deleteLater();
    ready_ = false;
    emit stopped();
}

void LocalServerManager::onReadyRead() {
    if (!process_) return;
    outputBuffer_.append(process_->readAll());
    int newline = -1;
    while ((newline = outputBuffer_.indexOf('\n')) >= 0) {
        const QByteArray line = outputBuffer_.left(newline).trimmed();
        outputBuffer_.remove(0, newline + 1);
        if (line == "PXC_SERVER_READY") {
            if (!ready_) {
                ready_ = true;
                emit started(address_, signalingPort_, apiPort_);
            }
        } else if (!line.isEmpty()) {
            emit output(QString::fromUtf8(line));
        }
    }
}

void LocalServerManager::onFinished(int exitCode, QProcess::ExitStatus status) {
    if (sender() != process_) return;
    const bool expected = stopping_;
    if (process_) process_->deleteLater();
    process_ = nullptr;
    ready_ = false;
    emit stopped();
    if (!expected && (status != QProcess::NormalExit || exitCode != 0)) {
        emit failed(QStringLiteral("本地服务器异常退出，code=") + QString::number(exitCode));
    }
}

void LocalServerManager::onErrorOccurred(QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
        emit failed(QStringLiteral("找不到或无法启动 PixelConnection 服务器模式"));
    }
}

}  // namespace pxc::gui
