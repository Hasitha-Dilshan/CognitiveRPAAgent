#ifdef PLATFORM_WINDOWS

#include "platform/WindowsPlatform.hpp"
#include "action/InputController.hpp" // for KeyCode / MouseButton enum values
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <vector>
#include <algorithm>

namespace rpa::platform {

namespace {
    /// RAII wrapper around a GDI device context + compatible bitmap, so
    /// a thrown exception or early return during capture can never leak
    /// GDI handles (a real risk in raw Win32 code — GDI handle leaks
    /// silently degrade the whole OS over a long-running agent process).
    class GdiCaptureBuffer {
    public:
        GdiCaptureBuffer(HDC sourceDC, int width, int height) : width_(width), height_(height) {
            memDC_ = CreateCompatibleDC(sourceDC);
            if (!memDC_) {
                throw std::runtime_error("WindowsPlatform: CreateCompatibleDC failed");
            }
            bitmap_ = CreateCompatibleBitmap(sourceDC, width, height);
            if (!bitmap_) {
                DeleteDC(memDC_);
                throw std::runtime_error("WindowsPlatform: CreateCompatibleBitmap failed");
            }
            oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDC_, bitmap_));
        }

        ~GdiCaptureBuffer() {
            if (memDC_ && oldBitmap_) {
                SelectObject(memDC_, oldBitmap_); // restore before deleting
            }
            if (bitmap_) DeleteObject(bitmap_);
            if (memDC_) DeleteDC(memDC_);
        }

        GdiCaptureBuffer(const GdiCaptureBuffer&) = delete;
        GdiCaptureBuffer& operator=(const GdiCaptureBuffer&) = delete;

        HDC dc() const { return memDC_; }
        HBITMAP bitmap() const { return bitmap_; }
        int width() const { return width_; }
        int height() const { return height_; }

    private:
        HDC memDC_ = nullptr;
        HBITMAP bitmap_ = nullptr;
        HBITMAP oldBitmap_ = nullptr;
        int width_;
        int height_;
    };

    /// RAII wrapper for a window/screen device context obtained via GetDC,
    /// so ReleaseDC is guaranteed even on early-exit error paths.
    class ScopedWindowDC {
    public:
        explicit ScopedWindowDC(HWND hwnd) : hwnd_(hwnd) {
            dc_ = GetDC(hwnd_); // hwnd_ == nullptr => DC for entire screen
            if (!dc_) {
                throw std::runtime_error("WindowsPlatform: GetDC failed");
            }
        }
        ~ScopedWindowDC() { if (dc_) ReleaseDC(hwnd_, dc_); }

        ScopedWindowDC(const ScopedWindowDC&) = delete;
        ScopedWindowDC& operator=(const ScopedWindowDC&) = delete;

        HDC get() const { return dc_; }

    private:
        HWND hwnd_;
        HDC dc_ = nullptr;
    };

    struct WindowSearchContext {
        std::wstring needle;
        HWND result = nullptr;
    };

    BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam) {
        auto* ctx = reinterpret_cast<WindowSearchContext*>(lParam);

        if (!IsWindowVisible(hwnd)) return TRUE;

        wchar_t titleBuf[512];
        const int len = GetWindowTextW(hwnd, titleBuf, static_cast<int>(std::size(titleBuf)));
        if (len <= 0) return TRUE;

        std::wstring title(titleBuf, static_cast<size_t>(len));
        // Case-insensitive substring search.
        auto toLower = [](std::wstring s) {
            std::transform(s.begin(), s.end(), s.begin(), ::towlower);
            return s;
        };
        if (toLower(title).find(toLower(ctx->needle)) != std::wstring::npos) {
            ctx->result = hwnd;
            return FALSE; // stop enumeration, found it
        }
        return TRUE;
    }
}

WindowsPlatform::WindowsPlatform() {
    // SendInput-based injection requires no special initialization, but we
    // verify early that the process can at least query the desktop DC —
    // fail fast at construction rather than deep inside a capture call.
    HDC testDC = GetDC(nullptr);
    if (!testDC) {
        throw std::runtime_error("WindowsPlatform: unable to acquire desktop device context");
    }
    ReleaseDC(nullptr, testDC);
}

WindowsPlatform::~WindowsPlatform() = default;

bool WindowsPlatform::bindToWindow(const std::string& windowTitleSubstring) {
    WindowSearchContext ctx;
    ctx.needle = utf8ToWide(windowTitleSubstring);
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&ctx));

    if (!ctx.result) {
        return false;
    }
    boundWindow_ = ctx.result;
    return true;
}

