#pragma once

// 管理由 Qt 客户端启动的自身 --server 子进程（同一可执行文件）。
//
// 安全边界：
//   - 只有用户点击启动按钮才启动；不做开机自启
//   - 不执行 sudo、不修改防火墙、不做路由器端口映射
//   - 只停止本对象自己启动的 QProcess，不触碰系统其它服务
//   - 进程绑定地址和端口由用户显式配置

#include <QObject>
#include <QProcess>
#include <QString>

namespace pxc::gui {

class LocalServerManager : public QObject {
    Q_OBJECT

public:
    explicit LocalServerManager(QObject* parent = nullptr);
    ~LocalServerManager() override;

    bool running() const;
    QString address() const { return address_; }
    quint16 signalingPort() const { return signalingPort_; }
    quint16 apiPort() const { return apiPort_; }

public slots:
    void start(const QString& executable, const QString& databasePath,
               const QString& bindAddress, quint16 signalingPort, quint16 apiPort);
    void stop();

signals:
    void started(const QString& address, quint16 signalingPort, quint16 apiPort);
    void stopped();
    void failed(const QString& reason);
    void output(const QString& line);
    void existingServerDetected(const QString& address, quint16 signalingPort, quint16 apiPort);

private slots:
    void onReadyRead();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError error);

private:
    void launch(const QString& executable, const QString& databasePath,
                const QString& bindAddress, quint16 signalingPort, quint16 apiPort);
    quint64 startGeneration_ = 0;
    QProcess* process_ = nullptr;
    QString   address_;
    quint16   signalingPort_ = 9910;
    quint16   apiPort_ = 29910;
    bool      stopping_ = false;
    bool      ready_ = false;
    QByteArray outputBuffer_;
};

}  // namespace pxc::gui
