#include "settings_page.h"
#include "server_status.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "client_controller.h"
#include "theme.h"
#include "toggle_switch.h"

namespace pxc::gui {
using namespace theme;
namespace {

constexpr const char* kTabNames[4] = {"常规", "安全", "键盘", "网络"};

// 功能快捷键的默认值（与 UU 远程参考图一致；空串 = 未设置）
struct DefaultShortcut {
    const char* key;       // QSettings 键
    const char* title;     // 行标题
    const char* def;       // 默认按键
    const char* hint;      // 无默认键时给占位提示
};
const DefaultShortcut kShortcuts[] = {
    {"unlock_mouse",       "使用被控端鼠标/解锁鼠标",        "Ctrl+Alt+Return", ""},
    {"privacy_off",        "被控时解除防窥模式",             "Ctrl+Alt+P", "需要被控端支持防窥"},
    {"screen_prev",        "切换上一个屏幕",                 "Ctrl+Alt+Left", ""},
    {"screen_next",        "切换下一个屏幕",                 "Ctrl+Alt+Right", ""},
    {"fullscreen",         "远控窗口全屏/窗口化",            "Ctrl+Alt+F", ""},
    {"fab_toggle",         "显示/隐藏悬浮球",                "Ctrl+Alt+G", ""},
    {"mute",               "控制端静音/取消静音",            "", "音频功能开发中"},
    {"boss_key",           "老板键（隐藏/显示所有客户端窗口）", "", "需要全局热键支持"},
    {"send_ctrl_alt_del",  "发送 Ctrl+Alt+Delete 组合键",    "", "需要被控端系统服务支持"},
    {"open_file_transfer", "打开文件传输功能",               "", "文件传输开发中"},
    {"privacy_screen",     "开启/关闭隐私屏保",              "", "需要被控端支持"},
    {"exit_remote",        "立即退出远控",                   "", ""},
};

QString shortcut_value(const QString& key, const QString& def) {
    QSettings settings;
    return settings.value(QStringLiteral("kb/") + key, def).toString();
}

void apply_autostart(bool on) {
    // Windows：写 HKCU Run 键；Linux：仅保存设置（正式版再补 .desktop）
    QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    if (on) {
        const QString exe = QCoreApplication::applicationFilePath();
        run.setValue(QStringLiteral("PixelConnection"),
                     QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(exe)));
    } else {
        run.remove(QStringLiteral("PixelConnection"));
    }
}

void apply_keep_awake(bool on) {
#if defined(_WIN32)
    // 进程存活期间持续阻止系统休眠（退出程序后 Windows 自动恢复）
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
#else
    (void)on;
#endif
}

}  // namespace

