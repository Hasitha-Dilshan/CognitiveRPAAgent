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
/// It owns the main execution loop and is the only class that mutates
/// agent-wide state. All subsystems are injected via unique_ptr for
/// testability and clean lifetime management (RAII, no raw owning pointers).
class CoreEngine {
public:
    explicit CoreEngine(std::unique_ptr<platform::IPlatform> platform);
    ~CoreEngine();

    // Non-copyable: this class owns unique hardware/OS resources.
    CoreEngine(const CoreEngine&) = delete;
    CoreEngine& operator=(const CoreEngine&) = delete;

    /// Loads base_skills.json and memory.json from disk into the knowledge base.
    bool initialize(const std::string& baseSkillsPath,
                     const std::string& memoryPath);

    /// Attempts to locate and click a named skill target on screen.
    /// Applies fallback chain: template match -> feature match -> OCR.
    /// Returns true if the action was executed successfully.
    bool executeSkill(const std::string& skillName);

    /// Blocks (without fixed sleep) until a described visual state is reached,
    /// or until the adaptive timeout elapses. Returns false on timeout.
    bool waitForState(const std::string& stateDescription,
                       int maxTimeoutMs = 30000);

    /// Starts the engine's background monitoring thread (safety watchdog).
    void start();

    /// Signals all loops to stop and joins the worker thread.
    void stop();

    /// True if the security layer has triggered an emergency halt.
    bool isHalted() const noexcept { return halted_.load(); }

private:
    /// Core fallback pipeline: template -> ORB feature match -> OCR text search.
    LocateResult locateTarget(const SkillTarget& target);

    /// Persists newly learned target features back to memory.json.
    void learnFromSuccess(SkillTarget& target, const LocateResult& result);

    /// Background thread body: watches for kill-switch conditions.
    void monitorLoop();

    std::unique_ptr<platform::IPlatform>      platform_;
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
};

} // namespace rpa::core