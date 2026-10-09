#include "device_page.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "settings_page.h"
#include "theme.h"
#include "toggle_switch.h"

namespace pxc::gui {
using namespace theme;

// ==================================================================
// SidebarItem：侧栏条目（自绘：图标 + 文字 + 右侧状态点/选中条）
// ==================================================================
class DevicePage::SidebarItem : public QWidget {
    Q_OBJECT
public:
    enum class Kind { Normal, Section };

    SidebarItem(const QString& text, QPixmap icon, QWidget* parent = nullptr)
        : QWidget(parent), text_(text), icon_(std::move(icon)), kind_(Kind::Normal) {
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(38);
        setMouseTracking(true);
        if (!icon_.isNull()) {
            const int pixels = qRound(20 * icon_.devicePixelRatio());
            icon_ = icon_.scaled(pixels, pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }

    static SidebarItem* makeSection(const QString& text, QWidget* parent = nullptr) {
        auto* item = new SidebarItem(text, QPixmap(), parent);
        item->kind_ = Kind::Section;
        item->setFixedHeight(34);
        return item;
    }

    void set_icon(const QPixmap& pm) {
        icon_ = pm;
        if (!icon_.isNull()) {
            const int pixels = qRound(20 * icon_.devicePixelRatio());
            icon_ = icon_.scaled(pixels, pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        update();
    }

    void set_dot_visible(bool on) { dot_ = on; update(); }
    void set_dot_online(bool online) {
        dot_color_ = online ? color(kOnlineGreen) : color(kOfflineGray);
        set_dot_visible(true);
    }
    void set_badge(const QString& badge) { badge_ = badge; update(); }
    void set_selected(bool selected) { selected_ = selected; update(); }
    bool is_selected() const { return selected_; }
    QString text() const { return text_; }

signals:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) pressed_ = true;
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (pressed_ && rect().contains(event->pos())) emit clicked();
        pressed_ = false;
    }
    void enterEvent(QEnterEvent* event) override { hover_ = true; update(); }
    void leaveEvent(QEvent* event) override { hover_ = false; update(); }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF body = rect().adjusted(2, 1, -6, -1);

        if (kind_ == Kind::Normal && (selected_ || hover_)) {
            p.setPen(Qt::NoPen);
            p.setBrush(color(selected_ ? kSelectedBg : kHoverBg));
            p.drawRoundedRect(body, 8, 8);
            if (selected_) {
                // 左侧品牌蓝小竖条（UU 远程选中样式）
                p.setBrush(color(kAccent));
                QRectF bar(body.left() + 2, body.center().y() - 8, 3, 16);
                p.drawRoundedRect(bar, 1.5, 1.5);
            }
        }

        const QColor text_color =
            kind_ == Kind::Section ? QColor(QString(kTextSecondary)) : QColor(QString(kTextPrimary));
        QFont font = this->font();
        font.setPointSizeF(kind_ == Kind::Section ? 9.0 : 10.0);
        font.setBold(kind_ == Kind::Section);
        p.setFont(font);
        p.setPen(text_color);

        const int icon_left = body.left() + 14;
        const int icon_size = 20;
        const int text_left = icon_left + icon_size + (kind_ == Kind::Section ? 4 : 10);
        if (kind_ == Kind::Section) {
            p.drawText(QRect(text_left - 8, 0, body.right() - text_left, height()),
                       Qt::AlignVCenter | Qt::AlignLeft, text_);
            return;
        }

        if (!icon_.isNull()) {
            p.drawPixmap(icon_left, (height() - icon_size) / 2, icon_);
        }
        QRect text_rect(text_left, 0, static_cast<int>(body.right()) - text_left, height());
        if (!badge_.isEmpty()) {
            p.drawText(text_rect.adjusted(0, 0, -60, 0),
                       Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(text_, Qt::ElideRight, text_rect.width() - 60));
            p.setPen(color(kTextSecondary));
            QFont badge_font = font;
            badge_font.setPointSizeF(8.5);
            p.setFont(badge_font);
            p.drawText(text_rect, Qt::AlignVCenter | Qt::AlignRight, badge_);
        } else {
            p.drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(text_, Qt::ElideRight, text_rect.width()));
        }

        if (dot_) {
            p.setPen(Qt::NoPen);
            p.setBrush(dot_color_);
            p.drawEllipse(QPointF(body.right() - 8, body.center().y()), 4, 4);
        }
    }

private:
    QString text_;
    QString badge_;
    QPixmap icon_;
    QColor  dot_color_ = color(kOnlineGreen);
    Kind    kind_ = Kind::Normal;
    bool    selected_ = false;
    bool    hover_    = false;
    bool    pressed_  = false;
    bool    dot_      = false;
};

// ==================================================================
// DeviceCard：设备卡（深色底图 + 进入桌面 + 底部工具条）
// ==================================================================
class DevicePage::DeviceCard : public QFrame {
    Q_OBJECT
public:
    explicit DeviceCard(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName("deviceCard");
        setFixedHeight(320);
        setCursor(Qt::PointingHandCursor);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        // 图片区域（背景由 DeviceCard::paintEvent 统一绘制并裁圆角）
        image_host_ = new QWidget(this);
        image_host_->setObjectName("deviceCardImage");
        image_host_->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        image_host_->setStyleSheet("background: transparent;");
        {
            auto* image_layout = new QVBoxLayout(image_host_);
            image_layout->setContentsMargins(0, 0, 0, 0);
            enter_button_ = new QPushButton(QStringLiteral("进入桌面　→"), image_host_);
            enter_button_->setObjectName("enterDesktopButton");
            enter_button_->setCursor(Qt::PointingHandCursor);
            enter_button_->setFixedSize(168, 46);
            enter_button_->setStyleSheet(QStringLiteral(
                "QPushButton { background: rgba(20,29,49,150); color: white;"
                " border: 1px solid rgba(255,255,255,90); border-radius: 23px;"
                " font-size: 15px; font-weight: 600; }"
                "QPushButton:hover { background: rgba(79,124,255,220);"
                " border-color: transparent; }"
                "QPushButton:disabled { background: rgba(90,99,113,120); color: #c2c7ce;"
                " border-color: rgba(255,255,255,40); }"));
            image_layout->addStretch();
            image_layout->addWidget(enter_button_, 0, Qt::AlignHCenter);
            image_layout->addStretch();
            image_layout->addSpacing(26);
        }

        // 底部工具条（白底，模仿 UU 远程）
        toolbar_ = new QWidget(this);
        toolbar_->setObjectName("deviceCardToolbar");
        toolbar_->setFixedHeight(52);
        toolbar_->setStyleSheet(QStringLiteral(
            "#deviceCardToolbar { background: white;"
            " border-bottom-left-radius: 12px; border-bottom-right-radius: 12px; }"));
        {
            auto* row = new QHBoxLayout(toolbar_);
            row->setContentsMargins(8, 4, 8, 4);
            row->setSpacing(0);
            const std::initializer_list<std::pair<const char*, const char*>> actions = {
                {"文件传输", "file"}, {"观看模式", "view"},
                {"终端", "terminal"}, {"端口映射", "ports"},
            };
            bool first = true;
            for (const auto& [label, key] : actions) {
                if (!first) row->addWidget(make_divider());
                const QString action_key = QString::fromLatin1(key);
                QPixmap icon = action_key == QStringLiteral("file")
                                   ? icon_folder(17, color(kTextPrimary))
                                   : action_key == QStringLiteral("view")
                                         ? icon_eye(17, color(kTextPrimary))
                                         : action_key == QStringLiteral("terminal")
                                               ? icon_terminal(17, color(kTextPrimary))
                                               : icon_ports(17, color(kTextPrimary));
                row->addWidget(make_action(label, action_key, icon), 1);
                first = false;
            }
            row->addWidget(make_divider());
            auto* apps = make_action(QString(), QStringLiteral("apps"), icon_grid9(18, color(kTextPrimary)));
            apps->setFixedWidth(40);
            row->addWidget(apps);
        }

        layout->addWidget(image_host_, 1);
        layout->addWidget(toolbar_);

        connect(enter_button_, &QPushButton::clicked, this, &DeviceCard::enterDesktop);
    }