SettingsPage::SettingsPage(ClientController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    if (controller_) {
        // 加入设备列表成功后刷新本机信息卡
        connect(controller_, &ClientController::deviceEnrolled, this,
                &SettingsPage::refresh);
        connect(controller_, &ClientController::connectionKeyChanged, this,
                &SettingsPage::refresh);
    }
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(34, 26, 34, 18);
    root->setSpacing(14);

    auto* title = new QLabel(QStringLiteral("设置"), this);
    QFont title_font = title->font();
    title_font.setPointSize(20);
    title_font.setBold(true);
    title->setFont(title_font);
    auto* header = new QHBoxLayout;
    header->addWidget(title);
    header->addStretch();
    auto* logout = new QPushButton(QStringLiteral("退出登录"), this);
    logout->setObjectName("settingsLogoutButton");
    logout->setCursor(Qt::PointingHandCursor);
    logout->setMinimumSize(112, 36);
    logout->setStyleSheet(QStringLiteral(
        "QPushButton#settingsLogoutButton { background: #e5484d; color: white;"
        " border: 1px solid #e5484d; border-radius: 8px; padding: 8px 18px;"
        " font-size: 14px; font-weight: 600; }"
        "QPushButton#settingsLogoutButton:hover { background: #d93d42; border-color: #d93d42; }"
        "QPushButton#settingsLogoutButton:pressed { background: #c53237; border-color: #c53237; }"));
    connect(logout, &QPushButton::clicked, this, &SettingsPage::logoutRequested);
    header->addWidget(logout, 0, Qt::AlignVCenter);
    root->addLayout(header);

    // ---- 标签行 ----
    auto* tab_row = new QHBoxLayout;
    tab_row->setSpacing(26);
    for (int i = 0; i < 4; ++i) {
        auto* tab = new QPushButton(QString::fromUtf8(kTabNames[i]), this);
        tab->setCursor(Qt::PointingHandCursor);
        tab->setFixedHeight(34);
        tab->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; font-size: 14px;"
            " color: %1; padding: 2px 0; }")
            .arg(kTextSecondary));
        connect(tab, &QPushButton::clicked, this, [this, i] {
            active_tab_ = i;
            if (tab_content_) tab_content_->setCurrentIndex(i);
            for (int j = 0; j < 4; ++j) {
                if (tabs_[j]) {
                    tabs_[j]->setStyleSheet(QStringLiteral(
                        "QPushButton { background: transparent; border: none; font-size: 14px;"
                        " color: %1; font-weight: %2; padding: 2px 0; }")
                        .arg(j == i ? kTextPrimary : kTextSecondary,
                             j == i ? "600" : "400"));
                }
                if (tab_underlines_[j]) {
                    tab_underlines_[j]->setVisible(j == i);
                }
            }
            if (i == 1 || i == 3) refresh();  // 安全/网络页有动态内容
        });
        tabs_[i] = tab;

        auto* cell = new QWidget(this);
        auto* cell_layout = new QVBoxLayout(cell);
        cell_layout->setContentsMargins(0, 0, 0, 0);
        cell_layout->setSpacing(3);
        cell_layout->addWidget(tab);
        auto* underline = new QFrame(cell);
        underline->setFixedHeight(3);
        underline->setFixedWidth(28);
        underline->setStyleSheet(
            QStringLiteral("background: %1; border-radius: 1.5px;").arg(kAccent));
        tab_underlines_[i] = underline;
        cell_layout->addWidget(underline, 0, Qt::AlignHCenter);
        tab_row->addWidget(cell);
    }
    tab_row->addStretch();
    root->addLayout(tab_row);

    // ---- 内容 ----
    pages_[0] = buildGeneralTab();
    pages_[1] = buildSecurityTab();
    pages_[2] = buildKeyboardTab();
    pages_[3] = buildNetworkTab();
    // Keep content hosts distinct from their scroll containers so refresh can
    // insert cards into the correct layout without leaving floating widgets.
    tab_content_ = new QStackedWidget(this);
    tab_content_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    root->addWidget(tab_content_, 1);
    for (int i = 0; i < 4; ++i) {
        auto* scroll = new QScrollArea(tab_content_);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
        pages_[i]->layout()->setSizeConstraint(QLayout::SetMinimumSize);
        scroll->setWidget(pages_[i]);
        tab_content_->addWidget(scroll);
    }
    // 激活第一个标签（复用上面的互斥逻辑，并顺带刷新动态内容）
    tabs_[0]->click();
}

QFrame* SettingsPage::make_card() {
    auto* card = new QFrame(this);
    card->setObjectName("settingsCard");
    card->setStyleSheet(QStringLiteral(
        "#settingsCard { background: white; border-radius: 12px;"
        " border: 1px solid %1; }").arg(kDivider));
    return card;
}

QWidget* SettingsPage::make_divider() {
    auto* divider = new QFrame(this);
    divider->setFixedHeight(1);
    divider->setStyleSheet(QStringLiteral("background: %1; border: none;").arg(kDivider));
    return divider;
}

QWidget* SettingsPage::make_row(const QPixmap& icon, const QString& title,
                                const QString& subtitle) {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(12);

    if (!icon.isNull()) {
        auto* icon_label = new QLabel(row);
        icon_label->setPixmap(icon);
        icon_label->setFixedSize(26, 26);
        icon_label->setAlignment(Qt::AlignCenter);
        layout->addWidget(icon_label);
    }

    auto* text_host = new QWidget(row);
    auto* text_layout = new QVBoxLayout(text_host);
    text_layout->setContentsMargins(0, 0, 0, 0);
    text_layout->setSpacing(3);
    auto* title_label = new QLabel(title, text_host);
    title_label->setWordWrap(true);
    title_label->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13.5px; background: transparent;")
            .arg(kTextPrimary));
    text_layout->addWidget(title_label);
    if (!subtitle.isEmpty()) {
        auto* subtitle_label = new QLabel(subtitle, text_host);
        subtitle_label->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11.5px; background: transparent;")
                .arg(kTextSecondary));
        subtitle_label->setWordWrap(true);
        text_layout->addWidget(subtitle_label);
    }
    layout->addWidget(text_host, 1);
    return row;
}

