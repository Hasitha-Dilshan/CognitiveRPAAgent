#include "perception/VisionProcessor.hpp"
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <algorithm>
#include <stdexcept>

namespace rpa::perception {

VisionProcessor::VisionProcessor() {
    // nfeatures=500 balances detection quality against per-frame cost —
    // UI icons are small, low-detail targets, so we don't need the 2000+
    // feature budget typical of full-scene ORB usage; this keeps a single
    // match attempt in the low-single-digit milliseconds range.
    orb_ = cv::ORB::create(
        /*nfeatures=*/500,
        /*scaleFactor=*/1.2f,
        /*nlevels=*/8,
        /*edgeThreshold=*/15,   // smaller than default (31) since UI icons
                                // are often small and near the frame edge
        /*firstLevel=*/0,
        /*WTA_K=*/2,
        cv::ORB::HARRIS_SCORE,
        /*patchSize=*/15,
        /*fastThreshold=*/20);

    // Brute-force Hamming matcher — correct distance metric for ORB's
    // binary descriptors, and brute-force is actually faster than FLANN
    // here given how few keypoints a single icon template produces.
    matcher_ = cv::DescriptorMatcher::create(cv::DescriptorMatcher::BRUTEFORCE_HAMMING);

    loadBuiltInIcons();
}

void VisionProcessor::loadBuiltInIcons() {
    // Hidden-menu affordance templates are loaded lazily/defensively: a
    // missing icon asset should degrade findHiddenMenuIcon() to "found
    // nothing" rather than crash VisionProcessor construction, since the
    // whole engine shouldn't fail to start over an optional asset.
    const std::vector<std::string> iconPaths = {
        "assets/templates/hamburger_menu.png",
        "assets/templates/kebab_menu_vertical.png",
        "assets/templates/kebab_menu_horizontal.png"
    };

    for (const auto& path : iconPaths) {
        cv::Mat icon = cv::imread(path, cv::IMREAD_COLOR);
        if (!icon.empty()) {
            hiddenMenuTemplates_.push_back(std::move(icon));
        }
        // Silently skip missing files — logged upstream by CoreEngine's
        // initialization sequence rather than here, keeping this
        // constructor exception-free for genuinely optional assets.
    }
}

MatchResult VisionProcessor::matchTemplate(const cv::Mat& screen,
                                            const cv::Mat& templateImg,
                                            double confidenceThreshold) const {
    MatchResult result;

    if (screen.empty() || templateImg.empty()) {
        return result; // matched stays false
    }
    if (templateImg.cols > screen.cols || templateImg.rows > screen.rows) {
        // Template larger than the search image is a caller error (e.g.
        // passing a full-screen reference instead of a cropped icon) —
        // return "no match" rather than letting cv::matchTemplate assert/throw.
        return result;
    }

    cv::Mat scoreMap;
    // TM_CCOEFF_NORMED: normalized correlation coefficient, robust to
    // brightness/contrast shifts between the captured screen and the
    // stored template (e.g. dark mode vs light mode of the same app).
    cv::matchTemplate(screen, templateImg, scoreMap, cv::TM_CCOEFF_NORMED);

    double minVal, maxVal;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(scoreMap, &minVal, &maxVal, &minLoc, &maxLoc);

    if (maxVal >= confidenceThreshold) {
        result.matched = true;
        result.confidence = maxVal;
        result.center = cv::Point(
            maxLoc.x + templateImg.cols / 2,
            maxLoc.y + templateImg.rows / 2);
    }

    return result;
}

MatchResult VisionProcessor::matchFeatures(const cv::Mat& screen,
                                            const cv::Mat& templateImg,
                                            double minGoodMatchRatio) const {
    MatchResult result;

    if (screen.empty() || templateImg.empty()) {
        return result;
    }

    // ORB operates on grayscale — color carries no keypoint-detection
    // benefit here and just adds conversion overhead if skipped.
    cv::Mat screenGray, templateGray;
    cv::cvtColor(screen, screenGray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(templateImg, templateGray, cv::COLOR_BGR2GRAY);

    std::vector<cv::KeyPoint> screenKeypoints, templateKeypoints;
    cv::Mat screenDescriptors, templateDescriptors;

    orb_->detectAndCompute(screenGray, cv::noArray(), screenKeypoints, screenDescriptors);
    orb_->detectAndCompute(templateGray, cv::noArray(), templateKeypoints, templateDescriptors);

    // Small/flat-color icons can legitimately produce too few keypoints
    // for reliable matching (e.g. a plain colored square). Falling
    // through to "no match" here is correct — it signals the caller
    // (CoreEngine) to proceed to the OCR fallback rather than trusting a
    // statistically meaningless match.
    constexpr int kMinKeypointsRequired = 4;
    if (screenDescriptors.empty() || templateDescriptors.empty() ||
        static_cast<int>(templateKeypoints.size()) < kMinKeypointsRequired) {
        return result;
    }

    // knnMatch with k=2 enables Lowe's ratio test below, which is
    // significantly more reliable at rejecting false positives than a
    // raw single-nearest-neighbor threshold — important here since UI
    // screens often contain many small, visually similar icons.
    std::vector<std::vector<cv::DMatch>> knnMatches;
    matcher_->knnMatch(templateDescriptors, screenDescriptors, knnMatches, 2);

    std::vector<cv::DMatch> goodMatches;
    goodMatches.reserve(knnMatches.size());
    constexpr float kLoweRatioThreshold = 0.75f;
    for (const auto& pair : knnMatches) {
        if (pair.size() < 2) continue; // not enough neighbors to apply the ratio test safely
        if (pair[0].distance < kLoweRatioThreshold * pair[1].distance) {
            goodMatches.push_back(pair[0]);
        }
    }

    const double matchRatio = templateKeypoints.empty()
        ? 0.0
        : static_cast<double>(goodMatches.size()) / static_cast<double>(templateKeypoints.size());

    if (matchRatio < minGoodMatchRatio || goodMatches.size() < 4) {
        // Fewer than 4 good matches also fails homography estimation
        // below (minimum needed to solve for a perspective transform),
        // so bail out early rather than letting findHomography fail silently.
        return result;
    }

    // Estimate where the template's center lands in screen space by
    // computing a homography from matched keypoints, rather than simply
    // averaging matched screen keypoint locations — homography correctly
    // accounts for the target having been scaled/rotated (the whole
    // point of using ORB over template matching for responsive layouts).
    std::vector<cv::Point2f> templatePoints, screenPoints;
    templatePoints.reserve(goodMatches.size());
    screenPoints.reserve(goodMatches.size());
    for (const auto& m : goodMatches) {
        templatePoints.push_back(templateKeypoints[m.queryIdx].pt);
        screenPoints.push_back(screenKeypoints[m.trainIdx].pt);
    }

    std::vector<uchar> inlierMask;
    cv::Mat homography = cv::findHomography(templatePoints, screenPoints,
                                             cv::RANSAC, 5.0, inlierMask);

    if (homography.empty()) {
        return result; // degenerate point configuration — treat as no match
    }

    // Count RANSAC inliers separately from raw "good matches" — inlier
    // ratio is the real confidence signal, since Lowe's ratio test alone
    // still admits geometrically inconsistent matches that RANSAC then filters.
    const int inlierCount = cv::countNonZero(inlierMask);
    const double inlierRatio = static_cast<double>(inlierCount) / static_cast<double>(goodMatches.size());

    constexpr double kMinInlierRatio = 0.5;
    if (inlierRatio < kMinInlierRatio) {
        return result;
    }

    // Project the template's center point through the homography to find
    // its corresponding location in screen space.
    std::vector<cv::Point2f> templateCenter = {
        cv::Point2f(static_cast<float>(templateImg.cols) / 2.0f,
                    static_cast<float>(templateImg.rows) / 2.0f)
    };
    std::vector<cv::Point2f> projectedCenter;
    cv::perspectiveTransform(templateCenter, projectedCenter, homography);

    result.matched = true;
    result.confidence = inlierRatio; // 0.0-1.0, consistent scale with matchTemplate's confidence
    result.center = cv::Point(
        static_cast<int>(std::round(projectedCenter[0].x)),
        static_cast<int>(std::round(projectedCenter[0].y)));

    return result;
}

bool VisionProcessor::detectLoadingIndicator(const cv::Mat& screen) const {
    if (screen.empty()) return false;

    // Heuristic rather than template-matching a specific spinner graphic
    // (which varies per app): loading spinners are near-universally
    // characterized by a tight cluster of saturated, non-background-colored
    // pixels undergoing continuous rotation. Since a single frame can't
    // observe motion, this detects the *static* visual signature instead —
    // a small circular arrangement of high-saturation pixels — which
    // covers the vast majority of modern UI spinner designs (Material,
    // Fluent, and most custom CSS spinners) without needing per-app assets.
    cv::Mat hsv;
    cv::cvtColor(screen, hsv, cv::COLOR_BGR2HSV);

    cv::Mat saturationMask;
    // High saturation + mid-to-high value catches vivid spinner colors
    // (typically brand blues/greens) while excluding grayscale UI chrome.
    cv::inRange(hsv, cv::Scalar(0, 120, 100), cv::Scalar(180, 255, 255), saturationMask);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(saturationMask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    for (const auto& contour : contours) {
        const double area = cv::contourArea(contour);
        // Spinners are small (roughly 16px-80px typical UI sizing) —
        // filters out large saturated regions like colored banners/buttons
        // which are not loading indicators.
        if (area < 50.0 || area > 4000.0) continue;

        const double perimeter = cv::arcLength(contour, true);
        if (perimeter <= 0.0) continue;

        // Circularity = 4π·Area / Perimeter². A perfect circle scores 1.0;
        // most spinner arcs/rings score 0.5-0.85 depending on how much of
        // the ring is rendered at capture time. Buttons/icons/text tend to
        // score well below this range due to sharper edges and corners.
        const double circularity = (4.0 * CV_PI * area) / (perimeter * perimeter);
        if (circularity > 0.4) {
            return true;
        }
    }

    return false;
}

std::optional<cv::Point> VisionProcessor::findHiddenMenuIcon(const cv::Mat& screen) const {
    if (screen.empty() || hiddenMenuTemplates_.empty()) {
        return std::nullopt;
    }

    // Hidden-menu icons are almost always positioned in the corners of a
    // toolbar/header, so search is restricted to the top strip of the
    // window rather than the full frame — meaningfully faster and avoids
    // false-positive matches against unrelated content lower on the page.
    const int searchHeight = std::min(screen.rows, std::max(60, screen.rows / 8));
    const cv::Rect topStrip(0, 0, screen.cols, searchHeight);
    const cv::Mat searchRegion = screen(topStrip);

    cv::Point bestMatch;
    double bestConfidence = 0.0;
    bool found = false;

    constexpr double kHiddenMenuConfidenceThreshold = 0.75; // slightly relaxed vs. general
                                                              // matching since hamburger/kebab
                                                              // icons are simple, low-detail shapes
                                                              // that naturally score lower even on
                                                              // a correct match
    for (const auto& tmpl : hiddenMenuTemplates_) {
        MatchResult result = matchTemplate(searchRegion, tmpl, kHiddenMenuConfidenceThreshold);
        if (result.matched && result.confidence > bestConfidence) {
            bestConfidence = result.confidence;
            bestMatch = result.center; // already in searchRegion-local coords == screen coords
                                        // since topStrip starts at (0,0)
            found = true;
        }
    }

    if (!found) {
        return std::nullopt;
    }
    return bestMatch;
}

} // namespace rpa::perception