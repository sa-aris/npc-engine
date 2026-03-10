#pragma once

#include "../core/types.hpp"
#include "../event/event_system.hpp"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <algorithm>
#include <optional>
#include <cmath>
#include <sstream>

namespace npc {

// ─── Skill Domain ─────────────────────────────────────────────────────────────
enum class SkillDomain {
    Combat,     // fighting, kills
    Trade,      // buying, selling, bargaining
    Farming,    // field work, harvesting
    Crafting,   // forge, workshop
    Social,     // persuasion, dialog, reputation
    Stealth,    // deception, sneaking
    Leadership  // group command, morale management
};

inline std::string skillDomainToString(SkillDomain d) {
    switch (d) {
        case SkillDomain::Combat:    return "Combat";
        case SkillDomain::Trade:     return "Trade";
        case SkillDomain::Farming:   return "Farming";
        case SkillDomain::Crafting:  return "Crafting";
        case SkillDomain::Social:    return "Social";
        case SkillDomain::Stealth:   return "Stealth";
        case SkillDomain::Leadership:return "Leadership";
    }
    return "Unknown";
}

// ─── Level-up Event ───────────────────────────────────────────────────────────
struct SkillLevelUpEvent {
    EntityId   entityId;
    SkillDomain domain;
    int        newLevel;
    std::string perkUnlocked;  // empty if none auto-unlocked
};

// ─── Compiled Bonuses ─────────────────────────────────────────────────────────
// All stat modifiers derived from unlocked perks; rebuilt after each level-up.
struct SkillBonuses {
    // Combat
    float damageMul        = 1.0f;
    float armorMul         = 1.0f;
    float critChanceMul    = 1.0f;
    float maxHealthBonus   = 0.0f;   // flat HP addition
    float maxStaminaBonus  = 0.0f;

    // Trade
    float buyMarkupMul     = 1.0f;   // < 1 = NPC charges less markup when selling
    float sellMarkdownMul  = 1.0f;   // > 1 = NPC pays more when buying
    float bargainFloorMul  = 1.0f;   // < 1 = NPC accepts lower offers
    float extraBargainRounds = 0.0f; // added to maxRounds

    // Work
    float workSpeedMul     = 1.0f;
    float workYieldMul     = 1.0f;
    float fatigueMul       = 1.0f;   // < 1 = less fatigue per hour

    // Social
    float persuasionMul    = 1.0f;   // added to skill check actor-skill
    float relGainMul       = 1.0f;

    // Leadership
    float groupMoraleBonus = 0.0f;   // flat morale added at group formation
    float commandRangeMul  = 1.0f;

    // Stealth / Deception
    float deceptionMul     = 1.0f;
    float detectionMul     = 1.0f;   // < 1 = harder to detect
};

// ─── Perk ─────────────────────────────────────────────────────────────────────
// A node in the skill tree, unlocked at a specific level threshold.
struct Perk {
    std::string              id;
    std::string              name;
    std::string              description;
    SkillDomain              domain;
    int                      requiredLevel = 1;
    std::vector<std::string> prerequisiteIds;

    // Applies modifiers to a SkillBonuses struct when this perk is unlocked
    std::function<void(SkillBonuses&)> apply;

    // Auto-unlock at this level (true = granted without player choice)
    bool autoUnlock = true;
};

// ─── Skill (per-domain progress) ─────────────────────────────────────────────
struct Skill {
    SkillDomain domain;
    int         level    = 0;    // 0-10
    float       xp       = 0.0f;

    static constexpr int MAX_LEVEL = 10;

    // XP needed to reach next level: 100 * 1.5^level (100, 150, 225, ...)
    float xpToNextLevel() const {
        if (level >= MAX_LEVEL) return 0.0f;
        return 100.0f * std::pow(1.5f, static_cast<float>(level));
    }

    float progress() const {
        float needed = xpToNextLevel();
        return needed > 0.0f ? std::min(xp / needed, 1.0f) : 1.0f;
    }

    bool canLevelUp() const { return level < MAX_LEVEL && xp >= xpToNextLevel(); }

    std::string toString() const {
        std::ostringstream ss;
        ss << skillDomainToString(domain) << " Lv" << level
           << " (" << static_cast<int>(xp) << "/"
           << static_cast<int>(xpToNextLevel()) << " XP)";
        return ss.str();
    }
};

// ─── SkillSystem ──────────────────────────────────────────────────────────────
class SkillSystem {
public:
    explicit SkillSystem(EntityId owner = INVALID_ENTITY)
        : owner_(owner) {
        // Initialise all domains at level 0
        for (auto d : allDomains())
            skills_[d] = Skill{d, 0, 0.0f};
        buildDefaultPerks();
    }

