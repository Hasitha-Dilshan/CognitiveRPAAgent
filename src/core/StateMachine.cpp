#include "core/StateMachine.hpp"
#include <algorithm>
#include <thread>

namespace rpa::core {

StateMachine::StateMachine(perception::ScreenCapture& screenCapture,
                            perception::VisionProcessor& visionProcessor)
    : screenCapture_(screenCapture), visionProcessor_(visionProcessor) {}

WaitResult StateMachine::waitForCondition(const std::string& stateName,
                                           const StateCondition& condition,
                                           std::atomic<bool>& abortFlag,
                                           int hardCeilingMs) {
    if (!condition) {
        // A null condition is a programming error, not a runtime failure —
        // fail loudly rather than silently timing out and confusing the
        // learning system with a bogus "TimedOut" sample.
        throw std::invalid_argument("StateMachine::waitForCondition: condition must not be null");
    }

    const int adaptiveTimeout = computeAdaptiveTimeout(stateName, hardCeilingMs);
    const auto start = std::chrono::steady_clock::now();
    auto pollInterval = std::chrono::milliseconds(kPollMinMs);

    while (true) {
        if (abortFlag.load(std::memory_order_relaxed)) {
            return { WaitOutcome::Aborted, std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start) };
        }

        cv::Mat frame;
        try {
            frame = screenCapture_.captureActiveRegion();
        } catch (const std::exception& e) {
            // Screen capture can transiently fail (e.g. display mode switch,
            // GPU driver hiccup on Linux/X11). Treat as "not yet satisfied"
            // rather than crashing the worker thread — retry on next poll.
            frame.release();
        }

        if (!frame.empty() && condition(frame)) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            recordObservedDuration(stateName, elapsed);
            return { WaitOutcome::StateReached, elapsed };
        }

        const auto elapsedSoFar = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsedSoFar.count() >= adaptiveTimeout) {
            return { WaitOutcome::TimedOut, elapsedSoFar };
        }

        std::this_thread::sleep_for(pollInterval);
        pollInterval = nextPollInterval(pollInterval);
    }
}

WaitResult StateMachine::waitUntilLoadingFinishes(std::atomic<bool>& abortFlag,
                                                   int hardCeilingMs) {
    auto condition = [this](const cv::Mat& frame) {
        return !visionProcessor_.detectLoadingIndicator(frame);
    };
    return waitForCondition("loading_finished", condition, abortFlag, hardCeilingMs);
}

WaitResult StateMachine::waitUntilElementAppears(const cv::Mat& templateImg,
                                                  std::atomic<bool>& abortFlag,
                                                  int hardCeilingMs) {
    if (templateImg.empty()) {
        throw std::invalid_argument("StateMachine::waitUntilElementAppears: templateImg is empty");
    }

    auto condition = [this, &templateImg](const cv::Mat& frame) {
        auto result = visionProcessor_.matchTemplate(frame, templateImg);
        if (result.matched) return true;
        // Fall back to feature matching in case the element rendered at a
        // different scale than the stored template (responsive layout).
        return visionProcessor_.matchFeatures(frame, templateImg).matched;
    };
    return waitForCondition("element_appear", condition, abortFlag, hardCeilingMs);
}

void StateMachine::recordObservedDuration(const std::string& stateName,
                                           std::chrono::milliseconds duration) {
    auto it = observedDurationsEma_.find(stateName);
    const double sample = static_cast<double>(duration.count());
    if (it == observedDurationsEma_.end()) {
        observedDurationsEma_[stateName] = sample;
    } else {
        // Exponential moving average: recent behavior matters more than
        // stale history, so the timeout adapts if a workload gets slower
        // (e.g. bigger PDF) or faster (e.g. cached warm start) over time.
        it->second = kEmaAlpha * sample + (1.0 - kEmaAlpha) * it->second;
    }
}

int StateMachine::computeAdaptiveTimeout(const std::string& stateName,
                                          int hardCeilingMs) const {
    auto it = observedDurationsEma_.find(stateName);
    int timeout;
    if (it == observedDurationsEma_.end()) {
        timeout = kDefaultTimeoutMs;
    } else {
        // Generous 3x safety margin over the observed mean, matching the
        // "case 2 was slower than case 1, don't false-positive an error"
        // requirement — the ceiling grows with real-world variance instead
        // of being a single guessed constant.
        timeout = static_cast<int>(it->second * 3.0);
    }
    return std::clamp(timeout, kDefaultTimeoutMs, hardCeilingMs);
}

std::chrono::milliseconds StateMachine::nextPollInterval(
    std::chrono::milliseconds current) const {
    const auto next = std::chrono::milliseconds(
        static_cast<long long>(current.count() * kPollBackoffFactor));
    return std::min(next, std::chrono::milliseconds(kPollMaxMs));
}

} // namespace rpa::core