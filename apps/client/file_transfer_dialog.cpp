#include "file_transfer_dialog.h"

#include <QDir>
#include <QDateTime>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include "client_controller.h"
#include "theme.h"

namespace pxc::gui {
namespace {

QString displaySize(qint64 bytes) {
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

QFileIconProvider& fileIcons() {
    static QFileIconProvider provider;
    return provider;
}

const QTableWidgetItem* selectedFile(const QTableWidget* table) {
    const auto rows = table->selectionModel()->selectedRows();
    if (rows.isEmpty()) return nullptr;
    const auto* item = table->item(rows.first().row(), 0);
    return item && !item->data(Qt::UserRole + 1).toBool() ? item : nullptr;
}

QTableWidget* makeFileTable(QWidget* parent) {
    auto* table = new QTableWidget(parent);
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("修改日期"),
                                      QStringLiteral("类型"), QStringLiteral("大小")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(28);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->setStyleSheet(QStringLiteral(
        "QTableWidget { background: white; alternate-background-color: #f8f9fb;"
        " border: none; selection-background-color: #e8efff; }"
        "QHeaderView::section { color: #697586; background: #fafbfc;"
        " border: none; border-bottom: 1px solid #e5e8ee; padding: 7px; }"));
    return table;
}

}  // namespace

FileTransferDialog::FileTransferDialog(ClientController* controller, const QString& remoteName,
                                       QWidget* parent)
    : QDialog(parent), controller_(controller), remote_name_(remoteName) {
    setWindowTitle(QStringLiteral("文件传输"));
    const QSize available = screen()->availableGeometry().size() - QSize(32, 48);
    setMinimumSize(QSize(640, 420).boundedTo(available));
    resize(QSize(1050, 680).boundedTo(available));
    buildUi();
    refreshLocal();
    QTimer::singleShot(350, this, [this] { requestRemoteList(QString()); });

    if (controller_) {
        connect(controller_, &ClientController::fileTransferMessageReceived, this,
                [this](const QJsonObject& message) {
                    if (message.value(QStringLiteral("op")).toString() == QStringLiteral("list_result"))
                        showRemoteList(message);
                    else if (message.value(QStringLiteral("op")).toString() == QStringLiteral("ready") &&
                             remote_path_->text().isEmpty())
                        requestRemoteList(QString());
                    else if (message.value(QStringLiteral("op")).toString() == QStringLiteral("mkdir_result") &&
                             message.contains(QStringLiteral("error")))
                        QMessageBox::warning(this, QStringLiteral("新建文件夹失败"),
                            message.value(QStringLiteral("error")).toString());
                });
        connect(controller_, &ClientController::fileTransferProgress, this,
                &FileTransferDialog::updateTransfer);
    }
}

void FileTransferDialog::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(10);

