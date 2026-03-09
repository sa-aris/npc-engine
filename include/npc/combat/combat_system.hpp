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

// ─── Resource Pool (Mana / Stamina) ─────────────────────────────────
struct ResourcePool {
    float current = 100.0f;
    float max = 100.0f;
    float regenRate = 5.0f;      // per game-hour (passive, always)
    float restRegenRate = 15.0f;  // per game-hour (while resting)

    float percent() const { return max > 0.0f ? current / max : 0.0f; }
    bool canSpend(float cost) const { return current >= cost; }
    void spend(float cost) { current = std::max(0.0f, current - cost); }
    void regen(float amount) { current = std::min(max, current + amount); }
};

// ─── Damage Resistances ─────────────────────────────────────────────
// Multiplier per damage type: <1.0 = resistant, 1.0 = normal, >1.0 = weak
struct DamageResistances {
    float physical = 1.0f;
    float magical = 1.0f;
    float fire = 1.0f;
    float ice = 1.0f;
    float poison = 1.0f;

    float get(DamageType type) const {
        switch (type) {
            case DamageType::Physical: return physical;
            case DamageType::Magical:  return magical;
            case DamageType::Fire:     return fire;
            case DamageType::Ice:      return ice;
            case DamageType::Poison:   return poison;
        }
        return 1.0f;
    }
};

struct CombatConfig {
    float passiveRegenRate = 1.0f;
    float awarenessThreatMul = 50.0f;
    float proximityThreatMul = 30.0f;
    float aoeDamageMul = 1.3f;
    float critMultiplier = 1.5f;
    float defenseMitigation = 0.5f;
    float flankDistance = 3.0f;
    float fleeDistance = 10.0f;
    float baseFleeLowHP = 0.2f;
    float baseFleeOutnumberedHP = 0.5f;
    int fleeOutnumberedCount = 3;
    float staminaSprintCost = 5.0f;  // per game-hour while sprinting
};

struct Ability {
    std::string name;
    AbilityType type = AbilityType::Melee;
    DamageType damageType = DamageType::Physical;
    float damage = 10.0f;
    float range = 1.5f;
    float cooldown = 2.0f;
    float currentCooldown = 0.0f;
    float healAmount = 0.0f;
    float manaCost = 0.0f;
    float staminaCost = 0.0f;

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
    ResourcePool stamina{100.0f, 100.0f, 5.0f, 15.0f};
    ResourcePool mana{0.0f, 0.0f, 3.0f, 10.0f};
    DamageResistances resistances;

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
    CombatConfig combatConfig;
    bool inCombat = false;

    // Personality-derived modifiers
    float fleeThresholdMod_ = 1.0f;
    float healThreshold_ = 0.5f;
    float threatAwarenessMod_ = 1.0f;

    void applyPersonality(float fleeThresholdMul, float healThresh, float threatAwarenessMul) {
        fleeThresholdMod_ = fleeThresholdMul;
        healThreshold_ = healThresh;
        threatAwarenessMod_ = threatAwarenessMul;
    }

    void update(float dt) {
        for (auto& ability : stats.abilities) {
            ability.currentCooldown = std::max(0.0f, ability.currentCooldown - dt);
        }

        if (!inCombat && stats.isAlive() && stats.health < stats.maxHealth) {
            stats.health = std::min(stats.maxHealth, stats.health + combatConfig.passiveRegenRate * dt);
        }

        // Passive resource regen (always, when alive)
        if (stats.isAlive()) {
            stats.stamina.regen(stats.stamina.regenRate * dt);
            stats.mana.regen(stats.mana.regenRate * dt);
        }
    }

