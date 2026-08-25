#include "perception/OCRProcessor.hpp"
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <opencv2/imgproc.hpp>

namespace rpa::perception {

namespace {
    std::string toLowerCopy(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                        [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    /// RAII wrapper for Leptonica PIX*, since pixDestroy takes a PIX**
    /// and forgetting to call it leaks the underlying image buffer.
    class ScopedPix {
    public:
        explicit ScopedPix(PIX* pix) : pix_(pix) {}
        ~ScopedPix() { if (pix_) pixDestroy(&pix_); }
        ScopedPix(const ScopedPix&) = delete;
        ScopedPix& operator=(const ScopedPix&) = delete;
        PIX* get() const { return pix_; }
    private:
        PIX* pix_;
    };
}

OCRProcessor::OCRProcessor(const std::string& tessdataPath, const std::string& language) {
    tessApi_ = std::make_unique<tesseract::TessBaseAPI>();

    const int initResult = tessApi_->Init(tessdataPath.c_str(), language.c_str(),
                                           tesseract::OEM_LSTM_ONLY);
    if (initResult != 0) {
        throw std::runtime_error(
            "OCRProcessor: Tesseract initialization failed for language '" + language +
            "' using tessdata path '" + tessdataPath +
            "' — verify the .traineddata file exists at this path");
    }

    // PSM_SPARSE_TEXT: treats the image as scattered UI text rather than a
    // uniform document/paragraph block, which matches how buttons/labels
    // are actually laid out on a screen rather than in a text document.
    tessApi_->SetPageSegMode(tesseract::PSM_SPARSE_TEXT);

    // Speed over exhaustive accuracy: UI text is short, high-contrast, and
    // rendered (not handwritten/noisy), so the default LSTM engine at
    // normal DPI assumptions is sufficient without additional tuning passes.
    tessApi_->SetVariable("tessedit_do_invert", "0");
}

OCRProcessor::~OCRProcessor() {
    if (tessApi_) {
        tessApi_->End(); // releases Tesseract's internal buffers explicitly
    }
}

PIX* OCRProcessor::matToPix(const cv::Mat& bgrFrame) const {
    if (bgrFrame.empty()) {
        throw std::invalid_argument("OCRProcessor: matToPix received an empty frame");
    }

    // Convert to grayscale first — OCR accuracy on UI screenshots is
    // consistently better on grayscale + thresholded input than raw color,
    // and it roughly halves the data Tesseract has to process.
    cv::Mat gray;
    cv::cvtColor(bgrFrame, gray, cv::COLOR_BGR2GRAY);

    // Light contrast boost helps with anti-aliased UI fonts on flat
    // backgrounds, which otherwise sometimes fall just below Tesseract's
    // internal binarization threshold.
    cv::Mat enhanced;
    cv::equalizeHist(gray, enhanced);

    PIX* pix = pixCreate(enhanced.cols, enhanced.rows, 8);
    if (!pix) {
        throw std::runtime_error("OCRProcessor: pixCreate failed (out of memory?)");
    }

    // Copy row-by-row rather than assuming matching stride — cv::Mat rows
    // can be padded, and Leptonica's PIX row stride is its own separate
    // value (wpl = words per line), so a raw memcpy across the whole
    // buffer would corrupt the image if the strides ever diverge.
    for (int y = 0; y < enhanced.rows; ++y) {
        const uchar* srcRow = enhanced.ptr<uchar>(y);
        for (int x = 0; x < enhanced.cols; ++x) {
            pixSetPixel(pix, x, y, static_cast<l_uint32>(srcRow[x]));
        }
    }

    return pix;
}

std::vector<OCRMatch> OCRProcessor::extractAllText(const cv::Mat& frame, float minConfidence) const {
    std::lock_guard<std::mutex> lock(tessMutex_);
    std::vector<OCRMatch> results;

    if (frame.empty()) {
        return results; // empty input is not an error condition here — just no results
    }

    ScopedPix pix(matToPix(frame));
    tessApi_->SetImage(pix.get());

    // Recognize() must run before any result accessors are valid; a
    // non-zero return indicates an internal Tesseract failure (rare, but
    // possible on a corrupt/degenerate image).
    if (tessApi_->Recognize(nullptr) != 0) {
        return results; // fail soft — treat as "no text found" rather than throwing,
                         // since this sits in a fallback path that itself should not crash the agent
    }

    std::unique_ptr<tesseract::ResultIterator> iter(tessApi_->GetIterator());
    if (!iter) {
        return results;
    }

    const tesseract::PageIteratorLevel level = tesseract::RIL_WORD;
    do {
        const std::unique_ptr<char[]> word(iter->GetUTF8Text(level));
        if (!word || word[0] == '\0') continue;

        const float confidence = iter->Confidence(level);
        if (confidence < minConfidence) continue;

        int left, top, right, bottom;
        if (!iter->BoundingBox(level, &left, &top, &right, &bottom)) continue;

        OCRMatch match;
        match.text = word.get();
        match.boundingBox = cv::Rect(left, top, right - left, bottom - top);
        match.confidence = confidence;
        results.push_back(std::move(match));

    } while (iter->Next(level));

    return results;
}

std::optional<OCRMatch> OCRProcessor::findText(const cv::Mat& frame,
                                                const std::string& targetPhrase,
                                                float minConfidence) const {
    if (targetPhrase.empty()) {
        return std::nullopt; // searching for nothing is not a valid request
    }

    const std::vector<OCRMatch> allMatches = extractAllText(frame, minConfidence);
    const std::string needle = toLowerCopy(targetPhrase);

    std::optional<OCRMatch> best;
    for (const auto& match : allMatches) {
        if (toLowerCopy(match.text).find(needle) == std::string::npos) {
            continue;
        }
        // Prefer the highest-confidence match if multiple words on screen
        // happen to contain the target substring (e.g. "Submit" appearing
        // in both a button and a tooltip).
        if (!best.has_value() || match.confidence > best->confidence) {
            best = match;
        }
    }

    return best;
}

} // namespace rpa::perception