    void set_actions_enabled(bool enabled, bool desktopSupported) {
        enter_enabled_ = enabled && desktopSupported;
        enter_button_->setEnabled(enter_enabled_);
        enter_button_->setToolTip(desktopSupported ? QString() :
            QStringLiteral("Harmony OS 设备仅支持作为主控端，无法进入其桌面"));
        for (auto* button : toolbar_->findChildren<QPushButton*>())
            button->setEnabled(enabled && (desktopSupported ||
                button->property("actionKey").toString() == QStringLiteral("file")));
    }

    void refresh_background() {
        const int image_h = height() - (toolbar_ ? toolbar_->height() : 52);
        if (preview_.isNull()) bg_ = device_card_background(qMax(width(), 1), qMax(image_h, 1));
        update();
    }

    void set_preview(const QImage& image) {
        if (image.isNull()) return;
        preview_ = image.copy();
        update();
    }

    void clear_preview() {
        preview_ = QImage();
        refresh_background();
    }

    QPixmap background_pixmap() const { return bg_; }

signals:
    void enterDesktop();
    void toolbarAction(const QString& key);

protected:
    void resizeEvent(QResizeEvent* event) override {
        QFrame::resizeEvent(event);
        const bool compact = width() < 500;
        for (auto* button : toolbar_->findChildren<QPushButton*>())
            button->setText(compact ? QString() : button->property("actionLabel").toString());
        refresh_background();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // 圆角裁剪整卡，再画图片区背景（工具条由自己的白底样式覆盖）
        QPainterPath path;
        path.addRoundedRect(rect(), 12, 12);
        p.setClipPath(path);

        const int image_h = height() - (toolbar_ ? toolbar_->height() : 52);
        if (!preview_.isNull()) {
            const QImage scaled = preview_.scaled(width(), image_h, Qt::KeepAspectRatioByExpanding,
                                                  Qt::SmoothTransformation);
            const int x = (scaled.width() - width()) / 2;
            const int y = (scaled.height() - image_h) / 2;
            p.drawImage(QRect(0, 0, width(), image_h), scaled,
                        QRect(x, y, width(), image_h));
            p.fillRect(QRect(0, 0, width(), image_h), QColor(12, 20, 35, 50));
        } else if (!bg_.isNull() && bg_.width() == width() && bg_.height() == image_h) {
            p.drawPixmap(0, 0, bg_);
        } else {
            QLinearGradient gradient(0, 0, width(), image_h);
            gradient.setColorAt(0.0, QColor("#233a5c"));
            gradient.setColorAt(1.0, QColor("#141d31"));
            p.fillRect(0, 0, width(), image_h, gradient);
        }
    }

private:
    QWidget* make_divider() {
        auto* divider = new QFrame(toolbar_);
        divider->setFixedSize(1, 22);
        divider->setStyleSheet(QStringLiteral("background: %1; border: none;").arg(kDivider));
        return divider;
    }

    QPushButton* make_action(const QString& text, const QString& key, QPixmap icon = {}) {
        auto* button = new QPushButton(toolbar_);
        button->setObjectName("cardAction");
        button->setCursor(Qt::PointingHandCursor);
        button->setFixedHeight(44);
        button->setMinimumWidth(32);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setProperty("actionLabel", text);
        button->setProperty("actionKey", key);
        button->setToolTip(text.isEmpty() ? QStringLiteral("应用") : text);
        button->setStyleSheet(QStringLiteral(
            "QPushButton#cardAction { background: transparent; border: none;"
            " color: %1; font-size: 13px; border-radius: 8px; padding: 4px; }"
            "QPushButton#cardAction:hover { background: %2; }"
            "QPushButton#cardAction:disabled { color: #b3b9c2; background: transparent; }")
            .arg(kTextPrimary, kHoverBg));
        button->setIcon(icon);
        button->setIconSize(QSize(17, 17));
        button->setText(text);
        connect(button, &QPushButton::clicked, this,
                [this, key] { emit toolbarAction(key); });
        return button;
    }

    QWidget*     image_host_ = nullptr;
    QWidget*     toolbar_    = nullptr;
    QPushButton* enter_button_ = nullptr;
    QPixmap      bg_;
    QImage       preview_;
    bool         enter_enabled_ = true;
};

// ==================================================================
// QuickLaunchBox：快速启动的虚线框 + 添加
// ==================================================================
class DevicePage::QuickLaunchBox : public QWidget {
    Q_OBJECT
public:
    explicit QuickLaunchBox(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(170);
        setCursor(Qt::PointingHandCursor);
    }
signals:
    void clicked();
protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) emit clicked();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        QPen pen(QColor(QString(kOfflineGray)), 1.4, Qt::DashLine);
        pen.setDashPattern({4, 4});
        p.setPen(pen);
        p.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 12, 12);

        // 圆形 + 号
        const QPointF c(width() / 2.0, height() / 2.0 - 14);
        p.setPen(Qt::NoPen);
        p.setBrush(color(kHoverBg));
        p.drawEllipse(c, 26, 26);
        QPixmap plus = icon_plus(18, color(kTextSecondary));
        p.drawPixmap(static_cast<int>(c.x()) - 9, static_cast<int>(c.y()) - 9, plus);

        p.setPen(color(kTextSecondary));
        QFont font = this->font();
        font.setPointSizeF(9.5);
        p.setFont(font);
        p.drawText(QRect(0, static_cast<int>(c.y()) + 32, width(), 24),
                   Qt::AlignHCenter, QStringLiteral("添加"));
    }
};

