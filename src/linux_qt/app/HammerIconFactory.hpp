#pragma once

#include <QIcon>
#include <QString>

namespace HammerIconFactory {
QIcon fromStrip(const QString& resourcePath, int index, int cellWidth, int cellHeight);
}
