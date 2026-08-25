#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <atomic>
#include <string>
#include <functional>
#include <opencv2/core.hpp>

namespace rpa::core {

/// Why the SecurityLayer decided to halt execution — surfaced to logs/UI
/// so the user understands *why* the agent stopped rather than just "it stopped".
enum class HaltReason {
    None,
    RepeatedClickLoop,      // Same coordinate clicked too many times too fast.
    NoProgressTimeout,      // Screen state hasn't changed across many actions.
    OutOfBoundsAction,      // Action targeted outside the sandboxed window region.
    ManualStop              // User or CoreEngine explicitly requested a stop.
};

/// Configuration knobs for the kill-switch, deliberately explicit rather
/// than hardcoded, so different tasks can tune sensitivity without
/// recompiling.
struct SecurityConfig {
    int    maxClicksPerWindow   = 5;     // e.g. 5 clicks...
    int    clickWindowMs        = 1000;  // ...within this many ms triggers a halt.
    int    clickProximityPx     = 8;     // clicks within this radius count as "same spot".
    int    maxNoProgressActions = 15;    // consecutive actions with no visual change.
    double frameDiffThreshold   = 0.5;   // % pixel change below which frames are "identical".
};

/// Tracks agent behavior in real time and trips a kill switch the moment
/// it detects runaway behavior: click loops, stuck-in-place execution, or
/// actions escaping the sandboxed application window. This is the agent's
/// immune system — it does not try to be smart about *why* something is
/// wrong, only fast and certain about *that* something is wrong.
///
/// Thread-safety: recordClick/recordFrame may be called from the engine's
/// worker thread while the UI thread polls isHalted()/lastHaltReason() —
/// all shared state is guarded by mutex_ or is a std::atomic.
class SecurityLayer {
public:
    explicit SecurityLayer(SecurityConfig config = SecurityConfig{});

    /// Call immediately before every mouse click the InputController issues.
    /// Returns false if the click should be *blocked* because it would
    /// itself trigger the loop threshold (pre-emptive, not just reactive).
    bool recordClickAndCheck(int x, int y);

    /// Call after every captured frame during an execution loop. Compares
    /// against the previous frame to detect "no visual progress" — the
    /// agent doing the same ineffective action repeatedly without the
    /// screen ever changing.
    void recordFrame(const cv::Mat& frame);

    /// Call before executing any action, with the bounding rectangle of the
    /// sandboxed target window. If (x, y) falls outside it, the layer halts
    /// immediately rather than letting the agent touch unrelated windows.
    bool checkWithinSandbox(int x, int y, const cv::Rect& sandboxBounds);

    /// Immediately halts the agent for an externally-detected reason
    /// (e.g. CoreEngine caught an unrecoverable exception).
    void triggerHalt(HaltReason reason);

    /// Resets all tracking state — call at the start of each new task/skill
    /// so history from a prior unrelated task doesn't bleed into the next.
    void reset();

    bool isHalted() const noexcept { return halted_.load(std::memory_order_acquire); }
    HaltReason lastHaltReason() const noexcept { return haltReason_.load(std::memory_order_acquire); }

    /// Optional hook so CoreEngine/UI can react instantly to a halt
    /// (e.g. force-release mouse buttons, log, alert the user) without
    /// polling. Invoked synchronously on the thread that detected the trip.
    void setOnHalt(std::function<void(HaltReason)> callback);

private:
    struct ClickEvent {
        int x, y;
        std::chrono::steady_clock::time_point timestamp;
    };

    void halt(HaltReason reason);
    double computeFrameDiffPercent(const cv::Mat& a, const cv::Mat& b) const;

    SecurityConfig config_;

    mutable std::mutex mutex_;
    std::deque<ClickEvent> recentClicks_;   // guarded by mutex_
    cv::Mat lastFrame_;                     // guarded by mutex_
    int noProgressCounter_ = 0;             // guarded by mutex_

    std::atomic<bool> halted_{false};
    std::atomic<HaltReason> haltReason_{HaltReason::None};
    std::function<void(HaltReason)> onHalt_;
};

} // namespace rpa::core