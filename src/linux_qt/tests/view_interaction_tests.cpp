// Interaction-level tests for MapViewWidget's 2D selection tool: synthetic
// mouse events drive the widget on the offscreen platform, and the emitted
// signals are compared between a plain brush and a point entity. The selection
// tool must treat both identically: click selects, click-on-selected cycles
// the transform handles instead of reselecting, drag moves.
#include "MapViewWidget.hpp"
#include "VmfScene.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>

#include <cstdio>
#include <memory>
#include <vector>

namespace {
int failures = 0;

void require(bool condition, const char* message)
{
    if (condition) {
        std::printf("ok - %s\n", message);
    } else {
        std::printf("FAIL - %s\n", message);
        ++failures;
    }
}

void sendClick(QWidget& widget, const QPointF& position)
{
    QMouseEvent press(QEvent::MouseButtonPress, position, widget.mapToGlobal(position.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, position, widget.mapToGlobal(position.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &release);
}

void sendDrag(QWidget& widget, const QPointF& from, const QPointF& to)
{
    QMouseEvent press(QEvent::MouseButtonPress, from, widget.mapToGlobal(from.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &press);
    QMouseEvent move(QEvent::MouseMove, to, widget.mapToGlobal(to.toPoint()),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, widget.mapToGlobal(to.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &release);
}

hammer::vmf::BrushGeometry makeBox(int id, double minX, double minY, double minZ,
                                   double maxX, double maxY, double maxZ)
{
    hammer::vmf::BrushGeometry brush;
    brush.object = {hammer::vmf::ObjectType::Solid, id};
    brush.id = id;
    brush.vertices = {
        {minX, minY, minZ}, {maxX, minY, minZ}, {maxX, maxY, minZ}, {minX, maxY, minZ},
        {minX, minY, maxZ}, {maxX, minY, maxZ}, {maxX, maxY, maxZ}, {minX, maxY, maxZ},
    };
    brush.edges = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4},
                   {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    return brush;
}

struct SignalLog {
    std::vector<hammer::vmf::ObjectRef> selections;
    int cycles = 0;
    MapViewWidget::TransformMode lastCycleMode{MapViewWidget::TransformMode::Scale};
    int moveStarts = 0;
    int moveDeltas = 0;
    int moveFinishes = 0;
    int clears = 0;

    void reset()
    {
        selections.clear();
        cycles = moveStarts = moveDeltas = moveFinishes = clears = 0;
    }
};

void attach(MapViewWidget& view, SignalLog& log)
{
    QObject::connect(&view, &MapViewWidget::selectionRequested, &view,
                     [&log](const hammer::vmf::ObjectRef& object, bool, bool) {
        log.selections.push_back(object);
    });
    QObject::connect(&view, &MapViewWidget::transformModeChangeRequested, &view,
                     [&log](MapViewWidget::TransformMode mode) {
        ++log.cycles;
        log.lastCycleMode = mode;
    });
    QObject::connect(&view, &MapViewWidget::moveStarted, &view, [&log] { ++log.moveStarts; });
    QObject::connect(&view, &MapViewWidget::moveDeltaRequested, &view,
                     [&log](const hammer::vmf::Vec3&) { ++log.moveDeltas; });
    QObject::connect(&view, &MapViewWidget::moveFinished, &view, [&log] { ++log.moveFinishes; });
    QObject::connect(&view, &MapViewWidget::clearSelectionRequested, &view, [&log] { ++log.clears; });
}

// Runs the shared click/cycle/drag scenario against one object and reports
// with a per-object label so brush and entity failures are distinguishable.
void runScenario(MapViewWidget& view, SignalLog& log, const hammer::vmf::ObjectRef& object,
                 const hammer::vmf::Bounds& bounds, const QPointF& center, const char* label)
{
    char message[160];

    view.setSelection({}, {});
    log.reset();
    sendClick(view, center);
    std::snprintf(message, sizeof message, "%s: first click selects it", label);
    require(log.selections.size() == 1 && log.selections.front() == object && log.cycles == 0,
            message);

    // A point entity skips Scale: its Scale state renders as Translate, so a
    // click from Scale advances to Rotate rather than Translate.
    const bool pointEntity = object.type == hammer::vmf::ObjectType::Entity;
    view.setSelection({object}, bounds);
    view.setTransformMode(MapViewWidget::TransformMode::Scale);
    log.reset();
    sendClick(view, center);
    std::snprintf(message, sizeof message,
                  "%s: clicking it while selected cycles handles instead of reselecting", label);
    require(log.selections.empty() && log.cycles == 1, message);
    std::snprintf(message, sizeof message, "%s: cycle from Scale lands on %s", label,
                  pointEntity ? "Rotate" : "Translate");
    require(log.lastCycleMode == (pointEntity ? MapViewWidget::TransformMode::Rotate
                                              : MapViewWidget::TransformMode::Translate),
            message);

    view.setTransformMode(MapViewWidget::TransformMode::Translate);
    log.reset();
    sendClick(view, center);
    std::snprintf(message, sizeof message, "%s: cycle from Translate lands on Rotate", label);
    require(log.cycles == 1 && log.lastCycleMode == MapViewWidget::TransformMode::Rotate, message);

    view.setTransformMode(MapViewWidget::TransformMode::Scale);
    log.reset();
    sendDrag(view, center, center + QPointF(48.0, 0.0));
    std::snprintf(message, sizeof message,
                  "%s: dragging it while selected moves it without reselecting or cycling", label);
    const bool dragBehaved = log.selections.empty() && log.cycles == 0 && log.moveStarts == 1 &&
                             log.moveDeltas >= 1 && log.moveFinishes == 1;
    if (!dragBehaved) {
        std::printf("   (selections %zu, cycles %d, moveStarts %d, moveDeltas %d, moveFinishes %d, clears %d)\n",
                    log.selections.size(), log.cycles, log.moveStarts, log.moveDeltas,
                    log.moveFinishes, log.clears);
    }
    require(dragBehaved, message);
}
} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);

    auto scene = std::make_shared<hammer::vmf::Scene>();
    scene->brushes.push_back(makeBox(1, -96.0, -96.0, 0.0, -32.0, -32.0, 64.0));
    hammer::vmf::EntityMarker entity;
    entity.id = 2;
    entity.object = {hammer::vmf::ObjectType::Entity, 2};
    entity.classname = "info_target";
    entity.origin = {128.0, 128.0, 0.0};
    scene->entities.push_back(entity);

    MapViewWidget view(MapViewWidget::Kind::Top);
    view.resize(800, 600);
    view.setScene(scene, false);
    view.setTool(MapViewWidget::Tool::Selection);
    view.setGridSnapEnabled(false);
    // Kill the initial auto-fit so screen positions follow the identity
    // mapping (center + world * zoom) the click coordinates below assume.
    view.resetView();

    SignalLog log;
    attach(view, log);

    // Top view: screen = center + (x, -y) * zoom, zoom 1, center (400, 300).
    const QPointF brushCenter(400.0 - 64.0, 300.0 + 64.0);
    hammer::vmf::Bounds brushBounds;
    brushBounds.valid = true;
    brushBounds.minimum = {-96.0, -96.0, 0.0};
    brushBounds.maximum = {-32.0, -32.0, 64.0};
    runScenario(view, log, scene->brushes.front().object, brushBounds, brushCenter, "brush");

    // The default (FGD-less) entity box is corner-anchored at the origin and
    // spans sizeMinimum..sizeMaximum; aim for the middle of the drawn box.
    // QRect::center() of an 800x600 widget is (399, 299).
    const QPointF entityCenter(399.0 + 136.0, 299.0 - 136.0);
    hammer::vmf::Bounds entityBounds;
    entityBounds.valid = true;
    entityBounds.minimum = entity.origin;
    entityBounds.maximum = entity.origin;
    runScenario(view, log, entity.object, entityBounds, entityCenter, "point entity");

    // Dragging a box in empty space selects every object wholly inside it.
    {
        view.resetView();
        view.setSelection({}, {});
        std::vector<hammer::vmf::ObjectRef> boxed;
        int boxEmissions = 0;
        QObject::connect(&view, &MapViewWidget::boxSelectionRequested, &view,
                         [&](const std::vector<hammer::vmf::ObjectRef>& objects, bool) {
            ++boxEmissions;
            boxed = objects;
        });
        // The brush projects to roughly (304, 332)-(368, 396) in this view.
        sendDrag(view, QPointF(280.0, 310.0), QPointF(400.0, 420.0));
        require(boxEmissions == 1, "a drag in empty space asks the document for a box selection");
        require(boxed.size() == 1 && boxed.front() == scene->brushes.front().object,
                "a box drawn around a brush selects it");

        // Touching is enough: a box that only cuts across the brush takes it.
        // (A drag started inside the brush is a move, not a box select, so a box
        // can only ever begin on empty space.)
        boxed.clear();
        sendDrag(view, QPointF(280.0, 310.0), QPointF(340.0, 420.0));
        require(boxed.size() == 1 && boxed.front() == scene->brushes.front().object,
                "a box crossing part of a brush selects it");
        // A box well clear of everything still selects nothing.
        boxed.clear();
        sendDrag(view, QPointF(600.0, 450.0), QPointF(700.0, 550.0));
        require(boxed.empty(), "a box that touches nothing selects nothing");

        // A box over the whole view takes the brush and the point entity both.
        boxed.clear();
        sendDrag(view, QPointF(20.0, 20.0), QPointF(780.0, 580.0));
        require(boxed.size() == 2, "a box over everything selects the brush and the entity");
    }

    // Block tool: the pending box is a world-space box shared by every 2D view,
    // so the two dimensions the drawing view cannot show are visible — and
    // resizable — in the others before Enter commits it.
    {
        MapViewWidget top(MapViewWidget::Kind::Top);
        top.resize(800, 600);
        top.setScene(scene, false);
        top.setGridSnapEnabled(false);
        top.resetView();
        top.setTool(MapViewWidget::Tool::Block);

        MapViewWidget front(MapViewWidget::Kind::Front);
        front.resize(800, 600);
        front.setScene(scene, false);
        front.setGridSnapEnabled(false);
        front.resetView();
        front.setTool(MapViewWidget::Tool::Block);

        // The relay the document performs between the views.
        QObject::connect(&top, &MapViewWidget::blockPreviewChanged, &front,
                         [&front](const hammer::vmf::Bounds& bounds, int axis) {
            front.setPendingBlock(bounds, axis);
        });

        // Drag out a box in the Top view: screen (400, 300) is world (1, 1).
        sendDrag(top, QPointF(420.0, 260.0), QPointF(500.0, 200.0));
        const hammer::vmf::Bounds pending = top.pendingBlock();
        require(pending.valid, "dragging in a 2D view leaves a pending box");
        require(std::abs(pending.maximum.x - pending.minimum.x - 80.0) <= 1.0 &&
                std::abs(pending.maximum.y - pending.minimum.y - 60.0) <= 1.0,
                "the pending box takes its two drawn dimensions from the drag");
        require(pending.maximum.z - pending.minimum.z >= 1.0,
                "the pending box has a depth along the axis the drawing view cannot show");
        require(top.pendingBlockExtrusionAxis() == 2,
                "a box drawn in the Top view extrudes along z");

        // The Front view now shows the same box, and can resize its depth.
        require(front.pendingBlock().valid, "the other 2D views show the same pending box");
        const double depthBefore = front.pendingBlock().maximum.z - front.pendingBlock().minimum.z;
        hammer::vmf::Bounds resized;
        QObject::connect(&front, &MapViewWidget::blockPreviewChanged, &front,
                         [&resized](const hammer::vmf::Bounds& bounds, int) { resized = bounds; });
        // Grab the box's top edge in the Front view and drag it up 40 pixels.
        const hammer::vmf::Bounds shown = front.pendingBlock();
        // Front view: screen x = 399 + y, screen y = 299 - z.
        const QPointF topEdge((399.0 + (shown.minimum.y + shown.maximum.y) * 0.5),
                              299.0 - shown.maximum.z);
        sendDrag(front, topEdge, topEdge - QPointF(0.0, 40.0));
        require(resized.valid, "dragging a handle in another view updates the pending box");
        require(std::abs((resized.maximum.z - resized.minimum.z) - (depthBefore + 40.0)) <= 1.0,
                "resizing in another view scales the dimension the drawing view could not show");
        require(std::abs(resized.maximum.x - shown.maximum.x) <= 0.001 &&
                std::abs(resized.minimum.x - shown.minimum.x) <= 0.001,
                "resizing in one view leaves the axes it does not draw alone");

        // Dragging the box's body moves it, like dragging a selected object.
        const hammer::vmf::Bounds beforeMove = front.pendingBlock();
        const QPointF bodyCenter((399.0 + (beforeMove.minimum.y + beforeMove.maximum.y) * 0.5),
                                 299.0 - (beforeMove.minimum.z + beforeMove.maximum.z) * 0.5);
        sendDrag(front, bodyCenter, bodyCenter + QPointF(24.0, -16.0));
        const hammer::vmf::Bounds afterMove = front.pendingBlock();
        require(std::abs((afterMove.minimum.y - beforeMove.minimum.y) - 24.0) <= 1.0 &&
                std::abs((afterMove.minimum.z - beforeMove.minimum.z) - 16.0) <= 1.0,
                "dragging the pending box's body moves it");
        require(std::abs((afterMove.maximum.y - afterMove.minimum.y) -
                         (beforeMove.maximum.y - beforeMove.minimum.y)) <= 0.001 &&
                std::abs((afterMove.maximum.z - afterMove.minimum.z) -
                         (beforeMove.maximum.z - beforeMove.minimum.z)) <= 0.001,
                "moving the pending box keeps its size");
        require(std::abs(afterMove.minimum.x - beforeMove.minimum.x) <= 0.001,
                "moving the pending box leaves the axis this view does not draw alone");
        require(resized.valid && std::abs(resized.minimum.y - afterMove.minimum.y) <= 0.001,
                "the move is relayed to the other views");

        // Enter in the view that only received the box still commits it, with
        // the extrusion axis of the view that drew it.
        int committedAxis = -1;
        int commits = 0;
        QObject::connect(&front, &MapViewWidget::blockCreationRequested, &front,
                         [&](const hammer::vmf::Vec3&, const hammer::vmf::Vec3&, int axis) {
            ++commits;
            committedAxis = axis;
        });
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(&front, &enter);
        require(commits == 1, "Enter commits the pending box from any 2D view");
        require(committedAxis == 2,
                "the committed box keeps the extrusion axis of the view that drew it");
        require(!front.pendingBlock().valid, "committing clears the pending box");
    }

    // Find Entities (and Center on Selection) bring a world point to the middle
    // of the view without disturbing the zoom.
    {
        const hammer::vmf::Vec3 target{512.0, -256.0, 32.0};
        view.centerOnWorldPoint(target, 64.0);
        const auto centered = view.viewCenterWorld();
        require(centered.has_value(), "a 2D view reports the world point at its middle");
        require(std::abs(centered->x - target.x) <= 1.0 && std::abs(centered->y - target.y) <= 1.0,
                "centering puts the requested point in the middle of the view");
        // Rounding pan to whole pixels is the only error allowed; the zoom is
        // untouched, so centering never re-frames the map.
        const auto reCentered = (view.centerOnWorldPoint(target, 64.0), view.viewCenterWorld());
        require(reCentered.has_value() && std::abs(reCentered->x - centered->x) <= 0.001 &&
                std::abs(reCentered->y - centered->y) <= 0.001,
                "centering twice on the same point is stable");
    }

    if (failures == 0) std::printf("all view interaction tests passed\n");
    else std::printf("%d view interaction test(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
