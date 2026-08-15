#pragma once

#include <QIcon>
#include <QString>

namespace ToolbarIcons {
// Returns a vector icon drawn on demand with the current application palette,
// so the glyphs follow light/dark theme switches and stay crisp on HiDPI.
QIcon icon(const QString& name);
}
