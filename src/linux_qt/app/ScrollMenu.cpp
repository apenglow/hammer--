#include "ScrollMenu.hpp"

#include <QMenuBar>
#include <QScreen>
#include <QShowEvent>

#if HAMMER_HAVE_QMENU_PRIVATE
#include <private/qmenu_p.h>
#endif

void ScrollMenu::showEvent(QShowEvent* event)
{
    QMenu::showEvent(event);
#if HAMMER_HAVE_QMENU_PRIVATE

    // Only reposition dropdowns opened from a menubar; context/submenu popups
    // keep stock behaviour.
    auto* d = QMenuPrivate::get(this);
    auto* bar = qobject_cast<QMenuBar*>(d->causedPopup.widget);
    if (!bar || !d->scroll) return;

    const QRect itemRect = bar->actionGeometry(menuAction());
    if (itemRect.isNull()) return;
    const int anchorY = bar->mapToGlobal(itemRect.bottomLeft()).y() + 1;

    // Wayland reports fictional global coordinates (the window "at" 0,0), so
    // both Qt's own fit-below math and any screen-based math here are off by
    // the real titlebar/panel heights — Qt places the menu, then the
    // compositor slides it up over the menubar anyway. Window-local
    // coordinates ARE consistent on every platform, so clamp the dropdown to
    // the main window's own extent: a popup that fits inside its parent
    // window can never be constraint-slid by the compositor. Cap by the
    // screen too for the X11 case of a window hanging off the bottom edge.
    const QWidget* topLevel = bar->window();
    const int windowBottom = topLevel->mapToGlobal(QPoint(0, topLevel->height())).y();
    int bottomLimit = windowBottom;
    if (QScreen* onScreen = screen())
        bottomLimit = qMin(bottomLimit, onScreen->availableGeometry().bottom());
    const int spaceBelow = qMax(120, bottomLimit - anchorY);

    if (sizeHint().height() <= spaceBelow) {
        // Fits below the menubar; just undo any slide-up Qt performed.
        if (y() < anchorY) move(x(), anchorY);
        return;
    }

    // Anchor below the menubar item and clamp to the space that is really
    // available. Setting ScrollDown with a zero offset is exactly the state
    // QMenu::popup itself creates for taller-than-screen menus, so painting,
    // hover auto-scroll, wheel, and key navigation all behave natively from
    // here. (This also re-clamps the case where Qt engaged its own scroller
    // against the fictional screen geometry.)
    d->scroll->scrollOffset = 0;
    d->scroll->scrollFlags = QMenuPrivate::QMenuScroller::ScrollDown;
    setGeometry(x(), anchorY, width(), spaceBelow);
    update();
#endif
}
