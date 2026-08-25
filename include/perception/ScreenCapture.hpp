#pragma once

#include <memory>
#include <mutex>
#include <chrono>
#include <opencv2/core.hpp>

#include "../platform/IPlatform.hpp"

namespace rpa::perception {

/// Thin, cache-aware wrapper around IPlatform::captureScreen(). Exists as
/// its own class (rather than CoreEngine calling IPlatform directly) for
/// two reasons: (1) it gives StateMachine's tight polling loop a single
/// place to enforce a minimum re-capture interval so rapid polling can't
/// hammer the OS harder than the screen can actually redraw, and (2) it
/// isolates every other perception class from IPlatform, so
/// VisionProcessor/OCRProcessor never need to know a platform exists.
class ScreenCapture {
public:
    explicit ScreenCapture(std::shared_ptr<platform::IPlatform> platform);

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    /// Captures the currently sandboxed/active region fresh from the OS.
    /// Throws std::runtime_error (propagated from IPlatform) on failure —
    /// callers in StateMachine already treat capture failure as "not yet
    /// satisfied" rather than fatal, so propagating is correct here.
    cv::Mat captureActiveRegion();

    /// Returns the last successfully captured frame without triggering a
    /// new capture. Empty Mat if nothing has been captured yet. Useful for
    /// SecurityLayer's frame-diff check to avoid a redundant capture call
    /// in the same polling tick.
    cv::Mat lastFrame() const;

    /// Returns the bounding rectangle of the currently sandboxed window,
    /// delegated straight through to IPlatform. Exposed here so
    /// VisionProcessor/CoreEngine don't need direct IPlatform access.
    cv::Rect activeRegionBounds() const;

private:
    std::shared_ptr<platform::IPlatform> platform_;

    mutable std::mutex frameMutex_;
    cv::Mat lastFrame_; // guarded by frameMutex_
};

} // namespace rpa::perception