WindowsPlatform::CaptureTarget WindowsPlatform::resolveCaptureTarget() const {
    CaptureTarget target;

    if (boundWindow_ && IsWindow(boundWindow_)) {
        RECT rect{};
        if (!GetClientRect(boundWindow_, &rect)) {
            throw std::runtime_error("WindowsPlatform: GetClientRect failed on bound window");
        }
        POINT topLeft{0, 0};
        if (!ClientToScreen(boundWindow_, &topLeft)) {
            throw std::runtime_error("WindowsPlatform: ClientToScreen failed on bound window");
        }
        target.hwnd = boundWindow_;
        target.originX = topLeft.x;
        target.originY = topLeft.y;
        target.width = rect.right - rect.left;
        target.height = rect.bottom - rect.top;
    } else {
        // Full virtual desktop (spans multiple monitors correctly, unlike
        // GetSystemMetrics(SM_CXSCREEN) which only covers the primary one).
        target.hwnd = nullptr;
        target.originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        target.originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        target.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        target.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }

    if (target.width <= 0 || target.height <= 0) {
        throw std::runtime_error("WindowsPlatform: resolved capture target has invalid dimensions "
                                  "(window may be minimized or off-screen)");
    }
    return target;
}

cv::Mat WindowsPlatform::captureScreen() {
    const CaptureTarget target = resolveCaptureTarget();

    // Capturing from the *screen* DC at the resolved origin, not from the
    // window's own DC directly — this correctly captures overlapping
    // windows/composited content (DWM) rather than stale window contents,
    // and works uniformly whether target.hwnd is set or not.
    ScopedWindowDC screenDC(nullptr);
    GdiCaptureBuffer buffer(screenDC.get(), target.width, target.height);

    const BOOL blitOk = BitBlt(
        buffer.dc(), 0, 0, target.width, target.height,
        screenDC.get(), target.originX, target.originY,
        SRCCOPY | CAPTUREBLT); // CAPTUREBLT ensures layered/transparent windows are included

    if (!blitOk) {
        throw std::runtime_error("WindowsPlatform: BitBlt failed during screen capture");
    }

    // Extract raw pixel data into a cv::Mat via GetDIBits, using a
    // top-down 32bpp BGRA layout that maps directly onto cv::Mat's memory
    // layout with zero manual pixel reordering.
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = target.width;
    bi.biHeight = -target.height; // negative = top-down DIB, avoids vertical flip
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    cv::Mat mat(target.height, target.width, CV_8UC4);
    const int scanLines = GetDIBits(
        buffer.dc(), buffer.bitmap(), 0, static_cast<UINT>(target.height),
        mat.data, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    if (scanLines == 0) {
        throw std::runtime_error("WindowsPlatform: GetDIBits failed during screen capture");
    }

    // Drop alpha channel — downstream OpenCV/Tesseract code expects BGR.
    cv::Mat bgr;
    cv::cvtColor(mat, bgr, cv::COLOR_BGRA2BGR);
    return bgr;
}

cv::Rect WindowsPlatform::getActiveWindowBounds() const {
    const CaptureTarget target = resolveCaptureTarget();
    return cv::Rect(target.originX, target.originY, target.width, target.height);
}

void WindowsPlatform::moveCursorTo(int x, int y) {
    if (!SetCursorPos(x, y)) {
        throw std::runtime_error("WindowsPlatform: SetCursorPos failed (x=" +
                                  std::to_string(x) + ", y=" + std::to_string(y) + ")");
    }
}

std::pair<int, int> WindowsPlatform::getCursorPosition() const {
    POINT pt{};
    if (!GetCursorPos(&pt)) {
        throw std::runtime_error("WindowsPlatform: GetCursorPos failed");
    }
    return {pt.x, pt.y};
}

void WindowsPlatform::mouseButtonDown(int buttonId) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    switch (buttonId) {
        case 0: input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
        case 1: input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; break;
        case 2: input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; break;
        default:
            throw std::invalid_argument("WindowsPlatform: unknown mouse button id " + std::to_string(buttonId));
    }
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        throw std::runtime_error("WindowsPlatform: SendInput (button down) failed");
    }
}

void WindowsPlatform::mouseButtonUp(int buttonId) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    switch (buttonId) {
        case 0: input.mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
        case 1: input.mi.dwFlags = MOUSEEVENTF_RIGHTUP; break;
        case 2: input.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; break;
        default:
            throw std::invalid_argument("WindowsPlatform: unknown mouse button id " + std::to_string(buttonId));
    }
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        throw std::runtime_error("WindowsPlatform: SendInput (button up) failed");
    }
}

void WindowsPlatform::keyDown(int platformKeyCode) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(platformKeyCode);
    input.ki.dwFlags = 0; // key down
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        throw std::runtime_error("WindowsPlatform: SendInput (key down) failed for vk=" +
                                  std::to_string(platformKeyCode));
    }
}

