#include "toggle_switch.h"

#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>

#include "theme.h"

namespace pxc::gui {
using namespace theme;

ToggleSwitch::ToggleSwitch(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setFixedSize(64, 26);
}

void ToggleSwitch::set_on(bool on) {
    if (on_ == on) return;
    on_ = on;
    update();
    emit toggled(on_);
}

void ToggleSwitch::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 左侧「开/关」文字
    p.setPen(on_ ? color(kAccent) : color(kTextSecondary));
    QFont font = this->font();
    font.setPointSizeF(10.0);
    p.setFont(font);
    p.drawText(QRect(0, 0, 26, height()), Qt::AlignVCenter | Qt::AlignRight,
               on_ ? QStringLiteral("开") : QStringLiteral("关"));

    // 轨道
    const QRectF track(30, (height() - 22) / 2.0, 30, 22);
    p.setPen(Qt::NoPen);
    p.setBrush(on_ ? color(kAccent) : color(kOfflineGray));
    p.drawRoundedRect(track, 11, 11);

    // 滑块（带一点白色描边，模仿 UU 的浮起感）
    const qreal x = on_ ? track.right() - 10.5 : track.left() + 10.5;
    p.setBrush(Qt::white);
    p.drawEllipse(QPointF(x, track.center().y()), 9.0, 9.0);
}

void ToggleSwitch::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        set_on(!on_);
    }
    QWidget::mouseReleaseEvent(event);
}

}  // namespace pxc::gui