    // ── XP Award ──────────────────────────────────────────────────────────────
    // Returns true if a level-up occurred.
    bool awardXP(SkillDomain domain, float amount,
                 EventBus* bus = nullptr) {
        auto& sk = skills_[domain];
        if (sk.level >= Skill::MAX_LEVEL) return false;

        sk.xp += amount;
        bool levelled = false;

        while (sk.canLevelUp()) {
            sk.xp -= sk.xpToNextLevel();
            ++sk.level;
            levelled = true;

            // Auto-unlock perks for this level
            std::string autoUnlocked;
            for (auto& [id, perk] : perks_) {
                if (perk.domain      != domain)     continue;
                if (perk.requiredLevel != sk.level) continue;
                if (!perk.autoUnlock)               continue;
                if (unlockedPerks_.count(id))       continue;
                if (!prerequisitesMet(perk))        continue;

                unlockedPerks_.insert(id);
                autoUnlocked = id;
            }

            rebuildBonuses();

            if (bus) bus->publish(SkillLevelUpEvent{
                owner_, domain, sk.level, autoUnlocked});
        }
        return levelled;
    }

    // ── Manual perk unlock (player-choice trees) ──────────────────────────────
    bool unlockPerk(const std::string& perkId) {
        auto it = perks_.find(perkId);
        if (it == perks_.end())           return false;
        if (unlockedPerks_.count(perkId)) return false;
        const auto& perk = it->second;
        if (skills_[perk.domain].level < perk.requiredLevel) return false;
        if (!prerequisitesMet(perk))      return false;

        unlockedPerks_.insert(perkId);
        rebuildBonuses();
        return true;
    }

    // ── Queries ───────────────────────────────────────────────────────────────
    int   level(SkillDomain d) const {
        auto it = skills_.find(d);
        return it != skills_.end() ? it->second.level : 0;
    }
    float xp(SkillDomain d) const {
        auto it = skills_.find(d);
        return it != skills_.end() ? it->second.xp : 0.0f;
    }
    const Skill& skill(SkillDomain d) const { return skills_.at(d); }

    const SkillBonuses& bonuses() const { return bonuses_; }

    bool isPerkUnlocked(const std::string& id) const {
        return unlockedPerks_.count(id) > 0;
    }

    // All perks available to unlock at current levels (prereqs met, not yet unlocked)
    std::vector<const Perk*> availablePerks() const {
        std::vector<const Perk*> out;
        for (const auto& [id, perk] : perks_) {
            if (unlockedPerks_.count(id)) continue;
            if (skills_.at(perk.domain).level < perk.requiredLevel) continue;
            if (!prerequisitesMet(perk)) continue;
            out.push_back(&perk);
        }
        return out;
    }

    std::vector<const Perk*> unlockedPerkList() const {
        std::vector<const Perk*> out;
        for (const auto& id : unlockedPerks_) {
            auto it = perks_.find(id);
            if (it != perks_.end()) out.push_back(&it->second);
        }
        return out;
    }

    // ── Domain-specific bonus helpers (for other systems to call) ─────────────
    float combatDamageMul()     const { return bonuses_.damageMul; }
    float combatArmorMul()      const { return bonuses_.armorMul; }
    float combatCritMul()       const { return bonuses_.critChanceMul; }
    float combatHealthBonus()   const { return bonuses_.maxHealthBonus; }
    float tradeMarkupMul()      const { return bonuses_.buyMarkupMul; }
    float tradeMarkdownMul()    const { return bonuses_.sellMarkdownMul; }
    float tradeBargainFloor()   const { return bonuses_.bargainFloorMul; }
    float workSpeedMul()        const { return bonuses_.workSpeedMul; }
    float workYieldMul()        const { return bonuses_.workYieldMul; }
    float fatigueMul()          const { return bonuses_.fatigueMul; }
    float persuasionMul()       const { return bonuses_.persuasionMul; }
    float deceptionMul()        const { return bonuses_.deceptionMul; }
    float groupMoraleBonus()    const { return bonuses_.groupMoraleBonus; }

    // ── EventBus integration ──────────────────────────────────────────────────
    // Subscribe to game events and auto-award XP.
    void subscribeToEvents(EventBus& bus) {
        // Combat XP: 20 for hit, 80 for kill
        bus.subscribe<CombatEvent>([this](const CombatEvent& ev) {
            if (ev.attacker == owner_) {
                float xp = ev.killed ? 80.0f : 20.0f;
                awardXP(SkillDomain::Combat, xp);
            }
        });
        // Trade XP: 10 per transaction
        bus.subscribe<TradeEvent>([this](const TradeEvent& ev) {
            if (ev.buyer == owner_ || ev.seller == owner_)
                awardXP(SkillDomain::Trade, 10.0f * ev.quantity);
        });
        // Quest XP: 150 per completion
        bus.subscribe<QuestCompletedEvent>([this](const QuestCompletedEvent& ev) {
            if (ev.takerId == owner_)
                awardXP(SkillDomain::Combat, 50.0f);  // generic XP spread
        });
    }

