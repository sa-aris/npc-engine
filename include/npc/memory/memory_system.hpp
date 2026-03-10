#pragma once

#include "../core/types.hpp"
#include <vector>
#include <string>
#include <optional>
#include <algorithm>
#include <functional>

namespace npc {

// ─── Memory Source ────────────────────────────────────────────────────────────
enum class MemorySource {
    Observed,
    Hearsay
};
inline std::string memorySourceToString(MemorySource s) {
    return s == MemorySource::Observed ? "observed" : "hearsay";
}

// ─── Memory Struct ────────────────────────────────────────────────────────────
struct Memory {
    MemoryType   type;
    MemorySource source        = MemorySource::Observed;

    std::optional<EntityId> entityId;
    std::optional<EntityId> sourceEntity;

    std::string description;
    float emotionalImpact  = 0.0f;
    float timestamp        = 0.0f;  // total game hours since sim start
    float importance       = 0.5f;
    float decayRate        = 0.01f;
    float currentStrength  = 1.0f;

    // Gossip
    float reliability      = 1.0f;
    int   hopCount         = 0;

    // Day tracking (set at creation; never changes)
    int   dayCreated       = 1;
};

// ─── Gossip Constants ─────────────────────────────────────────────────────────
namespace gossip {
    constexpr float BASE_HOP_DECAY         = 0.75f;
    constexpr float MIN_TRUST_MULTIPLIER   = 0.50f;
    constexpr float MAX_TRUST_MULTIPLIER   = 1.00f;
    constexpr float DISTORTION_THRESHOLD   = 0.35f;
    constexpr float MIN_ACCEPT_RELIABILITY = 0.10f;
}

// ─── MemorySystem ─────────────────────────────────────────────────────────────
struct NpcSerializer; // serialization friend
class MemorySystem {
    friend struct NpcSerializer;
public:
    explicit MemorySystem(size_t maxMemories = 100)
        : maxMemories_(maxMemories) {}

    // ── Add ───────────────────────────────────────────────────────────────────
    void addMemory(Memory mem) {
        mem.currentStrength = 1.0f;
        memories_.push_back(std::move(mem));
        if (memories_.size() > maxMemories_) forgetWeakest();
    }

    void addMemory(MemoryType type, const std::string& desc,
                   float emotionalImpact = 0.0f,
                   std::optional<EntityId> entity = std::nullopt,
                   float importance = 0.5f,
                   float timestamp  = 0.0f,
                   int   dayCreated = 1) {
        Memory m;
        m.type            = type;
        m.description     = desc;
        m.emotionalImpact = emotionalImpact;
        m.entityId        = entity;
        m.importance      = importance;
        m.timestamp       = timestamp;
        m.dayCreated      = dayCreated;
        m.source          = MemorySource::Observed;
        m.reliability     = 1.0f;
        m.hopCount        = 0;
        addMemory(std::move(m));
    }

    // ── Gossip ────────────────────────────────────────────────────────────────
    bool receiveGossip(Memory sourceMemory, EntityId tellerId,
                       float tellerTrust, float currentTime = 0.0f,
                       int   currentDay  = 1) {
        float trustMod = gossip::MIN_TRUST_MULTIPLIER +
                         (tellerTrust + 100.0f) / 200.0f *
                         (gossip::MAX_TRUST_MULTIPLIER - gossip::MIN_TRUST_MULTIPLIER);
        float newRel = sourceMemory.reliability * gossip::BASE_HOP_DECAY * trustMod;
        if (newRel < gossip::MIN_ACCEPT_RELIABILITY) return false;

        Memory gm          = sourceMemory;
        gm.source          = MemorySource::Hearsay;
        gm.sourceEntity    = tellerId;
        gm.reliability     = newRel;
        gm.hopCount        = sourceMemory.hopCount + 1;
        gm.timestamp       = currentTime;
        gm.currentStrength = 1.0f;
        gm.dayCreated      = currentDay;

        if (newRel < gossip::DISTORTION_THRESHOLD) {
            gm.emotionalImpact *= (newRel / gossip::DISTORTION_THRESHOLD);
            gm.description      = "[distorted] " + gm.description;
            gm.importance      *= 0.8f;
        }
        addMemory(std::move(gm));
        return true;
    }

    std::optional<Memory> prepareForGossip(const Memory& mem) const {
        if (mem.reliability     < gossip::MIN_ACCEPT_RELIABILITY) return std::nullopt;
        if (mem.currentStrength < 0.2f)                           return std::nullopt;
        return mem;
    }

    std::vector<Memory> gossipCandidates(float importanceThreshold = 0.4f) const {
        std::vector<Memory> out;
        for (const auto& m : memories_)
            if (m.importance >= importanceThreshold &&
                m.reliability >= gossip::MIN_ACCEPT_RELIABILITY &&
                m.currentStrength >= 0.2f)
                out.push_back(m);
        std::sort(out.begin(), out.end(), [](const Memory& a, const Memory& b){
            return (a.importance * a.reliability) > (b.importance * b.reliability);
        });
        return out;
    }

