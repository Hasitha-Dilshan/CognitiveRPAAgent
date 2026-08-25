#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <stdexcept>
#include <optional>
#include <opencv2/opencv.hpp>

#include "../perception/VisionProcessor.hpp"
#include "../perception/OCRProcessor.hpp"
#include "../perception/ScreenCapture.hpp"

namespace rpa::core {

/// Reason a wait ended, so callers (and the learning system) know
/// whether to trust the result or treat it as a failure to adapt from.
enum class WaitOutcome {
    StateReached,       // Target visual condition was detected.
    TimedOut,           // Adaptive timeout elapsed with no match.
    Aborted             // External stop (e.g. SecurityLayer halt) interrupted the wait.
};

struct WaitResult {
    WaitOutcome outcome = WaitOutcome::TimedOut;
    std::chrono::milliseconds elapsed{0};
};

/// A condition the StateMachine polls for. Rather than the engine hardcoding
/// "wait for Save button" logic, callers supply a predicate closure that
/// inspects a captured frame and returns true when the desired state exists.
/// This keeps StateMachine generic and reusable for any visual condition:
/// "loading spinner gone", "new element appeared", "window title changed", etc.
using StateCondition = std::function<bool(const cv::Mat& frame)>;

/// StateMachine replaces fixed `sleep(N)` waits with adaptive, condition-driven
/// polling. It never blocks for a duration decided in advance — it re-checks
/// the screen at a variable interval and stops the instant the condition is
/// satisfied, or when a *learned, adaptive* timeout is exceeded.
///
/// Adaptivity comes from two mechanisms:
///   1. Exponential polling backoff — starts fast (cheap early exit for quick
///      UI responses) and slows down for long-running tasks, saving CPU.
///   2. A running average of past durations per state name, so the timeout
///      ceiling itself grows/shrinks based on real observed behavior
///      (e.g. "PDF export" naturally gets a longer allowance than "menu open").
class StateMachine {
public:
    StateMachine(perception::ScreenCapture& screenCapture,
                 perception::VisionProcessor& visionProcessor);

    /// Blocks the calling thread (intended to be a background worker thread,
    /// never the UI thread) until `condition` returns true, the adaptive
    /// timeout is reached, or `abortFlag` is set true by another thread
    /// (e.g. SecurityLayer triggering an emergency halt).
    ///
    /// stateName is used purely as a key for the adaptive timeout learning —
    /// pass a stable identifier like "pdf_export" or "save_dialog_open".
    WaitResult waitForCondition(const std::string& stateName,
                                 const StateCondition& condition,
                                 std::atomic<bool>& abortFlag,
                                 int hardCeilingMs = 60000);

    /// Convenience wrapper: waits until VisionProcessor reports the loading
    /// indicator has *disappeared*. Common enough to deserve a named helper.
    WaitResult waitUntilLoadingFinishes(std::atomic<bool>& abortFlag,
                                         int hardCeilingMs = 60000);

    /// Convenience wrapper: waits until a template/feature match for
    /// `templateImg` first appears on screen (element has rendered).
    WaitResult waitUntilElementAppears(const cv::Mat& templateImg,
                                        std::atomic<bool>& abortFlag,
                                        int hardCeilingMs = 30000);

    /// Records how long a named state actually took to resolve successfully.
    /// Feeds the adaptive timeout model — called internally on success, but
    /// exposed publicly in case CoreEngine wants to record externally-timed
    /// operations too (e.g. OCR fallback path duration).
    void recordObservedDuration(const std::string& stateName,
                                 std::chrono::milliseconds duration);

private:
    /// Computes the current adaptive timeout ceiling for a given state name.
    /// Uses observed history if present (mean + generous safety margin),
    /// otherwise falls back to a conservative default so first-run behavior
    /// is still safe.
    int computeAdaptiveTimeout(const std::string& stateName,
                                int hardCeilingMs) const;

    /// Polling interval schedule: starts at pollMinMs_, backs off
    /// multiplicatively up to pollMaxMs_. Avoids hammering the CPU with
    /// screen captures while still reacting quickly to fast UI changes.
    std::chrono::milliseconds nextPollInterval(std::chrono::milliseconds current) const;

    perception::ScreenCapture&   screenCapture_;
    perception::VisionProcessor& visionProcessor_;

    // stateName -> exponential moving average of observed durations (ms).
    // Deliberately simple (no external DB) — persisted via KnowledgeBase
    // if CoreEngine chooses to serialize it alongside memory.json.
    std::unordered_map<std::string, double> observedDurationsEma_;

    static constexpr double kEmaAlpha = 0.3;          // weight for new samples
    static constexpr int    kDefaultTimeoutMs = 15000; // used when no history exists
    static constexpr int    kPollMinMs = 100;          // fastest re-check interval
    static constexpr int    kPollMaxMs = 2000;         // slowest re-check interval
    static constexpr double kPollBackoffFactor = 1.5;  // growth rate between polls
};

} // namespace rpa::core