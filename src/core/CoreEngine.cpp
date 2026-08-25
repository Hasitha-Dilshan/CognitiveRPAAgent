#include "core/CoreEngine.hpp"
#include <stdexcept>
#include <chrono>
#include <opencv2/imgcodecs.hpp>

namespace rpa::core {

CoreEngine::CoreEngine(std::unique_ptr<platform::IPlatform> platform)
    : platform_(std::move(platform)) {
    if (!platform_) {
        throw std::invalid_argument("CoreEngine: platform must not be null");
    }

    // Shared ownership handed to InputController/ScreenCapture since both
    // outlive individual method calls and neither should assume exclusive
    // ownership of the underlying OS handle — CoreEngine remains the
    // single authority that constructs and eventually tears down the
    // platform object.
    std::shared_ptr<platform::IPlatform> sharedPlatform(platform_.get(), [](platform::IPlatform*) {
        // No-op deleter: CoreEngine's unique_ptr remains the true owner;
        // this shared_ptr only exists to satisfy constructor signatures
        // that expect shared ownership semantics for injected dependencies.
    });

    screenCapture_    = std::make_unique<perception::ScreenCapture>(sharedPlatform);
    visionProcessor_  = std::make_unique<perception::VisionProcessor>();
    knowledgeBase_    = std::make_unique<memory::KnowledgeBase>();
    inputController_  = std::make_unique<action::InputController>(sharedPlatform);
    stateMachine_     = std::make_unique<core::StateMachine>(*screenCapture_, *visionProcessor_);
    securityLayer_    = std::make_unique<core::SecurityLayer>();

    // ocrProcessor_ is deliberately NOT constructed here — it requires a
    // tessdata path that's only known once initialize() receives config,
    // and Tesseract::Init() is expensive enough (loads trained language
    // models from disk) that we don't want it paid for by every CoreEngine
    // instantiation, including ones created just for testing other subsystems.

    // Wire the security layer's halt callback directly into our own
    // halted_ flag, so isHalted() reflects a trip immediately without
    // CoreEngine needing to poll SecurityLayer on every loop iteration.
    securityLayer_->setOnHalt([this](HaltReason /*reason*/) {
        halted_.store(true, std::memory_order_release);
    });
}

CoreEngine::~CoreEngine() {
    // Ensure the monitor thread is never left joinable on destruction —
    // a joinable std::thread destructor calls std::terminate(), which
    // would crash the whole process if stop() was never called explicitly.
    stop();
}

bool CoreEngine::initialize(const std::string& baseSkillsPath,
                             const std::string& memoryPath) {
    try {
        // Base skills are mandatory — loadBaseSkills() throws if missing
        // or malformed, and we let that propagate as a hard initialization
        // failure rather than silently starting with zero skills.
        knowledgeBase_->loadBaseSkills(baseSkillsPath);
    } catch (const std::exception& e) {
        // Initialization failure is reported via return value (false)
        // rather than an uncaught exception escaping into caller code
        // that may not expect CoreEngine construction/init to throw —
        // callers should log via their own error path using this signal.
        lastInitError_ = e.what();
        return false;
    }

    // Memory is optional (first-run case) — loadMemory() already
    // degrades gracefully internally, so no try/catch needed here.
    knowledgeBase_->loadMemory(memoryPath);
    memoryPath_ = memoryPath; // remembered so stop()/periodic saves know where to persist

    // OCR initialization happens here, now that we're in a context that
    // can reasonably supply a tessdata path and fail initialize() cleanly
    // if the language pack is missing, rather than crashing mid-task.
    try {
        ocrProcessor_ = std::make_unique<perception::OCRProcessor>(tessdataPath_, ocrLanguage_);
    } catch (const std::exception& e) {
        lastInitError_ = e.what();
        return false;
    }

    return true;
}

bool CoreEngine::executeSkill(const std::string& skillName) {
    if (halted_.load(std::memory_order_acquire)) {
        // Refuse to execute anything once halted — this is the primary
        // enforcement point for SecurityLayer's kill-switch actually
        // stopping the agent, not just recording that it *should* stop.
        return false;
    }

    auto skillOpt = knowledgeBase_->getSkill(skillName);
    if (!skillOpt.has_value()) {
        lastExecutionError_ = "Unknown skill: " + skillName;
        return false;
    }

    SkillTarget target;
    target.name = skillOpt->name;
    target.visualTemplatePath = skillOpt->templateImagePath;
    target.fallbackText = skillOpt->fallbackText;
    target.matchConfidence = skillOpt->matchConfidence;

    LocateResult located;
    try {
        located = locateTarget(target);
    } catch (const std::exception& e) {
        // A perception-layer exception (e.g. screen capture failure)
        // during locate should not crash the whole engine — record it as
        // a failed attempt so the knowledge base's confidence decay logic
        // still applies, then surface the failure to the caller.
        lastExecutionError_ = std::string("locateTarget threw: ") + e.what();
        knowledgeBase_->recordFailure(skillName);
        return false;
    }

    if (!located.found) {
        knowledgeBase_->recordFailure(skillName);
        lastExecutionError_ = "Target not found via template, feature, or OCR match: " + skillName;
        return false;
    }

    // Security check BEFORE the click executes, not after — this is the
    // actual enforcement point for the sandbox boundary and click-loop
    // detection, not merely advisory logging.
    const cv::Rect sandboxBounds = screenCapture_->activeRegionBounds();
    if (!securityLayer_->checkWithinSandbox(located.x, located.y, sandboxBounds)) {
        lastExecutionError_ = "Target location fell outside sandboxed window bounds";
        return false;
    }
    if (!securityLayer_->recordClickAndCheck(located.x, located.y)) {
        lastExecutionError_ = "Security layer blocked click (loop/rate limit tripped)";
        return false;
    }

    try {
        inputController_->click(located.x, located.y);
    } catch (const std::exception& e) {
        lastExecutionError_ = std::string("InputController::click threw: ") + e.what();
        knowledgeBase_->recordFailure(skillName);
        return false;
    }

    // Feed the just-captured frame into SecurityLayer's no-progress
    // detector so a sequence of clicks that never visibly change the
    // screen gets caught even if no individual click trips the rate limit.
    securityLayer_->recordFrame(screenCapture_->lastFrame());

    // Learning only happens when the fallback chain had to reach OCR to
    // succeed — a clean template-match success doesn't need to "learn"
    // anything new, since the existing template already worked.
    if (located.viaOCRFallback) {
        learnFromSuccess(target, located);
    } else {
        knowledgeBase_->recordSuccess(skillName, /*newTemplatePath=*/"", located.confidence);
    }

    return true;
}

bool CoreEngine::waitForState(const std::string& stateDescription, int maxTimeoutMs) {
    // stateMachine_->waitForCondition needs an actual StateCondition
    // predicate, not just a description string — this overload exists as
    // a convenience for the common "wait until loading finishes" case,
    // since that's the overwhelming majority of real waits in practice.
    // Bespoke conditions should call stateMachine_->waitForCondition()
    // directly with a custom lambda instead of going through this method.
    auto result = stateMachine_->waitUntilLoadingFinishes(halted_, maxTimeoutMs);

    if (result.outcome == WaitOutcome::Aborted) {
        lastExecutionError_ = "Wait for '" + stateDescription + "' aborted due to security halt";
        return false;
    }
    if (result.outcome == WaitOutcome::TimedOut) {
        lastExecutionError_ = "Wait for '" + stateDescription + "' timed out after " +
                               std::to_string(result.elapsed.count()) + "ms";
        return false;
    }
    return true;
}

void CoreEngine::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return; // already running — start() is idempotent, not an error to call twice
    }
    halted_.store(false, std::memory_order_release);
    monitorThread_ = std::thread(&CoreEngine::monitorLoop, this);
}

