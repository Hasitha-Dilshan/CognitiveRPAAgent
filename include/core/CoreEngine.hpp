#pragma once

#include <memory>
#include <thread>
#include <atomic>
#include <string>
#include <optional>
#include <vector>

#include "StateMachine.hpp"
#include "SecurityLayer.hpp"
#include "../perception/VisionProcessor.hpp"
#include "../perception/OCRProcessor.hpp"
#include "../perception/ScreenCapture.hpp"
#include "../memory/KnowledgeBase.hpp"
#include "../action/InputController.hpp"
#include "../platform/IPlatform.hpp"

namespace rpa::core {

/// Represents a single learned/predefined UI target the agent can locate.
struct SkillTarget {
    std::string name;                 // e.g. "SaveButton"
    std::string visualTemplatePath;   // path to reference image, may be empty
    std::string fallbackText;         // OCR fallback phrase, e.g. "Save"
    double matchConfidence = 0.85;    // current confidence threshold (adaptive)
};

/// Result of an attempt to locate a target on screen.
struct LocateResult {
    bool found = false;
    int x = 0;
    int y = 0;
    double confidence = 0.0;
    bool viaOCRFallback = false;
};

/// CoreEngine ties together perception, decision-making, memory and action.
class CoreEngine {
public:
    explicit CoreEngine(std::unique_ptr<platform::IPlatform> platform);
    ~CoreEngine();

    CoreEngine(const CoreEngine&) = delete;
    CoreEngine& operator=(const CoreEngine&) = delete;

    bool initialize(const std::string& baseSkillsPath, const std::string& memoryPath);
    bool executeSkill(const std::string& skillName);
    bool waitForState(const std::string& stateDescription, int maxTimeoutMs = 30000);
    void start();
    void stop();
    bool isHalted() const noexcept { return halted_.load(); }

    // --- UI/Main.cpp සඳහා අලුතින් එක් කළ කොටස් ---
    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    
    void resetSecurityState() {
        securityLayer_->reset();
        halted_.store(false, std::memory_order_release);
    }
    
    const std::string& lastInitError() const noexcept { return lastInitError_; }
    const std::string& lastExecutionError() const noexcept { return lastExecutionError_; }

private:
    LocateResult locateTarget(const SkillTarget& target);
    void learnFromSuccess(SkillTarget& target, const LocateResult& result);
    void monitorLoop();

    std::unique_ptr<platform::IPlatform>       platform_;
    std::unique_ptr<perception::ScreenCapture> screenCapture_;
    std::unique_ptr<perception::VisionProcessor> visionProcessor_;
    std::unique_ptr<perception::OCRProcessor>  ocrProcessor_;
    std::unique_ptr<memory::KnowledgeBase>     knowledgeBase_;
    std::unique_ptr<action::InputController>   inputController_;
    std::unique_ptr<core::StateMachine>        stateMachine_;
    std::unique_ptr<core::SecurityLayer>       securityLayer_;

    std::thread monitorThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> halted_{false};

    // --- UI/Main.cpp සඳහා අලුතින් එක් කළ විචල්‍යයන් ---
    std::string lastInitError_;
    std::string lastExecutionError_;
    std::string memoryPath_;
    std::string tessdataPath_ = "data/tessdata"; 
    std::string ocrLanguage_ = "eng";
};

} // namespace rpa::core