QWidget* SettingsPage::buildGeneralTab() {
    auto* host = new QWidget(this);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(14);

    // 开机自动启动
    {
        auto* card = make_card();
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(0, 0, 0, 0);
        card_layout->setSpacing(0);

        auto* row = make_row(icon_autostart(22, color(kTextPrimary)),
                             QStringLiteral("开机自动启动"));
        auto* toggle = new ToggleSwitch(card);
        QSettings settings;
        toggle->set_on(settings.value(QStringLiteral("general/autostart"), false).toBool());
        connect(toggle, &ToggleSwitch::toggled, this, [](bool on) {
            QSettings s;
            s.setValue(QStringLiteral("general/autostart"), on);
            apply_autostart(on);
        });
        row->layout()->addWidget(toggle);
        card_layout->addWidget(row);

        auto* row2 = make_row(QPixmap(), QStringLiteral("防止电脑休眠"),
                              QStringLiteral("休眠将导致电脑无法远程控制（强烈推荐开启）"));
        auto* toggle2 = new ToggleSwitch(card);
        toggle2->set_on(QSettings().value(QStringLiteral("general/keep_awake"), true).toBool());
        connect(toggle2, &ToggleSwitch::toggled, this, [](bool on) {
            QSettings s;
            s.setValue(QStringLiteral("general/keep_awake"), on);
            apply_keep_awake(on);
        });
        row2->layout()->addWidget(toggle2);
        card_layout->addWidget(make_divider());
        card_layout->addWidget(row2);

        layout->addWidget(card);
    }
    apply_keep_awake(QSettings().value(QStringLiteral("general/keep_awake"), true).toBool());

    // 自动更新 / 拖拽浮窗
    {
        auto* card = make_card();
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(0, 0, 0, 0);

        auto* row = make_row(icon_sync(22, color(kTextPrimary)),
                             QStringLiteral("自动更新"),
                             QStringLiteral("开启后会在电脑闲时自动更新，避免打扰（需要更新服务）"));
        auto* toggle = new ToggleSwitch(card);
        toggle->set_on(QSettings().value(QStringLiteral("general/auto_update"), false).toBool());
        connect(toggle, &ToggleSwitch::toggled, this, [](bool on) {
            QSettings().setValue(QStringLiteral("general/auto_update"), on);
        });
        row->layout()->addWidget(toggle);
        card_layout->addWidget(row);

        card_layout->addWidget(make_divider());

        auto* row2 = make_row(icon_eye(22, color(kTextPrimary)),
                              QStringLiteral("被控时拖拽文件显示发送浮窗"),
                              QStringLiteral("开启后，可向主控端拖拽发送文件（文件传输开发中）"));
        auto* toggle2 = new ToggleSwitch(card);
        toggle2->set_on(QSettings().value(QStringLiteral("general/drag_send"), true).toBool());
        connect(toggle2, &ToggleSwitch::toggled, this, [](bool on) {
            QSettings().setValue(QStringLiteral("general/drag_send"), on);
        });
        row2->layout()->addWidget(toggle2);
        card_layout->addWidget(row2);

        layout->addWidget(card);
    }

    // 接收文件路径 + 关闭窗口时
    {
        auto* card = make_card();
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(0, 0, 0, 0);

        auto* row = make_row(icon_folder_down(22, color(kTextPrimary)),
                             QStringLiteral("接收文件保存路径"));
        auto* path_label = new QLabel(
            QSettings().value(QStringLiteral("general/download_dir"),
                              QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
                .toString(),
            card);
        path_label->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; background: transparent;")
                .arg(kTextSecondary));
        path_label->setWordWrap(true);
        path_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto* pick = new QPushButton(QStringLiteral("浏览"), card);
        pick->setCursor(Qt::PointingHandCursor);
        connect(pick, &QPushButton::clicked, this, [this, path_label] {
            const QString dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("选择接收文件保存路径"),
                path_label->text());
            if (!dir.isEmpty()) {
                QSettings().setValue(QStringLiteral("general/download_dir"), dir);
                path_label->setText(dir);
            }
        });
        auto* right = new QHBoxLayout;
        right->setSpacing(10);
        right->addWidget(path_label);
        right->addWidget(pick);
        row->layout()->addItem(right);
        card_layout->addWidget(row);

        card_layout->addWidget(make_divider());

        auto* row2 = make_row(icon_window(22, color(kTextPrimary)),
                              QStringLiteral("关闭窗口时"));
        auto* combo = new QComboBox(card);
        combo->addItem(QStringLiteral("退出程序"));
        combo->addItem(QStringLiteral("最小化（托盘开发中）"));
        combo->setCurrentIndex(QSettings().value(QStringLiteral("general/close_mode"), 0).toInt());
        connect(combo, &QComboBox::currentIndexChanged, this, [](int index) {
            QSettings().setValue(QStringLiteral("general/close_mode"), index);
        });
        row2->layout()->addWidget(combo);
        card_layout->addWidget(row2);

        layout->addWidget(card);
    }

    layout->addStretch();
    return host;
}

