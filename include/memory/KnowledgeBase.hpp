#pragma once

#include <string>
#include <unordered_map>
#include <optional>
#include <mutex>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace rpa::memory {

/// Serializable visual fingerprint of a UI element. Mirrors what
/// VisionProcessor needs to re-locate a target: either a saved template
/// image path (for fast template matching) or raw ORB descriptors (for
/// scale-invariant feature matching), plus an OCR text fallback.
struct SkillRecord {
    std::string name;                  // unique key, e.g. "SaveButton"
    std::string templateImagePath;     // relative path under assets/templates/
    std::string fallbackText;          // OCR phrase fallback, e.g. "Save"
    double matchConfidence = 0.85;     // adaptive threshold, tuned over time
    int successCount = 0;              // times this record resolved successfully
    int failureCount = 0;              // times primary match failed (informs confidence decay)
    long long lastUpdatedEpochMs = 0;  // for staleness inspection / debugging

    /// Converts this record to/from JSON. Kept as free functions below
    /// (ADL-found) per nlohmann/json convention, rather than intrusive
    /// members, so SkillRecord itself has no library dependency leakage
    /// into other translation units that don't need it.
};

// nlohmann/json intrusive-free serialization hooks (ADL).
void to_json(nlohmann::json& j, const SkillRecord& r);
void from_json(const nlohmann::json& j, SkillRecord& r);

/// KnowledgeBase is the agent's persistent memory: a JSON-backed store of
/// "base skills" (pre-defined, shipped with the agent) merged with
/// "learned skills" (self-updated at runtime when a fallback match
/// succeeds). It is the single source of truth CoreEngine consults before
/// attempting to locate any UI target, and the single place learning
/// results get written back to.
///
/// Thread-safety: all public methods are internally synchronized, since
/// CoreEngine's worker thread reads/writes concurrently with any future
/// UI thread that might want to inspect learned skills for display.
class KnowledgeBase {
public:
    KnowledgeBase() = default;

    // Non-copyable: owns a mutex and represents a single source of truth;
    // copying would silently fork the agent's memory.
    KnowledgeBase(const KnowledgeBase&) = delete;
    KnowledgeBase& operator=(const KnowledgeBase&) = delete;

    /// Loads immutable/default skills shipped with the agent (base_skills.json).
    /// Throws std::runtime_error if the file is missing or malformed —
    /// base skills are required for the agent to function at all, so a
    /// silent failure here would be far more dangerous than a loud one.
    void loadBaseSkills(const std::string& path);

    /// Loads previously self-learned skills (memory.json). Unlike base
    /// skills, a missing or empty memory file is a normal first-run state,
    /// not an error — the agent simply starts with no learned overrides.
    void loadMemory(const std::string& path);

    /// Persists current learned skill state to disk at `path`, atomically
    /// (writes to a temp file then renames) so a crash mid-write never
    /// corrupts existing memory. Returns false (does not throw) on I/O
    /// failure so a save error never crashes the agent mid-task.
    bool saveMemory(const std::string& path) noexcept;

    /// Looks up a skill by name, checking learned overrides first (since
    /// they reflect the most recent, most accurate known state of that UI
    /// element) and falling back to the base/default definition.
    /// Returns std::nullopt if the name is unknown entirely.
    std::optional<SkillRecord> getSkill(const std::string& name) const;

    /// Called by CoreEngine::learnFromSuccess() after a fallback (e.g. OCR)
    /// match succeeds where the primary visual match failed. Updates or
    /// inserts the learned record and adjusts matchConfidence.
    /// `newTemplatePath` may be empty if only the confidence/text fallback
    /// is being reinforced rather than a new image being captured.
    void recordSuccess(const std::string& name,
                        const std::string& newTemplatePath,
                        double observedConfidence);

    /// Called when a primary match fails outright (before any fallback is
    /// attempted). Used to gradually lower matchConfidence so future
    /// attempts are more forgiving, per the "dynamic confidence adjustment"
    /// behavior discussed in the design.
    void recordFailure(const std::string& name);

    /// Registers a brand-new skill definition at runtime (e.g. a future
    /// "teach me a new action" feature). Overwrites any existing learned
    /// record with the same name.
    void upsertSkill(const SkillRecord& record);

    /// Returns true if a skill with this name exists in either base or
    /// learned stores — lets CoreEngine fail fast with a clear error
    /// instead of a confusing "not found" deep inside the vision pipeline.
    bool hasSkill(const std::string& name) const;

private:
    mutable std::mutex mutex_;

    // Base skills are effectively read-only after load — shipped defaults.
    std::unordered_map<std::string, SkillRecord> baseSkills_;

    // Learned skills override base skills by name and are what gets
    // persisted back to memory.json.
    std::unordered_map<std::string, SkillRecord> learnedSkills_;

    static constexpr double kMinConfidence = 0.45; // floor: below this, a
                                                     // match is too unreliable
                                                     // to trust even adaptively.
    static constexpr double kMaxConfidence = 0.98;  // ceiling: never claim
                                                     // near-certainty from a
                                                     // single visual heuristic.
    static constexpr double kFailureDecay = 0.05;   // confidence lowered per
                                                     // consecutive failure.
    static constexpr double kSuccessGrowth = 0.02;  // confidence nudged up
                                                     // per confirmed success.
};

} // namespace rpa::memory