// ==================================================================
// DevicePage
// ==================================================================
DevicePage::DevicePage(ClientController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setStyleSheet(QStringLiteral("DevicePage { background: %1; }").arg(kPageBg));

    auto* root_layout = new QHBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    buildSidebar();
    buildContentPane();

    root_layout->addWidget(sidebar_);
    root_layout->addWidget(new_divider_v(), 0);

    auto* content_host = new QWidget(this);
    content_host->setStyleSheet(QStringLiteral("background: %1;").arg(kPageBg));
    auto* content_layout = new QVBoxLayout(content_host);
    content_layout->setContentsMargins(20, 18, 20, 14);
    content_layout->setSpacing(14);

    // 标题行
    auto* header = new QHBoxLayout;
    status_pill_ = new QLabel(QStringLiteral("● 离线"), content_host);
    status_pill_->setObjectName("statusPill");
    status_pill_->setFixedSize(76, 30);
    status_pill_->setAlignment(Qt::AlignCenter);

    device_name_ = new QLabel(QStringLiteral("设备"), content_host);
    QFont name_font = device_name_->font();
    name_font.setPointSize(18);
    name_font.setBold(true);
    device_name_->setFont(name_font);
    device_name_->setWordWrap(true);
    device_name_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    controlled_badge_ = new QLabel(QStringLiteral("● 被控中"), content_host);
    controlled_badge_->setObjectName("controlledBadge");
    controlled_badge_->setFixedSize(84, 30);
    controlled_badge_->setAlignment(Qt::AlignCenter);
    controlled_badge_->hide();

    more_button_ = new QToolButton(content_host);
    more_button_->setObjectName(QStringLiteral("deviceMenuButton"));
    more_button_->setIcon(icon_more(18, color(kTextSecondary)));
    more_button_->setAutoRaise(true);
    more_button_->setCursor(Qt::PointingHandCursor);
    more_button_->setToolTip(QStringLiteral("更多"));

    header->addWidget(status_pill_);
    header->addSpacing(6);
    header->addWidget(device_name_, 1);
    header->addSpacing(8);
    header->addWidget(controlled_badge_);
    header->addStretch();
    header->addWidget(more_button_);

    card_ = new DeviceCard(content_host);
    quick_launch_ = new QuickLaunchBox(content_host);

    auto* quick_label = new QLabel(QStringLiteral("快速启动"), content_host);
    quick_label->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; font-weight: 600;").arg(kTextPrimary));

    status_label_ = new QLabel(content_host);
    status_label_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;").arg(kTextSecondary));
    status_label_->setWordWrap(true);

    content_layout->addLayout(header);
    content_layout->addWidget(card_);
    content_layout->addWidget(quick_label);
    content_layout->addWidget(quick_launch_);
    content_layout->addStretch();
    content_layout->addWidget(status_label_);

    // 右栏 = 设备详情 / 设置页 双页切换
    content_stack_ = new QStackedWidget(this);
    auto* detail_scroll = new QScrollArea(this);
    detail_scroll->setFrameShape(QFrame::NoFrame);
    detail_scroll->setWidgetResizable(true);
    detail_scroll->setWidget(content_host);
    device_pane_ = detail_scroll;
    content_stack_->addWidget(device_pane_);
    settings_page_ = new SettingsPage(controller_, this);
    connect(settings_page_, &SettingsPage::logoutRequested, this, &DevicePage::logoutRequested);
    content_stack_->addWidget(settings_page_);
    content_stack_->setCurrentWidget(device_pane_);
    root_layout->addWidget(content_stack_, 1);

    connect(card_, &DeviceCard::enterDesktop, this, &DevicePage::onConnectClicked);
    connect(card_, &DeviceCard::toolbarAction, this, &DevicePage::onCardAction);
    connect(quick_launch_, &QuickLaunchBox::clicked, this, [this] {
        setStatus(QStringLiteral("快速启动项即将推出，敬请期待"));
    });
    connect(more_button_, &QToolButton::clicked, this, &DevicePage::showDeviceMenu);
    connect(settings_row_, &SidebarItem::clicked, this, &DevicePage::openSettingsPage);
    connect(all_devices_row_, &SidebarItem::clicked, this, &DevicePage::openAllDevicesPage);
    connect(assist_row_, &SidebarItem::clicked, this, &DevicePage::openAssistPage);
    connect(favorites_row_, &SidebarItem::clicked, this, [this] {
        setStatus(QStringLiteral("暂无收藏设备：连接成功后设备会出现在这里"));
    });
    connect(my_devices_header_, &SidebarItem::clicked, this, [this] {
        device_visible_ = !device_visible_;
        device_rows_host_->setVisible(device_visible_);
    });
}

QWidget* DevicePage::new_divider_v() {
    auto* divider = new QFrame(this);
    divider->setFixedWidth(1);
    divider->setStyleSheet(QStringLiteral("background: %1; border: none;").arg(kDivider));
    return divider;
}

void DevicePage::buildSidebar() {
    sidebar_ = new QWidget(this);
    sidebar_->setFixedWidth(220);
    sidebar_->setObjectName("sidebar");
    sidebar_->setStyleSheet(
        QStringLiteral("#sidebar { background: %1; border: none; }").arg(kSidebarBg));

    auto* layout = new QVBoxLayout(sidebar_);
    layout->setContentsMargins(10, 18, 10, 14);
    layout->setSpacing(2);

    brand_label_ = new QLabel(QStringLiteral("PixelConnection 远程"), sidebar_);
    QFont brand_font = brand_label_->font();
    brand_font.setPointSize(11);
    brand_font.setBold(true);
    brand_label_->setFont(brand_font);
    brand_label_->setStyleSheet(QStringLiteral("color: %1; padding-left: 8px;").arg(kTextPrimary));
    layout->addWidget(brand_label_);
    layout->addSpacing(14);

    auto* sidebar_layout = layout;
    auto* navigation = new QScrollArea(sidebar_);
    navigation->setFrameShape(QFrame::NoFrame);
    navigation->setWidgetResizable(true);
    navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* navigation_body = new QWidget(navigation);
    layout = new QVBoxLayout(navigation_body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    navigation->setWidget(navigation_body);
    sidebar_layout->addWidget(navigation, 1);

    my_devices_header_ = SidebarItem::makeSection(QStringLiteral("我的设备"), sidebar_);
    layout->addWidget(my_devices_header_);

    device_rows_host_ = new QWidget(sidebar_);
    device_rows_ = new QVBoxLayout(device_rows_host_);
    device_rows_->setContentsMargins(0, 0, 0, 0);
    device_rows_->setSpacing(2);
    layout->addWidget(device_rows_host_);

    auto* empty_hint = new QLabel(QStringLiteral("　　还没有设备"), device_rows_host_);
    empty_hint->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; padding: 6px 0;")
                                  .arg(kTextSecondary));
    device_rows_->addWidget(empty_hint);
    empty_hint->setObjectName("deviceEmptyHint");

    all_devices_row_ = new SidebarItem(QStringLiteral("全部设备"),
                                       icon_grid_dots(20, color(kTextPrimary)), sidebar_);
    layout->addWidget(all_devices_row_);
    layout->addSpacing(10);

    auto* assist_header = SidebarItem::makeSection(QStringLiteral("远程协助"), sidebar_);
    layout->addWidget(assist_header);

    assist_row_ = new SidebarItem(QStringLiteral("开始协助"),
                                  icon_assist(20, color(kAccent)), sidebar_);
    assist_row_->setObjectName("assistNav");
    layout->addWidget(assist_row_);

    favorites_row_ = new SidebarItem(QStringLiteral("收藏设备"),
                                     icon_star(20, color(kTextPrimary)), sidebar_);
    layout->addWidget(favorites_row_);

    layout->addStretch();
    layout = sidebar_layout;

    auto* divider = new QFrame(sidebar_);
    divider->setFixedHeight(1);
    divider->setStyleSheet(QStringLiteral("background: %1; border: none;").arg(kDivider));
    layout->addWidget(divider);
    layout->addSpacing(6);

    settings_row_ = new SidebarItem(QStringLiteral("设置"),
                                    icon_settings(20, color(kTextPrimary)), sidebar_);
    layout->addWidget(settings_row_);
}

