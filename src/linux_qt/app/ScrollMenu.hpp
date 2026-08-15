#pragma once

#include <QMenu>

// A menubar dropdown that scrolls instead of sliding up over the menubar.
//
// Stock QMenu only engages its internal scroller when the menu is taller than
// the whole screen; a menu that merely doesn't fit below the menubar is moved
// up to fit (QMenu::popup's off-screen correction runs before its scroll
// branch). ScrollMenu re-anchors itself below its menubar item after Qt has
// placed it, clamps its height to the space that is actually available, and
// turns on QMenu's own scroller state so the usual scroll arrows, hover
// auto-scroll, wheel, and keyboard navigation all work.
//
// This reaches into QMenuPrivate (see ScrollMenu.cpp); the AppImage bundles
// the exact Qt build this is compiled against, so the private layout matches.
class ScrollMenu final : public QMenu
{
public:
    using QMenu::QMenu;

protected:
    void showEvent(QShowEvent* event) override;
};
