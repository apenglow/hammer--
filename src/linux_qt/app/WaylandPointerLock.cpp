#include "WaylandPointerLock.hpp"
#include "MapViewWidget.hpp"

#include <QGuiApplication>
#include <QMetaObject>
#include <QTimer>
#include <QWindow>
#include <QWidget>

#ifdef HAMMER_HAVE_WAYLAND_CAPTURE
#ifdef HAMMER_WAYLAND_SURFACE_FROM_QT_PRIVATE
#include <QtGui/private/qplatformwindow_p.h>
#endif
#include <wayland-client.h>
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cstring>

namespace {
const wl_registry_listener RegistryListener{
    &WaylandPointerLock::registryGlobal,
    &WaylandPointerLock::registryGlobalRemove,
};

const zwp_locked_pointer_v1_listener LockedPointerListener{
    &WaylandPointerLock::pointerLocked,
    &WaylandPointerLock::pointerUnlocked,
};

const zwp_relative_pointer_v1_listener RelativePointerListener{
    &WaylandPointerLock::relativeMotion,
};
} // namespace
#endif

WaylandPointerLock::WaylandPointerLock(MapViewWidget* owner)
    : owner_(owner)
{
}

WaylandPointerLock::~WaylandPointerLock()
{
    end();
}

bool WaylandPointerLock::begin(QWidget* viewport)
{
#ifdef HAMMER_HAVE_WAYLAND_CAPTURE
    if (active_) return true;
    if (!viewport || !owner_) return false;
    if (!QGuiApplication::platformName().contains(QStringLiteral("wayland"),
                                                   Qt::CaseInsensitive)) return false;

    auto* waylandApplication = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    QWindow* qtWindow = viewport->window()->windowHandle();
    if (!waylandApplication || !qtWindow) return false;

#ifdef HAMMER_WAYLAND_SURFACE_FROM_WINID
    // Qt 6.9+ exposes the QWaylandWindow wl_surface through QWindow::winId().
    // This avoids a dependency on Qt's versioned private development headers.
    surface_ = reinterpret_cast<wl_surface*>(qtWindow->winId());
#elif defined(HAMMER_WAYLAND_SURFACE_FROM_QT_PRIVATE)
    auto* waylandWindow = qtWindow->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!waylandWindow) return false;
    surface_ = waylandWindow->surface();
#else
    return false;
#endif

    display_ = waylandApplication->display();
    compositor_ = waylandApplication->compositor();
    pointer_ = waylandApplication->pointer();
    viewport_ = viewport;
    if (!display_ || !compositor_ || !pointer_ || !surface_) {
        destroyProtocolObjects();
        return false;
    }

    active_ = true;
    locked_ = false;
    ending_ = false;
    const std::uint64_t attempt = ++attempt_;
    if (!requestGlobals()) {
        end();
        return false;
    }

    // Qt owns and dispatches this wl_display. Never perform a blocking
    // wl_display_roundtrip() on it; the registry and lock callbacks arrive on
    // the normal Qt event loop. Fall back to Qt's soft grab only when the
    // compositor does not confirm the lock within a reasonable interval.
    QTimer::singleShot(1000, owner_, [this, attempt] {
        if (active_ && attempt_ == attempt && !locked_) {
            end();
            if (owner_) owner_->handleWaylandPointerLockUnavailable();
        }
    });
    wl_display_flush(display_);
    return true;
#else
    Q_UNUSED(viewport);
    return false;
#endif
}

void WaylandPointerLock::end()
{
#ifdef HAMMER_HAVE_WAYLAND_CAPTURE
    if (!active_ && !registry_ && !constraints_ && !relativeManager_) return;
    ending_ = true;
    // Wayland forbids clients from warping the pointer directly. The locked
    // pointer protocol provides a compositor hint for the position restored
    // when the lock is released, so keep that at the camera viewport center.
    centerCursor(viewport_);
    destroyProtocolObjects();
    ending_ = false;
#endif
    ++attempt_;
    active_ = false;
    locked_ = false;
    viewport_ = nullptr;
    regionRect_ = {};
}

void WaylandPointerLock::updateRegion(QWidget* viewport)
{
#ifdef HAMMER_HAVE_WAYLAND_CAPTURE
    if (!active_ || !lockedPointer_ || !viewport || !compositor_) return;
    const QRect next = surfaceRegion(viewport);
    if (next == regionRect_) return;
    regionRect_ = next;

    wl_region* region = wl_compositor_create_region(compositor_);
    if (!region) return;
    wl_region_add(region, regionRect_.x(), regionRect_.y(),
                  std::max(1, regionRect_.width()), std::max(1, regionRect_.height()));
    zwp_locked_pointer_v1_set_region(lockedPointer_, region);
    wl_region_destroy(region);
    centerCursor(viewport);
#else
    Q_UNUSED(viewport);
#endif
}

void WaylandPointerLock::centerCursor(QWidget* viewport)
{
#ifdef HAMMER_HAVE_WAYLAND_CAPTURE
    if (!active_ || !lockedPointer_ || !viewport || !display_) return;
    viewport_ = viewport;
    regionRect_ = surfaceRegion(viewport);
    const QPoint center = regionRect_.center();
    zwp_locked_pointer_v1_set_cursor_position_hint(
        lockedPointer_, wl_fixed_from_int(center.x()), wl_fixed_from_int(center.y()));
    // Cursor hints are double-buffered protocol state. Commit immediately so
    // the center hint is in effect before a Z-camera lock is destroyed.
    if (surface_) wl_surface_commit(surface_);
    wl_display_flush(display_);
#else
    Q_UNUSED(viewport);
#endif
}