QWidget* SettingsPage::buildSecurityTab() {
    auto* host = new QWidget(this);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(14);

    // 本机信息（设备 ID + 连接密钥 + 加入设备列表）
    {
        auto* card = make_card();
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(0, 0, 0, 0);

        auto* row = make_row(icon_device(22, color(kTextPrimary)),
                             QStringLiteral("本机设备 ID"));
        auto* id_label = new QLabel(
            controller_ && !controller_->localDeviceId().isEmpty()
                ? controller_->localDeviceId()
                : QStringLiteral("尚未加入设备列表"),
            card);
        local_id_value_ = id_label;
        id_label->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12.5px; background: transparent;")
                .arg(kTextSecondary));
        row->layout()->addWidget(id_label);
        card_layout->addWidget(row);

        card_layout->addWidget(make_divider());

        auto* row2 = make_row(icon_shield(22, color(kTextPrimary)),
                              QStringLiteral("本机验证密码"),
                              QStringLiteral("主控端首次连接本机时输入；只显示在本机，不会上传"));
        auto* key_value = new QLabel(
            controller_ && !controller_->localConnectionKey().isEmpty()
                ? controller_->localConnectionKey()
                : QStringLiteral("（加入设备列表后生成）"),
            card);
        QFont key_font = key_value->font();
        key_font.setBold(true);
        key_font.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
        key_value->setFont(key_font);
        key_value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        key_value->setStyleSheet(
            QStringLiteral("color: %1; font-size: 15px; background: transparent;")
                .arg(kAccentDeep));
        auto* copy = new QPushButton(QStringLiteral("复制"), card);
        copy->setCursor(Qt::PointingHandCursor);
        copy->setFixedWidth(72);
        connect(copy, &QPushButton::clicked, this, [this] {
            if (controller_ && !controller_->localConnectionKey().isEmpty()) {
                QGuiApplication::clipboard()->setText(controller_->localConnectionKey());
            }
        });
        row2->layout()->addWidget(key_value);
        row2->layout()->addWidget(copy);
        card_layout->addWidget(row2);

        card_layout->addWidget(make_divider());

        auto* row3 = new QWidget(card);
        auto* row3_layout = new QHBoxLayout(row3);
        row3_layout->setContentsMargins(56, 12, 18, 14);
        auto* hint = new QLabel(
            controller_ && !controller_->localDeviceId().isEmpty()
                ? QStringLiteral("本机已加入设备列表")
                : QStringLiteral("登录后可把本机加入账号的设备列表"),
            row3);
        hint->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; background: transparent;")
                .arg(kTextSecondary));
        auto* enroll = new QPushButton(QStringLiteral("把本机加入设备列表"), row3);
        enroll->setObjectName("accentButton");
        enroll->setCursor(Qt::PointingHandCursor);
        enroll->setVisible(!controller_ || controller_->localDeviceId().isEmpty());
        connect(enroll, &QPushButton::clicked, this, [this] {
            if (controller_) controller_->addThisDevice();
        });
        row3_layout->addWidget(hint, 1);
        row3_layout->addWidget(enroll);
        card_layout->addWidget(row3);

        layout->addWidget(card);
    }

    // 允许被控 + 连接方式
    {
        auto* card = make_card();
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(0, 0, 0, 0);

        auto* row = make_row(icon_monitor_lock(22, color(kTextPrimary)),
                             QStringLiteral("允许同账号控制本设备"));
        auto* toggle = new ToggleSwitch(card);
        toggle->set_on(QSettings().value(QStringLiteral("security/allow_same_account"), true)
                           .toBool());
        connect(toggle, &ToggleSwitch::toggled, this, [](bool on) {
            QSettings().setValue(QStringLiteral("security/allow_same_account"), on);
        });
        row->layout()->addWidget(toggle);
        card_layout->addWidget(row);

        card_layout->addWidget(make_divider());

        auto* row2 = make_row(icon_shield(22, color(kTextPrimary)),
                              QStringLiteral("远程协助连接本设备的方式"),
                              QStringLiteral("通过本机【设备 ID】和【设备验证密码】即可发起远程协助"));
        auto* combo = new QComboBox(card);
        combo->addItem(QStringLiteral("验证码连接"));
        combo->setFixedWidth(140);
        row2->layout()->addWidget(combo);
        card_layout->addWidget(row2);

        card_layout->addWidget(make_divider());

        // 自定义验证码（连接密钥）
        auto* row3 = make_row(icon_shield(22, color(kTextPrimary)),
                              QStringLiteral("自定义验证码"),
                              QStringLiteral("主控端首次连接时输入；修改后其他设备需用新密码连接本机"));
        auto* modify = new QPushButton(QStringLiteral("修改"), card);
        modify->setCursor(Qt::PointingHandCursor);
        modify->setFixedWidth(88);
        connect(modify, &QPushButton::clicked, this, [this] {
            if (!controller_) return;
            bool ok = false;
            const QString key = QInputDialog::getText(
                this, QStringLiteral("修改自定义验证码"),
                QStringLiteral("请输入新的验证密码（5 位以上）\n"
                               "其他设备连接本机时需要输入它"),
                QLineEdit::Password, QString(), &ok);
            if (!ok) return;
            if (!controller_->setConnectionKey(key)) {
                QMessageBox::warning(this, QStringLiteral("修改失败"),
                                     QStringLiteral("验证密码至少需要 5 个字符"));
                return;
            }
            refresh();
        });
        row3->layout()->addWidget(modify);
        card_layout->addWidget(row3);

        layout->addWidget(card);
    }

    // 已保存的设备密钥
    saved_keys_host_ = buildSavedKeysCard();
    layout->addWidget(saved_keys_host_);

    layout->addStretch();
    return host;
}

