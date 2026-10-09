#pragma once

// UI 主题：颜色、全局样式表与手绘图标。
//
// 设计语言模仿「网易UU远程」：浅灰页面、白色侧栏、圆角卡片、
// 品牌蓝点缀、绿色在线点。所有图标用 QPainter 画，不引入图片资源，
// Windows/Linux 表现一致。

#include <QColor>
#include <QPixmap>
#include <QString>

namespace pxc::gui::theme {

// ---- 调色板 ----
inline constexpr const char* kPageBg        = "#f5f6f8";
inline constexpr const char* kSidebarBg     = "#ffffff";
inline constexpr const char* kCardBg        = "#ffffff";
inline constexpr const char* kTextPrimary   = "#1f2329";
inline constexpr const char* kTextSecondary = "#8a9099";
inline constexpr const char* kTextOnDark    = "#ffffff";
inline constexpr const char* kAccent        = "#4f7cff";
inline constexpr const char* kAccentDeep    = "#3562e8";
inline constexpr const char* kOnlineGreen   = "#26c281";
inline constexpr const char* kOfflineGray   = "#c2c7cc";
inline constexpr const char* kDangerRed     = "#e5484d";
inline constexpr const char* kDivider       = "#eceef1";
inline constexpr const char* kHoverBg       = "#f2f3f5";
inline constexpr const char* kSelectedBg    = "#eef1f6";
inline constexpr const char* kInputBg       = "#ffffff";
inline constexpr const char* kInputBorder   = "#dde1e6";

inline QColor color(const char* name) { return QColor(QString(name)); }

// ---- 全局样式表（main.cpp 里 app.setStyleSheet）----
QString app_stylesheet();

// ---- macOS 风格系统符号（统一圆头线条/圆角，颜色可指定）----
QPixmap icon_device(int size, const QColor& color);    // 显示器（设备）
QPixmap icon_mobile(int size, const QColor& color);    // 手机/平板
QPixmap icon_grid_dots(int size, const QColor& color); // 2x2 圆角方格（全部设备）
QPixmap icon_assist(int size, const QColor& color);    // 远程协助
QPixmap icon_star(int size, const QColor& color);      // 收藏设备
QPixmap icon_settings(int size, const QColor& color);  // 设置（齿轮）
QPixmap icon_folder(int size, const QColor& color);    // 文件传输
QPixmap icon_play(int size, const QColor& color);      // 观看模式
QPixmap icon_terminal(int size, const QColor& color);  // 终端 >_
QPixmap icon_ports(int size, const QColor& color);     // 端口映射 <O>
QPixmap icon_grid9(int size, const QColor& color);     // 九宫格（应用）
QPixmap icon_plus(int size, const QColor& color);      // +
QPixmap icon_chevron_up(int size, const QColor& color);    // ^
QPixmap icon_more(int size, const QColor& color);          // ⋮
QPixmap icon_refresh(int size, const QColor& color);       // ⟳
QPixmap icon_monitor_small(int size, const QColor& color); // 小屏幕（控制中心）
QPixmap icon_power(int size, const QColor& color);         // 断开
QPixmap icon_copy(int size, const QColor& color);          // 复制
QPixmap icon_center(int size, const QColor& color);        // 控制中心（四宫格）

// ---- 设置页图标 ----
QPixmap icon_pencil(int size, const QColor& color);        // 编辑（铅笔）
QPixmap icon_back(int size, const QColor& color);          // 返回 ←
QPixmap icon_keyboard(int size, const QColor& color);      // 键盘
QPixmap icon_shield(int size, const QColor& color);        // 安全/验证码
QPixmap icon_monitor_lock(int size, const QColor& color);  // 允许控制本设备
QPixmap icon_moon(int size, const QColor& color);          // 防窥/隐私
QPixmap icon_autostart(int size, const QColor& color);     // 开机自启
QPixmap icon_moon_sleep(int size, const QColor& color);    // 防止休眠
QPixmap icon_sync(int size, const QColor& color);          // 自动更新
QPixmap icon_folder_down(int size, const QColor& color);   // 文件路径
QPixmap icon_window(int size, const QColor& color);        // 关闭窗口时
QPixmap icon_network(int size, const QColor& color);       // 网络
QPixmap icon_eye(int size, const QColor& color);           // 拖拽发送浮窗

// 深色设备卡片底图（模仿 UU 远程设备卡的深色照片质感）
QPixmap device_card_background(int width, int height);

}  // namespace pxc::gui::theme
