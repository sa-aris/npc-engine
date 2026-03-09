#pragma once

#include "../core/types.hpp"
#include "../core/vec2.hpp"
#include <map>
#include <vector>
#include <optional>
#include <functional>
#include <cmath>

namespace npc {

// Callback: returns true if line-of-sight is clear between two points
using LoSChecker = std::function<bool(Vec2, Vec2)>;

// Callback: counts solid (wall/building) cells on the line between two points
using WallCounter = std::function<int(Vec2, Vec2)>;

struct PerceptionConfig {
    float sightRange = 15.0f;
    float sightAngle = 120.0f;  // degrees (total FOV)
    float hearingRange = 10.0f;
    float awarenessDecayRate = 0.1f; // per second
    float sightAwarenessWeight = 0.5f;
    float hearingAwarenessWeight = 0.3f;
    float forgetTimeout = 30.0f;
    float combatAwarenessThreshold = 0.8f;
    float alertAwarenessThreshold = 0.5f;
    float suspiciousAwarenessThreshold = 0.2f;
    float inCombatNoise = 0.8f;
    float defaultNoise = 0.3f;
    float wallSoundDamping = 0.5f;  // each wall halves the effective noise
};

struct PerceivedEntity {
    EntityId entityId = INVALID_ENTITY;
    Vec2 lastKnownPosition;
    AwarenessLevel awareness = AwarenessLevel::Unaware;
    float awarenessValue = 0.0f;
    float lastSeenTime = 0.0f;
    bool isHostile = false;
    float noiseLevel = 0.0f;
};

struct SensoryInput {
    EntityId entityId;
    Vec2 position;
    float noiseLevel = 0.0f;
    bool isHostile = false;
};

class PerceptionSystem {
public:
    PerceptionConfig config;
    LoSChecker losChecker;
    WallCounter wallCounter;

    void update(Vec2 ownerPos, Vec2 ownerFacing,
                const std::vector<SensoryInput>& entities,
                float currentTime, float dt) {

        // Decay old awareness
        for (auto& [id, pe] : perceived_) {
            pe.awarenessValue -= config.awarenessDecayRate * dt;
            if (pe.awarenessValue < 0.0f) pe.awarenessValue = 0.0f;
            pe.awareness = valueToLevel(pe.awarenessValue);
        }

        // Process new sensory input
        for (const auto& input : entities) {
            float dist = ownerPos.distanceTo(input.position);

            bool seen = canSee(ownerPos, ownerFacing, input.position, dist);
            bool heard = canHear(ownerPos, input.position, dist, input.noiseLevel);

            if (!seen && !heard) continue;

            auto& pe = perceived_[input.entityId];
            pe.entityId = input.entityId;
            pe.lastKnownPosition = input.position;
            pe.isHostile = input.isHostile;
            pe.noiseLevel = input.noiseLevel;

            if (seen) {
                float sightContribution = 1.0f - (dist / config.sightRange);
                pe.awarenessValue = std::min(1.0f, pe.awarenessValue + sightContribution * config.sightAwarenessWeight);
                pe.lastSeenTime = currentTime;
            }
            if (heard) {
                float effNoise = effectiveNoise(ownerPos, input.position, input.noiseLevel);
                float hearContribution = effNoise * (1.0f - dist / config.hearingRange);
                pe.awarenessValue = std::min(1.0f, pe.awarenessValue + hearContribution * config.hearingAwarenessWeight);
            }

            pe.awareness = valueToLevel(pe.awarenessValue);
        }

        // Remove completely forgotten entities
        for (auto it = perceived_.begin(); it != perceived_.end(); ) {
            if (it->second.awarenessValue <= 0.0f && currentTime - it->second.lastSeenTime > config.forgetTimeout) {
                it = perceived_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool canSee(Vec2 ownerPos, Vec2 ownerFacing, Vec2 targetPos, float dist) const {
        if (dist > config.sightRange) return false;

        Vec2 toTarget = (targetPos - ownerPos).normalized();
        Vec2 facing = ownerFacing.normalized();
        if (facing.lengthSquared() < 0.01f) facing = Vec2(1, 0);

        float dot = facing.dot(toTarget);
        float halfAngleRad = (config.sightAngle / 2.0f) * (3.14159f / 180.0f);
        if (dot < std::cos(halfAngleRad)) return false;

        // Line-of-sight occlusion check
        if (losChecker && !losChecker(ownerPos, targetPos)) return false;

        return true;
    }

    float effectiveNoise(Vec2 ownerPos, Vec2 targetPos, float noiseLevel) const {
        float noise = noiseLevel;
        if (wallCounter) {
            int walls = wallCounter(ownerPos, targetPos);
            for (int i = 0; i < walls; ++i)
                noise *= config.wallSoundDamping;
        }
        return noise;
    }

    bool canHear(Vec2 ownerPos, Vec2 targetPos, float dist, float noiseLevel) const {
        float effNoise = effectiveNoise(ownerPos, targetPos, noiseLevel);
        return dist <= config.hearingRange * effNoise;
    }

    std::vector<PerceivedEntity> getThreats() const {
        std::vector<PerceivedEntity> threats;
        for (const auto& [id, pe] : perceived_) {
            if (pe.isHostile && pe.awareness >= AwarenessLevel::Alert) {
                threats.push_back(pe);
            }
        }
        std::sort(threats.begin(), threats.end(),
            [](const PerceivedEntity& a, const PerceivedEntity& b) {
                return a.awarenessValue > b.awarenessValue;
            });
        return threats;
    }

    std::optional<PerceivedEntity> getMostDangerousThreat() const {
        auto threats = getThreats();
        if (threats.empty()) return std::nullopt;
        return threats.front();
    }

    const std::map<EntityId, PerceivedEntity>& perceived() const { return perceived_; }
    bool hasPerceivedEntity(EntityId id) const { return perceived_.count(id) > 0; }
    void forgetEntity(EntityId id) { perceived_.erase(id); }
    void clearAll() { perceived_.clear(); }

    void forceAwareness(EntityId id, Vec2 pos, AwarenessLevel level, bool hostile, float time) {
        auto& pe = perceived_[id];
        pe.entityId = id;
        pe.lastKnownPosition = pos;
        pe.awareness = level;
        pe.awarenessValue = levelToValue(level);
        pe.isHostile = hostile;
        pe.lastSeenTime = time;
    }

private:
    AwarenessLevel valueToLevel(float v) const {
        if (v >= config.combatAwarenessThreshold) return AwarenessLevel::Combat;
        if (v >= config.alertAwarenessThreshold) return AwarenessLevel::Alert;
        if (v >= config.suspiciousAwarenessThreshold) return AwarenessLevel::Suspicious;
        return AwarenessLevel::Unaware;
    }

    static float levelToValue(AwarenessLevel l) {
        switch (l) {
            case AwarenessLevel::Combat:     return 1.0f;
            case AwarenessLevel::Alert:      return 0.7f;
            case AwarenessLevel::Suspicious: return 0.35f;
            case AwarenessLevel::Unaware:    return 0.0f;
        }
        return 0.0f;
    }

    std::map<EntityId, PerceivedEntity> perceived_;
};

} // namespace npc
