#include "theme.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QtGlobal>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace pxc::gui::theme {
namespace {

QPen symbol_pen(const QColor& color, const QRectF& rect) {
    QPen pen(color, rect.width() * 0.065);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

QPainterPath folder_outline(const QRectF& r) {
    const qreal w = r.width(), h = r.height();
    QPainterPath path;
    path.moveTo(w * 0.16, h * 0.26);
    path.lineTo(w * 0.36, h * 0.26);
    path.quadTo(w * 0.39, h * 0.26, w * 0.42, h * 0.30);
    path.lineTo(w * 0.48, h * 0.36);
    path.lineTo(w * 0.84, h * 0.36);
    path.quadTo(w * 0.90, h * 0.36, w * 0.90, h * 0.42);
    path.lineTo(w * 0.90, h * 0.76);
    path.quadTo(w * 0.90, h * 0.82, w * 0.84, h * 0.82);
    path.lineTo(w * 0.16, h * 0.82);
    path.quadTo(w * 0.10, h * 0.82, w * 0.10, h * 0.76);
    path.lineTo(w * 0.10, h * 0.32);
    path.quadTo(w * 0.10, h * 0.26, w * 0.16, h * 0.26);
    path.closeSubpath();
    return path;
}

QPixmap make_icon(int size, const QColor& color, const std::function<void(QPainter&, const QRectF&)>& draw) {
    // Keep the oversampled pixels instead of shrinking them to a 1x bitmap.
    // Qt selects the correct physical size for 125/150/200% display scaling.
    constexpr int scale = 4;
    QPixmap pm(size * scale, size * scale);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.scale(scale / 2.0, scale / 2.0);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF rect(0, 0, size * 2, size * 2);
    draw(p, rect);
    p.end();
    pm.setDevicePixelRatio(scale);
    return pm;
}

}  // namespace

QString app_stylesheet() {
    // 占位符顺序：%1 文字主色 %2 按钮底色 %3 输入框边 %4 品牌蓝 %5 深底文字
    //             %6 品牌蓝深 %7 危险红 %8 次要文字 %9 悬停底 %10 输入底 %11 选中底
    QString qss = QStringLiteral(R"(
* {
    font-family: "Microsoft YaHei UI", "PingFang SC", "Noto Sans CJK SC", "Segoe UI", sans-serif;
    outline: none;
}
QToolTip {
    background: #1f2329; color: #ffffff; border: none;
    padding: 6px 10px; border-radius: 4px; font-size: 12px;
}
QLabel { color: %1; }

QPushButton {
    background: %2; color: %1; border: 1px solid %3;
    border-radius: 6px; padding: 7px 16px; font-size: 13px;
}
QPushButton:hover { background: #f7f8fa; border-color: #c9ced6; }
QPushButton:pressed { background: #eef1f6; }
QPushButton:disabled { color: #b3b9c2; background: #f5f6f8; }

QPushButton#accentButton {
    background: %4; color: %5; border: none; font-weight: 600;
}
QPushButton#accentButton:hover   { background: %6; }
QPushButton#accentButton:pressed { background: #2b52c8; }
QPushButton#accentButton:disabled { background: #e1e5ec; color: #929ba8; }

QPushButton#dangerButton {
    background: #ffffff; color: %7; border: 1px solid %7;
}
QPushButton#dangerButton:hover { background: #fdecec; }

QPushButton#flatTool {
    background: transparent; border: none; color: %8; padding: 6px 10px;
}
QPushButton#flatTool:hover { background: %9; border-radius: 6px; }

QLineEdit, QComboBox, QSpinBox {
    background: %10; color: %1; border: 1px solid %3;
    border-radius: 6px; padding: 7px 10px; font-size: 13px;
    selection-background-color: %4;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: %4; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView {
    background: white; border: 1px solid %3; border-radius: 6px;
    selection-background-color: %11; selection-color: %1;
}

QMessageBox, QDialog { background: white; }
QScrollBar:vertical {
    background: transparent; width: 8px; margin: 0;
}
QScrollBar::handle:vertical {
    background: #d4d8de; border-radius: 4px; min-height: 30px;
}
QScrollBar::handle:vertical:hover { background: #b9bec6; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
)");
    return qss.arg(QString(kTextPrimary), QString(kCardBg), QString(kInputBorder),
                   QString(kAccent), QString(kTextOnDark), QString(kAccentDeep),
                   QString(kDangerRed), QString(kTextSecondary), QString(kHoverBg),
                   QString(kInputBg), QString(kSelectedBg));
}

// 以下图标保留 4x 像素画布和 DPR=4，避免提前缩成低分辨率位图。

QPixmap icon_device(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        // 外屏
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.08, r.top() + r.height() * 0.18,
                                 r.width() * 0.84, r.height() * 0.52),
                          r.width() * 0.08, r.width() * 0.08);
        // 底座
        p.drawLine(QPointF(r.center().x(), r.top() + r.height() * 0.70),
                   QPointF(r.center().x(), r.top() + r.height() * 0.82));
        p.drawLine(QPointF(r.left() + r.width() * 0.26, r.top() + r.height() * 0.86),
                   QPointF(r.left() + r.width() * 0.74, r.top() + r.height() * 0.86));
    });
}