QWidget* SettingsPage::buildSavedKeysCard() {
    auto* card = make_card();
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(0, 0, 0, 0);

    auto* header = make_row(icon_shield(22, color(kTextPrimary)),
                            QStringLiteral("已记住的设备密钥"),
                            QStringLiteral("首次连接某台设备时记住其密钥，之后连接不再询问；可在此清除"));
    card_layout->addWidget(header);

    const QStringList devices =
        controller_ ? controller_->savedKeyDevices() : QStringList();
    if (devices.isEmpty()) {
        auto* empty = new QLabel(QStringLiteral("　　暂无记住的设备密钥"), card);
        empty->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; padding: 0 0 14px 56px;")
                .arg(kTextSecondary));
        card_layout->addWidget(empty);
        return card;
    }

    for (int i = 0; i < devices.size(); ++i) {
        if (i > 0) card_layout->addWidget(make_divider());
        auto* row = new QWidget(card);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(56, 12, 18, 12);
        auto* name = new QLabel(devices.at(i), row);
        name->setStyleSheet(
            QStringLiteral("color: %1; font-size: 13px; background: transparent;")
                .arg(kTextPrimary));
        auto* forget = new QPushButton(QStringLiteral("忘记"), row);
        forget->setCursor(Qt::PointingHandCursor);
        forget->setFixedWidth(72);
        const QString device_id = devices.at(i);
        connect(forget, &QPushButton::clicked, this, [this, device_id] {
            if (controller_) controller_->forgetSavedKey(device_id);
            rebuildSavedKeys();
        });
        row_layout->addWidget(name, 1);
        row_layout->addWidget(forget);
        card_layout->addWidget(row);
    }
    return card;
}

void SettingsPage::rebuildSavedKeys() {
    if (!saved_keys_host_) return;
    saved_keys_host_->setParent(nullptr);
    delete saved_keys_host_;
    saved_keys_host_ = buildSavedKeysCard();
    if (pages_[1]) {
        auto* layout = qobject_cast<QVBoxLayout*>(pages_[1]->layout());
        if (layout) layout->insertWidget(layout->count() - 1, saved_keys_host_);
    }
}

