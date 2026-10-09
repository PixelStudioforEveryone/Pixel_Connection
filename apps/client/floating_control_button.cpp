#include "floating_control_button.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QVariantAnimation>

#include "theme.h"

namespace pxc::gui {
using namespace theme;
namespace {

constexpr qreal kShadowSpread = 10;

}  // namespace

FloatingControlButton::FloatingControlButton(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    resize(kFloatSize, kFloatSize);
}

QSize FloatingControlButton::docked_size(Edge edge) const {
    const bool hover = hovered_;
    const int  thick = hover ? kDockThickHover : kDockThick;
    if (edge == Edge::Left || edge == Edge::Right) return QSize(thick, kDockLength);
    return QSize(kDockLength, thick);
}

QRect FloatingControlButton::docked_rect(Edge edge) const {
    const QSize  size = docked_size(edge);
    const QRect  parent_rect = parentWidget() ? parentWidget()->rect() : QRect(0, 0, 800, 600);
    const QPoint last = last_floating_pos_ + QPoint(kFloatSize / 2, kFloatSize / 2);

    switch (edge) {
        case Edge::Left:
            return QRect(0, qBound(parent_rect.top() + 8,
                                   last.y() - size.height() / 2,
                                   parent_rect.bottom() - size.height() - 8),
                         size.width(), size.height());
        case Edge::Right:
            return QRect(parent_rect.right() - size.width() + 1,
                         qBound(parent_rect.top() + 8,
                                last.y() - size.height() / 2,
                                parent_rect.bottom() - size.height() - 8),
                         size.width(), size.height());
        case Edge::Top:
            return QRect(qBound(parent_rect.left() + 8,
                                last.x() - size.width() / 2,
                                parent_rect.right() - size.width() - 8),
                         0, size.width(), size.height());
        case Edge::Bottom:
            return QRect(qBound(parent_rect.left() + 8,
                                last.x() - size.width() / 2,
                                parent_rect.right() - size.width() - 8),
                         parent_rect.bottom() - size.height() + 1,
                         size.width(), size.height());
        default:
            break;
    }
    return geometry();
}

void FloatingControlButton::apply_geometry(const QRect& rect) {
    setGeometry(rect);
    update();
}

void FloatingControlButton::animate_to(const QRect& target) {
    animation_ = std::make_unique<QVariantAnimation>();
    animation_->setDuration(160);
    animation_->setStartValue(geometry());
    animation_->setEndValue(target);
    animation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation_.get(), &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { apply_geometry(value.toRect()); });
    animation_->start();
}

FloatingControlButton::Edge FloatingControlButton::nearest_edge() const {
    const QRect parent_rect = parentWidget() ? parentWidget()->rect() : QRect(0, 0, 800, 600);
    const QRect self        = geometry();

    const int left_dist   = self.left();
    const int right_dist  = parent_rect.right() - self.right();
    const int top_dist    = self.top();
    const int bottom_dist = parent_rect.bottom() - self.bottom();

    int   best = left_dist;
    Edge  edge = Edge::Left;
    if (right_dist < best) { best = right_dist; edge = Edge::Right; }
    if (top_dist < best)   { best = top_dist;    edge = Edge::Top; }
    if (bottom_dist < best){ best = bottom_dist; edge = Edge::Bottom; }

    return best <= kSnapPixels ? edge : Edge::None;
}

void FloatingControlButton::dock(Edge edge) {
    if (edge == Edge::None) return;
    mode_       = Mode::Docked;
    dock_edge_  = edge;
    animate_to(docked_rect(edge));
}

void FloatingControlButton::undock() {
    mode_      = Mode::Floating;
    dock_edge_ = Edge::None;
    // QPoint 没有大小比较，x/y 分别钳制
    QPoint pos = last_floating_pos_;
    if (parentWidget()) {
        const QSize parent_size = parentWidget()->size();
        pos.setX(qBound(0, pos.x(), qMax(0, parent_size.width() - kFloatSize)));
        pos.setY(qBound(0, pos.y(), qMax(0, parent_size.height() - kFloatSize)));
    }
    animate_to(QRect(pos, QSize(kFloatSize, kFloatSize)));
}

void FloatingControlButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF area = rect();

    if (mode_ == Mode::Floating) {
        const QRectF ball = area.adjusted(kShadowSpread / 2, kShadowSpread / 2,
                                          -kShadowSpread / 2, -kShadowSpread / 2);

        // 阴影
        QRadialGradient shadow(ball.center(), ball.width() / 2 + kShadowSpread);
        shadow.setColorAt(0.55, QColor(20, 30, 60, 80));
        shadow.setColorAt(1.0, QColor(20, 30, 60, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(shadow);
        p.drawEllipse(ball.adjusted(-kShadowSpread, -kShadowSpread,
                                    kShadowSpread, kShadowSpread));

        // 球体：品牌蓝渐变
        QLinearGradient gradient(ball.topLeft(), ball.bottomRight());
        gradient.setColorAt(0.0, QColor("#5d8bff"));
        gradient.setColorAt(1.0, QColor(kAccentDeep));
        p.setBrush(gradient);
        p.setPen(QPen(QColor(255, 255, 255, 60), 1.2));
        p.drawEllipse(ball);

        // 控制中心图标：白色四宫格
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255));
        const qreal cell = ball.width() * 0.17;
        const qreal gap  = ball.width() * 0.055;
        const QPointF c  = ball.center();
        const qreal x0   = c.x() - cell - gap / 2;
        const qreal y0   = c.y() - cell - gap / 2;
        p.drawRoundedRect(QRectF(x0, y0, cell, cell), cell * 0.28, cell * 0.28);
        p.drawRoundedRect(QRectF(x0 + cell + gap, y0, cell, cell), cell * 0.28, cell * 0.28);
        p.drawRoundedRect(QRectF(x0, y0 + cell + gap, cell, cell), cell * 0.28, cell * 0.28);
        p.drawRoundedRect(QRectF(x0 + cell + gap, y0 + cell + gap, cell, cell),
                          cell * 0.28, cell * 0.28);
        return;
    }

    // 贴边小按钮：半埋的圆角胶囊 + 呼吸感的白色小点
    const QColor base = hovered_ ? QColor(kAccent) : QColor(90, 125, 235, 200);
    p.setPen(Qt::NoPen);
    p.setBrush(base);
    const qreal radius = (dock_edge_ == Edge::Left || dock_edge_ == Edge::Right)
                             ? area.width() / 2.0 : area.height() / 2.0;
    p.drawRoundedRect(area, radius, radius);

    p.setBrush(QColor(255, 255, 255, hovered_ ? 235 : 170));
    const qreal d = 3.6;
    if (dock_edge_ == Edge::Left || dock_edge_ == Edge::Right) {
        p.drawEllipse(QPointF(area.center().x(), area.center().y()), d, d);
    } else {
        p.drawEllipse(QPointF(area.center().x(), area.center().y()), d, d);
    }
}

void FloatingControlButton::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    dragging_    = false;
    press_local_ = event->position();
    press_global_ = event->globalPosition().toPoint();
    if (mode_ == Mode::Docked) {
        // 从贴边状态拖出：立刻回到悬浮球跟随鼠标
        mode_ = Mode::Floating;
        dock_edge_ = Edge::None;
        const QPoint center = mapToParent(event->position().toPoint());
        move(center - QPoint(kFloatSize / 2, kFloatSize / 2));
        resize(kFloatSize, kFloatSize);
        update();
    }
}

void FloatingControlButton::mouseMoveEvent(QMouseEvent* event) {
    if (mode_ == Mode::Docked) {
        hovered_ = true;
        setGeometry(docked_rect(dock_edge_));
        update();
        return;
    }
    if (!(event->buttons() & Qt::LeftButton)) return;

    if (!dragging_) {
        const QPointF delta = event->position() - press_local_;
        if ((event->globalPosition().toPoint() - press_global_).manhattanLength() < 6) {
            return;
        }
        dragging_ = true;
        Q_UNUSED(delta);
    }

    const QRect parent_rect = parentWidget() ? parentWidget()->rect() : QRect(0, 0, 800, 600);
    QPoint center = mapToParent(event->position().toPoint());
    center.setX(qBound(parent_rect.left() + 4, center.x(), parent_rect.right() - 4));
    center.setY(qBound(parent_rect.top() + 4, center.y(), parent_rect.bottom() - 4));
    move(center - QPoint(kFloatSize / 2, kFloatSize / 2));
}

void FloatingControlButton::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    if (dragging_) {
        dragging_ = false;
        last_floating_pos_ = pos();
        const Edge edge = nearest_edge();
        if (edge != Edge::None) {
            dock(edge);
        }
        return;
    }
    // 单击
    if (mode_ == Mode::Docked) {
        last_floating_pos_ = pos();
        undock();
    }
    emit activated();
}

void FloatingControlButton::enterEvent(QEnterEvent*) {
    hovered_ = true;
    if (mode_ == Mode::Docked) {
        setGeometry(docked_rect(dock_edge_));
    }
    update();
}

void FloatingControlButton::leaveEvent(QEvent*) {
    hovered_ = false;
    if (mode_ == Mode::Docked) {
        setGeometry(docked_rect(dock_edge_));
    }
    update();
}

void FloatingControlButton::resizeEvent(QResizeEvent*) {
    update();
}

}  // namespace pxc::gui
