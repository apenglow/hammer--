#pragma once

// A ray-traced frame is produced synchronously on the GUI thread, and a scene
// carrying detail props is heavy enough that a steady stream of them starves
// everything else in the process. That is what makes the Open dialog take
// seconds to appear and then miss mouse input.
//
// Suspending on QApplication::activeModalWidget() is not enough: the file
// dialog is the platform's native one. Under GNOME/Wayland that is the GTK or
// xdg-desktop-portal chooser, which is not a QWidget - Qt reports no active
// modal widget at all, so the preview keeps rendering right through it. The
// callers that open such a dialog therefore say so explicitly, by holding a
// PreviewRenderSuspension for as long as the dialog is up.

namespace hammer::app {

namespace detail {
inline int previewRenderSuspendCount = 0;
}

// True while any caller is holding a PreviewRenderSuspension. GUI thread only:
// the counter is deliberately not atomic, because every dialog that needs it
// blocks the GUI thread by definition.
inline bool previewRenderingSuspended()
{
    return detail::previewRenderSuspendCount > 0;
}

// Scoped: hold one across a blocking call that must own the GUI thread.
class PreviewRenderSuspension
{
public:
    PreviewRenderSuspension() { ++detail::previewRenderSuspendCount; }
    ~PreviewRenderSuspension() { --detail::previewRenderSuspendCount; }

    PreviewRenderSuspension(const PreviewRenderSuspension&) = delete;
    PreviewRenderSuspension& operator=(const PreviewRenderSuspension&) = delete;
};

} // namespace hammer::app