QWidget* SettingsPage::buildKeyboardTab() {
    auto* host = new QWidget(this);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(14);

    // ---- 仅控制端响应的快捷键 ----
    {
        auto* card = make_card();
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(0, 0, 0, 0);

        auto* header = make_row(icon_keyboard(22, color(kTextPrimary)),
                                QStringLiteral("仅控制端响应的快捷键"),
                                QStringLiteral("远控时按下以下快捷键，仅在控制端本地响应，不会发送到被控端。"));
        auto* add = new QPushButton(QStringLiteral("添加"), card);
        add->setCursor(Qt::PointingHandCursor);
        add->setFixedWidth(88);
        connect(add, &QPushButton::clicked, this, [this] {
            QSettings settings;
            QStringList list = settings.value(QStringLiteral("kb/custom")).toStringList();
            list.append(QString());
            settings.setValue(QStringLiteral("kb/custom"), list);
            refresh();
        });
        header->layout()->addWidget(add);
        card_layout->addWidget(header);

        QStringList custom = QSettings().value(QStringLiteral("kb/custom")).toStringList();
        if (custom.isEmpty()) {
            auto* empty = new QLabel(QStringLiteral("　　当前暂无配置"), card);
            empty->setStyleSheet(
                QStringLiteral("color: %1; font-size: 12px; padding: 0 0 14px 56px;")
                    .arg(kTextSecondary));
            card_layout->addWidget(empty);
        } else {
            for (int i = 0; i < custom.size(); ++i) {
                if (i > 0) card_layout->addWidget(make_divider());
                auto* row = new QWidget(card);
                auto* row_layout = new QHBoxLayout(row);
                row_layout->setContentsMargins(56, 10, 18, 10);
                auto* edit = new QKeySequenceEdit(
                    QKeySequence(custom.at(i)), row);
                edit->setStyleSheet(QStringLiteral(
                    "QKeySequenceEdit { background: #eef3ff; color: %1; border: none;"
                    " border-radius: 6px; padding: 5px 12px; font-size: 12px; min-width: 120px; }")
                    .arg(kAccentDeep));
                const int index = i;
                connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                        [index](const QKeySequence& seq) {
                            QSettings settings;
                            QStringList list =
                                settings.value(QStringLiteral("kb/custom")).toStringList();
                            if (index < list.size()) {
                                list[index] = seq.toString();
                                settings.setValue(QStringLiteral("kb/custom"), list);
                            }
                        });
                auto* del = new QPushButton(QStringLiteral("删除"), row);
                del->setCursor(Qt::PointingHandCursor);
                del->setFixedWidth(72);
                connect(del, &QPushButton::clicked, this, [this, index] {
                    QSettings settings;
                    QStringList list =
                        settings.value(QStringLiteral("kb/custom")).toStringList();
                    if (index < list.size()) list.removeAt(index);
                    settings.setValue(QStringLiteral("kb/custom"), list);
                    refresh();
                });
                row_layout->addWidget(edit, 1);
                row_layout->addWidget(del);
                card_layout->addWidget(row);
            }
        }
        layout->addWidget(card);
    }

    // ---- 功能快捷键 ----
    {
        auto* card = make_card();
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(0, 0, 0, 0);

        auto* header = make_row(icon_keyboard(22, color(kTextPrimary)),
                                QStringLiteral("功能快捷键"),
                                QStringLiteral("按下以下快捷键时，仅在本地触发对应功能，不会发送到被控端。"));
        auto* reset = new QPushButton(QStringLiteral("还原默认按键"), card);
        reset->setCursor(Qt::PointingHandCursor);
        connect(reset, &QPushButton::clicked, this, [this] {
            QSettings settings;
            for (const auto& sc : kShortcuts) {
                settings.setValue(QStringLiteral("kb/") + sc.key,
                                  QString::fromUtf8(sc.def));
            }
            settings.remove(QStringLiteral("kb/custom"));
            refresh();
        });
        header->layout()->addWidget(reset);
        card_layout->addWidget(header);

        for (int i = 0; i < static_cast<int>(sizeof(kShortcuts) / sizeof(kShortcuts[0]));
             ++i) {
            const DefaultShortcut& sc = kShortcuts[i];
            if (i > 0) card_layout->addWidget(make_divider());
            auto* row = new QWidget(card);
            auto* row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(56, 12, 18, 12);
            auto* title = new QLabel(QString::fromUtf8(sc.title), row);
            title->setStyleSheet(
                QStringLiteral("color: %1; font-size: 13px; background: transparent;")
                    .arg(kTextPrimary));
            auto* edit = new QKeySequenceEdit(
                QKeySequence(shortcut_value(QString::fromUtf8(sc.key),
                                            QString::fromUtf8(sc.def))),
                row);
            edit->setStyleSheet(QStringLiteral(
                "QKeySequenceEdit { background: #eef3ff; color: %1; border: none;"
                " border-radius: 6px; padding: 5px 12px; font-size: 12px; min-width: 130px; }"
                "QKeySequenceEdit:focus { background: #e0eaff; }")
                .arg(kAccentDeep));
            const QString key_name = QString::fromUtf8(sc.key);
            connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                    [key_name](const QKeySequence& seq) {
                        QSettings().setValue(QStringLiteral("kb/") + key_name,
                                             seq.toString());
                    });
            row_layout->addWidget(title, 1);
            row_layout->addWidget(edit);
            card_layout->addWidget(row);
        }
        layout->addWidget(card);
    }

    // ---- macOS 按键映射（预留展示）----
    {
        auto* card = make_card();
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(0, 0, 0, 0);

        auto* header = make_row(icon_keyboard(22, color(kTextPrimary)),
                                QStringLiteral("远控 macOS 按键映射"),
                                QStringLiteral("控制 macOS 设备时的按键对应关系（预留）"));
        card_layout->addWidget(header);

        const std::initializer_list<std::pair<const char*, const char*>> maps = {
            {"Win 键", "⌘ Command"},
            {"Alt 键", "⌥ Option"},
            {"Ctrl 键", "⌃ Control"},
        };
        bool first = true;
        for (const auto& [name, value] : maps) {
            if (!first) card_layout->addWidget(make_divider());
            auto* row = new QWidget(card);
            auto* row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(56, 12, 18, 12);
            auto* title = new QLabel(QString::fromUtf8(name), row);
            title->setStyleSheet(
                QStringLiteral("color: %1; font-size: 13px; background: transparent;")
                    .arg(kTextPrimary));
            auto* combo = new QComboBox(row);
            combo->addItem(QString::fromUtf8(value));
            combo->setEnabled(false);
            combo->setToolTip(QStringLiteral("控制 macOS 设备时生效（预留）"));
            row_layout->addWidget(title, 1);
            row_layout->addWidget(combo);
            card_layout->addWidget(row);
            first = false;
        }
        layout->addWidget(card);
    }

    layout->addStretch();
    return host;
}

