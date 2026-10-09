#pragma once

// UU 远程风格的开关（iOS 样式）：右侧圆角轨道 + 滑块，左侧显示「开/关」文字。

#include <QWidget>

namespace pxc::gui {

class ToggleSwitch : public QWidget {
    Q_OBJECT

public:
    explicit ToggleSwitch(QWidget* parent = nullptr);

    bool is_on() const { return on_; }
    void set_on(bool on);

signals:
    void toggled(bool on);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    QSize sizeHint() const override { return QSize(64, 26); }

private:
    bool on_ = false;
};

}  // namespace pxc::gui