void WindowsPlatform::keyUp(int platformKeyCode) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(platformKeyCode);
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        throw std::runtime_error("WindowsPlatform: SendInput (key up) failed for vk=" +
                                  std::to_string(platformKeyCode));
    }
}

void WindowsPlatform::injectUnicodeText(const std::string& utf8Text) {
    const std::wstring wide = utf8ToWide(utf8Text);
    if (wide.empty()) return;

    // KEYEVENTF_UNICODE lets us inject arbitrary Unicode (including
    // Sinhala/complex scripts) without needing a matching physical
    // keyboard layout — each UTF-16 code unit becomes one down+up pair.
    std::vector<INPUT> inputs;
    inputs.reserve(wide.size() * 2);

    for (wchar_t wc : wide) {
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = static_cast<WORD>(wc);
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(down);

        INPUT up{};
        up.type = INPUT_KEYBOARD;
        up.ki.wScan = static_cast<WORD>(wc);
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }

    const UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    if (sent != inputs.size()) {
        throw std::runtime_error("WindowsPlatform: injectUnicodeText partially failed (" +
                                  std::to_string(sent) + "/" + std::to_string(inputs.size()) + " events sent)");
    }
}

int WindowsPlatform::translateKeyCode(int abstractKeyCode) const {
    using action::KeyCode;
    switch (static_cast<KeyCode>(abstractKeyCode)) {
        case KeyCode::Enter:      return VK_RETURN;
        case KeyCode::Escape:     return VK_ESCAPE;
        case KeyCode::Tab:        return VK_TAB;
        case KeyCode::Backspace:  return VK_BACK;
        case KeyCode::Delete:     return VK_DELETE;
        case KeyCode::ArrowUp:    return VK_UP;
        case KeyCode::ArrowDown:  return VK_DOWN;
        case KeyCode::ArrowLeft:  return VK_LEFT;
        case KeyCode::ArrowRight: return VK_RIGHT;
        case KeyCode::Ctrl:       return VK_CONTROL;
        case KeyCode::Alt:        return VK_MENU;
        case KeyCode::Shift:      return VK_SHIFT;
        case KeyCode::Meta:       return VK_LWIN;
        case KeyCode::A: case KeyCode::B: case KeyCode::C: case KeyCode::D:
        case KeyCode::E: case KeyCode::F: case KeyCode::G: case KeyCode::H:
        case KeyCode::I: case KeyCode::J: case KeyCode::K: case KeyCode::L:
        case KeyCode::M: case KeyCode::N: case KeyCode::O: case KeyCode::P:
        case KeyCode::Q: case KeyCode::R: case KeyCode::S: case KeyCode::T:
        case KeyCode::U: case KeyCode::V: case KeyCode::W: case KeyCode::X:
        case KeyCode::Y: case KeyCode::Z:
            // VK codes for 'A'-'Z' are contiguous and equal to ASCII 'A'-'Z'.
            return 'A' + (abstractKeyCode - static_cast<int>(KeyCode::A));
        case KeyCode::Num0: case KeyCode::Num1: case KeyCode::Num2:
        case KeyCode::Num3: case KeyCode::Num4: case KeyCode::Num5:
        case KeyCode::Num6: case KeyCode::Num7: case KeyCode::Num8:
        case KeyCode::Num9:
            return '0' + (abstractKeyCode - static_cast<int>(KeyCode::Num0));
        case KeyCode::F1: case KeyCode::F2: case KeyCode::F3: case KeyCode::F4:
        case KeyCode::F5: case KeyCode::F6: case KeyCode::F7: case KeyCode::F8:
        case KeyCode::F9: case KeyCode::F10: case KeyCode::F11: case KeyCode::F12:
            return VK_F1 + (abstractKeyCode - static_cast<int>(KeyCode::F1));
        default:
            throw std::invalid_argument("WindowsPlatform: unmapped abstract KeyCode " +
                                         std::to_string(abstractKeyCode));
    }
}

int WindowsPlatform::translateMouseButton(int abstractButtonId) const {
    using action::MouseButton;
    switch (static_cast<MouseButton>(abstractButtonId)) {
        case MouseButton::Left:   return 0;
        case MouseButton::Right:  return 1;
        case MouseButton::Middle: return 2;
        default:
            throw std::invalid_argument("WindowsPlatform: unmapped abstract MouseButton " +
                                         std::to_string(abstractButtonId));
    }
}

std::string WindowsPlatform::wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int sizeNeeded = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return {};
    std::string result(static_cast<size_t>(sizeNeeded), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                         result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}

std::wstring WindowsPlatform::utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int sizeNeeded = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (sizeNeeded <= 0) return {};
    std::wstring result(static_cast<size_t>(sizeNeeded), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                         result.data(), sizeNeeded);
    return result;
}

} // namespace rpa::platform

#endif // PLATFORM_WINDOWS