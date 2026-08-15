#pragma once

#include <QRect>

#include <cstdint>

class MapViewWidget;
class QWidget;

struct wl_display;
struct wl_registry;
struct wl_surface;
struct wl_pointer;
struct wl_compositor;
struct zwp_pointer_constraints_v1;
struct zwp_locked_pointer_v1;
struct zwp_relative_pointer_manager_v1;
struct zwp_relative_pointer_v1;

// Native Wayland pointer confinement for Z-camera mode. The class deliberately
// uses Qt's existing Wayland connection and surface: opening a second display
// connection would not own the pointer focus attached to the editor window.
class WaylandPointerLock final
{
public:
    explicit WaylandPointerLock(MapViewWidget* owner);
    ~WaylandPointerLock();

    WaylandPointerLock(const WaylandPointerLock&) = delete;
    WaylandPointerLock& operator=(const WaylandPointerLock&) = delete;

    bool begin(QWidget* viewport);
    void end();
    void updateRegion(QWidget* viewport);
    void centerCursor(QWidget* viewport);

    bool active() const { return active_; }
    bool locked() const { return locked_; }

private:
    MapViewWidget* owner_{nullptr};
    QWidget* viewport_{nullptr};
    bool active_{false};
    bool locked_{false};
    bool ending_{false};
    QRect regionRect_;
    std::uint64_t attempt_{0};

#ifdef HAMMER_HAVE_WAYLAND_CAPTURE
    wl_display* display_{nullptr};
    wl_registry* registry_{nullptr};
    wl_surface* surface_{nullptr};
    wl_pointer* pointer_{nullptr};
    wl_compositor* compositor_{nullptr};
    zwp_pointer_constraints_v1* constraints_{nullptr};
    zwp_locked_pointer_v1* lockedPointer_{nullptr};
    zwp_relative_pointer_manager_v1* relativeManager_{nullptr};
    zwp_relative_pointer_v1* relativePointer_{nullptr};

public:
    static void registryGlobal(void* data, wl_registry* registry, unsigned int name,
                               const char* interface, unsigned int version);
    static void registryGlobalRemove(void* data, wl_registry* registry, unsigned int name);
    static void pointerLocked(void* data, zwp_locked_pointer_v1* pointer);
    static void pointerUnlocked(void* data, zwp_locked_pointer_v1* pointer);
    static void relativeMotion(void* data, zwp_relative_pointer_v1* pointer,
                               unsigned int timeHi, unsigned int timeLo,
                               int dx, int dy, int dxUnaccelerated, int dyUnaccelerated);

private:
    bool requestGlobals();
    bool createConstraint();
    QRect surfaceRegion(QWidget* viewport) const;
    void destroyProtocolObjects();
#endif
};