QPixmap icon_mobile(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const QRectF body(r.left() + r.width() * 0.24, r.top() + r.height() * 0.06,
                          r.width() * 0.52, r.height() * 0.88);
        p.drawRoundedRect(body, r.width() * 0.09, r.width() * 0.09);
        p.drawLine(QPointF(body.left() + r.width() * 0.15, body.top() + r.height() * 0.12),
                   QPointF(body.right() - r.width() * 0.15, body.top() + r.height() * 0.12));
        p.drawEllipse(QPointF(body.center().x(), body.bottom() - r.height() * 0.10),
                      r.width() * 0.025, r.width() * 0.025);
    });
}

QPixmap icon_grid_dots(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        p.setPen(symbol_pen(color, r));
        p.setBrush(Qt::NoBrush);
        const qreal d = r.width() * 0.26;
        const qreal m = (r.width() - 2 * d) / 3.0;
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                const QPointF c(r.left() + d / 2 + m + col * (d + m),
                                r.top() + d / 2 + m + row * (d + m));
                p.drawRoundedRect(QRectF(c.x() - d / 2, c.y() - d / 2, d, d), d * 0.18, d * 0.18);
            }
        }
    });
}

QPixmap icon_assist(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        // 两个互相交叠的圆角方块（协助）
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.10, r.top() + r.height() * 0.10,
                                 r.width() * 0.55, r.height() * 0.55), 3, 3);
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.38, r.top() + r.height() * 0.38,
                                 r.width() * 0.52, r.height() * 0.52), 3, 3);
    });
}

QPixmap icon_star(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPainterPath star;
        const QPointF c = r.center();
        const qreal R = r.width() * 0.42;
        const qreal s = R * 0.45;
        for (int i = 0; i < 10; ++i) {
            const qreal angle = -M_PI / 2 + i * M_PI / 5;
            const qreal rad = (i % 2 == 0) ? R : s;
            const QPointF pt(c.x() + rad * std::cos(angle), c.y() + rad * std::sin(angle));
            if (i == 0) star.moveTo(pt);
            else star.lineTo(pt);
        }
        star.closeSubpath();
        p.setPen(symbol_pen(color, r));
        p.setBrush(Qt::NoBrush);
        p.drawPath(star);
    });
}

QPixmap icon_settings(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        p.setPen(pen);
        const QPointF c = r.center();
        // Eight broad, rounded teeth and a clear center, matching the system
        // symbol language used by the sidebar, settings cards and control tools.
        QVector<QPointF> vertices;
        for (int i = 0; i < 8; ++i) {
            for (const auto& point : {std::pair<qreal, qreal>{-22.5, 0.32},
                                    {-12.0, 0.32}, {-9.0, 0.43},
                                    {9.0, 0.43}, {12.0, 0.32}}) {
                const qreal angle = (i * 45.0 + point.first) * M_PI / 180.0;
                vertices.append(c + QPointF(std::cos(angle), std::sin(angle)) * r.width() * point.second);
            }
        }
        const auto before = [&](int i) {
            return vertices[i] * 0.8 + vertices[(i + vertices.size() - 1) % vertices.size()] * 0.2;
        };
        QPainterPath gear;
        gear.moveTo(before(0));
        for (int i = 0; i < vertices.size(); ++i) {
            const QPointF after = vertices[i] * 0.8 + vertices[(i + 1) % vertices.size()] * 0.2;
            gear.quadTo(vertices[i], after);
            gear.lineTo(before((i + 1) % vertices.size()));
        }
        gear.closeSubpath();
        p.drawPath(gear);
        p.drawEllipse(c, r.width() * 0.14, r.width() * 0.14);
    });
}