    // ── Temporal recall ───────────────────────────────────────────────────────
    // Recall memories created on a specific game day.
    std::vector<Memory> recallByDay(int day) const {
        std::vector<Memory> out;
        for (const auto& m : memories_)
            if (m.dayCreated == day) out.push_back(m);
        return out;
    }

    // Recall memories from the last N game days (relative to currentDay).
    std::vector<Memory> recallRecent(int dayCount, int currentDay) const {
        std::vector<Memory> out;
        int cutoff = currentDay - dayCount;
        for (const auto& m : memories_)
            if (m.dayCreated >= cutoff) out.push_back(m);
        std::sort(out.begin(), out.end(), [](const Memory& a, const Memory& b){
            return a.timestamp > b.timestamp;
        });
        return out;
    }

    // Human-readable relative timestamp: "today", "yesterday", "3 days ago"
    static std::string relativeTime(int dayCreated, float timestamp,
                                     int currentDay, float currentTime) {
        int dayDiff = currentDay - dayCreated;
        if (dayDiff == 0) {
            // Same day — use hour
            float hoursAgo = currentTime - timestamp;
            if (hoursAgo < 1.0f)   return "moments ago";
            if (hoursAgo < 2.0f)   return "an hour ago";
            return std::to_string(static_cast<int>(hoursAgo)) + " hours ago";
        }
        if (dayDiff == 1) return "yesterday";
        if (dayDiff <= 6) return std::to_string(dayDiff) + " days ago";
        return "over a week ago";
    }

    // Get a natural-language summary of a memory.
    // e.g. "Yesterday I witnessed: wolf attack near village"
    std::string describeMemory(const Memory& m,
                                int currentDay, float currentTime) const {
        std::string when = relativeTime(m.dayCreated, m.timestamp, currentDay, currentTime);
        std::string src  = (m.source == MemorySource::Hearsay) ? "I heard that " : "I witnessed: ";
        return when + " — " + src + m.description;
    }

    // ── Time update ───────────────────────────────────────────────────────────
    void update(float dt) {
        for (auto& m : memories_) {
            m.currentStrength -= m.decayRate * dt;
            m.currentStrength  = std::max(0.0f, m.currentStrength);
        }
        memories_.erase(
            std::remove_if(memories_.begin(), memories_.end(),
                [](const Memory& m){
                    return m.currentStrength <= 0.0f && m.importance < 0.7f;
                }),
            memories_.end());
    }

    // ── Standard recall ───────────────────────────────────────────────────────
    std::vector<Memory> recall(MemoryType type) const {
        std::vector<Memory> result;
        for (const auto& m : memories_)
            if (m.type == type) result.push_back(m);
        std::sort(result.begin(), result.end(),
            [](const Memory& a, const Memory& b){ return a.timestamp > b.timestamp; });
        return result;
    }

    std::vector<Memory> recallAbout(EntityId entity) const {
        std::vector<Memory> result;
        for (const auto& m : memories_)
            if (m.entityId.has_value() && *m.entityId == entity)
                result.push_back(m);
        return result;
    }

    std::vector<Memory> recallObserved(MemoryType type) const {
        std::vector<Memory> result;
        for (const auto& m : memories_)
            if (m.type == type && m.source == MemorySource::Observed)
                result.push_back(m);
        return result;
    }

    std::vector<Memory> recallHearsay() const {
        std::vector<Memory> result;
        for (const auto& m : memories_)
            if (m.source == MemorySource::Hearsay) result.push_back(m);
        return result;
    }

    float getOpinionOf(EntityId entity) const {
        float opinion = 0.0f; int count = 0;
        for (const auto& m : memories_) {
            if (m.entityId.has_value() && *m.entityId == entity) {
                opinion += m.emotionalImpact * m.currentStrength * m.reliability;
                ++count;
            }
        }
        return count > 0 ? std::clamp(opinion / count, -1.0f, 1.0f) : 0.0f;
    }

    bool hasMemoryOf(MemoryType type,
                     std::optional<EntityId> entity = std::nullopt) const {
        for (const auto& m : memories_)
            if (m.type == type)
                if (!entity.has_value() || m.entityId == entity) return true;
        return false;
    }

    const std::vector<Memory>& allMemories() const { return memories_; }

    Memory* mostRecent() {
        if (memories_.empty()) return nullptr;
        return &*std::max_element(memories_.begin(), memories_.end(),
            [](const Memory& a, const Memory& b){ return a.timestamp < b.timestamp; });
    }

private:
    void forgetWeakest() {
        if (memories_.empty()) return;
        auto it = std::min_element(memories_.begin(), memories_.end(),
            [](const Memory& a, const Memory& b){
                return (a.currentStrength * a.importance * a.reliability) <
                       (b.currentStrength * b.importance * b.reliability);
            });
        memories_.erase(it);
    }

    std::vector<Memory> memories_;
    size_t maxMemories_;
};

} // namespace npc