void DevicePage::buildContentPane() {
    // 内容区在构造函数里组装；此函数保留给未来拆分
}

void DevicePage::openAllDevicesPage() {
    setNavSelection(all_devices_row_);
    if (!all_devices_page_) {
        all_devices_page_ = new QWidget(content_stack_);
        auto* page = new QVBoxLayout(all_devices_page_);
        page->setContentsMargins(34, 26, 34, 18);
        page->setSpacing(14);
        auto* title = new QLabel(QStringLiteral("全部设备"), all_devices_page_);
        QFont font = title->font();
        font.setPointSize(19);
        font.setBold(true);
        title->setFont(font);
        page->addWidget(title);
        auto* scroll = new QScrollArea(all_devices_page_);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* body = new QWidget(scroll);
        all_devices_layout_ = new QVBoxLayout(body);
        all_devices_layout_->setContentsMargins(0, 4, 0, 10);
        all_devices_layout_->setSpacing(8);
        scroll->setWidget(body);
        page->addWidget(scroll, 1);
        content_stack_->addWidget(all_devices_page_);
    }
    rebuildAllDevicesPage();
    content_stack_->setCurrentWidget(all_devices_page_);
}

void DevicePage::rebuildAllDevicesPage() {
    if (!all_devices_layout_) return;
    while (all_devices_layout_->count()) {
        QLayoutItem* item = all_devices_layout_->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    if (devices_.isEmpty()) {
        auto* empty = new QLabel(QStringLiteral("暂无设备。登录后可在「设置」中将本机加入设备列表。"),
                                 all_devices_page_);
        empty->setStyleSheet(QStringLiteral("color: %1; padding: 12px;").arg(kTextSecondary));
        all_devices_layout_->addWidget(empty);
        all_devices_layout_->addStretch();
        return;
    }
    QVector<int> computers;
    QVector<int> mobile;
    for (int i = 0; i < devices_.size(); ++i) {
        const QString platform = devices_.at(i).platform.toLower();
        (platform.contains(QStringLiteral("android")) || platform.contains(QStringLiteral("ios")) ||
         platform.contains(QStringLiteral("phone")) || platform.contains(QStringLiteral("tablet"))
             ? mobile : computers).push_back(i);
    }
    const auto add_group = [this](const QString& label, const QVector<int>& indices) {
        if (indices.isEmpty()) return;
        auto* heading = new QLabel(QStringLiteral("⌄  %1 %2").arg(label).arg(indices.size()),
                                   all_devices_page_);
        heading->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; padding: 4px 2px;")
                                   .arg(kTextPrimary));
        all_devices_layout_->addWidget(heading);
        for (int index : indices) {
            const DeviceRow device = devices_.at(index);
            const QString name = device.name.isEmpty() ? device.deviceId : device.name;
            const QString platform = device.platform.toLower();
            const auto strip = [](QString value) { value.remove(QLatin1Char('-')); return value; };
            const bool local = controller_ && !controller_->localDeviceId().isEmpty() &&
                               strip(device.deviceId).compare(strip(controller_->localDeviceId()),
                                                               Qt::CaseInsensitive) == 0;
            auto* card = new QFrame(all_devices_page_);
            card->setStyleSheet(QStringLiteral("QFrame { background: white; border: 1px solid %1; border-radius: 7px; }")
                                    .arg(kDivider));
            auto* row = new QHBoxLayout(card);
            row->setContentsMargins(14, 9, 12, 9);
            row->setSpacing(12);
            auto* icon = new QLabel(card);
            icon->setFixedSize(40, 40);
            icon->setAlignment(Qt::AlignCenter);
            const bool is_mobile = platform.contains(QStringLiteral("android")) ||
                                   platform.contains(QStringLiteral("ios")) ||
                                   platform.contains(QStringLiteral("phone")) ||
                                   platform.contains(QStringLiteral("tablet"));
            icon->setPixmap(is_mobile ? icon_mobile(24, Qt::white) : icon_device(24, Qt::white));
            icon->setStyleSheet(QStringLiteral("background: %1; border-radius: 6px;").arg(kAccent));
            row->addWidget(icon);
            auto* name_label = new QLabel(name, card);
            name_label->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; border: none;")
                                          .arg(kTextPrimary));
            row->addWidget(name_label, 1);
            if (local) {
                auto* badge = new QLabel(QStringLiteral("本机"), card);
                badge->setStyleSheet(QStringLiteral("color: %1; background: #e7efff; border: none; padding: 3px 7px; border-radius: 4px;")
                                         .arg(kAccent));
                row->addWidget(badge);
            }
            auto* info = new QPushButton(QStringLiteral("ⓘ"), card);
            info->setFixedSize(34, 34);
            info->setToolTip(QStringLiteral("设备信息"));
            info->setStyleSheet(QStringLiteral("QPushButton { color: %1; border: none; font-size: 18px; }")
                                    .arg(kTextPrimary));
            connect(info, &QPushButton::clicked, this, [this, device, local] {
                QMessageBox::information(this, QStringLiteral("设备信息"),
                    QStringLiteral("设备名称：%1\n设备 ID：%2\n平台：%3\n状态：%4%5")
                        .arg(device.name.isEmpty() ? device.deviceId : device.name,
                             device.deviceId,
                             device.platform.isEmpty() ? QStringLiteral("未知") : device.platform,
                             device.online ? QStringLiteral("在线") : QStringLiteral("离线"),
                             local ? QStringLiteral("\n本机") : QString()));
            });
            row->addWidget(info);
            auto* open = new QPushButton(QStringLiteral("›"), card);
            open->setFixedSize(28, 34);
            open->setStyleSheet(QStringLiteral("QPushButton { color: %1; border: none; font-size: 22px; }")
                                    .arg(kTextSecondary));
            connect(open, &QPushButton::clicked, this, [this, index] { selectDevice(index); });
            row->addWidget(open);
            all_devices_layout_->addWidget(card);
        }
        all_devices_layout_->addSpacing(12);
    };
    add_group(QStringLiteral("电脑"), computers);
    add_group(QStringLiteral("手机/平板"), mobile);
    all_devices_layout_->addStretch();
}

