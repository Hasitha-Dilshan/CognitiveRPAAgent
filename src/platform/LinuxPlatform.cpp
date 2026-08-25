#ifdef PLATFORM_LINUX

#include "platform/LinuxPlatform.hpp"
#include "action/InputController.hpp"
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cctype>

namespace rpa::platform {

namespace {
    /// RAII wrapper around XImage* returned by XGetImage, ensuring
    /// XDestroyImage is always called even on early-exit/exception paths.
    class ScopedXImage {
    public:
        explicit ScopedXImage(XImage* img) : img_(img) {}
        ~ScopedXImage() { if (img_) XDestroyImage(img_); }

        ScopedXImage(const ScopedXImage&) = delete;
        ScopedXImage& operator=(const ScopedXImage&) = delete;

        XImage* get() const { return img_; }
        explicit operator bool() const { return img_ != nullptr; }

    private:
        XImage* img_;
    };

    std::string toLowerCopy(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                        [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    /// X11 error handler installed for the lifetime of the process.
    /// Xlib's default handler calls exit() on unhandled errors, which
    /// would take down the entire agent for a single bad request (e.g.
    /// querying a window that closed between calls). We instead record
    /// the error and let calling code decide how to react.
    thread_local bool g_lastCallHadXError = false;
    thread_local std::string g_lastXErrorText;

    int xErrorHandler(Display* dpy, XErrorEvent* event) {
        char buf[256];
        XGetErrorText(dpy, event->error_code, buf, sizeof(buf));
        g_lastCallHadXError = true;
        g_lastXErrorText = buf;
        return 0; // must return an int; value is ignored by Xlib
    }
}

LinuxPlatform::LinuxPlatform() {
    XSetErrorHandler(xErrorHandler);

    display_ = XOpenDisplay(nullptr); // nullptr = use $DISPLAY env var
    if (!display_) {
        throw std::runtime_error(
            "LinuxPlatform: XOpenDisplay failed — no X server available "
            "(is DISPLAY set? is this running under a headless session without Xvfb?)");
    }

    screen_ = DefaultScreen(display_);
    rootWindow_ = RootWindow(display_, screen_);

    int xtestEventBase, xtestErrorBase, xtestMajor, xtestMinor;
    xtestAvailable_ = XTestQueryExtension(
        display_, &xtestEventBase, &xtestErrorBase, &xtestMajor, &xtestMinor);
    if (!xtestAvailable_) {
        XCloseDisplay(display_);
        display_ = nullptr;
        throw std::runtime_error(
            "LinuxPlatform: XTest extension unavailable on this X server — "
            "input synthesis cannot function. Install/enable the XTest extension.");
    }
}

LinuxPlatform::~LinuxPlatform() {
    if (display_) {
        XCloseDisplay(display_);
    }
}

std::string LinuxPlatform::getWindowTitle(Window win) const {
    // Prefer the modern EWMH _NET_WM_NAME (UTF-8) property; fall back to
    // the legacy WM_NAME if the window manager/app doesn't set it.
    Atom netWmName = XInternAtom(display_, "_NET_WM_NAME", False);
    Atom utf8String = XInternAtom(display_, "UTF8_STRING", False);

    Atom actualType;
    int actualFormat;
    unsigned long itemCount, bytesAfter;
    unsigned char* data = nullptr;

    g_lastCallHadXError = false;
    const int status = XGetWindowProperty(
        display_, win, netWmName, 0, 1024, False, utf8String,
        &actualType, &actualFormat, &itemCount, &bytesAfter, &data);

    std::string result;
    if (status == Success && data && itemCount > 0) {
        result.assign(reinterpret_cast<char*>(data), itemCount);
        XFree(data);
        return result;
    }
    if (data) XFree(data);

    // Fallback: legacy WM_NAME (Latin-1/ASCII typically).
    XTextProperty textProp;
    if (XGetWMName(display_, win, &textProp) && textProp.value) {
        result.assign(reinterpret_cast<char*>(textProp.value), textProp.nitems);
        XFree(textProp.value);
    }
    return result;
}

Window LinuxPlatform::findWindowByTitle(Window root, const std::string& needle) const {
    const std::string needleLower = toLowerCopy(needle);

    // Check this window first.
    const std::string title = getWindowTitle(root);
    if (!title.empty() && toLowerCopy(title).find(needleLower) != std::string::npos) {
        return root;
    }

    // Recurse into children (X11 window trees can be deep; this is a
    // standard bounded depth-first search bounded by the actual window
    // tree, which in practice never exceeds a few hundred nodes).
    Window rootReturn, parentReturn;
    Window* children = nullptr;
    unsigned int childCount = 0;

    g_lastCallHadXError = false;
    if (!XQueryTree(display_, root, &rootReturn, &parentReturn, &children, &childCount)) {
        return 0;
    }

    Window found = 0;
    for (unsigned int i = 0; i < childCount && found == 0; ++i) {
        found = findWindowByTitle(children[i], needle);
    }
    if (children) XFree(children);

    return found;
}

bool LinuxPlatform::bindToWindow(const std::string& windowTitleSubstring) {
    const Window found = findWindowByTitle(rootWindow_, windowTitleSubstring);
    if (found == 0) {
        return false;
    }
    boundWindow_ = found;
    return true;
}

LinuxPlatform::CaptureTarget LinuxPlatform::resolveCaptureTarget() const {
    CaptureTarget target;

    if (boundWindow_ != 0) {
        XWindowAttributes attrs{};
        g_lastCallHadXError = false;
        if (!XGetWindowAttributes(display_, boundWindow_, &attrs) || g_lastCallHadXError) {
            throw std::runtime_error(
                "LinuxPlatform: XGetWindowAttributes failed on bound window "
                "(it may have been closed): " + g_lastXErrorText);
        }

        // Translate window-local (0,0) to root/screen coordinates.
        Window childReturn;
        int screenX = 0, screenY = 0;
        XTranslateCoordinates(display_, boundWindow_, rootWindow_,
                               0, 0, &screenX, &screenY, &childReturn);

        target.window = boundWindow_;
        target.originX = screenX;
        target.originY = screenY;
        target.width = attrs.width;
        target.height = attrs.height;
    } else {
        XWindowAttributes attrs{};
        XGetWindowAttributes(display_, rootWindow_, &attrs);
        target.window = 0;
        target.originX = 0;
        target.originY = 0;
        target.width = attrs.width;
        target.height = attrs.height;
    }

    if (target.width <= 0 || target.height <= 0) {
        throw std::runtime_error(
            "LinuxPlatform: resolved capture target has invalid dimensions "
            "(window may be minimized, unmapped, or off-screen)");
    }
    return target;
}

cv::Mat LinuxPlatform::captureScreen() {
    const CaptureTarget target = resolveCaptureTarget();

    // Capture from the root window at the resolved screen-space origin
    // (rather than the bound window's own drawable) so overlapping/
    // occluding windows are correctly reflected, mirroring the approach
    // used in WindowsPlatform::captureScreen().
    g_lastCallHadXError = false;
    ScopedXImage image(XGetImage(
        display_, rootWindow_,
        target.originX, target.originY, target.width, target.height,
        AllPlanes, ZPixmap));

    if (!image || g_lastCallHadXError) {
        throw std::runtime_error(
            "LinuxPlatform: XGetImage failed during screen capture: " + g_lastXErrorText);
    }

    // XImage pixel format varies by display depth/visual; handle the
    // common 24/32-bit TrueColor case explicitly and fail loudly on
    // anything else rather than silently producing garbage pixels.
    if (image.get()->bits_per_pixel != 32 && image.get()->bits_per_pixel != 24) {
        throw std::runtime_error(
            "LinuxPlatform: unsupported X11 visual depth (" +
            std::to_string(image.get()->bits_per_pixel) +
            " bpp) — only 24/32-bit TrueColor is supported");
    }

    // XImage data is BGRX/BGRA on virtually all little-endian Linux
    // desktops (matches cv::Mat's native BGR channel order), so this is
    // a direct memory view with stride handling — no manual per-pixel
    // channel swapping needed.
    cv::Mat wrapped(target.height, target.width, CV_8UC4,
                     image.get()->data, static_cast<size_t>(image.get()->bytes_per_line));

    cv::Mat bgr;
    cv::cvtColor(wrapped, bgr, cv::COLOR_BGRA2BGR);
    return bgr; // deep copy happens in cvtColor's output allocation;
                // safe to use after `image` (and its backing buffer) is destroyed
}

cv::Rect LinuxPlatform::getActiveWindowBounds() const {
    const CaptureTarget target = resolveCaptureTarget();
    return cv::Rect(target.originX, target.originY, target.width, target.height);
}

void LinuxPlatform::moveCursorTo(int x, int y) {
    g_lastCallHadXError = false;
    XTestFakeMotionEvent(display_, screen_, x, y, CurrentTime);
    XFlush(display_);
    if (g_lastCallHadXError) {
        throw std::runtime_error("LinuxPlatform: XTestFakeMotionEvent failed: " + g_lastXErrorText);
    }
}

std::pair<int, int> LinuxPlatform::getCursorPosition() const {
    Window rootReturn, childReturn;
    int rootX, rootY, winX, winY;
    unsigned int mask;

    g_lastCallHadXError = false;
    const Bool ok = XQueryPointer(display_, rootWindow_, &rootReturn, &childReturn,
                                   &rootX, &rootY, &winX, &winY, &mask);
    if (!ok || g_lastCallHadXError) {
        throw std::runtime_error("LinuxPlatform: XQueryPointer failed: " + g_lastXErrorText);
    }
    return {rootX, rootY};
}

void LinuxPlatform::mouseButtonDown(int buttonId) {
    // X11 button numbering: 1=left, 2=middle, 3=right (note: differs from
    // our abstract 0=left/1=right/2=middle ordering — translated below).
    g_lastCallHadXError = false;
    XTestFakeButtonEvent(display_, static_cast<unsigned int>(buttonId), True, CurrentTime);
    XFlush(display_);
    if (g_lastCallHadXError) {
        throw std::runtime_error("LinuxPlatform: XTestFakeButtonEvent (down) failed: " + g_lastXErrorText);
    }
}

void LinuxPlatform::mouseButtonUp(int buttonId) {
    g_lastCallHadXError = false;
    XTestFakeButtonEvent(display_, static_cast<unsigned int>(buttonId), False, CurrentTime);
    XFlush(display_);
    if (g_lastCallHadXError) {
        throw std::runtime_error("LinuxPlatform: XTestFakeButtonEvent (up) failed: " + g_lastXErrorText);
    }
}

void LinuxPlatform::keyDown(int platformKeyCode) {
    const KeyCode keycode = XKeysymToKeycode(display_, static_cast<KeySym>(platformKeyCode));
    if (keycode == 0) {
        throw std::runtime_error(
            "LinuxPlatform: XKeysymToKeycode found no mapping for keysym " +
            std::to_string(platformKeyCode));
    }
    g_lastCallHadXError = false;
    XTestFakeKeyEvent(display_, keycode, True, CurrentTime);
    XFlush(display_);
    if (g_lastCallHadXError) {
        throw std::runtime_error("LinuxPlatform: XTestFakeKeyEvent (down) failed: " + g_lastXErrorText);
    }
}

void LinuxPlatform::keyUp(int platformKeyCode) {
    const KeyCode keycode = XKeysymToKeycode(display_, static_cast<KeySym>(platformKeyCode));
    if (keycode == 0) {
        throw std::runtime_error(
            "LinuxPlatform: XKeysymToKeycode found no mapping for keysym " +
            std::to_string(platformKeyCode));
    }
    g_lastCallHadXError = false;
    XTestFakeKeyEvent(display_, keycode, False, CurrentTime);
    XFlush(display_);
    if (g_lastCallHadXError) {
        throw std::runtime_error("LinuxPlatform: XTestFakeKeyEvent (up) failed: " + g_lastXErrorText);
    }
}

void LinuxPlatform::injectUnicodeText(const std::string& utf8Text) {
    // XTest has no native "inject arbitrary Unicode string" call the way
    // Win32's KEYEVENTF_UNICODE does. The portable, WM-agnostic approach
    // is to temporarily remap an unused keycode to each character's
    // keysym via XChangeKeyboardMapping, press it, then restore — this
    // correctly handles Sinhala and other complex-script text without
    // requiring the user's actual keyboard layout to support it.
    if (utf8Text.empty()) return;

    // Decode UTF-8 into Unicode codepoints.
    std::vector<unsigned int> codepoints;
    for (size_t i = 0; i < utf8Text.size();) {
        unsigned char c = utf8Text[i];
        unsigned int cp = 0;
        int extraBytes = 0;
        if ((c & 0x80) == 0x00)      { cp = c; extraBytes = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extraBytes = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extraBytes = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extraBytes = 3; }
        else { ++i; continue; } // skip invalid leading byte rather than throwing —
                                  // a single malformed byte shouldn't abort the whole type action

        if (i + extraBytes >= utf8Text.size()) break;
        bool valid = true;
        for (int b = 1; b <= extraBytes; ++b) {
            unsigned char cont = utf8Text[i + b];
            if ((cont & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (cont & 0x3F);
        }
        i += extraBytes + 1;
        if (valid) codepoints.push_back(cp);
    }

    // Reserve the highest available keycode in the map for temporary remapping.
    int minKeycode, maxKeycode;
    XDisplayKeycodes(display_, &minKeycode, &maxKeycode);
    const KeyCode scratchKeycode = static_cast<KeyCode>(maxKeycode);

    for (unsigned int cp : codepoints) {
        // X11 keysyms for Unicode codepoints beyond Latin-1 use the
        // 0x01000000 + codepoint convention (per X11 keysym2ucs spec).
        const KeySym keysym = (cp <= 0xFF) ? static_cast<KeySym>(cp)
                                            : static_cast<KeySym>(0x01000000 + cp);

        KeySym keysyms[1] = { keysym };
        XChangeKeyboardMapping(display_, scratchKeycode, 1, keysyms, 1);
        XSync(display_, False);

        g_lastCallHadXError = false;
        XTestFakeKeyEvent(display_, scratchKeycode, True, CurrentTime);
        XTestFakeKeyEvent(display_, scratchKeycode, False, CurrentTime);
        XFlush(display_);

        if (g_lastCallHadXError) {
            throw std::runtime_error(
                "LinuxPlatform: injectUnicodeText failed on codepoint U+" +
                std::to_string(cp) + ": " + g_lastXErrorText);
        }
    }
}

int LinuxPlatform::translateKeyCode(int abstractKeyCode) const {
    using action::KeyCode;
    switch (static_cast<KeyCode>(abstractKeyCode)) {
        case KeyCode::Enter:      return XK_Return;
        case KeyCode::Escape:     return XK_Escape;
        case KeyCode::Tab:        return XK_Tab;
        case KeyCode::Backspace:  return XK_BackSpace;
        case KeyCode::Delete:     return XK_Delete;
        case KeyCode::ArrowUp:    return XK_Up;
        case KeyCode::ArrowDown:  return XK_Down;
        case KeyCode::ArrowLeft:  return XK_Left;
        case KeyCode::ArrowRight: return XK_Right;
        case KeyCode::Ctrl:       return XK_Control_L;
        case KeyCode::Alt:        return XK_Alt_L;
        case KeyCode::Shift:      return XK_Shift_L;
        case KeyCode::Meta:       return XK_Super_L;
        case KeyCode::A: return XK_a; case KeyCode::B: return XK_b;
        case KeyCode::C: return XK_c; case KeyCode::D: return XK_d;
        case KeyCode::E: return XK_e; case KeyCode::F: return XK_f;
        case KeyCode::G: return XK_g; case KeyCode::H: return XK_h;
        case KeyCode::I: return XK_i; case KeyCode::J: return XK_j;
        case KeyCode::K: return XK_k; case KeyCode::L: return XK_l;
        case KeyCode::M: return XK_m; case KeyCode::N: return XK_n;
        case KeyCode::O: return XK_o; case KeyCode::P: return XK_p;
        case KeyCode::Q: return XK_q; case KeyCode::R: return XK_r;
        case KeyCode::S: return XK_s; case KeyCode::T: return XK_t;
        case KeyCode::U: return XK_u; case KeyCode::V: return XK_v;
        case KeyCode::W: return XK_w; case KeyCode::X: return XK_x;
        case KeyCode::Y: return XK_y; case KeyCode::Z: return XK_z;
        case KeyCode::Num0: return XK_0; case KeyCode::Num1: return XK_1;
        case KeyCode::Num2: return XK_2; case KeyCode::Num3: return XK_3;
        case KeyCode::Num4: return XK_4; case KeyCode::Num5: return XK_5;
        case KeyCode::Num6: return XK_6; case KeyCode::Num7: return XK_7;
        case KeyCode::Num8: return XK_8; case KeyCode::Num9: return XK_9;
        case KeyCode::F1: return XK_F1; case KeyCode::F2: return XK_F2;
        case KeyCode::F3: return XK_F3; case KeyCode::F4: return XK_F4;
        case KeyCode::F5: return XK_F5; case KeyCode::F6: return XK_F6;
        case KeyCode::F7: return XK_F7; case KeyCode::F8: return XK_F8;
        case KeyCode::F9: return XK_F9; case KeyCode::F10: return XK_F10;
        case KeyCode::F11: return XK_F11; case KeyCode::F12: return XK_F12;
        default:
            throw std::invalid_argument("LinuxPlatform: unmapped abstract KeyCode " +
                                         std::to_string(abstractKeyCode));
    }
}

int LinuxPlatform::translateMouseButton(int abstractButtonId) const {
    using action::MouseButton;
    // X11/XTest button numbering: 1=left, 2=middle, 3=right.
    switch (static_cast<MouseButton>(abstractButtonId)) {
        case MouseButton::Left:   return 1;
        case MouseButton::Right:  return 3;
        case MouseButton::Middle: return 2;
        default:
            throw std::invalid_argument("LinuxPlatform: unmapped abstract MouseButton " +
                                         std::to_string(abstractButtonId));
    }
}

} // namespace rpa::platform

#endif // PLATFORM_LINUX