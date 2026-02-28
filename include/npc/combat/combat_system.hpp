#pragma once

#include "../core/types.hpp"
#include "../core/vec2.hpp"
#include "../core/random.hpp"
#include "../perception/perception_system.hpp"
#include <vector>
#include <string>
#include <optional>
#include <algorithm>
#include <cmath>

namespace npc {

struct Ability {
    std::string name;
    AbilityType type = AbilityType::Melee;
    DamageType damageType = DamageType::Physical;
    float damage = 10.0f;
    float range = 1.5f;
    float cooldown = 2.0f;       // game hours
    float currentCooldown = 0.0f;
    float healAmount = 0.0f;     // for Heal type

    bool isReady() const { return currentCooldown <= 0.0f; }
};

struct CombatStats {
    float health = 100.0f;
    float maxHealth = 100.0f;
    float attack = 15.0f;
    float defense = 8.0f;
    float speed = 5.0f;
    float critChance = 0.1f;
    std::vector<Ability> abilities;

    float healthPercent() const {
        return maxHealth > 0.0f ? health / maxHealth : 0.0f;
    }

    bool isAlive() const { return health > 0.0f; }
};

struct ThreatEntry {
    EntityId entityId;
    float threatValue;
    float distance;
    Vec2 position;

    bool operator>(const ThreatEntry& o) const { return threatValue > o.threatValue; }
};

class CombatSystem {
public:
    CombatStats stats;
    bool inCombat = false;

    void update(float dt) {
        // Reduce cooldowns
        for (auto& ability : stats.abilities) {
            ability.currentCooldown = std::max(0.0f, ability.currentCooldown - dt);
        }

        // Passive regeneration out of combat (only if alive)
        if (!inCombat && stats.isAlive() && stats.health < stats.maxHealth) {
            stats.health = std::min(stats.maxHealth, stats.health + 1.0f * dt);
        }
    }

    void evaluateThreats(const std::vector<PerceivedEntity>& perceived, Vec2 myPos) {
        threatTable_.clear();
        for (const auto& pe : perceived) {
            if (!pe.isHostile || pe.awareness < AwarenessLevel::Alert) continue;

            float dist = myPos.distanceTo(pe.lastKnownPosition);
            float threat = pe.awarenessValue * 50.0f;
            threat += (1.0f / std::max(1.0f, dist)) * 30.0f; // closer = more threat
            threatTable_.push_back({pe.entityId, threat, dist, pe.lastKnownPosition});
        }
        std::sort(threatTable_.begin(), threatTable_.end(),
            [](const ThreatEntry& a, const ThreatEntry& b) {
                return a.threatValue > b.threatValue;
            });

        inCombat = !threatTable_.empty();
    }

    std::optional<ThreatEntry> selectTarget() const {
        if (threatTable_.empty()) return std::nullopt;
        return threatTable_.front();
    }

    const Ability* selectAbility(float distanceToTarget) const {
        const Ability* best = nullptr;
        float bestScore = -1.0f;

        for (const auto& ab : stats.abilities) {
            if (!ab.isReady()) continue;
            if (distanceToTarget > ab.range) continue;

            float score = ab.damage;
            if (ab.type == AbilityType::AoE) score *= 1.3f;
            if (score > bestScore) {
                bestScore = score;
                best = &ab;
            }
        }
        return best;
    }

    const Ability* selectHealAbility() const {
        for (const auto& ab : stats.abilities) {
            if (ab.type == AbilityType::Heal && ab.isReady()) return &ab;
        }
        return nullptr;
    }

    struct DamageResult {
        float damageDealt;
        bool isCrit;
        bool targetKilled;
    };

    DamageResult dealDamage(CombatSystem& target, const Ability& ability) {
        bool crit = Random::instance().chance(stats.critChance);
        float dmg = (stats.attack + ability.damage) * (crit ? 1.5f : 1.0f);
        float mitigated = std::max(1.0f, dmg - target.stats.defense * 0.5f);

        target.stats.health -= mitigated;
        bool killed = target.stats.health <= 0.0f;
        if (killed) target.stats.health = 0.0f;

        // Reset cooldown
        for (auto& ab : stats.abilities) {
            if (ab.name == ability.name) {
                ab.currentCooldown = ab.cooldown;
                break;
            }
        }

        return {mitigated, crit, killed};
    }

    float heal(const Ability& ability) {
        float amount = ability.healAmount;
        stats.health = std::min(stats.maxHealth, stats.health + amount);
        for (auto& ab : stats.abilities) {
            if (ab.name == ability.name) {
                ab.currentCooldown = ab.cooldown;
                break;
            }
        }
        return amount;
    }

    void takeDamage(float amount) {
        float mitigated = std::max(1.0f, amount - stats.defense * 0.5f);
        stats.health = std::max(0.0f, stats.health - mitigated);
    }

    bool shouldFlee() const {
        return stats.healthPercent() < 0.2f ||
               (threatTable_.size() >= 3 && stats.healthPercent() < 0.5f);
    }

    bool shouldHeal() const {
        return stats.healthPercent() < 0.5f && selectHealAbility() != nullptr;
    }

    Vec2 getFlankPosition(Vec2 myPos, Vec2 targetPos) const {
        Vec2 dir = (myPos - targetPos).normalized();
        Vec2 perp = {-dir.y, dir.x};
        float flankDist = 3.0f;
        return targetPos + perp * flankDist;
    }

    Vec2 getFleePosition(Vec2 myPos, Vec2 threatPos) const {
        Vec2 dir = (myPos - threatPos).normalized();
        return myPos + dir * 10.0f;
    }

    const std::vector<ThreatEntry>& threatTable() const { return threatTable_; }
    int threatCount() const { return static_cast<int>(threatTable_.size()); }

private:
    std::vector<ThreatEntry> threatTable_;
};

} // namespace npc