void DevicePage::openAssistPage() {
    setNavSelection(assist_row_);
    if (!assist_page_) {
        assist_page_ = new QWidget(content_stack_);
        auto* container = new QVBoxLayout(assist_page_);
        container->setContentsMargins(0, 0, 0, 0);
        auto* scroll = new QScrollArea(assist_page_);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        auto* body = new QWidget(scroll);
        auto* page = new QVBoxLayout(body);
        scroll->setWidget(body);
        container->addWidget(scroll);
        page->setContentsMargins(20, 18, 20, 14);
        page->setSpacing(16);
        auto* title = new QLabel(QStringLiteral("远程协助"), assist_page_);
        QFont title_font = title->font();
        title_font.setPointSize(19);
        title_font.setBold(true);
        title->setFont(title_font);
        page->addWidget(title);

        auto* local = new QFrame(assist_page_);
        local->setStyleSheet(QStringLiteral("QFrame { background: white; border: 1px solid %1; border-radius: 8px; }")
                                 .arg(kDivider));
        auto* local_layout = new QVBoxLayout(local);
        local_layout->setContentsMargins(18, 16, 18, 16);
        auto* local_title_row = new QHBoxLayout;
        auto* local_title = new QLabel(QStringLiteral("本设备"), local);
        QFont local_font = local_title->font();
        local_font.setBold(true);
        local_title->setFont(local_font);
        local_title_row->addWidget(local_title);
        local_title_row->addStretch();
        auto* allow_text = new QLabel(QStringLiteral("允许他人远程协助"), local);
        allow_text->setStyleSheet(QStringLiteral("border: none;"));
        local_title_row->addWidget(allow_text);
        assist_allow_toggle_ = new ToggleSwitch(local);
        assist_allow_toggle_->set_on(QSettings().value(QStringLiteral("security/allow_remote"), true).toBool());
        connect(assist_allow_toggle_, &ToggleSwitch::toggled, this, [](bool on) {
            QSettings().setValue(QStringLiteral("security/allow_remote"), on);
        });
        local_title_row->addWidget(assist_allow_toggle_);
        local_layout->addLayout(local_title_row);
        auto* id_row = new QHBoxLayout;
        local_id_label_ = new QLabel(local);
        local_id_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        local_id_label_->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: 700; color: %1; border: none;")
                                           .arg(kTextPrimary));
        id_row->addWidget(local_id_label_, 1);
        auto* copy_id = new QPushButton(QStringLiteral("复制 ID"), local);
        connect(copy_id, &QPushButton::clicked, this, [this] {
            if (local_device_id_.isEmpty()) return;
            QApplication::clipboard()->setText(local_device_id_);
            setStatus(QStringLiteral("已复制本机设备 ID"));
        });
        id_row->addWidget(copy_id);
        auto* share = new QPushButton(QStringLiteral("复制并分享"), local);
        connect(share, &QPushButton::clicked, this, [this] {
            if (local_device_id_.isEmpty() || local_connection_key_.isEmpty()) {
                setStatus(QStringLiteral("请先在设置中将本机加入设备列表"), true);
                return;
            }
            QApplication::clipboard()->setText(QStringLiteral("设备 ID：%1\n连接密钥：%2")
                                                   .arg(local_device_id_, local_connection_key_));
            setStatus(QStringLiteral("已复制设备 ID 和连接密钥，请通过可信渠道发送给伙伴"));
        });
        id_row->addWidget(share);
        local_layout->addLayout(id_row);
        auto* key_row = new QHBoxLayout;
        auto* key_caption = new QLabel(QStringLiteral("连接密钥"), local);
        key_caption->setStyleSheet(QStringLiteral("color: %1; border: none;").arg(kTextSecondary));
        key_row->addWidget(key_caption);
        local_key_label_ = new QLabel(local);
        local_key_label_->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600; border: none;"));
        key_row->addWidget(local_key_label_, 1);
        auto* reveal = new QPushButton(QStringLiteral("显示"), local);
        reveal->setCheckable(true);
        auto* copy_key = new QPushButton(QStringLiteral("复制密钥"), local);
        connect(reveal, &QPushButton::toggled, this, [this, reveal](bool revealed) {
            local_key_label_->setText(local_connection_key_.isEmpty()
                                          ? QStringLiteral("尚未设置")
                                          : revealed ? local_connection_key_ : QStringLiteral("••••••••"));
            reveal->setText(revealed ? QStringLiteral("隐藏") : QStringLiteral("显示"));
        });
        connect(copy_key, &QPushButton::clicked, this, [this] {
            if (local_connection_key_.isEmpty()) return;
            QApplication::clipboard()->setText(local_connection_key_);
            setStatus(QStringLiteral("已复制本机连接密钥"));
        });
        key_row->addWidget(reveal);
        key_row->addWidget(copy_key);
        local_layout->addLayout(key_row);
        page->addWidget(local);

        auto* partner = new QFrame(assist_page_);
        partner->setStyleSheet(local->styleSheet());
        auto* partner_layout = new QVBoxLayout(partner);
        partner_layout->setContentsMargins(18, 16, 18, 16);
        auto* partner_title = new QLabel(QStringLiteral("远程伙伴设备"), partner);
        QFont partner_font = partner_title->font();
        partner_font.setBold(true);
        partner_title->setFont(partner_font);
        partner_layout->addWidget(partner_title);
        auto* hint = new QLabel(QStringLiteral("输入对方提供的设备 ID 与连接密钥以开始远程控制。"), partner);
        hint->setStyleSheet(QStringLiteral("color: %1; border: none;").arg(kTextSecondary));
        partner_layout->addWidget(hint);
        auto* input_row = new QHBoxLayout;
        partner_id_field_ = new QLineEdit(partner);
        partner_id_field_->setObjectName("partnerDeviceId");
        partner_id_field_->setPlaceholderText(QStringLiteral("请输入设备 ID"));
        input_row->addWidget(partner_id_field_, 1);
        auto* connect_button = new QPushButton(QStringLiteral("连接"), partner);
        partner_connect_button_ = connect_button;
        connect_button->setProperty("assistConnect", true);
        connect_button->setObjectName("accentButton");
        input_row->addWidget(connect_button);
        partner_layout->addLayout(input_row);
        page->addWidget(partner);

        auto* recent_header = new QHBoxLayout;
        auto* recent_title = new QLabel(QStringLiteral("最近连接"), assist_page_);
        recent_header->addWidget(recent_title);
        recent_header->addStretch();
        auto* clear_recent = new QPushButton(QStringLiteral("清除记录"), assist_page_);
        clear_recent->setFlat(true);
        recent_header->addWidget(clear_recent);
        page->addLayout(recent_header);
        assist_recent_layout_ = new QVBoxLayout;
        assist_recent_layout_->setContentsMargins(0, 0, 0, 0);
        assist_recent_layout_->setSpacing(4);
        page->addLayout(assist_recent_layout_);
        page->addStretch();
        connect(clear_recent, &QPushButton::clicked, this, [this] {
            QSettings().remove(QStringLiteral("assist/recent_devices"));
            rebuildAssistRecentList();
        });
        auto connect_partner = [this] {
            if (controlled_active_) {
                setStatus(QStringLiteral("本机正在被远控，暂不能连接其他设备"), true);
                return;
            }
            const QString device_id = partner_id_field_->text().trimmed();
            if (device_id.isEmpty()) {
                setStatus(QStringLiteral("请输入伙伴设备 ID"), true);
                return;
            }
            const auto strip = [](QString value) { value.remove(QLatin1Char('-')); return value; };
            if (!local_device_id_.isEmpty() && strip(device_id).compare(strip(local_device_id_), Qt::CaseInsensitive) == 0) {
                setStatus(QStringLiteral("不能连接本机自己"), true);
                return;
            }
            for (const auto& device : devices_) {
                if (strip(device.deviceId).compare(strip(device_id), Qt::CaseInsensitive) == 0 &&
                    !device.supportsRemoteDesktop()) {
                    setStatus(QStringLiteral("Harmony OS 设备无法被远控，请使用设备卡片的文件传输入口"), true);
                    return;
                }
            }
            QString key = controller_ ? controller_->savedKeyFor(device_id) : QString();
            if (key.isEmpty()) {
                bool ok = false;
                key = QInputDialog::getText(this, QStringLiteral("连接认证"),
                    QStringLiteral("请输入伙伴提供的连接密钥"), QLineEdit::Password, QString(), &ok);
                if (!ok) return;
                if (key.trimmed().isEmpty()) {
                    setStatus(QStringLiteral("连接密钥不能为空"), true);
                    return;
                }
            }
            QSettings settings;
            QStringList recent = settings.value(QStringLiteral("assist/recent_devices")).toStringList();
            recent.removeAll(device_id);
            recent.prepend(device_id);
            while (recent.size() > 8) recent.removeLast();
            settings.setValue(QStringLiteral("assist/recent_devices"), recent);
            rebuildAssistRecentList();
            emit connectRequested(device_id, key.trimmed());
        };
        connect(connect_button, &QPushButton::clicked, this, connect_partner);
        connect(partner_id_field_, &QLineEdit::returnPressed, this, connect_partner);
        connect(partner_id_field_, &QLineEdit::textChanged, this, [this](const QString& text) {
            partner_connect_button_->setEnabled(!controlled_active_ && !text.trimmed().isEmpty());
        });
        partner_id_field_->setEnabled(!controlled_active_);
        partner_connect_button_->setEnabled(!controlled_active_ && !partner_id_field_->text().trimmed().isEmpty());
        rebuildAssistRecentList();
        content_stack_->addWidget(assist_page_);
    }
    if (local_id_label_) local_id_label_->setText(local_device_id_.isEmpty()
        ? QStringLiteral("尚未将本机加入设备列表") : local_device_id_);
    if (local_key_label_) local_key_label_->setText(local_connection_key_.isEmpty()
        ? QStringLiteral("尚未设置") : QStringLiteral("••••••••"));
    content_stack_->setCurrentWidget(assist_page_);
}

