#include "HammerIconFactory.hpp"

#include <QColor>
#include <QImage>
#include <QPixmap>

namespace HammerIconFactory {
QIcon fromStrip(const QString& resourcePath, int index, int cellWidth, int cellHeight)
{
    QImage strip(resourcePath);
    if (strip.isNull() || cellWidth <= 0 || cellHeight <= 0 || index < 0) {
        return {};
    }

    const int x = index * cellWidth;
    if (x + cellWidth > strip.width() || cellHeight > strip.height()) {
        return {};
    }

    QImage tile = strip.copy(x, 0, cellWidth, cellHeight).convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < tile.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(tile.scanLine(y));
        for (int px = 0; px < tile.width(); ++px) {
            const QColor color = QColor::fromRgba(line[px]);
            if (color.red() > 245 && color.green() < 12 && color.blue() > 245) {
                line[px] = qRgba(color.red(), color.green(), color.blue(), 0);
            }
        }
    }

    return QIcon(QPixmap::fromImage(tile));
}
}