    auto* title_row = new QHBoxLayout;
    auto* local_title = new QLabel(QStringLiteral("本机电脑"), this);
    auto* remote_title = new QLabel(remote_name_, this);
    remote_title->setToolTip(QStringLiteral("远程设备：%1").arg(remote_name_));
    for (QLabel* title : {local_title, remote_title}) {
        QFont font = title->font();
        font.setPointSize(14);
        font.setBold(true);
        title->setFont(font);
        title->setWordWrap(true);
        title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    title_row->addWidget(local_title, 1);
    title_row->addWidget(remote_title, 1, Qt::AlignRight);
    root->addLayout(title_row);
    auto* actions = new QHBoxLayout;
    upload_button_ = new QPushButton(QStringLiteral("发送到远端　→"), this);
    download_button_ = new QPushButton(QStringLiteral("←　下载到本机"), this);
    upload_button_->setObjectName("accentButton");
    download_button_->setObjectName("accentButton");
    upload_button_->setEnabled(false);
    download_button_->setEnabled(false);
    actions->addStretch();
    actions->addWidget(upload_button_);
    actions->addWidget(download_button_);
    actions->addStretch();
    root->addLayout(actions);

    auto* panes = new QHBoxLayout;
    panes->setSpacing(16);
    auto make_pane = [this](const QString& title, QLineEdit*& path, QTableWidget*& table,
                            bool local) {
        auto* frame = new QFrame(this);
        frame->setStyleSheet(QStringLiteral("QFrame { background: white; border: 1px solid #dfe3e9; border-radius: 7px; }"));
        auto* pane = new QVBoxLayout(frame);
        pane->setContentsMargins(12, 10, 12, 10);
        pane->setSpacing(8);
        auto* label = new QLabel(title, frame);
        label->setStyleSheet(QStringLiteral("font-weight: 600; border: none;"));
        pane->addWidget(label);
        auto* path_row = new QHBoxLayout;
        auto* up = new QPushButton(QStringLiteral("上级"), frame);
        auto* refresh = new QPushButton(QStringLiteral("刷新"), frame);
        auto* mkdir = new QPushButton(QStringLiteral("新建文件夹"), frame);
        for (auto* button : {up, refresh, mkdir})
            button->setStyleSheet(QStringLiteral("padding: 5px 8px;"));
        path = new QLineEdit(frame);
        path_row->addWidget(up);
        path_row->addWidget(refresh);
        path_row->addWidget(mkdir);
        path_row->addStretch();
        pane->addLayout(path_row);
        pane->addWidget(path);
        table = makeFileTable(frame);
        pane->addWidget(table, 1);
        if (local) {
            QObject::connect(up, &QPushButton::clicked, this, [this] {
                const QString parent = QDir(local_path_->text()).absoluteFilePath(QStringLiteral(".."));
                local_path_->setText(QDir::cleanPath(parent));
                refreshLocal();
            });
            QObject::connect(refresh, &QPushButton::clicked, this, &FileTransferDialog::refreshLocal);
            QObject::connect(path, &QLineEdit::returnPressed, this, &FileTransferDialog::refreshLocal);
            QObject::connect(mkdir, &QPushButton::clicked, this, [this] {
                bool ok = false;
                const QString name = QInputDialog::getText(this, QStringLiteral("新建文件夹"),
                    QStringLiteral("文件夹名称"), QLineEdit::Normal, QString(), &ok).trimmed();
                if (ok && !name.isEmpty() && QDir(local_path_->text()).mkdir(name)) refreshLocal();
            });
            QObject::connect(table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
                const auto* item = local_table_->item(row, 0);
                if (!item || !item->data(Qt::UserRole + 1).toBool()) return;
                local_path_->setText(item->data(Qt::UserRole).toString());
                refreshLocal();
            });
        } else {
            QObject::connect(up, &QPushButton::clicked, this, [this] {
                const QString current = remote_path_->text();
                const QString parent = QFileInfo(current).absolutePath();
                requestRemoteList(parent == current ? QString() : parent);
            });
            QObject::connect(refresh, &QPushButton::clicked, this, [this] {
                requestRemoteList(remote_path_->text());
            });
            QObject::connect(path, &QLineEdit::returnPressed, this, [this] {
                requestRemoteList(remote_path_->text());
            });
            QObject::connect(mkdir, &QPushButton::clicked, this, [this] {
                bool ok = false;
                const QString name = QInputDialog::getText(this, QStringLiteral("新建远程文件夹"),
                    QStringLiteral("文件夹名称"), QLineEdit::Normal, QString(), &ok).trimmed();
                if (ok && !name.isEmpty() && controller_)
                    controller_->createRemoteDirectory(remote_path_->text(), name);
            });
            QObject::connect(table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
                const auto* item = remote_table_->item(row, 0);
                if (!item || !item->data(Qt::UserRole + 1).toBool()) return;
                requestRemoteList(item->data(Qt::UserRole).toString());
            });
        }
        return frame;
    };
    panes->addWidget(make_pane(QStringLiteral("本地文件"), local_path_, local_table_, true), 1);
    panes->addWidget(make_pane(QStringLiteral("远程文件"), remote_path_, remote_table_, false), 1);
    root->addLayout(panes, 4);