QWidget* SettingsPage::buildNetworkTab() {
    auto* host = new QWidget(this);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(14);

    {
        auto* card = make_card();
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(0, 0, 0, 0);

        auto* row = make_row(icon_network(22, color(kTextPrimary)),
                             QStringLiteral("账号 API"), QStringLiteral("服务器地址，可供其他设备登录"));
        api_value_ = new QLabel(QStringLiteral("-"), card);
        api_value_->setObjectName(QStringLiteral("pxc-share-api"));
        qobject_cast<QLabel*>(api_value_)->setTextInteractionFlags(Qt::TextSelectableByMouse);
        qobject_cast<QLabel*>(api_value_)->setWordWrap(true);
        api_value_->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12.5px; background: transparent;")
                .arg(kTextSecondary));
        row->layout()->addWidget(api_value_);
        card_layout->addWidget(row);

        card_layout->addWidget(make_divider());

        auto* row2 = make_row(icon_network(22, color(kTextPrimary)),
                              QStringLiteral("信令服务器"));
        ws_value_ = new QLabel(QStringLiteral("-"), card);
        ws_value_->setObjectName(QStringLiteral("pxc-share-ws"));
        qobject_cast<QLabel*>(ws_value_)->setTextInteractionFlags(Qt::TextSelectableByMouse);
        qobject_cast<QLabel*>(ws_value_)->setWordWrap(true);
        ws_value_->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12.5px; background: transparent;")
                .arg(kTextSecondary));
        row2->layout()->addWidget(ws_value_);
        card_layout->addWidget(row2);

        auto* actions = new QHBoxLayout;
        actions->setContentsMargins(20, 8, 20, 0);
        server_endpoints_ = new QComboBox(card);
        server_endpoints_->setObjectName(QStringLiteral("pxc-server-endpoints"));
        server_endpoints_->hide();
        actions->addWidget(server_endpoints_, 1);
        auto* refresh = new QPushButton(QStringLiteral("刷新地址"), card);
        connect(refresh, &QPushButton::clicked, this, &SettingsPage::refresh);
        actions->addWidget(refresh);
        copy_server_ = new QPushButton(QStringLiteral("复制服务器地址"), card);
        copy_server_->setObjectName(QStringLiteral("pxc-copy-server-address"));
        copy_server_->setEnabled(false);
        actions->addWidget(copy_server_);
        card_layout->addLayout(actions);
        connect(server_endpoints_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
            const auto pair = server_endpoints_->itemData(index).toStringList();
            if (pair.size() != 2) return;
            qobject_cast<QLabel*>(api_value_)->setText(pair.at(0));
            qobject_cast<QLabel*>(ws_value_)->setText(pair.at(1));
        });
        connect(copy_server_, &QPushButton::clicked, this, [this] {
            QGuiApplication::clipboard()->setText(qobject_cast<QLabel*>(api_value_)->text() + QLatin1Char('\n') +
                                                 qobject_cast<QLabel*>(ws_value_)->text());
            copy_server_->setText(QStringLiteral("已复制"));
        });

        auto* source = new QLabel(card);
        source->setObjectName(QStringLiteral("pxc-server-origin"));
        source->setWordWrap(true);
        source->setTextInteractionFlags(Qt::TextSelectableByMouse);
        source->setContentsMargins(20, 8, 20, 12);
        card_layout->addWidget(source);

        layout->addWidget(card);
    }

    {
        auto* card = make_card();
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(0, 0, 0, 0);

        auto* row = make_row(icon_network(22, color(kTextPrimary)),
                             QStringLiteral("STUN 服务器"),
                             QStringLiteral("用于 P2P 打洞；画面与键鼠数据永远直连，不经过服务器"));
        card_layout->addWidget(row);
        auto* info = new QLabel(QStringLiteral("　　stun.l.google.com:19302\n"
                                               "　　stun.cloudflare.com:3478"), card);
        info->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; padding: 0 0 14px 0;")
                .arg(kTextSecondary));
        card_layout->addWidget(info);
        layout->addWidget(card);
    }

    {
        auto* card = make_card();
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(0, 0, 0, 0);

        auto* row = make_row(icon_network(22, color(kTextPrimary)),
                             QStringLiteral("TURN 中继（可选）"),
                             QStringLiteral("直连失败时由中继服务器转发；直连成功时数据不经过它。"
                                            "格式 turn://主机:端口，账号密码为中继服务的凭据"));
        card_layout->addWidget(row);

        const std::initializer_list<std::tuple<const char*, const char*>> turn_fields = {
            {"turn_url",  "中继地址"},
            {"turn_user", "账号"},
            {"turn_pass", "密码"},
        };
        bool first_field = true;
        for (const auto& [key, label] : turn_fields) {
            if (!first_field) card_layout->addWidget(make_divider());
            auto* row_field = new QWidget(card);
            auto* field_layout = new QHBoxLayout(row_field);
            field_layout->setContentsMargins(56, 10, 18, 10);
            auto* name = new QLabel(QString::fromUtf8(label), row_field);
            name->setStyleSheet(
                QStringLiteral("color: %1; font-size: 12.5px; background: transparent;")
                    .arg(kTextSecondary));
            auto* edit = new QLineEdit(
                QSettings().value(QStringLiteral("net/") + QString::fromUtf8(key))
                    .toString(),
                row_field);
            edit->setFixedWidth(280);
            if (QString::fromUtf8(key) == QStringLiteral("turn_pass")) {
                edit->setEchoMode(QLineEdit::Password);
            }
            const QString key_name = QString::fromUtf8(key);
            connect(edit, &QLineEdit::textChanged, this, [key_name](const QString& text) {
                QSettings().setValue(QStringLiteral("net/") + key_name, text.trimmed());
            });
            field_layout->addWidget(name);
            field_layout->addStretch();
            field_layout->addWidget(edit);
            card_layout->addWidget(row_field);
            first_field = false;
        }

        layout->addWidget(card);
    }

    layout->addStretch();
    return host;
}

