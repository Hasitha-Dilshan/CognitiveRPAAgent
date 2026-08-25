#pragma once

#include <string>
#include <utility>
#include <opencv2/core.hpp>

namespace rpa::platform {

/// Platform abstraction boundary. Every OS-specific system call in this
/// entire project must go through an implementation of this interface —
/// WindowsPlatform.cpp (Win32 API) or LinuxPlatform.cpp (X11/Xlib).
/// No other file in the codebase should contain a single #ifdef _WIN32
/// or #ifdef __linux__ outside of these two implementation files and the
/// CMake build configuration.
class IPlatform {
public:
    virtual ~IPlatform() = default;

    // --- Screen capture -----------------------------------------------
    /// Captures the current contents of the sandboxed target window (or
    /// full screen if no window handle is bound) as a BGR cv::Mat.
    virtual cv::Mat captureScreen() = 0;

    /// Returns the bounding rectangle of the currently sandboxed
    /// application window, used by SecurityLayer::checkWithinSandbox().
    virtual cv::Rect getActiveWindowBounds() const = 0;

    // --- Mouse -----------------------------------------------------------
    virtual void moveCursorTo(int x, int y) = 0;
    virtual std::pair<int, int> getCursorPosition() const = 0;
    virtual void mouseButtonDown(int buttonId) = 0;
    virtual void mouseButtonUp(int buttonId) = 0;

    // --- Keyboard -------------------------------------------------------
    virtual void keyDown(int platformKeyCode) = 0;
    virtual void keyUp(int platformKeyCode) = 0;
    virtual void injectUnicodeText(const std::string& utf8Text) = 0;

    // --- Key/button code translation ------------------------------------
    /// Translates an OS-agnostic action::KeyCode into this platform's
    /// native key code (VK_* on Windows, XKeysym on Linux). Declared here
    /// so InputController never needs platform-specific headers.
    virtual int translateKeyCode(int abstractKeyCode) const = 0;
    virtual int translateMouseButton(int abstractButtonId) const = 0;
};

} // namespace rpa::platform