QPixmap icon_folder(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        p.setPen(pen);
        p.drawPath(folder_outline(r));
    });
}

QPixmap icon_play(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        p.setPen(pen);
        p.drawRoundedRect(QRectF(r).adjusted(r.width() * 0.08, r.height() * 0.14,
                                             -r.width() * 0.08, -r.height() * 0.14), 3, 3);
        QPainterPath tri;
        const qreal cx = r.center().x() + r.width() * 0.03;
        const qreal cy = r.center().y();
        tri.moveTo(cx - r.width() * 0.10, cy - r.height() * 0.14);
        tri.lineTo(cx - r.width() * 0.10, cy + r.height() * 0.14);
        tri.lineTo(cx + r.width() * 0.13, cy);
        tri.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(tri);
    });
}

QPixmap icon_terminal(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawRoundedRect(r.adjusted(r.width() * 0.08, r.height() * 0.16,
                                    -r.width() * 0.08, -r.height() * 0.16), r.width() * 0.09, r.width() * 0.09);
        // >_
        const QPointF a(r.left() + r.width() * 0.24, r.top() + r.height() * 0.36);
        const QPointF b(r.left() + r.width() * 0.39, r.center().y());
        const QPointF c(r.left() + r.width() * 0.24, r.top() + r.height() * 0.64);
        p.drawLine(a, b);
        p.drawLine(b, c);
        p.drawLine(QPointF(r.left() + r.width() * 0.54, r.top() + r.height() * 0.64),
                   QPointF(r.left() + r.width() * 0.74, r.top() + r.height() * 0.64));
    });
}

QPixmap icon_ports(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawRoundedRect(QRectF(r.width() * 0.12, r.height() * 0.23, r.width() * 0.22, r.height() * 0.54), 3, 3);
        p.drawRoundedRect(QRectF(r.width() * 0.66, r.height() * 0.23, r.width() * 0.22, r.height() * 0.54), 3, 3);
        p.drawLine(QPointF(r.width() * 0.34, r.center().y()), QPointF(r.width() * 0.66, r.center().y()));
    });
}

QPixmap icon_grid9(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        const qreal cell = r.width() * 0.18;
        const qreal gap  = r.width() * 0.09;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                p.drawRoundedRect(QRectF(r.left() + r.width() * 0.14 + col * (cell + gap),
                                         r.top() + r.height() * 0.14 + row * (cell + gap), cell, cell), 2, 2);
            }
        }
    });
}

QPixmap icon_plus(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(r.center().x(), r.top() + r.height() * 0.25),
                   QPointF(r.center().x(), r.bottom() - r.height() * 0.25));
        p.drawLine(QPointF(r.left() + r.width() * 0.25, r.center().y()),
                   QPointF(r.right() - r.width() * 0.25, r.center().y()));
    });
}

QPixmap icon_chevron_up(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(r.left() + r.width() * 0.24, r.top() + r.height() * 0.60),
                   QPointF(r.center().x(), r.top() + r.height() * 0.34));
        p.drawLine(QPointF(r.center().x(), r.top() + r.height() * 0.34),
                   QPointF(r.right() - r.width() * 0.24, r.top() + r.height() * 0.60));
    });
}

QPixmap icon_more(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        for (int i = 0; i < 3; ++i) {
            const qreal y = r.top() + r.height() * (0.25 + i * 0.25);
            p.drawEllipse(QPointF(r.center().x(), y), r.width() * 0.07, r.width() * 0.07);
        }
    });
}