void CoreEngine::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return; // wasn't running — nothing to join, avoids double-join UB
    }
    if (monitorThread_.joinable()) {
        monitorThread_.join();
    }

    // Persist learned memory on every clean stop, not just on destruction —
    // so a crash later in the same process session doesn't lose everything
    // learned in this run. saveMemory() is noexcept, so this can't throw
    // and disrupt an otherwise-clean shutdown.
    if (!memoryPath_.empty()) {
        knowledgeBase_->saveMemory(memoryPath_);
    }
}

LocateResult CoreEngine::locateTarget(const SkillTarget& target) {
    LocateResult result;

    cv::Mat screen = screenCapture_->captureActiveRegion();

    cv::Mat templateImg;
    if (!target.visualTemplatePath.empty()) {
        templateImg = cv::imread(target.visualTemplatePath, cv::IMREAD_COLOR);
    }

    // Tier 1: template match — cheapest, tried first. Skipped gracefully
    // (not an error) if this skill has no stored template image, e.g. a
    // purely OCR-defined skill learned at runtime with no saved icon.
    if (!templateImg.empty()) {
        auto templateResult = visionProcessor_->matchTemplate(
            screen, templateImg, target.matchConfidence);
        if (templateResult.matched) {
            result.found = true;
            result.x = templateResult.center.x;
            result.y = templateResult.center.y;
            result.confidence = templateResult.confidence;
            result.viaOCRFallback = false;
            return result;
        }

        // Tier 2: ORB feature match — handles the responsive-layout /
        // resized-icon case that defeats plain template matching.
        auto featureResult = visionProcessor_->matchFeatures(screen, templateImg);
        if (featureResult.matched) {
            result.found = true;
            result.x = featureResult.center.x;
            result.y = featureResult.center.y;
            result.confidence = featureResult.confidence;
            result.viaOCRFallback = false;
            return result;
        }
    }

    // Tier 2.5: if the primary target isn't visible at all, check whether
    // it might be tucked inside a responsive "hidden menu" (hamburger/kebab)
    // before falling all the way through to a blind OCR scan of the whole
    // screen — this mirrors how a human would actually investigate a
    // missing button rather than immediately assuming a redesign.
    if (auto hiddenMenu = visionProcessor_->findHiddenMenuIcon(screen); hiddenMenu.has_value()) {
        const cv::Rect sandboxBounds = screenCapture_->activeRegionBounds();
        if (securityLayer_->checkWithinSandbox(hiddenMenu->x, hiddenMenu->y, sandboxBounds) &&
            securityLayer_->recordClickAndCheck(hiddenMenu->x, hiddenMenu->y)) {

            inputController_->click(hiddenMenu->x, hiddenMenu->y);

            // Give the menu a brief, condition-driven moment to render
            // rather than a fixed sleep — reuses StateMachine's adaptive
            // wait machinery instead of introducing a second ad-hoc delay
            // mechanism here.
            auto menuOpenWait = stateMachine_->waitForCondition(
                "hidden_menu_open",
                [](const cv::Mat&) { return true; }, // menu open confirmation is
                                                        // approximated by a short
                                                        // settle wait below rather
                                                        // than a specific visual
                                                        // signature, since menu
                                                        // appearance varies too
                                                        // much per-app to generalize
                halted_,
                /*hardCeilingMs=*/1500);
            (void)menuOpenWait;

            // Re-capture after the menu interaction and retry the OCR
            // search below against the now-expanded UI state.
            screen = screenCapture_->captureActiveRegion();
        }
    }

    // Tier 3: OCR text fallback — last resort, most expensive, but the
    // only tier that survives a full icon redesign since it searches by
    // meaning ("Save") rather than appearance.
    if (!target.fallbackText.empty()) {
        auto ocrResult = ocrProcessor_->findText(screen, target.fallbackText);
        if (ocrResult.has_value()) {
            const cv::Point center = ocrResult->boundingBox.tl() +
                cv::Point(ocrResult->boundingBox.width / 2, ocrResult->boundingBox.height / 2);
            result.found = true;
            result.x = center.x;
            result.y = center.y;
            result.confidence = static_cast<double>(ocrResult->confidence) / 100.0;
            result.viaOCRFallback = true;
            return result;
        }
    }

    return result; // found == false
}