    // Manual XP for work shift end
    void onWorkShiftCompleted(SkillDomain domain, float hoursWorked) {
        awardXP(domain, hoursWorked * 15.0f);
    }

    // ── Summary ───────────────────────────────────────────────────────────────
    std::string summary() const {
        std::ostringstream ss;
        for (const auto& [d, sk] : skills_)
            if (sk.level > 0 || sk.xp > 0.0f)
                ss << "  " << sk.toString() << "\n";
        return ss.str();
    }

    // ── Custom perk registration ──────────────────────────────────────────────
    void registerPerk(Perk perk) {
        perks_[perk.id] = std::move(perk);
    }

private:
    static std::vector<SkillDomain> allDomains() {
        return {SkillDomain::Combat,   SkillDomain::Trade,
                SkillDomain::Farming,  SkillDomain::Crafting,
                SkillDomain::Social,   SkillDomain::Stealth,
                SkillDomain::Leadership};
    }

    bool prerequisitesMet(const Perk& perk) const {
        for (const auto& preId : perk.prerequisiteIds)
            if (!unlockedPerks_.count(preId)) return false;
        return true;
    }

    void rebuildBonuses() {
        bonuses_ = SkillBonuses{};
        for (const auto& id : unlockedPerks_) {
            auto it = perks_.find(id);
            if (it != perks_.end() && it->second.apply)
                it->second.apply(bonuses_);
        }
    }

