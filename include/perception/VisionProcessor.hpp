#pragma once

#include <opencv2/opencv.hpp>
#include <opencv2/features2d.hpp>
#include <string>
#include <optional>
#include <vector>

namespace rpa::perception {

/// Result of a vision match attempt, in screen-space coordinates.
struct MatchResult {
    bool matched = false;
    cv::Point center{0, 0};
    double confidence = 0.0;
};

/// Handles all pixel-level perception: template matching for fast/simple
/// cases, and ORB feature matching for scale/rotation-invariant recognition
/// when the UI has resized (responsive layouts).
class VisionProcessor {
public:
    VisionProcessor();

    /// Fast path: normalized cross-correlation template matching.
    /// Cheap, but fails if the target has scaled or changed appearance.
    MatchResult matchTemplate(const cv::Mat& screen,
                               const cv::Mat& templateImg,
                               double confidenceThreshold = 0.85) const;

    /// Robust path: ORB keypoint/descriptor matching, invariant to
    /// moderate scaling and rotation. Used when template matching fails,
    /// e.g. after a window resize changes icon dimensions.
    MatchResult matchFeatures(const cv::Mat& screen,
                               const cv::Mat& templateImg,
                               double minGoodMatchRatio = 0.25) const;

    /// Detects whether a "loading" style indicator (spinner, progress bar)
    /// is present, used by StateMachine to avoid fixed-delay waits.
    bool detectLoadingIndicator(const cv::Mat& screen) const;

    /// Locates candidate "hidden menu" icons (hamburger '☰', kebab '⋮')
    /// using template matching against a small built-in icon set.
    std::optional<cv::Point> findHiddenMenuIcon(const cv::Mat& screen) const;

private:
    cv::Ptr<cv::ORB> orb_;
    cv::Ptr<cv::DescriptorMatcher> matcher_;

    // Reference templates for common hidden-menu affordances, loaded once.
    std::vector<cv::Mat> hiddenMenuTemplates_;

    void loadBuiltInIcons();
};

} // namespace rpa::perception