#pragma once

#include <QDialog>
#include <QHash>
#include <QString>

class QLineEdit;
class QTableWidget;
class QPushButton;
class QJsonObject;

namespace pxc::gui {

class ClientController;

class FileTransferDialog : public QDialog {
    Q_OBJECT
public:
    explicit FileTransferDialog(ClientController* controller, const QString& remoteName,
                                QWidget* parent = nullptr);

private:
    void buildUi();
    void refreshLocal();
    void requestRemoteList(const QString& path);
    void showRemoteList(const QJsonObject& message);
    void uploadSelected();
    void downloadSelected();
    void updateTransferButtons();
    void addTransferRow(const QString& id, const QString& name, qint64 size,
                        const QString& source, const QString& destination,
                        const QString& state);
    void updateTransfer(const QString& id, const QString& name, qint64 transferred,
                        qint64 total, const QString& state);

    ClientController* controller_ = nullptr;
    QString remote_name_;
    QString requested_remote_path_;
    QLineEdit* local_path_ = nullptr;
    QLineEdit* remote_path_ = nullptr;
    QTableWidget* local_table_ = nullptr;
    QTableWidget* remote_table_ = nullptr;
    QTableWidget* transfers_table_ = nullptr;
    QPushButton* upload_button_ = nullptr;
    QPushButton* download_button_ = nullptr;
    QHash<QString, int> transfer_rows_;
};

}  // namespace pxc::gui