    // ── Default perk trees ────────────────────────────────────────────────────
    void buildDefaultPerks() {
        // ── Combat tree ───────────────────────────────────────────────────────
        registerPerk({"basic_training",    "Basic Training",
            "+8% damage",                  SkillDomain::Combat, 1, {},
            [](SkillBonuses& b){ b.damageMul *= 1.08f; }});

        registerPerk({"combat_endurance",  "Combat Endurance",
            "+15 max stamina",             SkillDomain::Combat, 2,
            {"basic_training"},
            [](SkillBonuses& b){ b.maxStaminaBonus += 15.0f; }});

        registerPerk({"power_strike",      "Power Strike",
            "+20% damage",                 SkillDomain::Combat, 3,
            {"basic_training"},
            [](SkillBonuses& b){ b.damageMul *= 1.20f; }});

        registerPerk({"battle_hardened",   "Battle Hardened",
            "+25 max HP, +10% armor",      SkillDomain::Combat, 4,
            {"combat_endurance"},
            [](SkillBonuses& b){
                b.maxHealthBonus += 25.0f;
                b.armorMul       *= 1.10f; }});

        registerPerk({"veteran_fighter",   "Veteran Fighter",
            "+30% damage, +15% armor",     SkillDomain::Combat, 5,
            {"power_strike", "battle_hardened"},
            [](SkillBonuses& b){
                b.damageMul *= 1.30f;
                b.armorMul  *= 1.15f; }});

        registerPerk({"precise_strikes",   "Precise Strikes",
            "+40% crit chance",            SkillDomain::Combat, 7,
            {"veteran_fighter"},
            [](SkillBonuses& b){ b.critChanceMul *= 1.40f; }});

        registerPerk({"elite_warrior",     "Elite Warrior",
            "+50% damage, crit deals 2×",  SkillDomain::Combat, 10,
            {"precise_strikes"},
            [](SkillBonuses& b){
                b.damageMul    *= 1.50f;
                b.critChanceMul*= 2.00f; }});

        // ── Trade tree ────────────────────────────────────────────────────────
        registerPerk({"haggler",           "Haggler",
            "Buy prices 5% cheaper",       SkillDomain::Trade, 1, {},
            [](SkillBonuses& b){ b.buyMarkupMul *= 0.95f; }});

        registerPerk({"silver_tongue",     "Silver Tongue",
            "+10% sell price",             SkillDomain::Trade, 2,
            {"haggler"},
            [](SkillBonuses& b){ b.sellMarkdownMul *= 1.10f; }});

        registerPerk({"market_instinct",   "Market Instinct",
            "+1 extra bargain round",      SkillDomain::Trade, 3,
            {"haggler"},
            [](SkillBonuses& b){ b.extraBargainRounds += 1.0f; }});

        registerPerk({"trade_network",     "Trade Network",
            "10% better buy+sell",         SkillDomain::Trade, 5,
            {"silver_tongue", "market_instinct"},
            [](SkillBonuses& b){
                b.buyMarkupMul   *= 0.90f;
                b.sellMarkdownMul*= 1.10f; }});

        registerPerk({"tough_bargainer",   "Tough Bargainer",
            "NPC accepts 15% lower floor", SkillDomain::Trade, 7,
            {"trade_network"},
            [](SkillBonuses& b){ b.bargainFloorMul *= 0.85f; }});

        registerPerk({"master_trader",     "Master Trader",
            "+25% all prices, +2 rounds",  SkillDomain::Trade, 10,
            {"tough_bargainer"},
            [](SkillBonuses& b){
                b.buyMarkupMul      *= 0.75f;
                b.sellMarkdownMul   *= 1.25f;
                b.extraBargainRounds += 2.0f; }});

        // ── Farming tree ──────────────────────────────────────────────────────
        registerPerk({"green_thumb",       "Green Thumb",
            "+10% crop yield",             SkillDomain::Farming, 1, {},
            [](SkillBonuses& b){ b.workYieldMul *= 1.10f; }});

        registerPerk({"hardy_worker",      "Hardy Worker",
            "-15% fatigue from farm work", SkillDomain::Farming, 2,
            {"green_thumb"},
            [](SkillBonuses& b){ b.fatigueMul *= 0.85f; }});

        registerPerk({"efficient_planting","Efficient Planting",
            "+15% work speed",             SkillDomain::Farming, 3,
            {"green_thumb"},
            [](SkillBonuses& b){ b.workSpeedMul *= 1.15f; }});

        registerPerk({"master_harvester",  "Master Harvester",
            "+35% yield, +20% speed",      SkillDomain::Farming, 6,
            {"hardy_worker", "efficient_planting"},
            [](SkillBonuses& b){
                b.workYieldMul *= 1.35f;
                b.workSpeedMul *= 1.20f; }});

        // ── Crafting tree ─────────────────────────────────────────────────────
        registerPerk({"apprentice_craft",  "Apprentice Craft",
            "+10% crafting speed",         SkillDomain::Crafting, 1, {},
            [](SkillBonuses& b){ b.workSpeedMul *= 1.10f; }});

        registerPerk({"quality_materials", "Quality Materials",
            "+15% item yield/quality",     SkillDomain::Crafting, 3,
            {"apprentice_craft"},
            [](SkillBonuses& b){ b.workYieldMul *= 1.15f; }});

        registerPerk({"master_craftsman",  "Master Craftsman",
            "+30% speed, +30% quality",    SkillDomain::Crafting, 7,
            {"quality_materials"},
            [](SkillBonuses& b){
                b.workSpeedMul *= 1.30f;
                b.workYieldMul *= 1.30f; }});

        // ── Social tree ───────────────────────────────────────────────────────
        registerPerk({"smooth_talker",     "Smooth Talker",
            "+10% persuasion skill",       SkillDomain::Social, 1, {},
            [](SkillBonuses& b){ b.persuasionMul *= 1.10f; }});

        registerPerk({"well_liked",        "Well Liked",
            "+20% relationship gains",     SkillDomain::Social, 3,
            {"smooth_talker"},
            [](SkillBonuses& b){ b.relGainMul *= 1.20f; }});

        registerPerk({"silver_words",      "Silver Words",
            "+25% persuasion+deception",   SkillDomain::Social, 6,
            {"well_liked"},
            [](SkillBonuses& b){
                b.persuasionMul *= 1.25f;
                b.deceptionMul  *= 1.25f; }});

        // ── Leadership tree ───────────────────────────────────────────────────
        registerPerk({"inspiring_presence","Inspiring Presence",
            "+10 group morale on form",    SkillDomain::Leadership, 2, {},
            [](SkillBonuses& b){ b.groupMoraleBonus += 10.0f; }});

        registerPerk({"tactical_command", "Tactical Command",
            "+25% command range",          SkillDomain::Leadership, 4,
            {"inspiring_presence"},
            [](SkillBonuses& b){ b.commandRangeMul *= 1.25f; }});

        registerPerk({"warlord",           "Warlord",
            "+25 morale, +50% range",      SkillDomain::Leadership, 8,
            {"tactical_command"},
            [](SkillBonuses& b){
                b.groupMoraleBonus += 25.0f;
                b.commandRangeMul  *= 1.50f; }});

        // ── Stealth tree ──────────────────────────────────────────────────────
        registerPerk({"light_step",        "Light Step",
            "-15% detection chance",       SkillDomain::Stealth, 1, {},
            [](SkillBonuses& b){ b.detectionMul *= 0.85f; }});

        registerPerk({"forked_tongue",     "Forked Tongue",
            "+15% deception skill",        SkillDomain::Stealth, 3,
            {"light_step"},
            [](SkillBonuses& b){ b.deceptionMul *= 1.15f; }});
    }

    EntityId                        owner_;
    std::map<SkillDomain, Skill>    skills_;
    std::map<std::string, Perk>     perks_;
    std::set<std::string>           unlockedPerks_;
    SkillBonuses                    bonuses_;
};

} // namespace npc
