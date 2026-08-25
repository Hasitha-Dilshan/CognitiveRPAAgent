#include "action/InputController.hpp"
#include <cmath>
#include <algorithm>
#include <thread>

namespace rpa::action {

InputController::InputController(std::shared_ptr<platform::IPlatform> platform)
    : platform_(std::move(platform)) {
    if (!platform_) {
        throw std::invalid_argument("InputController: platform must not be null");
    }
}

void InputController::interpolatedMove(int fromX, int fromY, int toX, int toY,
                                        std::chrono::milliseconds duration) {
    // Zero/negative duration means "just teleport" — some callers (e.g.
    // dragTo's initial press position) don't need natural motion, only
    // the final position to be exactly right.
    if (duration.count() <= 0) {
        platform_->moveCursorTo(toX, toY);
        return;
    }

    const int steps = std::max(1, static_cast<int>(duration.count() / kMoveStepIntervalMs));
    const double dx = static_cast<double>(toX - fromX) / steps;
    const double dy = static_cast<double>(toY - fromY) / steps;

    for (int i = 1; i <= steps; ++i) {
        // Round rather than truncate on each step so accumulated rounding
        // error can't leave the cursor a pixel or two short of the exact
        // target after the loop — the final iteration (i == steps) always
        // lands exactly on (toX, toY) since dx*steps == toX-fromX exactly.
        const int stepX = fromX + static_cast<int>(std::round(dx * i));
        const int stepY = fromY + static_cast<int>(std::round(dy * i));

        // A single failed intermediate move step during otherwise-natural
        // motion is not worth aborting the whole gesture over — the OS
        // occasionally drops a SetCursorPos/XTestFakeMotionEvent call
        // under load without it indicating a real problem. Only the final
        // step's success is load-bearing (asserted by the explicit final
        // moveCursorTo call below), so intermediate failures are swallowed.
        try {
            platform_->moveCursorTo(stepX, stepY);
        } catch (const std::exception&) {
            // Swallowed intentionally — see comment above.
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kMoveStepIntervalMs));
    }

    // Guarantee exact final placement regardless of any intermediate step
    // failures or rounding drift — this call's exceptions are NOT
    // swallowed, since landing precisely on the intended target is the
    // one guarantee callers (click(), dragTo()) actually depend on.
    platform_->moveCursorTo(toX, toY);
}

void InputController::moveTo(int x, int y, std::chrono::milliseconds duration) {
    const auto [currentX, currentY] = platform_->getCursorPosition();
    interpolatedMove(currentX, currentY, x, y, duration);
}

void InputController::click(int x, int y, MouseButton button, int postClickDelayMs) {
    moveTo(x, y);

    const int platformButtonId = platform_->translateMouseButton(static_cast<int>(button));

    platform_->mouseButtonDown(platformButtonId);

    // Small held-down duration (not a configurable param — this is an
    // implementation-level realism detail, not a caller-tunable behavior)
    // since some applications' click handlers distinguish a genuine click
    // from a zero-duration synthetic down+up pair issued in the same
    // event-loop tick.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    platform_->mouseButtonUp(platformButtonId);

    if (postClickDelayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(postClickDelayMs));
    }
}

void InputController::doubleClick(int x, int y, MouseButton button) {
    moveTo(x, y);
    const int platformButtonId = platform_->translateMouseButton(static_cast<int>(button));

    // Issued as two rapid down/up pairs rather than delegating to a
    // platform-level "double click" primitive — neither Win32's SendInput
    // nor XTest expose one directly; both OSes recognize a double-click
    // purely from the timing between two ordinary click events, which is
    // exactly what this reproduces.
    platform_->mouseButtonDown(platformButtonId);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    platform_->mouseButtonUp(platformButtonId);

    // Gap between clicks must stay comfortably under the OS's configured
    // double-click time threshold (typically 400-500ms default on both
    // Windows and most Linux desktop environments) or this registers as
    // two separate single clicks instead.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    platform_->mouseButtonDown(platformButtonId);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    platform_->mouseButtonUp(platformButtonId);
}

void InputController::pressButton(MouseButton button) {
    const int platformButtonId = platform_->translateMouseButton(static_cast<int>(button));
    platform_->mouseButtonDown(platformButtonId);
}

void InputController::releaseButton(MouseButton button) {
    const int platformButtonId = platform_->translateMouseButton(static_cast<int>(button));
    platform_->mouseButtonUp(platformButtonId);
}

void InputController::dragTo(int x1, int y1, int x2, int y2,
                              MouseButton button, std::chrono::milliseconds duration) {
    // Move to the start position first with no button held — this is a
    // plain positioning move, not part of the drag gesture itself, so it
    // intentionally does not use the caller-specified `duration` (that
    // duration governs the drag motion, not getting into position for it).
    moveTo(x1, y1);

    pressButton(button);

    // Brief settle delay after pressing down before motion begins —
    // mirrors how a human drag naturally has a small pause between
    // "finger down" and "finger starts moving," which some drag-and-drop
    // implementations (particularly HTML5 DnD) rely on to distinguish a
    // drag gesture from a simple click.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    interpolatedMove(x1, y1, x2, y2, duration);

    // Settle at the drop target before releasing — many drop-zone
    // implementations only arm on mouseover/mousemove events and need a
    // brief moment to register the hover state before mouseup fires.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    releaseButton(button);
}

void InputController::typeText(const std::string& utf8Text) {
    if (utf8Text.empty()) return;

    // Delegates directly to the platform's native Unicode injection
    // (KEYEVENTF_UNICODE on Windows, temporary keycode remapping on
    // Linux/X11) rather than InputController attempting to iterate
    // characters and call pressKey() per character — pressKey() only
    // covers the fixed KeyCode enum (English letters/digits/function
    // keys), which cannot represent Sinhala or other complex-script text.
    // injectUnicodeText() is the one path capable of typing arbitrary
    // Unicode content, matching the platform layer's own documented intent.
    platform_->injectUnicodeText(utf8Text);
}

void InputController::pressKey(KeyCode key, const std::vector<KeyCode>& modifiers) {
    std::vector<int> platformModifierCodes;
    platformModifierCodes.reserve(modifiers.size());
    for (KeyCode mod : modifiers) {
        platformModifierCodes.push_back(platform_->translateKeyCode(static_cast<int>(mod)));
    }

    const int platformKeyCode = platform_->translateKeyCode(static_cast<int>(key));

    // Press modifiers down in order first (e.g. Ctrl before S for Ctrl+S),
    // matching how a human physically holds a modifier before pressing
    // the main key — some applications' keyboard shortcut handlers are
    // sensitive to modifier-then-key ordering and won't recognize the
    // combination if the main key arrives first.
    for (int modCode : platformModifierCodes) {
        platform_->keyDown(modCode);
    }

    platform_->keyDown(platformKeyCode);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    platform_->keyUp(platformKeyCode);

    // Release modifiers in reverse order (last pressed, first released) —
    // symmetric with the press order above and matches natural human
    // key-release behavior, avoiding a brief invalid intermediate state
    // where a later-pressed modifier is released before an earlier one.
    for (auto it = platformModifierCodes.rbegin(); it != platformModifierCodes.rend(); ++it) {
        platform_->keyUp(*it);
    }
}

std::pair<int, int> InputController::currentCursorPosition() const {
    return platform_->getCursorPosition();
}

} // namespace rpa::action