void DevicePage::rebuildAssistRecentList() {
    if (!assist_recent_layout_) return;
    while (assist_recent_layout_->count()) {
        QLayoutItem* item = assist_recent_layout_->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    const QStringList recent = QSettings().value(QStringLiteral("assist/recent_devices")).toStringList();
    if (recent.isEmpty()) {
        auto* hint = new QLabel(QStringLiteral("暂无最近连接记录"), assist_page_);
        hint->setStyleSheet(QStringLiteral("color: %1; padding: 6px;").arg(kTextSecondary));
        assist_recent_layout_->addWidget(hint);
        return;
    }
    for (const QString& id : recent) {
        auto* row = new QHBoxLayout;
        auto* id_button = new QPushButton(id, assist_page_);
        id_button->setStyleSheet(QStringLiteral("QPushButton { background: white; border: 1px solid %1; border-radius: 5px; padding: 7px 10px; text-align: left; }")
                                     .arg(kDivider));
        connect(id_button, &QPushButton::clicked, this, [this, id] {
            if (partner_id_field_) partner_id_field_->setText(id);
        });
        row->addWidget(id_button, 1);
        auto* remove = new QPushButton(QStringLiteral("×"), assist_page_);
        remove->setFixedWidth(34);
        connect(remove, &QPushButton::clicked, this, [this, id] {
            QSettings settings;
            QStringList items = settings.value(QStringLiteral("assist/recent_devices")).toStringList();
            items.removeAll(id);
            settings.setValue(QStringLiteral("assist/recent_devices"), items);
            rebuildAssistRecentList();
        });
        row->addWidget(remove);
        assist_recent_layout_->addLayout(row);
    }
}

void DevicePage::rebuildDeviceList() {
    // 清空旧行
    while (device_rows_->count() > 0) {
        QLayoutItem* item = device_rows_->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    if (devices_.isEmpty()) {
        auto* hint = new QLabel(QStringLiteral("　　还没有设备，点击「设置」里的"
                                               "「把本机加入设备列表」"), device_rows_host_);
        hint->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; padding: 6px 8px;")
                                .arg(kTextSecondary));
        hint->setWordWrap(true);
        device_rows_->addWidget(hint);
    } else {
        for (int i = 0; i < devices_.size(); ++i) {
            const DeviceRow& device = devices_.at(i);
            // 本机设备打上「本机」标志，且不可被连接。
            // ID 比较前去掉连字符：服务器存规范化 ID（无连字符），
            // 本地 identity 带连字符。
            const auto strip_dash = [](const QString& s) {
                return QString(s).remove(QLatin1Char('-'));
            };
            const bool is_local =
                controller_ && !controller_->localDeviceId().isEmpty() &&
                strip_dash(device.deviceId).compare(
                    strip_dash(controller_->localDeviceId()), Qt::CaseInsensitive) == 0;
            QString display_name =
                device.name.isEmpty() ? device.deviceId : device.name;
            if (is_local) display_name += QStringLiteral("　（本机）");
            const QString platform = device.platform.toLower();
            const bool is_mobile = platform.contains(QStringLiteral("android")) ||
                                   platform.contains(QStringLiteral("ios")) ||
                                   platform.contains(QStringLiteral("phone")) ||
                                   platform.contains(QStringLiteral("tablet"));
            auto* item = new SidebarItem(display_name,
                                         is_mobile ? icon_mobile(20, color(kTextPrimary))
                                                   : icon_device(20, color(kTextPrimary)),
                                         device_rows_host_);
            item->set_dot_online(device.online);
            item->set_selected(i == selected_ &&
                (!content_stack_ || content_stack_->currentWidget() == device_pane_));
            connect(item, &SidebarItem::clicked, this, [this, i] { selectDevice(i); });
            device_rows_->addWidget(item);
        }
    }

    if (selected_ >= devices_.size()) selected_ = devices_.isEmpty() ? -1 : 0;
    updateHeader();
}

void DevicePage::selectDevice(int row) {
    selected_ = row;
    setNavSelection(nullptr);
    // 刷新选中态；点选设备回到设备详情页
    for (int i = 0; i < device_rows_->count(); ++i) {
        if (auto* item = qobject_cast<SidebarItem*>(device_rows_->itemAt(i)->widget())) {
            item->set_selected(i == row);
        }
    }
    if (content_stack_) content_stack_->setCurrentWidget(device_pane_);
    updateHeader();
    showSelectedDesktopPreview();
}

void DevicePage::updateHeader() {
    const DeviceRow device = selected_ >= 0 && selected_ < devices_.size()
                                 ? devices_.at(selected_)
                                 : DeviceRow();

    const QString name = device.name.isEmpty()
                             ? (device.deviceId.isEmpty() ? QStringLiteral("我的设备")
                                                          : device.deviceId)
                             : device.name;
    device_name_->setText(name);
    const auto normalized = [](QString id) { return id.remove(QLatin1Char('-')).toUpper(); };
    const bool local = !local_device_id_.isEmpty() && normalized(device.deviceId) == normalized(local_device_id_);
    card_->set_actions_enabled(!controlled_active_ && !device.deviceId.isEmpty() && !local,
                               device.supportsRemoteDesktop());

    if (device.deviceId.isEmpty()) {
        status_pill_->setText(QStringLiteral("● 无设备"));
        status_pill_->setStyleSheet(QStringLiteral(
            "background: #1f2329; color: white; border-radius: 15px;"
            " font-size: 12px;"));
    } else if (device.online) {
        status_pill_->setText(QStringLiteral("● 在线"));
        status_pill_->setStyleSheet(QStringLiteral(
            "background: #1f2329; color: %1; border-radius: 15px;"
            " font-size: 12px;").arg(kOnlineGreen));
    } else {
        status_pill_->setText(QStringLiteral("● 离线"));
        status_pill_->setStyleSheet(QStringLiteral(
            "background: #1f2329; color: %1; border-radius: 15px;"
            " font-size: 12px;").arg(kOfflineGray));
    }
}

void DevicePage::showDeviceMenu() {
    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral(
        "QMenu { background: white; border: 1px solid %1; border-radius: 8px;"
        " padding: 6px; }"
        "QMenu::item { padding: 8px 28px; border-radius: 6px; color: %2; }"
        "QMenu::item:selected { background: %3; }"
        "QMenu::separator { height: 1px; background: %1; margin: 4px 8px; }")
        .arg(kDivider, kTextPrimary, kHoverBg));

    if (!devices_.isEmpty()) {
        for (int i = 0; i < devices_.size(); ++i) {
            const DeviceRow& device = devices_.at(i);
            QAction* action = menu.addAction(QStringLiteral("%1　%2")
                                                 .arg(device.name.isEmpty()
                                                          ? device.deviceId
                                                          : device.name,
                                                      device.online ? QStringLiteral("●")
                                                                    : QStringLiteral("○")));
            connect(action, &QAction::triggered, this, [this, i] { selectDevice(i); });
        }
        menu.addSeparator();
    }
    menu.addAction(QStringLiteral("打开设置"), this, &DevicePage::openSettingsPage);
    menu.addAction(QStringLiteral("刷新设备"), this, &DevicePage::refreshRequested);
    auto* remove = menu.addAction(QStringLiteral("删除设备"), this, &DevicePage::deleteSelectedDevice);
    remove->setObjectName(QStringLiteral("deleteDeviceAction"));
    const QString selected = selectedDeviceId().remove(QLatin1Char('-')).toUpper();
    remove->setEnabled(!selected.isEmpty() && !removing_devices_.contains(selected));
    menu.addAction(QStringLiteral("把本机加入设备列表"), this,
                   &DevicePage::addThisDeviceRequested);
    menu.addSeparator();
    menu.addAction(QStringLiteral("退出登录"), this, &DevicePage::logoutRequested);

    menu.exec(QCursor::pos());
}