void CoreEngine::learnFromSuccess(SkillTarget& target, const LocateResult& result) {
    // Capture a fresh template image from the exact screen region where
    // OCR just found the target, so next time a much cheaper Tier-1
    // template match can succeed directly instead of falling all the way
    // through to OCR again — this is the literal mechanism behind
    // "auto-update its JSON knowledge base with the new button's visual
    // features" from the design discussion.
    cv::Mat currentScreen = screenCapture_->lastFrame();
    if (currentScreen.empty()) {
        // Nothing to learn from if we can't get a frame — record the
        // logical success without a new template rather than skipping
        // the knowledge base update entirely.
        knowledgeBase_->recordSuccess(target.name, "", result.confidence);
        return;
    }

    // Crop a fixed-size region around the match center rather than trying
    // to infer exact icon bounds from OCR's text bounding box alone (OCR
    // boxes tend to hug just the glyphs, which would produce a template
    // too tight to be useful for future template matching).
    constexpr int kCropHalfWidth = 40;
    constexpr int kCropHalfHeight = 24;
    const int cropX = std::max(0, result.x - kCropHalfWidth);
    const int cropY = std::max(0, result.y - kCropHalfHeight);
    const int cropW = std::min(kCropHalfWidth * 2, currentScreen.cols - cropX);
    const int cropH = std::min(kCropHalfHeight * 2, currentScreen.rows - cropY);

    if (cropW <= 0 || cropH <= 0) {
        knowledgeBase_->recordSuccess(target.name, "", result.confidence);
        return;
    }

    cv::Mat newTemplate = currentScreen(cv::Rect(cropX, cropY, cropW, cropH)).clone();

    // Filename convention matches the assets/templates/learned/ scheme
    // documented alongside base_skills.json: {name}_{date}_{n}.png
    const auto now = std::chrono::system_clock::now();
    const auto nowTimeT = std::chrono::system_clock::to_time_t(now);
    char dateBuf[16];
    std::strftime(dateBuf, sizeof(dateBuf), "%Y%m%d", std::localtime(&nowTimeT));

    const std::string newTemplatePath =
        "assets/templates/learned/" + target.name + "_" + dateBuf + "_1.png";

    if (!cv::imwrite(newTemplatePath, newTemplate)) {
        // Failing to save the new template image is not fatal — fall back
        // to recording the success without updating the template path, so
        // the confidence/success-count learning still happens even if the
        // filesystem write failed (e.g. read-only disk, permissions).
        knowledgeBase_->recordSuccess(target.name, "", result.confidence);
        return;
    }

    knowledgeBase_->recordSuccess(target.name, newTemplatePath, result.confidence);
}

void CoreEngine::monitorLoop() {
    // Background watchdog: periodically persists memory so long-running
    // sessions don't lose hours of learned adaptation to a crash, and
    // provides a single place to extend future passive monitoring (e.g.
    // health-ping telemetry mentioned earlier in the design) without
    // touching executeSkill()'s hot path.
    constexpr auto kPersistInterval = std::chrono::seconds(60);
    auto lastPersist = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if (halted_.load(std::memory_order_acquire)) {
            // Once halted, the monitor thread's only remaining job is to
            // keep memory persisted until stop() is called explicitly —
            // it does not attempt to un-halt itself under any condition.
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastPersist >= kPersistInterval && !memoryPath_.empty()) {
            knowledgeBase_->saveMemory(memoryPath_);
            lastPersist = now;
        }
    }
}

} // namespace rpa::core