QPixmap icon_refresh(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        const QRectF arc = r.adjusted(r.width() * 0.15, r.height() * 0.15,
                                      -r.width() * 0.15, -r.height() * 0.15);
        p.drawArc(arc, 40 * 16, 280 * 16);
        // 箭头
        QPainterPath tri;
        const QPointF tip(r.right() - r.width() * 0.16, r.top() + r.height() * 0.28);
        tri.moveTo(tip);
        tri.lineTo(tip.x() - r.width() * 0.18, tip.y() - r.height() * 0.02);
        tri.lineTo(tip.x() - r.width() * 0.10, tip.y() + r.height() * 0.14);
        tri.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(tri);
    });
}

QPixmap icon_monitor_small(int size, const QColor& color) {
    return icon_device(size, color);
}

QPixmap icon_power(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawArc(r.adjusted(r.width() * 0.15, r.height() * 0.18,
                             -r.width() * 0.15, -r.height() * 0.15),
                  300 * 16, 300 * 16);
        p.drawLine(QPointF(r.center().x(), r.top() + r.height() * 0.14),
                   QPointF(r.center().x(), r.center().y()));
    });
}

QPixmap icon_copy(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        p.setPen(pen);
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.30, r.top() + r.height() * 0.10,
                                 r.width() * 0.55, r.height() * 0.55), 3, 3);
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.14, r.top() + r.height() * 0.34,
                                 r.width() * 0.55, r.height() * 0.55), 3, 3);
    });
}

QPixmap icon_center(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        const qreal cell = r.width() * 0.34;
        const qreal gap  = r.width() * 0.10;
        const qreal x0 = r.center().x() - cell - gap / 2;
        const qreal y0 = r.center().y() - cell - gap / 2;
        p.drawRoundedRect(QRectF(x0, y0, cell, cell), cell * 0.25, cell * 0.25);
        p.drawRoundedRect(QRectF(x0 + cell + gap, y0, cell, cell), cell * 0.25, cell * 0.25);
        p.drawRoundedRect(QRectF(x0, y0 + cell + gap, cell, cell), cell * 0.25, cell * 0.25);
        p.drawRoundedRect(QRectF(x0 + cell + gap, y0 + cell + gap, cell, cell),
                          cell * 0.25, cell * 0.25);
    });
}

QPixmap icon_pencil(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(r.left() + r.width() * 0.18, r.bottom() - r.height() * 0.18),
                   QPointF(r.right() - r.width() * 0.42, r.top() + r.height() * 0.42));
        p.drawLine(QPointF(r.right() - r.width() * 0.42, r.top() + r.height() * 0.42),
                   QPointF(r.right() - r.width() * 0.26, r.top() + r.height() * 0.26));
        p.drawLine(QPointF(r.right() - r.width() * 0.26, r.top() + r.height() * 0.26),
                   QPointF(r.right() - r.width() * 0.40, r.top() + r.height() * 0.12));
        p.drawLine(QPointF(r.right() - r.width() * 0.40, r.top() + r.height() * 0.12),
                   QPointF(r.left() + r.width() * 0.18, r.bottom() - r.height() * 0.18));
    });
}

QPixmap icon_back(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        QPainterPath path;
        path.moveTo(r.center().x() + r.width() * 0.18, r.top() + r.height() * 0.22);
        path.lineTo(r.left() + r.width() * 0.24, r.center().y());
        path.lineTo(r.center().x() + r.width() * 0.18, r.bottom() - r.height() * 0.22);
        p.drawPath(path);
        p.drawLine(QPointF(r.left() + r.width() * 0.24, r.center().y()),
                   QPointF(r.right() - r.width() * 0.18, r.center().y()));
    });
}

QPixmap icon_keyboard(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        p.setPen(pen);
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.06, r.top() + r.height() * 0.22,
                                 r.width() * 0.88, r.height() * 0.56), 3, 3);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        const qreal key = r.width() * 0.08;
        for (int i = 0; i < 4; ++i) {
            p.drawRoundedRect(QRectF(r.left() + r.width() * 0.14 + i * (key * 1.4),
                                     r.top() + r.height() * 0.34, key, key * 0.6), 1.5, 1.5);
        }
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.14, r.top() + r.height() * 0.56,
                                 r.width() * 0.62, key * 0.6), 1.5, 1.5);
    });
}

