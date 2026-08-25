#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <opencv2/core.hpp>
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>

namespace rpa::perception {

/// A single OCR-detected word/phrase with its screen-space bounding box,
/// used both for exact text search and for reporting where a match was found.
struct OCRMatch {
    std::string text;
    cv::Rect boundingBox;
    float confidence = 0.0f; // Tesseract's 0-100 confidence for this word
};

/// Wraps the Tesseract C++ API to provide semantic text-based fallback
/// search: "find the word 'Submit' anywhere on screen" instead of relying
/// on pixel-perfect icon matching. This is the last resort in the
/// perception fallback chain — template match, then ORB feature match,
/// then this.
class OCRProcessor {
public:
    /// `tessdataPath` is the directory containing trained language data
    /// (e.g. eng.traineddata). `language` defaults to English but can be
    /// extended (e.g. "eng+sin" for mixed English/Sinhala UI text).
    explicit OCRProcessor(const std::string& tessdataPath,
                           const std::string& language = "eng");
    ~OCRProcessor();

    OCRProcessor(const OCRProcessor&) = delete;
    OCRProcessor& operator=(const OCRProcessor&) = delete;

    /// Runs OCR across the full frame and returns every detected word
    /// above `minConfidence` with its bounding box. This is the
    /// expensive call — CoreEngine should only invoke it after template
    /// and feature matching have both failed, not on every poll tick.
    std::vector<OCRMatch> extractAllText(const cv::Mat& frame, float minConfidence = 60.0f) const;

    /// Searches for a specific phrase (case-insensitive substring match
    /// against OCR'd words/lines) and returns the first/best match found.
    /// Returns std::nullopt if nothing above `minConfidence` matches.
    std::optional<OCRMatch> findText(const cv::Mat& frame,
                                      const std::string& targetPhrase,
                                      float minConfidence = 60.0f) const;

private:
    /// Converts a BGR cv::Mat into a Leptonica PIX*, the image format
    /// Tesseract's API consumes internally. Returned pointer is
    /// caller-owned (wrapped in RAII at the call site).
    PIX* matToPix(const cv::Mat& bgrFrame) const;

    std::unique_ptr<tesseract::TessBaseAPI> tessApi_;
    mutable std::mutex tessMutex_; // TessBaseAPI is not thread-safe; serialize access
};

} // namespace rpa::perception