void SettingsPage::refresh() {
    if (auto* source = findChild<QLabel*>(QStringLiteral("pxc-server-origin"))) {
        const QString api = QSettings().value(QStringLiteral("net/api_url")).toString();
        const QString ws = QSettings().value(QStringLiteral("net/ws_url")).toString();
        const auto generation = ++server_probe_generation_;
        source->setText(QStringLiteral("正在确认服务器来源…"));
        qobject_cast<QLabel*>(api_value_)->setText(QStringLiteral("正在获取服务器地址…"));
        qobject_cast<QLabel*>(ws_value_)->setText(QStringLiteral("正在获取服务器地址…"));
        copy_server_->setEnabled(false);
        copy_server_->setText(QStringLiteral("复制服务器地址"));
        server_endpoints_->hide();
        probeServer(this, api, [this, source, api, ws, generation](const ServerStatus& status) {
            if (generation != server_probe_generation_ || api != QSettings().value(QStringLiteral("net/api_url")).toString() ||
                ws != QSettings().value(QStringLiteral("net/ws_url")).toString()) return;
            const auto endpoints = serverEndpoints(status, api, ws);
            source->setText(status.description() + (endpoints.isEmpty()
                ? QStringLiteral("\n未获得其他设备可用的地址。服务器需提供局域网监听或代理的对外地址。")
                : QStringLiteral("\n复制以上两条地址，填入其他设备的登录页。")));
            server_endpoints_->blockSignals(true);
            server_endpoints_->clear();
            for (const auto& endpoint : endpoints)
                server_endpoints_->addItem(endpoint.api, QStringList{endpoint.api, endpoint.signaling});
            server_endpoints_->blockSignals(false);
            server_endpoints_->setVisible(endpoints.size() > 1);
            qobject_cast<QLabel*>(api_value_)->setText(endpoints.isEmpty() ? QStringLiteral("未提供可共享地址") : endpoints.first().api);
            qobject_cast<QLabel*>(ws_value_)->setText(endpoints.isEmpty() ? QStringLiteral("未提供可共享地址") : endpoints.first().signaling);
            copy_server_->setEnabled(!endpoints.isEmpty());
        });
    }
    if (controller_) {
        if (auto* label = qobject_cast<QLabel*>(local_id_value_)) {
            label->setText(controller_->localDeviceId().isEmpty()
                               ? QStringLiteral("本机尚未加入设备列表")
                               : controller_->localDeviceId());
        }
        // 自定义验证码行右侧显示当前状态
        QSettings settings;
        settings.setValue(QStringLiteral("security/has_custom_key"),
                          !controller_->localConnectionKey().isEmpty());
    }
    rebuildSavedKeys();

}

}  // namespace pxc::gui