void DevicePage::deleteSelectedDevice() {
    const QString id = selectedDeviceId();
    const QString normalized = QString(id).remove(QLatin1Char('-')).toUpper();
    if (id.isEmpty() || removing_devices_.contains(normalized)) return;
    const QString name = selectedDeviceName().isEmpty() ? id : selectedDeviceName();
    const auto answer = QMessageBox::question(this, QStringLiteral("删除设备"),
        QStringLiteral("确定从当前账号删除设备「%1」吗？\n\n设备 ID：%2\n"
                       "同时清除本机保存的连接密钥、最近连接记录和壁纸缓存。"
                       "若正在连接此设备，会断开该会话。\n设备上的文件不会被删除。")
            .arg(name, id), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    removing_devices_.insert(normalized);
    setStatus(QStringLiteral("正在删除设备：") + name);
    emit removeDeviceRequested(id);
}

void DevicePage::finishDeviceRemoval(const QString& deviceId, bool success, const QString& message) {
    const auto normalized = [](QString id) { return id.remove(QLatin1Char('-')).toUpper(); };
    const QString id = normalized(deviceId);
    removing_devices_.remove(id);
    if (success) {
        if (!local_device_id_.isEmpty() && normalized(local_device_id_) == id)
            QFile::remove(desktopPreviewPath(local_device_id_));
        const auto pendingPreviews = preview_capture_devices_;
        for (const QString& pending : pendingPreviews) {
            if (normalized(pending) == id) {
                preview_capture_devices_.remove(pending);
                QFile::remove(desktopPreviewPath(pending));
            }
        }
        QVector<DeviceRow> remaining;
        for (const auto& device : devices_) {
            if (normalized(device.deviceId) == id) {
                QFile::remove(desktopPreviewPath(device.deviceId));
                preview_capture_devices_.remove(device.deviceId);
            } else remaining.push_back(device);
        }
        QFile::remove(desktopPreviewPath(deviceId));
        preview_capture_devices_.remove(deviceId);
        setDevices(remaining);
        rebuildAssistRecentList();
    }
    setStatus(message, !success);
}

void DevicePage::onCardAction(const QString& key) {
    if (key == QStringLiteral("file")) {
        if (selectedDeviceId().isEmpty()) {
            setStatus(QStringLiteral("请先选择远程设备"), true);
            return;
        }
        connectSelectedDevice(true);
        return;
    }
    const QString feature = key == QStringLiteral("view") ? QStringLiteral("观看模式")
                           : key == QStringLiteral("terminal") ? QStringLiteral("终端")
                           : key == QStringLiteral("ports") ? QStringLiteral("端口映射")
                           : QStringLiteral("此功能");
    setStatus(feature + QStringLiteral("尚未启用；当前可使用桌面控制与文件传输"));
}

void DevicePage::openSettingsPage() {
    setNavSelection(settings_row_);
    if (settings_page_) settings_page_->refresh();
    content_stack_->setCurrentWidget(settings_page_);
}

void DevicePage::setNavSelection(SidebarItem* selected) {
    for (SidebarItem* item : {all_devices_row_, assist_row_, favorites_row_, settings_row_}) {
        if (item) item->set_selected(item == selected);
    }
    if (device_rows_) {
        for (int i = 0; i < device_rows_->count(); ++i) {
            if (auto* item = qobject_cast<SidebarItem*>(device_rows_->itemAt(i)->widget()))
                item->set_selected(false);
        }
    }
}

QString DevicePage::selectedDeviceId() const {
    return selected_ >= 0 && selected_ < devices_.size()
               ? devices_.at(selected_).deviceId
               : QString();
}

QString DevicePage::selectedDeviceName() const {
    return selected_ >= 0 && selected_ < devices_.size()
               ? devices_.at(selected_).name
               : QString();
}

void DevicePage::setDevices(const QVector<DeviceRow>& devices) {
    const QString previous = selectedDeviceId();
    devices_ = devices;
    if (!previous.isEmpty()) {
        selected_ = -1;
        for (int i = 0; i < devices_.size(); ++i)
            if (devices_[i].deviceId == previous) { selected_ = i; break; }
    }
    if (selected_ < 0 && !devices.isEmpty()) selected_ = 0;
    rebuildDeviceList();
    rebuildAllDevicesPage();
    showSelectedDesktopPreview();
}

void DevicePage::setLocalDevice(const QString& deviceId, const QString& connectionKey) {
    local_device_id_      = deviceId;
    local_connection_key_ = connectionKey;
    updateHeader();
    if (local_id_label_) local_id_label_->setText(local_device_id_.isEmpty()
        ? QStringLiteral("尚未将本机加入设备列表") : local_device_id_);
    if (local_key_label_) local_key_label_->setText(local_connection_key_.isEmpty()
        ? QStringLiteral("尚未设置") : QStringLiteral("••••••••"));
    if (settings_page_ && content_stack_ &&
        content_stack_->currentWidget() == settings_page_) {
        settings_page_->refresh();
    }
}

void DevicePage::setServerInfo(const QString& apiUrl, const QString& wsUrl) {
    api_url_ = apiUrl;
    ws_url_  = wsUrl;
    QSettings settings;
    settings.setValue(QStringLiteral("net/api_url"), apiUrl);
    settings.setValue(QStringLiteral("net/ws_url"), wsUrl);
    if (settings_page_) settings_page_->refresh();
}

void DevicePage::setStatus(const QString& text, bool error) {
    status_label_->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                                     .arg(error ? kDangerRed : kTextSecondary));
    status_label_->setText(text);
}

