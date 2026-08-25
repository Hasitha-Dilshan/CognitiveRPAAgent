#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <stdexcept>

#include "../platform/IPlatform.hpp"

namespace rpa::action {

/// Mouse buttons the controller can synthesize events for.
enum class MouseButton {
    Left,
    Right,
    Middle
};

/// Abstract keyboard key codes, deliberately OS-agnostic. IPlatform
/// implementations translate these into VK_* codes on Windows or
/// XKeysym values on Linux/X11, so nothing above this layer ever
/// touches a platform-specific constant.
enum class KeyCode {
    Enter, Escape, Tab, Backspace, Delete,
    ArrowUp, ArrowDown, ArrowLeft, ArrowRight,
    Ctrl, Alt, Shift, Meta,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12
};

/// Thrown when an input action cannot be synthesized at the OS level
/// (e.g. platform API call failed). Distinct from SecurityLayer blocking
/// an action — this represents a genuine hardware/OS-level failure.
class InputActionException : public std::runtime_error {
public:
    explicit InputActionException(const std::string& msg) : std::runtime_error(msg) {}
};

/// InputController is the single choke point through which the agent
/// touches the mouse and keyboard. It never talks to Win32/X11 directly —
/// all of that lives behind the injected IPlatform implementation — so
/// this class contains zero #ifdef blocks and is trivially unit-testable
/// with a mock IPlatform.
///
/// Every action here is intentionally "dumb": InputController does not
/// decide *where* to click or *whether* it's safe to click — CoreEngine
/// and SecurityLayer own those decisions upstream. This class only
/// executes already-approved actions as reliably as possible.
class InputController {
public:
    explicit InputController(std::shared_ptr<platform::IPlatform> platform);

    // Non-copyable: represents exclusive control of shared OS input state
    // (cursor position, key modifier state); copying would be meaningless.
    InputController(const InputController&) = delete;
    InputController& operator=(const InputController&) = delete;

    /// Moves the cursor to absolute screen coordinates (x, y). Movement is
    /// performed in small interpolated steps rather than a single teleport
    /// jump, since some applications' hover/drag-detection logic only
    /// registers movement events, not instantaneous position changes.
    /// Throws InputActionException if the platform layer reports failure.
    void moveTo(int x, int y, std::chrono::milliseconds duration = std::chrono::milliseconds(80));

    /// Performs a full click (move + press + release) at (x, y).
    /// `postClickDelayMs` is a small settle delay (not a blind wait for a
    /// result — StateMachine handles that) to let the OS/app register the
    /// press before the next action fires.
    void click(int x, int y, MouseButton button = MouseButton::Left,
               int postClickDelayMs = 30);

    /// Performs a double-click at (x, y) using the platform's native
    /// double-click timing threshold rather than two independent clicks,
    /// since some apps distinguish "two fast clicks" from "double-click"
    /// at the OS event level.
    void doubleClick(int x, int y, MouseButton button = MouseButton::Left);

    /// Presses and holds a mouse button without releasing — paired with
    /// releaseButton() to implement drag operations.
    void pressButton(MouseButton button);
    void releaseButton(MouseButton button);

    /// Convenience drag helper: press at (x1,y1), interpolated move to
    /// (x2,y2), release. Used for e.g. dragging a file onto a drop zone.
    void dragTo(int x1, int y1, int x2, int y2,
                MouseButton button = MouseButton::Left,
                std::chrono::milliseconds duration = std::chrono::milliseconds(200));

    /// Types a Unicode string via the platform's text-input injection
    /// (not per-key synthesis where avoidable), so IME/non-ASCII text
    /// (relevant here given Sinhala-language UI content) is entered
    /// correctly rather than dropped or mangled.
    void typeText(const std::string& utf8Text);

    /// Presses a single key (with optional modifiers held during the
    /// press), e.g. pressKey(KeyCode::S, {KeyCode::Ctrl}) for Ctrl+S.
    void pressKey(KeyCode key, const std::vector<KeyCode>& modifiers = {});

    /// Returns the current cursor position by querying the platform layer
    /// directly (not cached), so callers always see ground truth — useful
    /// for SecurityLayer's proximity checks and for verifying moveTo()
    /// actually landed where intended.
    std::pair<int, int> currentCursorPosition() const;

private:
    /// Shared linear interpolation used by moveTo() and dragTo() so both
    /// produce the same natural, app-friendly motion characteristics.
    void interpolatedMove(int fromX, int fromY, int toX, int toY,
                           std::chrono::milliseconds duration);

    std::shared_ptr<platform::IPlatform> platform_;

    static constexpr int kMoveStepIntervalMs = 8; // ~120Hz motion sampling
};

} // namespace rpa::action