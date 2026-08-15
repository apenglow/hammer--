#include "ToolbarIcons.hpp"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>

// All glyphs are drawn in a 16x16 logical box and scaled to the requested
// rect, using the live application palette so they track theme switches.

namespace {

struct GlyphColors {
    QColor ink;
    QColor accent;
    QColor soft; // translucent ink for fills and secondary strokes
};

QPen stroke(const QColor& color, qreal width = 1.5)
{
    QPen pen(color, width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

void drawGridLines(QPainter& p, const QRectF& r, int cells)
{
    for (int i = 1; i < cells; ++i) {
        const qreal x = r.left() + r.width() * i / cells;
        const qreal y = r.top() + r.height() * i / cells;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
}

void drawEye(QPainter& p, const GlyphColors& c, bool open)
{
    QPainterPath lid;
    lid.moveTo(2, 8);
    lid.cubicTo(5, open ? 3.4 : 8, 11, open ? 3.4 : 8, 14, 8);
    p.setPen(stroke(c.ink));
    p.setBrush(Qt::NoBrush);
    p.drawPath(lid);
    if (open) {
        QPainterPath lower;
        lower.moveTo(2, 8);
        lower.cubicTo(5, 12.6, 11, 12.6, 14, 8);
        p.drawPath(lower);
        p.setBrush(c.accent);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(8, 8), 2.1, 2.1);
    } else {
        p.drawLine(QPointF(4.2, 8.6), QPointF(3.2, 10.6));
        p.drawLine(QPointF(8, 9.2), QPointF(8, 11.4));
        p.drawLine(QPointF(11.8, 8.6), QPointF(12.8, 10.6));
    }
}

void drawPadlock(QPainter& p, const GlyphColors& c, const QRectF& body)
{
    p.setPen(stroke(c.ink, 1.4));
    p.setBrush(Qt::NoBrush);
    const qreal cx = body.center().x();
    const qreal shackleW = body.width() * 0.55;
    p.drawArc(QRectF(cx - shackleW / 2, body.top() - body.height() * 0.75,
                     shackleW, body.height() * 1.1),
              0, 180 * 16);
    p.setBrush(c.soft);
    p.drawRoundedRect(body, 1.2, 1.2);
    p.setBrush(c.ink);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(cx, body.center().y()), 1.0, 1.0);
}

void drawCurvedArrow(QPainter& p, const GlyphColors& c, bool leftward)
{
    p.setPen(stroke(c.ink, 1.7));
    p.setBrush(Qt::NoBrush);
    QRectF arc(3, 4, 10, 10);
    // Open arc with the arrow head at the top end.
    p.drawArc(arc, leftward ? 60 * 16 : -240 * 16, 240 * 16);
    QPainterPath head;
    const QPointF tip = leftward ? QPointF(2.6, 5.4) : QPointF(13.4, 5.4);
    head.moveTo(tip);
    head.lineTo(tip + QPointF(leftward ? 4.2 : -4.2, -1.6));
    head.lineTo(tip + QPointF(leftward ? 1.4 : -1.4, 3.4));
    head.closeSubpath();
    p.setBrush(c.ink);
    p.setPen(Qt::NoPen);
    p.drawPath(head);
}

void drawTerrain(QPainter& p, const GlyphColors& c)
{
    QPainterPath hill;
    hill.moveTo(1.5, 13);
    hill.lineTo(5, 6.5);
    hill.lineTo(8.5, 10);
    hill.lineTo(11.5, 4.5);
    hill.lineTo(14.5, 13);
    p.setPen(stroke(c.ink));
    p.setBrush(Qt::NoBrush);
    p.drawPath(hill);
    p.drawLine(QPointF(1.5, 13), QPointF(14.5, 13));
}

void drawGlyph(const QString& name, QPainter& p, const GlyphColors& c)
{
    p.setRenderHint(QPainter::Antialiasing, true);

    if (name == u"grid2d") {
        const QRectF r(2, 2.5, 12, 11);
        p.setPen(stroke(c.ink, 1.3));
        p.drawRect(r);
        p.setPen(stroke(c.soft, 1.0));
        drawGridLines(p, r, 3);
    } else if (name == u"grid3d") {
        p.setPen(stroke(c.ink, 1.3));
        const QPointF a(3, 5.5), b(9.5, 3), cc(14, 5), d(7.5, 7.5);
        const QPointF a2(3, 11.5), d2(7.5, 13.5), c2(14, 11);
        p.drawPolygon(QPolygonF() << a << b << cc << d);
        p.drawPolygon(QPolygonF() << a << d << d2 << a2);
        p.drawPolygon(QPolygonF() << d << cc << c2 << d2);
        p.setPen(stroke(c.accent, 1.0));
        p.drawLine((a + d) / 2, (b + cc) / 2);
        p.drawLine((a + a2) / 2 + QPointF(4.5, 1), (cc + c2) / 2);
    } else if (name == u"gridSmaller" || name == u"gridLarger") {
        const QRectF r(2, 3, 9, 9);
        p.setPen(stroke(c.ink, 1.2));
        p.drawRect(r);
        p.setPen(stroke(c.soft, 0.9));
        drawGridLines(p, r, name == u"gridSmaller" ? 4 : 2);
        p.setPen(stroke(c.accent, 1.7));
        p.drawLine(QPointF(10.5, 13), QPointF(15, 13));
        if (name == u"gridLarger") {
            p.drawLine(QPointF(12.75, 10.75), QPointF(12.75, 15.25));
        }
    } else if (name == u"loadWindowState" || name == u"saveWindowState") {
        p.setPen(stroke(c.ink, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(2, 3, 12, 10), 1.5, 1.5);
        p.drawLine(QPointF(2, 6), QPointF(14, 6));
        const bool load = name == u"loadWindowState";
        p.setPen(stroke(c.accent, 1.6));
        p.drawLine(QPointF(8, load ? 7.5 : 12), QPointF(8, load ? 12 : 7.5));
        const qreal tipY = load ? 12 : 7.5;
        const qreal dir = load ? -1 : 1;
        p.drawLine(QPointF(8, tipY), QPointF(5.8, tipY + 2.2 * dir));
        p.drawLine(QPointF(8, tipY), QPointF(10.2, tipY + 2.2 * dir));
    } else if (name == u"undo") {
        drawCurvedArrow(p, c, true);
    } else if (name == u"redo") {
        drawCurvedArrow(p, c, false);
    } else if (name == u"carve") {
        QPainterPath rect;
        rect.addRect(QRectF(2, 4, 9.5, 9.5));
        QPainterPath bite;
        bite.addEllipse(QPointF(12.5, 4.5), 4.5, 4.5);
        p.setPen(stroke(c.ink, 1.4));
        p.setBrush(c.soft);
        p.drawPath(rect.subtracted(bite));
        p.setPen(stroke(c.accent, 1.1));
        p.setBrush(Qt::NoBrush);
        QPen dashed = stroke(c.accent, 1.1);
        dashed.setStyle(Qt::DashLine);
        p.setPen(dashed);
        p.drawEllipse(QPointF(12.5, 4.5), 3.4, 3.4);
    } else if (name == u"group" || name == u"ungroup") {
        const bool apart = name == u"ungroup";
        const qreal gap = apart ? 1.6 : 0;
        p.setPen(stroke(c.ink, 1.3));
        p.setBrush(c.soft);
        p.drawRect(QRectF(3 - gap, 3 - gap, 6, 6));
        p.drawRect(QRectF(7 + gap, 7 + gap, 6, 6));
        p.setPen(stroke(c.accent, 1.3));
        p.setBrush(Qt::NoBrush);
        if (!apart) {
            // Selection corners tying the two boxes together.
            p.drawPolyline(QPolygonF() << QPointF(1, 4) << QPointF(1, 1) << QPointF(4, 1));
            p.drawPolyline(QPolygonF() << QPointF(12, 15) << QPointF(15, 15) << QPointF(15, 12));
        } else {
            p.drawLine(QPointF(6.2, 9.8), QPointF(9.8, 6.2));
            p.drawLine(QPointF(6.2, 6.2), QPointF(7.4, 7.4));
            p.drawLine(QPointF(9.8, 9.8), QPointF(8.6, 8.6));
        }
    } else if (name == u"ignoreGroups") {
        p.setPen(stroke(c.ink, 1.3));
        p.setBrush(c.soft);
        p.drawRect(QRectF(2.5, 2.5, 5.5, 5.5));
        p.drawRect(QRectF(8, 8, 5.5, 5.5));
        p.setPen(stroke(c.accent, 1.8));
        p.drawLine(QPointF(3, 13.5), QPointF(13.5, 2.5));
    } else if (name == u"hideSelected" || name == u"hideUnselected") {
        drawEye(p, c, false);
        const bool selected = name == u"hideSelected";
        p.setPen(stroke(selected ? c.accent : c.ink, 1.2));
        p.setBrush(selected ? QBrush(c.accent) : QBrush(Qt::NoBrush));
        p.drawRect(QRectF(11, 1.5, 3.6, 3.6));
    } else if (name == u"quickHide" || name == u"quickHideUnselected") {
        drawEye(p, c, false);
        // Lightning bolt marks the "quick" variants.
        QPainterPath bolt;
        bolt.moveTo(13.2, 0.8);
        bolt.lineTo(10.8, 4.2);
        bolt.lineTo(12.6, 4.2);
        bolt.lineTo(10.6, 7.2);
        p.setPen(stroke(c.accent, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawPath(bolt);
        if (name == u"quickHideUnselected") {
            p.setPen(stroke(c.ink, 1.1));
            p.drawRect(QRectF(1.4, 1.6, 3.2, 3.2));
        }
    } else if (name == u"unhideQuickHide") {
        drawEye(p, c, true);
    } else if (name == u"showAllVisGroups") {
        p.setPen(stroke(c.ink, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(QPolygonF() << QPointF(8, 1.5) << QPointF(14.5, 5) << QPointF(8, 8.5) << QPointF(1.5, 5));
        p.drawPolyline(QPolygonF() << QPointF(1.5, 8.2) << QPointF(8, 11.7) << QPointF(14.5, 8.2));
        p.setPen(stroke(c.accent, 1.3));
        p.drawPolyline(QPolygonF() << QPointF(1.5, 11.2) << QPointF(8, 14.7) << QPointF(14.5, 11.2));
    } else if (name == u"cut") {
        p.setPen(stroke(c.ink, 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(4.5, 10.2), QPointF(12.5, 2.5));
        p.drawLine(QPointF(11.5, 10.2), QPointF(3.5, 2.5));
        p.drawEllipse(QPointF(4.4, 12.4), 2.0, 2.0);
        p.drawEllipse(QPointF(11.6, 12.4), 2.0, 2.0);
    } else if (name == u"copy") {
        p.setPen(stroke(c.ink, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(5.5, 5.5, 8.5, 9), 1.5, 1.5);
        p.setBrush(c.soft);
        p.drawRoundedRect(QRectF(2, 1.5, 8.5, 9), 1.5, 1.5);
    } else if (name == u"paste") {
        p.setPen(stroke(c.ink, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(3, 3, 10, 11.5), 1.5, 1.5);
        p.setBrush(c.soft);
        p.drawRoundedRect(QRectF(5.5, 1.2, 5, 3.4), 1.2, 1.2);
        p.setPen(stroke(c.accent, 1.2));
        p.drawLine(QPointF(5.5, 8), QPointF(10.5, 8));
        p.drawLine(QPointF(5.5, 11), QPointF(9, 11));
    } else if (name == u"cordonToggle" || name == u"cordonEdit") {
        QPen dashed = stroke(c.accent, 1.4);
        dashed.setStyle(Qt::DashLine);
        p.setPen(dashed);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(2, 2.5, 12, 11));
        if (name == u"cordonToggle") {
            p.setPen(stroke(c.ink, 1.2));
            p.setBrush(c.soft);
            p.drawRect(QRectF(5.5, 6, 5, 4.5));
        } else {
            // Pencil across the cordon bounds.
            p.setPen(stroke(c.ink, 1.4));
            p.drawLine(QPointF(5.5, 11.5), QPointF(12.5, 4.5));
            p.drawLine(QPointF(12.5, 4.5), QPointF(13.7, 5.7));
            p.drawLine(QPointF(13.7, 5.7), QPointF(6.7, 12.7));
            p.drawLine(QPointF(6.7, 12.7), QPointF(4.6, 13.4));
            p.drawLine(QPointF(4.6, 13.4), QPointF(5.5, 11.5));
        }
    } else if (name == u"radiusCulling") {
        p.setPen(stroke(c.accent, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(8, 8), 6, 6);
        p.setPen(stroke(c.ink, 1.3));
        p.drawLine(QPointF(8, 8), QPointF(12.3, 3.7));
        p.setBrush(c.ink);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(8, 8), 1.4, 1.4);
    } else if (name == u"selectByHandles") {
        p.setPen(stroke(c.soft, 1.1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(3, 3, 10, 10));
        p.setPen(stroke(c.accent, 1.6));
        p.drawLine(QPointF(6, 6), QPointF(10, 10));
        p.drawLine(QPointF(10, 6), QPointF(6, 10));
        p.setPen(Qt::NoPen);
        p.setBrush(c.ink);
        for (const QPointF& corner : {QPointF(3, 3), QPointF(13, 3), QPointF(3, 13), QPointF(13, 13)}) {
            p.drawRect(QRectF(corner.x() - 1.1, corner.y() - 1.1, 2.2, 2.2));
        }
    } else if (name == u"autoSelect") {
        QPen dashed = stroke(c.ink, 1.2);
        dashed.setStyle(Qt::DashLine);
        p.setPen(dashed);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(2, 2, 9.5, 9.5));
        QPainterPath cursor;
        cursor.moveTo(8.5, 8.5);
        cursor.lineTo(14.5, 10.8);
        cursor.lineTo(11.9, 11.9);
        cursor.lineTo(10.8, 14.5);
        cursor.closeSubpath();
        p.setPen(stroke(c.ink, 1.0));
        p.setBrush(c.accent);
        p.drawPath(cursor);
    } else if (name == u"textureAlign") {
        p.setPen(stroke(c.ink, 1.3));
        p.setBrush(c.soft);
        p.drawPolygon(QPolygonF() << QPointF(2, 4) << QPointF(11, 2) << QPointF(14, 12) << QPointF(5, 14));
        p.setPen(stroke(c.accent, 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(5.4, 8.2), QPointF(10.6, 7.2));
        p.drawLine(QPointF(10.6, 7.2), QPointF(9, 5.9));
        p.drawLine(QPointF(10.6, 7.2), QPointF(9.4, 8.9));
    } else if (name == u"dispMask") {
        drawTerrain(p, c);
        QPen dashed = stroke(c.accent, 1.1);
        dashed.setStyle(Qt::DashLine);
        p.setPen(dashed);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(1.5, 2, 13, 11));
    } else if (name == u"disp3d") {
        drawTerrain(p, c);
        p.setPen(stroke(c.accent, 1.0));
        p.drawLine(QPointF(5, 6.5), QPointF(5, 13));
        p.drawLine(QPointF(8.5, 10), QPointF(8.5, 13));
        p.drawLine(QPointF(11.5, 4.5), QPointF(11.5, 13));
    } else if (name == u"dispWalkable") {
        drawTerrain(p, c);
        p.setPen(stroke(c.accent, 1.7));
        p.drawLine(QPointF(10.4, 1.6), QPointF(12.2, 3.4));
        p.drawLine(QPointF(12.2, 3.4), QPointF(15.2, 0.6));
    } else if (name == u"dispEdgeCollapse") {
        drawTerrain(p, c);
        p.setPen(Qt::NoPen);
        p.setBrush(c.accent);
        for (const QPointF& v : {QPointF(5, 6.5), QPointF(8.5, 10), QPointF(11.5, 4.5)}) {
            p.drawEllipse(v, 1.3, 1.3);
        }
    } else if (name == u"textureLock") {
        drawPadlock(p, c, QRectF(4.5, 7, 7, 6.5));
    } else if (name == u"textureScaleLock") {
        drawPadlock(p, c, QRectF(2.5, 8, 6.5, 6));
        p.setPen(stroke(c.accent, 1.5));
        p.drawLine(QPointF(10.5, 10.5), QPointF(14.7, 6.3));
        p.drawLine(QPointF(14.7, 6.3), QPointF(14.7, 9));
        p.drawLine(QPointF(14.7, 6.3), QPointF(12, 6.3));
        p.drawLine(QPointF(10.5, 10.5), QPointF(10.5, 7.8));
        p.drawLine(QPointF(10.5, 10.5), QPointF(13.2, 10.5));
    } else if (name == u"runMap") {
        QPainterPath play;
        play.moveTo(4.5, 2.5);
        play.lineTo(13.5, 8);
        play.lineTo(4.5, 13.5);
        play.closeSubpath();
        p.setPen(stroke(c.accent, 1.3));
        p.setBrush(c.accent);
        p.drawPath(play);
    } else if (name == u"showHelpers") {
        p.setPen(stroke(c.ink, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(8, 8), 3.2, 3.2);
        QPen dashed = stroke(c.accent, 1.1);
        dashed.setStyle(Qt::DotLine);
        p.setPen(dashed);
        p.drawEllipse(QPointF(8, 8), 6.3, 6.3);
    } else if (name == u"models2d") {
        p.setPen(stroke(c.ink, 1.2));
        p.setBrush(Qt::NoBrush);
        const QPointF a(4, 5.5), b(9, 3.5), cc(12.5, 5.5), d(7.5, 7.5);
        p.drawPolygon(QPolygonF() << a << b << cc << d);
        p.drawPolygon(QPolygonF() << a << d << QPointF(7.5, 12.5) << QPointF(4, 10.5));
        p.drawPolygon(QPolygonF() << d << cc << QPointF(12.5, 10.5) << QPointF(7.5, 12.5));
        p.setPen(stroke(c.accent, 1.0));
        p.drawLine(QPointF(4, 10.5), QPointF(12.5, 5.5));
    } else if (name == u"modelFade") {
        p.setPen(stroke(c.ink, 1.3));
        p.setBrush(c.soft);
        p.drawRect(QRectF(2, 4.5, 6, 7));
        QPen dashed = stroke(c.soft, 1.2);
        dashed.setStyle(Qt::DashLine);
        p.setPen(dashed);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(10, 4.5, 4.5, 7));
    } else if (name == u"collisionWire") {
        p.setPen(stroke(c.ink, 1.3));
        p.setBrush(Qt::NoBrush);
        QPolygonF hull;
        hull << QPointF(8, 1.8) << QPointF(13.8, 5) << QPointF(13.8, 11)
             << QPointF(8, 14.2) << QPointF(2.2, 11) << QPointF(2.2, 5);
        p.drawPolygon(hull);
        p.setPen(stroke(c.soft, 1.0));
        p.drawLine(QPointF(2.2, 5), QPointF(13.8, 11));
        p.drawLine(QPointF(13.8, 5), QPointF(2.2, 11));
        p.drawLine(QPointF(8, 1.8), QPointF(8, 14.2));
    } else if (name == u"detailObjects") {
        p.setPen(stroke(c.ink, 1.3));
        for (int i = 0; i < 3; ++i) {
            const qreal x = 3.5 + i * 4.5;
            p.drawLine(QPointF(x, 13.5), QPointF(x, 8));
            p.drawLine(QPointF(x, 10), QPointF(x - 1.8, 7.2));
            p.drawLine(QPointF(x, 10), QPointF(x + 1.8, 7.2));
        }
        p.setPen(stroke(c.soft, 1.1));
        p.drawLine(QPointF(2, 13.5), QPointF(14, 13.5));
    } else if (name == u"nodraw") {
        p.setPen(stroke(c.ink, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(2.5, 2.5, 11, 11));
        p.setPen(stroke(c.soft, 1.1));
        p.drawLine(QPointF(2.5, 13.5), QPointF(13.5, 2.5));
        p.drawLine(QPointF(2.5, 8), QPointF(8, 2.5));
        p.drawLine(QPointF(8, 13.5), QPointF(13.5, 8));
    } else if (name == u"snapGrid") {
        p.setPen(stroke(c.soft, 1.0));
        drawGridLines(p, QRectF(1, 1, 14, 14), 3);
        // Magnet.
        QPainterPath magnet;
        magnet.moveTo(5, 3.5);
        magnet.arcTo(QRectF(4, 3.5, 8, 9), 180, 180);
        p.setPen(stroke(c.accent, 1.8));
        p.setBrush(Qt::NoBrush);
        p.drawPath(magnet);
        p.setPen(stroke(c.ink, 1.8));
        p.drawLine(QPointF(5, 3.2), QPointF(5, 6));
        p.drawLine(QPointF(11, 3.2), QPointF(11, 6));
    } else {
        // Unknown name: draw a hollow placeholder so a typo is visible.
        p.setPen(stroke(c.accent, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(3, 3, 10, 10));
        p.drawLine(QPointF(3, 3), QPointF(13, 13));
    }
}

class DrawnIconEngine final : public QIconEngine
{
public:
    explicit DrawnIconEngine(QString name) : name_(std::move(name)) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        const QPalette pal = QApplication::palette();
        GlyphColors colors;
        const QPalette::ColorGroup group =
            mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Normal;
        colors.ink = pal.color(group, QPalette::ButtonText);
        colors.accent = mode == QIcon::Disabled ? colors.ink
                                                : pal.color(QPalette::Highlight);
        colors.soft = colors.ink;
        colors.soft.setAlphaF(0.35);

        painter->save();
        painter->translate(rect.topLeft());
        painter->scale(rect.width() / 16.0, rect.height() / 16.0);
        drawGlyph(name_, *painter, colors);
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override
    {
        QPixmap pm(size * scale);
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        QPainter painter(&pm);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

    QIconEngine* clone() const override { return new DrawnIconEngine(name_); }
    QString key() const override { return QStringLiteral("HammerDrawnIcon"); }
    QString iconName() override { return name_; }

private:
    QString name_;
};

} // namespace

namespace ToolbarIcons {
QIcon icon(const QString& name)
{
    return QIcon(new DrawnIconEngine(name));
}
}
