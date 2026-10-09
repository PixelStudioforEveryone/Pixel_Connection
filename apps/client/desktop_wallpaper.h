#pragma once

#include <QImage>
#include <QSize>
#include <QString>

namespace pxc::gui {

// Read the configured primary-monitor wallpaper. Never capture screen pixels.
// Called on a worker thread; display size is supplied by the GUI thread.
QImage read_desktop_wallpaper(QSize displaySize, QString* error = nullptr);

}  // namespace pxc::gui