    auto* transfer_frame = new QFrame(this);
    transfer_frame->setStyleSheet(QStringLiteral("QFrame { background: white; border: 1px solid #dfe3e9; border-radius: 7px; }"));
    auto* transfer_layout = new QVBoxLayout(transfer_frame);
    transfer_layout->setContentsMargins(12, 9, 12, 12);
    auto* transfer_header = new QHBoxLayout;
    auto* transfer_title = new QLabel(QStringLiteral("传输列表"), transfer_frame);
    QFont font = transfer_title->font();
    font.setPointSize(14);
    font.setBold(true);
    transfer_title->setFont(font);
    transfer_header->addWidget(transfer_title);
    transfer_header->addStretch();
    auto* clear = new QPushButton(QStringLiteral("清除已完成"), transfer_frame);
    transfer_header->addWidget(clear);
    transfer_layout->addLayout(transfer_header);
    transfers_table_ = new QTableWidget(transfer_frame);
    transfers_table_->setColumnCount(5);
    transfers_table_->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("状态"),
        QStringLiteral("大小 / 进度"), QStringLiteral("发送路径"), QStringLiteral("接收路径")});
    transfers_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    transfers_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    transfers_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    transfers_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    transfers_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    transfers_table_->verticalHeader()->hide();
    transfers_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    transfers_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    transfers_table_->setShowGrid(false);
    transfer_layout->addWidget(transfers_table_);
    root->addWidget(transfer_frame, 1);

    connect(upload_button_, &QPushButton::clicked, this, &FileTransferDialog::uploadSelected);
    connect(download_button_, &QPushButton::clicked, this, &FileTransferDialog::downloadSelected);
    connect(local_table_, &QTableWidget::itemSelectionChanged, this,
            &FileTransferDialog::updateTransferButtons);
    connect(remote_table_, &QTableWidget::itemSelectionChanged, this,
            &FileTransferDialog::updateTransferButtons);
    connect(clear, &QPushButton::clicked, this, [this] {
        for (int row = transfers_table_->rowCount() - 1; row >= 0; --row) {
            const QString status = transfers_table_->item(row, 1)->text();
            if (status == QStringLiteral("已完成") || status == QStringLiteral("传输失败") ||
                status == QStringLiteral("远端拒绝接收")) {
                const QString id = transfers_table_->item(row, 0)->data(Qt::UserRole).toString();
                transfer_rows_.remove(id);
                transfers_table_->removeRow(row);
            }
        }
        transfer_rows_.clear();
        for (int row = 0; row < transfers_table_->rowCount(); ++row)
            transfer_rows_.insert(transfers_table_->item(row, 0)->data(Qt::UserRole).toString(), row);
    });
}

void FileTransferDialog::refreshLocal() {
    QString path = local_path_->text().trimmed();
    if (path.isEmpty()) path = QDir::homePath();
    QDir dir(path);
    if (!dir.exists()) return;
    local_table_->clearSelection();
    local_table_->clearContents();
    local_path_->setText(dir.absolutePath());
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                                     QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    local_table_->setRowCount(entries.size());
    for (int row = 0; row < entries.size(); ++row) {
        const QFileInfo& info = entries.at(row);
        auto* name = new QTableWidgetItem(fileIcons().icon(info), info.fileName());
        name->setData(Qt::UserRole, info.absoluteFilePath());
        name->setData(Qt::UserRole + 1, info.isDir());
        name->setData(Qt::UserRole + 2, static_cast<qlonglong>(info.isDir() ? 0 : info.size()));
        local_table_->setItem(row, 0, name);
        local_table_->setItem(row, 1, new QTableWidgetItem(info.lastModified().toString(QStringLiteral("yyyy/MM/dd HH:mm"))));
        local_table_->setItem(row, 2, new QTableWidgetItem(info.isDir() ? QStringLiteral("文件夹") : info.suffix()));
        local_table_->setItem(row, 3, new QTableWidgetItem(info.isDir() ? QStringLiteral("—") : displaySize(info.size())));
    }
    updateTransferButtons();
}

void FileTransferDialog::requestRemoteList(const QString& path) {
    if (!controller_) return;
    requested_remote_path_ = path;
    remote_table_->clearSelection();
    updateTransferButtons();
    controller_->requestRemoteFileList(path);
}

