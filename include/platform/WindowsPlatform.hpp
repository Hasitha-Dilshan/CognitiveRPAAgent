#pragma once

#ifdef PLATFORM_WINDOWS

#include "IPlatform.hpp"
#include <windows.h>
#include <string>

namespace rpa::platform {

/// Win32-backed implementation of IPlatform. Uses GDI BitBlt for screen
/// capture and SendInput for synthetic mouse/keyboard events, which is
/// the modern, DPI-aware, driver-level input injection API (preferred
/// over the deprecated mouse_event/keybd_event functions).
class WindowsPlatform : public IPlatform {
public:
    WindowsPlatform();
    ~WindowsPlatform() override;

    // Non-copyable: owns GDI device context handles.
    WindowsPlatform(const WindowsPlatform&) = delete;
    WindowsPlatform& operator=(const WindowsPlatform&) = delete;

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
    /// (by title substring match) instead of the full virtual screen.
    /// Called once during agent startup to establish the sandbox target.
    /// Returns false if no matching window is found.
    bool bindToWindow(const std::string& windowTitleSubstring);

private:
    /// Converts a Windows UTF-16 wide string to UTF-8, since the rest of
    /// the codebase (JSON, OCR, logging) standardizes on UTF-8 internally.
    static std::string wideToUtf8(const std::wstring& wide);
    static std::wstring utf8ToWide(const std::string& utf8);

    /// Resolves the DC and dimensions to capture from — either the bound
    /// window (if set) or the full virtual desktop.
    struct CaptureTarget {
        HWND hwnd = nullptr; // nullptr means "full virtual screen"
        int width = 0;
        int height = 0;
        int originX = 0;
        int originY = 0;
    };
    CaptureTarget resolveCaptureTarget() const;

    HWND boundWindow_ = nullptr; // not owned; nullptr = unbound (full screen)
};

} // namespace rpa::platform

#endif // PLATFORM_WINDOWS