#ifdef HAMMER_HAVE_WAYLAND_CAPTURE
bool WaylandPointerLock::requestGlobals()
{
    registry_ = wl_display_get_registry(display_);
    if (!registry_) return false;
    return wl_registry_add_listener(registry_, &RegistryListener, this) == 0;
}

bool WaylandPointerLock::createConstraint()
{
    if (!active_ || lockedPointer_ || !constraints_ || !relativeManager_ ||
        !pointer_ || !surface_ || !compositor_ || !viewport_) return false;

    relativePointer_ =
        zwp_relative_pointer_manager_v1_get_relative_pointer(relativeManager_, pointer_);
    if (!relativePointer_) return false;
    zwp_relative_pointer_v1_add_listener(relativePointer_, &RelativePointerListener, this);

    regionRect_ = surfaceRegion(viewport_);
    wl_region* region = wl_compositor_create_region(compositor_);
    if (!region) return false;
    wl_region_add(region, regionRect_.x(), regionRect_.y(),
                  std::max(1, regionRect_.width()), std::max(1, regionRect_.height()));
    lockedPointer_ = zwp_pointer_constraints_v1_lock_pointer(
        constraints_, surface_, pointer_, region,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    wl_region_destroy(region);
    if (!lockedPointer_) return false;
    zwp_locked_pointer_v1_add_listener(lockedPointer_, &LockedPointerListener, this);
    centerCursor(viewport_);
    wl_display_flush(display_);
    return true;
}

QRect WaylandPointerLock::surfaceRegion(QWidget* viewport) const
{
    // Constraint coordinates are local to the top-level wl_surface. Qt widget
    // coordinates already exclude the window manager frame, so frame margins
    // must not be added here.
    QWidget* topLevel = viewport->window();
    return QRect(viewport->mapTo(topLevel, QPoint(0, 0)), viewport->size());
}

void WaylandPointerLock::destroyProtocolObjects()
{
    if (lockedPointer_) {
        zwp_locked_pointer_v1_destroy(lockedPointer_);
        lockedPointer_ = nullptr;
    }
    if (relativePointer_) {
        zwp_relative_pointer_v1_destroy(relativePointer_);
        relativePointer_ = nullptr;
    }
    if (constraints_) {
        zwp_pointer_constraints_v1_destroy(constraints_);
        constraints_ = nullptr;
    }
    if (relativeManager_) {
        zwp_relative_pointer_manager_v1_destroy(relativeManager_);
        relativeManager_ = nullptr;
    }
    if (registry_) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }
    if (display_) wl_display_flush(display_);
    display_ = nullptr;
    surface_ = nullptr;
    pointer_ = nullptr;
    compositor_ = nullptr;
}

void WaylandPointerLock::registryGlobal(void* data, wl_registry* registry, unsigned int name,
                                        const char* interface, unsigned int version)
{
    auto* self = static_cast<WaylandPointerLock*>(data);
    if (!self || !interface) return;

    if (std::strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        self->constraints_ = static_cast<zwp_pointer_constraints_v1*>(wl_registry_bind(
            registry, name, &zwp_pointer_constraints_v1_interface, std::min(version, 1u)));
    } else if (std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        self->relativeManager_ = static_cast<zwp_relative_pointer_manager_v1*>(wl_registry_bind(
            registry, name, &zwp_relative_pointer_manager_v1_interface,
            std::min(version, 1u)));
    }
    if (self->constraints_ && self->relativeManager_ && !self->lockedPointer_) {
        self->createConstraint();
    }
}

void WaylandPointerLock::registryGlobalRemove(void*, wl_registry*, unsigned int)
{
}

void WaylandPointerLock::pointerLocked(void* data, zwp_locked_pointer_v1*)
{
    auto* self = static_cast<WaylandPointerLock*>(data);
    if (!self) return;
    self->locked_ = true;
    self->centerCursor(self->viewport_);
}

void WaylandPointerLock::pointerUnlocked(void* data, zwp_locked_pointer_v1*)
{
    auto* self = static_cast<WaylandPointerLock*>(data);
    if (!self) return;
    self->locked_ = false;
    if (!self->ending_ && self->owner_) {
        QMetaObject::invokeMethod(self->owner_, [owner = self->owner_] {
            owner->handleWaylandPointerLockLost();
        }, Qt::QueuedConnection);
    }
}

void WaylandPointerLock::relativeMotion(void* data, zwp_relative_pointer_v1*,
                                        unsigned int, unsigned int,
                                        int dx, int dy, int dxUnaccelerated,
                                        int dyUnaccelerated)
{
    auto* self = static_cast<WaylandPointerLock*>(data);
    if (!self || !self->active_ || !self->owner_) return;

    Q_UNUSED(dx);
    Q_UNUSED(dy);
    const double relativeX = wl_fixed_to_double(dxUnaccelerated);
    const double relativeY = wl_fixed_to_double(dyUnaccelerated);
    self->owner_->applyCapturedMouseDelta(relativeX, relativeY);
}
#endif