QPixmap icon_shield(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        QPainterPath path;
        path.moveTo(r.center().x(), r.top() + r.height() * 0.12);
        path.lineTo(r.right() - r.width() * 0.18, r.top() + r.height() * 0.26);
        path.lineTo(r.right() - r.width() * 0.20, r.center().y() + r.height() * 0.18);
        path.quadTo(r.center().x(), r.bottom() - r.height() * 0.10,
                    r.left() + r.width() * 0.20, r.center().y() + r.height() * 0.18);
        path.lineTo(r.left() + r.width() * 0.18, r.top() + r.height() * 0.26);
        path.closeSubpath();
        p.drawPath(path);
    });
}

QPixmap icon_monitor_lock(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        p.setPen(pen);
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.08, r.top() + r.height() * 0.14,
                                 r.width() * 0.84, r.height() * 0.56), 3, 3);
        p.drawLine(QPointF(r.center().x(), r.top() + r.height() * 0.70),
                   QPointF(r.center().x(), r.top() + r.height() * 0.80));
        p.drawLine(QPointF(r.left() + r.width() * 0.28, r.top() + r.height() * 0.84),
                   QPointF(r.left() + r.width() * 0.72, r.top() + r.height() * 0.84));
        // 锁体
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(r.center().x() - r.width() * 0.10,
                                 r.center().y() - r.height() * 0.04,
                                 r.width() * 0.20, r.height() * 0.16), 2, 2);
        p.setPen(QPen(color, r.width() * 0.05));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(r.center().x() - r.width() * 0.07,
                         r.center().y() - r.height() * 0.14,
                         r.width() * 0.14, r.height() * 0.14), 0, 180 * 16);
    });
}

QPixmap icon_moon(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPainterPath moon;
        moon.addEllipse(QRectF(r.left() + r.width() * 0.16, r.top() + r.height() * 0.12,
                               r.width() * 0.76, r.height() * 0.76));
        QPainterPath cut;
        cut.addEllipse(QRectF(r.left() + r.width() * 0.34, r.top() + r.height() * 0.02,
                              r.width() * 0.72, r.height() * 0.72));
        p.drawPath(moon.subtracted(cut));
    });
}

QPixmap icon_autostart(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        p.setPen(pen);
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.10, r.top() + r.height() * 0.14,
                                 r.width() * 0.80, r.height() * 0.62), 3, 3);
        QPainterPath check;
        check.moveTo(r.center().x() - r.width() * 0.10, r.center().y());
        check.lineTo(r.center().x() - r.width() * 0.02, r.center().y() + r.height() * 0.08);
        check.lineTo(r.center().x() + r.width() * 0.14, r.center().y() - r.height() * 0.10);
        p.drawPath(check);
    });
}

QPixmap icon_moon_sleep(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        p.setPen(pen);
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.08, r.top() + r.height() * 0.16,
                                 r.width() * 0.84, r.height() * 0.54), 3, 3);
        p.drawLine(QPointF(r.center().x(), r.top() + r.height() * 0.70),
                   QPointF(r.center().x(), r.top() + r.height() * 0.80));
        p.drawLine(QPointF(r.left() + r.width() * 0.28, r.top() + r.height() * 0.84),
                   QPointF(r.left() + r.width() * 0.72, r.top() + r.height() * 0.84));
        // 月亮 zZ
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(r.right() - r.width() * 0.24, r.top() + r.height() * 0.30),
                      r.width() * 0.07, r.height() * 0.07);
    });
}

QPixmap icon_sync(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        const QRectF arc = r.adjusted(r.width() * 0.18, r.height() * 0.18,
                                      -r.width() * 0.18, -r.height() * 0.18);
        p.drawArc(arc, 30 * 16, 140 * 16);
        p.drawArc(arc, 210 * 16, 140 * 16);
        QPainterPath a1, a2;
        a1.moveTo(arc.right() - r.width() * 0.02, arc.center().y() - r.height() * 0.16);
        a1.lineTo(arc.right() + r.width() * 0.02, arc.center().y() - r.height() * 0.02);
        a1.lineTo(arc.right() - r.width() * 0.16, arc.center().y() - r.height() * 0.04);
        a2.moveTo(arc.left() + r.width() * 0.02, arc.center().y() + r.height() * 0.16);
        a2.lineTo(arc.left() - r.width() * 0.02, arc.center().y() + r.height() * 0.02);
        a2.lineTo(arc.left() + r.width() * 0.16, arc.center().y() + r.height() * 0.04);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(a1);
        p.drawPath(a2);
    });
}

