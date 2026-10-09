#pragma once

// 悬浮控制按钮（远程控制界面的「控制中心」入口）。
//
// 行为（跨平台，Win/Linux 一致）：
//   - 56px 圆形按钮，可用鼠标拖动到窗口内任意位置；
//   - 拖到（或释放时靠近）窗口边缘 32px 内自动「贴边」：变成一枚半埋
//     在屏幕边缘的小竖条按钮，避免遮挡远程画面；
//   - 贴边状态下鼠标移上去会略微探出；单击（不拖动）则还原成悬浮圆形
//     按钮并发出 activated()（由外层弹出控制中心面板）。
//
// 实现说明：QVariantAnimation 平滑过渡；坐标全部相对父控件（视频画布）。

#include <memory>
#include <QWidget>

#include <QVariantAnimation>

namespace pxc::gui {

class FloatingControlButton : public QWidget {
    Q_OBJECT

public:
    explicit FloatingControlButton(QWidget* parent = nullptr);

    QSize sizeHint() const override { return QSize(kFloatSize, kFloatSize); }

signals:
    // 单击（非拖动）时触发：贴边状态会先还原成悬浮球
    void activated();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class Mode { Floating, Docked };
    enum class Edge { None, Left, Right, Top, Bottom };

    void dock(Edge edge);
    void undock();
    Edge nearest_edge() const;
    void animate_to(const QRect& target);
    QRect docked_rect(Edge edge) const;
    QSize docked_size(Edge edge) const;
    void apply_geometry(const QRect& rect);

    static constexpr int kFloatSize    = 56;
    static constexpr int kDockThick    = 10;   // 贴边小按钮露出宽度
    static constexpr int kDockThickHover = 16;
    static constexpr int kDockLength   = 44;
    static constexpr int kSnapPixels   = 32;   // 距边缘多少像素吸附

    Mode mode_            = Mode::Floating;
    Edge dock_edge_       = Edge::None;
    Edge hover_dock_edge_ = Edge::None;
    bool dragging_        = false;
    bool hovered_         = false;
    QPointF press_local_;
    QPoint  press_global_;
    QPoint  last_floating_pos_ = {120, 120};
    std::unique_ptr<QVariantAnimation> animation_;
};

}  // namespace pxc::gui
