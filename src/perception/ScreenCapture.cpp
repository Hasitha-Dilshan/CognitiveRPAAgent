#include "perception/ScreenCapture.hpp"
#include <stdexcept>

namespace rpa::perception {

ScreenCapture::ScreenCapture(std::shared_ptr<platform::IPlatform> platform)
    : platform_(std::move(platform)) {
    if (!platform_) {
        throw std::invalid_argument("ScreenCapture: platform must not be null");
    }
}

cv::Mat ScreenCapture::captureActiveRegion() {
    // No artificial throttling here — StateMachine already paces its own
    // polling interval (100ms-2000ms adaptive backoff), so ScreenCapture
    // stays a simple, honest pass-through rather than duplicating that 
    // rate-limiting logic in two places with potentially conflicting timings.
    cv::Mat frame = platform_->captureScreen();

    if (frame.empty()) {
        throw std::runtime_error("ScreenCapture: platform returned an empty frame");
    }

    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        frame.copyTo(lastFrame_);
    }

    return frame;
}

cv::Mat ScreenCapture::lastFrame() const {
    std::lock_guard<std::mutex> lock(frameMutex_);
    return lastFrame_.clone(); // clone so caller can't mutate our cached copy
}

cv::Rect ScreenCapture::activeRegionBounds() const {
    return platform_->getActiveWindowBounds();
}

} // namespace rpa::perception