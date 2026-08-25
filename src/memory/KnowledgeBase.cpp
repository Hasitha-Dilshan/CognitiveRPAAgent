#include "memory/KnowledgeBase.hpp"
#include <chrono>
#include <algorithm>
#include <cstdio>
namespace rpa::memory {
void to_json(nlohmann::json& j, const SkillRecord& r) {
    j = nlohmann::json{{"name",r.name},{"template_image_path",r.templateImagePath},{"fallback_text",r.fallbackText},{"match_confidence",r.matchConfidence},{"success_count",r.successCount},{"failure_count",r.failureCount},{"last_updated_epoch_ms",r.lastUpdatedEpochMs}};
}
void from_json(const nlohmann::json& j, SkillRecord& r) {
    r.name = j.value("name", std::string{});
    r.templateImagePath = j.value("template_image_path", std::string{});
    r.fallbackText = j.value("fallback_text", std::string{});
    r.matchConfidence = j.value("match_confidence", 0.85);
    r.successCount = j.value("success_count", 0);
    r.failureCount = j.value("failure_count", 0);
    r.lastUpdatedEpochMs = j.value("last_updated_epoch_ms", 0LL);
}
namespace { long long nowEpochMs() { using namespace std::chrono; return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count(); } }
void KnowledgeBase::loadBaseSkills(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) throw std::runtime_error("Cannot open base_skills.json");
    nlohmann::json j; try { file >> j; } catch(...) { throw std::runtime_error("JSON parse error"); }
    if (!j.contains("skills") || !j["skills"].is_array()) throw std::runtime_error("Invalid base_skills.json");
    std::lock_guard<std::mutex> lock(mutex_);
    baseSkills_.clear();
    for (const auto& entry : j["skills"]) {
        SkillRecord record = entry.get<SkillRecord>();
        if (!record.name.empty()) baseSkills_[record.name] = std::move(record);
    }
}
void KnowledgeBase::loadMemory(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;
    nlohmann::json j; try { file >> j; } catch(...) { return; }
    if (!j.contains("learned_skills") || !j["learned_skills"].is_array()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    learnedSkills_.clear();
    for (const auto& entry : j["learned_skills"]) {
        SkillRecord record = entry.get<SkillRecord>();
        if (!record.name.empty()) learnedSkills_[record.name] = std::move(record);
    }
}
bool KnowledgeBase::saveMemory(const std::string& path) noexcept {
    try {
        nlohmann::json j;
        { std::lock_guard<std::mutex> lock(mutex_);
          nlohmann::json arr = nlohmann::json::array();
          for (const auto& [name, record] : learnedSkills_) arr.push_back(record);
          j["learned_skills"] = std::move(arr); }
        const std::string tempPath = path + ".tmp";
        { std::ofstream out(tempPath, std::ios::trunc); if (!out.is_open()) return false; out << j.dump(2); if (!out.good()) return false; }
        if (std::rename(tempPath.c_str(), path.c_str()) != 0) return false;
        return true;
    } catch(...) { return false; }
}
std::optional<SkillRecord> KnowledgeBase::getSkill(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = learnedSkills_.find(name); it != learnedSkills_.end()) return it->second;
    if (auto it = baseSkills_.find(name); it != baseSkills_.end()) return it->second;
    return std::nullopt;
}
void KnowledgeBase::recordSuccess(const std::string& name, const std::string& newTemplatePath, double observedConfidence) {
    std::lock_guard<std::mutex> lock(mutex_);
    SkillRecord record;
    if (auto it = learnedSkills_.find(name); it != learnedSkills_.end()) record = it->second;
    else if (auto it2 = baseSkills_.find(name); it2 != baseSkills_.end()) record = it2->second;
    else { record.name = name; record.matchConfidence = 0.85; }
    if (!newTemplatePath.empty()) record.templateImagePath = newTemplatePath;
    record.successCount += 1;
    record.matchConfidence = std::clamp(std::max(record.matchConfidence, observedConfidence) + kSuccessGrowth, kMinConfidence, kMaxConfidence);
    record.lastUpdatedEpochMs = nowEpochMs();
    learnedSkills_[name] = std::move(record);
}
void KnowledgeBase::recordFailure(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    SkillRecord record; bool exists = true;
    if (auto it = learnedSkills_.find(name); it != learnedSkills_.end()) record = it->second;
    else if (auto it2 = baseSkills_.find(name); it2 != baseSkills_.end()) record = it2->second;
    else exists = false;
    if (!exists) return;
    record.failureCount += 1;
    record.matchConfidence = std::clamp(record.matchConfidence - kFailureDecay, kMinConfidence, kMaxConfidence);
    record.lastUpdatedEpochMs = nowEpochMs();
    learnedSkills_[name] = std::move(record);
}
void KnowledgeBase::upsertSkill(const SkillRecord& record) {
    if (record.name.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    SkillRecord copy = record;
    copy.lastUpdatedEpochMs = nowEpochMs();
    learnedSkills_[copy.name] = std::move(copy);
}
bool KnowledgeBase::hasSkill(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return learnedSkills_.count(name) > 0 || baseSkills_.count(name) > 0;
}
}