QPixmap icon_folder_down(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        p.setPen(pen);
        p.drawPath(folder_outline(r));
        // 向下箭头
        p.drawLine(QPointF(r.center().x(), r.top() + r.height() * 0.50),
                   QPointF(r.center().x(), r.top() + r.height() * 0.66));
        QPainterPath tri;
        tri.moveTo(r.center().x() - r.width() * 0.06, r.top() + r.height() * 0.60);
        tri.lineTo(r.center().x() + r.width() * 0.06, r.top() + r.height() * 0.60);
        tri.lineTo(r.center().x(), r.top() + r.height() * 0.70);
        tri.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(tri);
    });
}

QPixmap icon_window(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        p.setPen(pen);
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.10, r.top() + r.height() * 0.16,
                                 r.width() * 0.80, r.height() * 0.68), 3, 3);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(r.left() + r.width() * 0.22, r.top() + r.height() * 0.28),
                      r.width() * 0.035, r.width() * 0.035);
        p.drawEllipse(QPointF(r.left() + r.width() * 0.34, r.top() + r.height() * 0.28),
                      r.width() * 0.035, r.width() * 0.035);
    });
}

QPixmap icon_network(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        p.setPen(pen);
        p.drawEllipse(r.center(), r.width() * 0.36, r.height() * 0.36);
        p.drawEllipse(r.center(), r.width() * 0.16, r.height() * 0.36);
        p.drawLine(QPointF(r.left() + r.width() * 0.10, r.center().y()),
                   QPointF(r.right() - r.width() * 0.10, r.center().y()));
        p.drawArc(QRectF(r.center().x() - r.width() * 0.24, r.top() + r.height() * 0.14,
                         r.width() * 0.48, r.height() * 0.30), 0, 180 * 16);
        p.drawArc(QRectF(r.center().x() - r.width() * 0.24, r.bottom() - r.height() * 0.44,
                         r.width() * 0.48, r.height() * 0.30), 180 * 16, 180 * 16);
    });
}

QPixmap icon_eye(int size, const QColor& color) {
    return make_icon(size, color, [&](QPainter& p, const QRectF& r) {
        QPen pen = symbol_pen(color, r);
        p.setPen(pen);
        QPainterPath eye;
        eye.moveTo(r.left() + r.width() * 0.10, r.center().y());
        eye.quadTo(r.center().x(), r.top() + r.height() * 0.10,
                   r.right() - r.width() * 0.10, r.center().y());
        eye.quadTo(r.center().x(), r.bottom() - r.height() * 0.10,
                   r.left() + r.width() * 0.10, r.center().y());
        p.drawPath(eye);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(r.center(), r.width() * 0.10, r.height() * 0.10);
    });
}

QPixmap device_card_background(int width, int height) {
    QPixmap pm(width, height);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 深蓝夜色渐变 + 柔和光斑，模仿 UU 远程设备卡的深色照片质感
    QLinearGradient gradient(0, 0, width, height);
    gradient.setColorAt(0.0, QColor("#233a5c"));
    gradient.setColorAt(0.5, QColor("#1b2a45"));
    gradient.setColorAt(1.0, QColor("#141d31"));
    p.fillRect(0, 0, width, height, gradient);

    QRadialGradient glow(QPointF(width * 0.72, height * 0.30), width * 0.5);
    glow.setColorAt(0.0, QColor(120, 160, 255, 70));
    glow.setColorAt(1.0, QColor(120, 160, 255, 0));
    p.fillRect(0, 0, width, height, glow);

    QRadialGradient glow2(QPointF(width * 0.15, height * 0.85), width * 0.4);
    glow2.setColorAt(0.0, QColor(64, 201, 158, 46));
    glow2.setColorAt(1.0, QColor(64, 201, 158, 0));
    p.fillRect(0, 0, width, height, glow2);

    // 细网格点，增加质感
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 14));
    for (int y = 18; y < height; y += 26) {
        for (int x = 18; x < width; x += 26) {
            p.drawEllipse(QPointF(x, y), 1.2, 1.2);
        }
    }
    p.end();
    return pm;
}

}  // namespace pxc::gui::theme