    void evaluateThreats(const std::vector<PerceivedEntity>& perceived, Vec2 myPos) {
        threatTable_.clear();
        for (const auto& pe : perceived) {
            if (!pe.isHostile || pe.awareness < AwarenessLevel::Alert) continue;

            float dist = myPos.distanceTo(pe.lastKnownPosition);
            float threat = pe.awarenessValue * combatConfig.awarenessThreatMul * threatAwarenessMod_;
            threat += (1.0f / std::max(1.0f, dist)) * combatConfig.proximityThreatMul;
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
            if (!stats.stamina.canSpend(ab.staminaCost)) continue;
            if (!stats.mana.canSpend(ab.manaCost)) continue;

            float score = ab.damage;
            if (ab.type == AbilityType::AoE) score *= combatConfig.aoeDamageMul;
            if (score > bestScore) {
                bestScore = score;
                best = &ab;
            }
        }
        return best;
    }

    const Ability* selectHealAbility() const {
        for (const auto& ab : stats.abilities) {
            if (ab.type == AbilityType::Heal && ab.isReady()
                && stats.stamina.canSpend(ab.staminaCost)
                && stats.mana.canSpend(ab.manaCost)) return &ab;
        }
        return nullptr;
    }

    struct DamageResult {
        float damageDealt;
        bool isCrit;
        bool targetKilled;
        float resistanceMultiplier;
    };

    DamageResult dealDamage(CombatSystem& target, const Ability& ability) {
        bool crit = Random::instance().chance(stats.critChance);
        float dmg = (stats.attack + ability.damage) * (crit ? combatConfig.critMultiplier : 1.0f);
        float resMul = target.stats.resistances.get(ability.damageType);
        float mitigated = std::max(1.0f, dmg * resMul - target.stats.defense * combatConfig.defenseMitigation);

        target.stats.health -= mitigated;
        bool killed = target.stats.health <= 0.0f;
        if (killed) target.stats.health = 0.0f;

        // Spend resources
        stats.stamina.spend(ability.staminaCost);
        stats.mana.spend(ability.manaCost);

        for (auto& ab : stats.abilities) {
            if (ab.name == ability.name) {
                ab.currentCooldown = ab.cooldown;
                break;
            }
        }

        return {mitigated, crit, killed, resMul};
    }

    float heal(const Ability& ability) {
        float amount = ability.healAmount;
        stats.health = std::min(stats.maxHealth, stats.health + amount);

        // Spend resources
        stats.stamina.spend(ability.staminaCost);
        stats.mana.spend(ability.manaCost);

        for (auto& ab : stats.abilities) {
            if (ab.name == ability.name) {
                ab.currentCooldown = ab.cooldown;
                break;
            }
        }
        return amount;
    }

    void takeDamage(float amount) {
        float mitigated = std::max(1.0f, amount - stats.defense * combatConfig.defenseMitigation);
        stats.health = std::max(0.0f, stats.health - mitigated);
    }

    bool shouldFlee() const {
        float lowThreshold = combatConfig.baseFleeLowHP * fleeThresholdMod_;
        float outnumberedThreshold = combatConfig.baseFleeOutnumberedHP * fleeThresholdMod_;
        return stats.healthPercent() < lowThreshold ||
               (static_cast<int>(threatTable_.size()) >= combatConfig.fleeOutnumberedCount &&
                stats.healthPercent() < outnumberedThreshold);
    }

    bool shouldHeal() const {
        return stats.healthPercent() < healThreshold_ && selectHealAbility() != nullptr;
    }

    Vec2 getFlankPosition(Vec2 myPos, Vec2 targetPos) const {
        Vec2 dir = (myPos - targetPos).normalized();
        Vec2 perp = {-dir.y, dir.x};
        return targetPos + perp * combatConfig.flankDistance;
    }

    Vec2 getFleePosition(Vec2 myPos, Vec2 threatPos) const {
        Vec2 dir = (myPos - threatPos).normalized();
        return myPos + dir * combatConfig.fleeDistance;
    }

    const std::vector<ThreatEntry>& threatTable() const { return threatTable_; }
    int threatCount() const { return static_cast<int>(threatTable_.size()); }

private:
    std::vector<ThreatEntry> threatTable_;
};

} // namespace npc