void DevicePage::setControlledActive(bool active) {
    controlled_active_ = active;
    controlled_badge_->setVisible(active);
    updateHeader();
    if (partner_id_field_) partner_id_field_->setEnabled(!active);
    if (partner_connect_button_)
        partner_connect_button_->setEnabled(!active && !partner_id_field_->text().trimmed().isEmpty());
    if (active) {
        controlled_badge_->setStyleSheet(QStringLiteral(
            "background: %1; color: white; border-radius: 15px; font-size: 12px;")
            .arg(kDangerRed));
        setStatus(QStringLiteral("本机正在被远程控制"));
    }
}

void DevicePage::setSessionActive(bool active) {
    session_active_ = active;
}

QString DevicePage::desktopPreviewPath(const QString& deviceId) const {
    const auto key = QCryptographicHash::hash(deviceId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/wallpaper-previews/") + QString::fromLatin1(key) + QStringLiteral(".png");
}

QImage DevicePage::desktopPreviewImage(const QString& deviceId) const {
    if (deviceId.isEmpty()) return {};
    return QImage(desktopPreviewPath(deviceId));
}

void DevicePage::showSelectedDesktopPreview() {
    if (!card_) return;
    card_->clear_preview();
    card_->set_preview(desktopPreviewImage(selectedDeviceId()));
}

void DevicePage::beginDesktopPreviewCapture(const QString& deviceId) {
    if (!deviceId.isEmpty()) preview_capture_devices_.insert(deviceId);
}

void DevicePage::setDesktopPreviewImage(const QString& deviceId, const QImage& image) {
    // Save the configured primary-screen wallpaper once per new session.
    // Video frames never feed this cache; preserve it until a wallpaper arrives.
    if (deviceId.isEmpty() || !preview_capture_devices_.contains(deviceId) || image.isNull()) return;
    preview_capture_devices_.remove(deviceId);
    const QImage snapshot = image.scaled(QSize(960, 540), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QString path = desktopPreviewPath(deviceId);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !snapshot.save(&file, "PNG") || !file.commit())
        qWarning() << "Failed to save desktop preview for" << deviceId;
    if (card_ && selectedDeviceId() == deviceId) card_->set_preview(snapshot);
}

void DevicePage::onConnectClicked() {
    connectSelectedDevice(false);
}

void DevicePage::connectSelectedDevice(bool fileTransferOnly) {
    if (controlled_active_) {
        setStatus(QStringLiteral("本机正在被远控，暂不能连接其他设备"), true);
        return;
    }
    const QString deviceId = selectedDeviceId();
    if (deviceId.isEmpty()) {
        setStatus(QStringLiteral("请先在左侧选择一台设备"), true);
        return;
    }
    if (!fileTransferOnly && !devices_.at(selected_).supportsRemoteDesktop()) {
        setStatus(QStringLiteral("Harmony OS 设备无法被远控，请使用文件传输"), true);
        return;
    }
    // 本机判断同样要去掉连字符再比较（服务器 ID 无连字符）
    const auto strip_dash_guard = [](const QString& s) {
        return QString(s).remove(QLatin1Char('-'));
    };
    if (controller_ && !controller_->localDeviceId().isEmpty() &&
        strip_dash_guard(deviceId).compare(
            strip_dash_guard(controller_->localDeviceId()),
            Qt::CaseInsensitive) == 0) {
        setStatus(QStringLiteral("不能远程控制本机自己；请选择其他设备"), true);
        return;
    }

    // 已记住该设备的连接密钥（首次连接成功后保存）则直接连接，不再询问。
    // 输入的是连接密钥，不是设备私钥；密钥不离开本机，
    // 只用于在已建立的加密通道上做一次挑战应答。
    if (controller_ && !controller_->savedKeyFor(deviceId).isEmpty()) {
        setStatus(QStringLiteral("正在连接 ") + deviceId +
                  QStringLiteral(" ...（使用已记住的密钥）"));
        emit connectRequested(deviceId, QString(), fileTransferOnly);
        return;
    }

    bool ok = false;
    const QString key = QInputDialog::getText(
        this, QStringLiteral("连接认证"),
        QStringLiteral("请输入设备 %1 的连接密钥\n（在被控端「设置-安全」中设置；"
                       "首次连接成功后会记住，之后不再询问）")
            .arg(selectedDeviceName().isEmpty() ? deviceId : selectedDeviceName()),
        QLineEdit::Password, QString(), &ok);

    if (!ok) return;
    if (key.trimmed().isEmpty()) {
        setStatus(QStringLiteral("必须提供连接密钥才能连接"), true);
        return;
    }

    setStatus(QStringLiteral("正在连接 ") + deviceId + QStringLiteral(" ..."));
    emit connectRequested(deviceId, key.trimmed(), fileTransferOnly);
}

}  // namespace pxc::gui

// SidebarItem / DeviceCard / QuickLaunchBox 定义在本文件且带 Q_OBJECT，
// AUTOMOC 生成的元对象代码在这里编入。
#include "device_page.moc"
