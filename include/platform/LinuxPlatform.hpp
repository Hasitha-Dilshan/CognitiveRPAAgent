#pragma once

#ifdef PLATFORM_LINUX

#include "IPlatform.hpp"
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <string>
#include <memory>

namespace rpa::platform {

/// X11/Xlib-backed implementation of IPlatform. Uses XGetImage for screen
/// capture and the XTest extension for synthetic input injection — the
/// standard, widely-supported approach across X11 window managers.
///
/// Note: Wayland is not covered by this implementation. Most Wayland
/// compositors run an XWayland compatibility layer, under which this
/// class still functions for XWayland-backed windows, but native Wayland
/// windows are outside XTest's reach — a known constraint documented here
/// rather than silently failing later.
class LinuxPlatform : public IPlatform {
public:
    LinuxPlatform();
    ~LinuxPlatform() override;

    LinuxPlatform(const LinuxPlatform&) = delete;
    LinuxPlatform& operator=(const LinuxPlatform&) = delete;

    cv::Mat captureScreen() override;
    cv::Rect getActiveWindowBounds() const override;

    void moveCursorTo(int x, int y) override;
    std::pair<int, int> getCursorPosition() const override;
    void mouseButtonDown(int buttonId) override;
    void mouseButtonUp(int buttonId) override;

    void keyDown(int platformKeyCode) override;
    void keyUp(int platformKeyCode) override;
    void injectUnicodeText(const std::string& utf8Text) override;

    int translateKeyCode(int abstractKeyCode) const override;
    int translateMouseButton(int abstractButtonId) const override;

    /// Binds capture/sandbox operations to a specific top-level window
    /// (by title substring match, via _NET_WM_NAME/WM_NAME) instead of
    /// the full root window. Returns false if no matching window is found.
    bool bindToWindow(const std::string& windowTitleSubstring);

private:
    struct CaptureTarget {
        Window window = 0; // 0 means "root window" (full screen)
        int width = 0;
        int height = 0;
        int originX = 0;
        int originY = 0;
    };
    CaptureTarget resolveCaptureTarget() const;

    /// Recursively searches the window tree under `root` for a window
    /// whose title contains `needle` (case-insensitive).
    Window findWindowByTitle(Window root, const std::string& needle) const;
    std::string getWindowTitle(Window win) const;

    Display* display_ = nullptr; // owned; closed in destructor
    int screen_ = 0;
    Window rootWindow_ = 0;
    Window boundWindow_ = 0; // 0 = unbound (full screen)
    bool xtestAvailable_ = false;
};

} // namespace rpa::platform

#endif // PLATFORM_LINUX