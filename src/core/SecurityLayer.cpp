#include "core/SecurityLayer.hpp"
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace rpa::core {

SecurityLayer::SecurityLayer(SecurityConfig config) : config_(std::move(config)) {}

bool SecurityLayer::recordClickAndCheck(int x, int y) {
    if (halted_.load(std::memory_order_acquire)) {
        // Already tripped — refuse every further click until reset().
        // Fail-safe default: when in doubt, block the action.
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();

    // Drop clicks outside the sliding time window first, so the count
    // reflects only recent activity.
    const auto windowStart = now - std::chrono::milliseconds(config_.clickWindowMs);
    while (!recentClicks_.empty() && recentClicks_.front().timestamp < windowStart) {
        recentClicks_.pop_front();
    }

    recentClicks_.push_back({x, y, now});

    // Count how many recent clicks landed within clickProximityPx of this
    // one — catches both exact-repeat clicks and jittery near-repeats.
    int nearbyCount = 0;
    for (const auto& c : recentClicks_) {
        const double dist = std::hypot(c.x - x, c.y - y);
        if (dist <= config_.clickProximityPx) {
            ++nearbyCount;
        }
    }

    if (nearbyCount >= config_.maxClicksPerWindow) {
        // Unlock before calling halt() to avoid deadlocking against
        // triggerHalt()'s own lock-free atomic writes, and so the onHalt_
        // callback (which may call back into this object) doesn't deadlock.
        lock.~lock_guard();
        halt(HaltReason::RepeatedClickLoop);
        return false;
    }

    return true;
}

void SecurityLayer::recordFrame(const cv::Mat& frame) {
    if (halted_.load(std::memory_order_acquire) || frame.empty()) {
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_);

    if (lastFrame_.empty()) {
        frame.copyTo(lastFrame_);
        noProgressCounter_ = 0;
        return;
    }

    // Guard against comparing mismatched sizes (e.g. window was resized
    // between captures) — treat as "progress" rather than crashing on a
    // cv::absdiff size assertion.
    if (lastFrame_.size() != frame.size() || lastFrame_.type() != frame.type()) {
        frame.copyTo(lastFrame_);
        noProgressCounter_ = 0;
        return;
    }

    const double diffPercent = computeFrameDiffPercent(lastFrame_, frame);
    frame.copyTo(lastFrame_);

    if (diffPercent < config_.frameDiffThreshold) {
        ++noProgressCounter_;
    } else {
        noProgressCounter_ = 0;
    }

    const bool shouldHalt = noProgressCounter_ >= config_.maxNoProgressActions;
    lock.unlock();

    if (shouldHalt) {
        halt(HaltReason::NoProgressTimeout);
    }
}

bool SecurityLayer::checkWithinSandbox(int x, int y, const cv::Rect& sandboxBounds) {
    if (halted_.load(std::memory_order_acquire)) {
        return false;
    }

    const bool inside = sandboxBounds.contains(cv::Point(x, y));
    if (!inside) {
        halt(HaltReason::OutOfBoundsAction);
        return false;
    }
    return true;
}

void SecurityLayer::triggerHalt(HaltReason reason) {
    halt(reason);
}

void SecurityLayer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    recentClicks_.clear();
    lastFrame_.release();
    noProgressCounter_ = 0;
    halted_.store(false, std::memory_order_release);
    haltReason_.store(HaltReason::None, std::memory_order_release);
}

void SecurityLayer::setOnHalt(std::function<void(HaltReason)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    onHalt_ = std::move(callback);
}

void SecurityLayer::halt(HaltReason reason) {
    // Only the *first* trip should fire the callback — avoid spamming it
    // if multiple checks trip in quick succession from different threads.
    bool expected = false;
    if (!halted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    haltReason_.store(reason, std::memory_order_release);

    std::function<void(HaltReason)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = onHalt_;
    }
    if (callback) {
        callback(reason);
    }
}

double SecurityLayer::computeFrameDiffPercent(const cv::Mat& a, const cv::Mat& b) const {
    cv::Mat diff;
    cv::absdiff(a, b, diff);
    if (diff.channels() > 1) {
        cv::cvtColor(diff, diff, cv::COLOR_BGR2GRAY);
    }
    cv::Mat mask;
    cv::threshold(diff, mask, 15, 255, cv::THRESH_BINARY); // ignore minor noise/anti-aliasing
    const int changedPixels = cv::countNonZero(mask);
    const int totalPixels = mask.rows * mask.cols;
    return totalPixels > 0 ? (100.0 * changedPixels / totalPixels) : 0.0;
}

} // namespace rpa::core