void FileTransferDialog::showRemoteList(const QJsonObject& message) {
    remote_table_->clearSelection();
    remote_table_->clearContents();
    updateTransferButtons();
    const QString path = message.value(QStringLiteral("path")).toString();
    if (message.contains(QStringLiteral("error"))) {
        remote_path_->setText(path);
        remote_table_->setRowCount(0);
        QMessageBox::warning(this, QStringLiteral("无法打开远程目录"),
                             message.value(QStringLiteral("error")).toString());
        return;
    }
    remote_path_->setText(path);
    const QJsonArray entries = message.value(QStringLiteral("entries")).toArray();
    remote_table_->setRowCount(entries.size());
    for (int row = 0; row < entries.size(); ++row) {
        const QJsonObject entry = entries.at(row).toObject();
        const bool directory = entry.value(QStringLiteral("directory")).toBool();
        auto* name = new QTableWidgetItem(fileIcons().icon(directory ? QFileIconProvider::Folder
                                                                      : QFileIconProvider::File),
                                          entry.value(QStringLiteral("name")).toString());
        name->setData(Qt::UserRole, entry.value(QStringLiteral("path")).toString());
        name->setData(Qt::UserRole + 1, directory);
        name->setData(Qt::UserRole + 2, static_cast<qlonglong>(entry.value(QStringLiteral("size")).toDouble()));
        remote_table_->setItem(row, 0, name);
        remote_table_->setItem(row, 1, new QTableWidgetItem(
            QDateTime::fromString(entry.value(QStringLiteral("modified")).toString(), Qt::ISODate)
                .toString(QStringLiteral("yyyy/MM/dd HH:mm"))));
        remote_table_->setItem(row, 2, new QTableWidgetItem(directory ? QStringLiteral("文件夹")
            : QFileInfo(entry.value(QStringLiteral("name")).toString()).suffix()));
        remote_table_->setItem(row, 3, new QTableWidgetItem(directory ? QStringLiteral("—")
            : displaySize(static_cast<qint64>(entry.value(QStringLiteral("size")).toDouble()))));
    }
    updateTransferButtons();
}

void FileTransferDialog::updateTransferButtons() {
    upload_button_->setEnabled(controller_ && selectedFile(local_table_));
    download_button_->setEnabled(controller_ && selectedFile(remote_table_));
}

void FileTransferDialog::uploadSelected() {
    if (!controller_) return;
    const QTableWidgetItem* item = selectedFile(local_table_);
    if (!item) return;
    const QString path = item->data(Qt::UserRole).toString();
    const QFileInfo info(path);
    const QString id = controller_->offerFileUpload(path);
    if (!id.isEmpty()) addTransferRow(id, info.fileName(), info.size(), path,
                                      QStringLiteral("远程设备"), QStringLiteral("等待远端接受"));
}

void FileTransferDialog::downloadSelected() {
    if (!controller_) return;
    const QTableWidgetItem* item = selectedFile(remote_table_);
    if (!item) return;
    const QString path = item->data(Qt::UserRole).toString();
    const QString local = QFileDialog::getSaveFileName(this, QStringLiteral("保存到本机"),
        QDir(QDir::homePath()).filePath(item->text()));
    if (local.isEmpty()) return;
    const QString id = controller_->requestRemoteFileDownload(path, local);
    if (!id.isEmpty()) addTransferRow(id, QFileInfo(path).fileName(),
        item->data(Qt::UserRole + 2).toLongLong(), path, local,
        QStringLiteral("准备下载"));
}

void FileTransferDialog::addTransferRow(const QString& id, const QString& name, qint64 size,
                                        const QString& source, const QString& destination,
                                        const QString& state) {
    if (transfer_rows_.contains(id)) {
        const int existing = transfer_rows_.value(id);
        if (existing >= 0 && existing < transfers_table_->rowCount()) {
            transfers_table_->item(existing, 0)->setText(name);
            transfers_table_->item(existing, 2)->setText(displaySize(size));
            transfers_table_->item(existing, 3)->setText(source);
            transfers_table_->item(existing, 4)->setText(destination);
        }
        return;
    }
    const int row = transfers_table_->rowCount();
    transfers_table_->insertRow(row);
    auto* item = new QTableWidgetItem(name);
    item->setData(Qt::UserRole, id);
    transfers_table_->setItem(row, 0, item);
    transfers_table_->setItem(row, 1, new QTableWidgetItem(state));
    transfers_table_->setItem(row, 2, new QTableWidgetItem(displaySize(size)));
    transfers_table_->setItem(row, 3, new QTableWidgetItem(source));
    transfers_table_->setItem(row, 4, new QTableWidgetItem(destination));
    transfer_rows_.insert(id, row);
}

void FileTransferDialog::updateTransfer(const QString& id, const QString& name,
                                       qint64 transferred, qint64 total,
                                       const QString& state) {
    if (id.isEmpty()) return;
    int row = transfer_rows_.value(id, -1);
    if (row < 0) {
        addTransferRow(id, name, total, QStringLiteral("远程设备"), QStringLiteral("本机"), state);
        row = transfer_rows_.value(id, -1);
    }
    if (row < 0) return;
    transfers_table_->item(row, 1)->setText(state);
    transfers_table_->item(row, 2)->setText(total > 0
        ? QStringLiteral("%1 / %2").arg(displaySize(transferred), displaySize(total))
        : displaySize(transferred));
}

}  // namespace pxc::gui
