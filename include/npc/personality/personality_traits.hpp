#pragma once

#include <string>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace npc {

// Five core personality axes, each 0.0 to 1.0 with 0.5 as neutral.
// At neutral (0.5), all modifier functions return values that produce
// NO change from the existing baseline behavior.
//
// Courage      (0=cowardly, 1=fearless)
// Sociability  (0=introvert, 1=extrovert)
// Greed        (0=generous, 1=greedy)
// Patience     (0=impulsive, 1=calm)
// Intelligence (0=simple, 1=clever)

struct PersonalityTraits {
    float courage      = 0.5f;
    float sociability  = 0.5f;
    float greed        = 0.5f;
    float patience     = 0.5f;
    float intelligence = 0.5f;

    // --- Courage Modifiers ---

    // Multiplier for flee HP thresholds. Range: [0.5, 1.5].
    // High courage -> lower threshold (braver). Low courage -> higher threshold (cowardly).
    float fleeThresholdMultiplier() const {
        return 1.0f + (0.5f - courage);
    }

    // Multiplier on fear emotion intensity. Range: [0.5, 1.5].
    float fearIntensityMultiplier() const {
        return 1.0f + (0.5f - courage);
    }

    // Multiplier on anger combat boost. Range: [0.5, 1.5].
    float angerCombatMultiplier() const {
        return 0.5f + courage;
    }

    // --- Sociability Modifiers ---

    // Multiplier on Social need decay rate. Range: [0.5, 1.5].
    float socialDecayMultiplier() const {
        return 0.5f + sociability;
    }

    // Multiplier on positive relationship deltas. Range: [0.5, 1.5].
    float relationshipGainMultiplier() const {
        return 0.5f + sociability;
    }

    // Multiplier for friend threshold. Range: [0.7, 1.3].
    float friendThresholdMultiplier() const {
        return 1.3f - sociability * 0.6f;
    }

    // --- Greed Modifiers ---

    // Buy markup multiplier. Range: [0.7, 1.3].
    float buyMarkupMultiplier() const {
        return 0.7f + greed * 0.6f;
    }

    // Sell markdown multiplier. Range: [0.7, 1.3].
    float sellMarkdownMultiplier() const {
        return 1.3f - greed * 0.6f;
    }

    // Scarcity price multiplier. Range: [0.7, 1.3].
    float scarcityMultiplier() const {
        return 0.7f + greed * 0.6f;
    }

    // Relationship discount multiplier. Range: [0.5, 1.5].
    float relationshipDiscountMultiplier() const {
        return 1.5f - greed;
    }

    // --- Patience Modifiers ---

    // Multiplier on anger emotion intensity. Range: [0.5, 1.5].
    float angerIntensityMultiplier() const {
        return 1.5f - patience;
    }

    // Mood stability multiplier (combines patience + intelligence). Range: [0.5, 1.5].
    float moodStabilityMultiplier() const {
        float avg = (patience + intelligence) * 0.5f;
        return 1.5f - avg;
    }

    // Multiplier on negative relationship deltas. Range: [0.5, 1.5].
    float negativeRelationshipMultiplier() const {
        return 1.5f - patience;
    }

    // --- Intelligence Modifiers ---

    // Multiplier on sight range. Range: [0.7, 1.3].
    float sightRangeMultiplier() const {
        return 0.7f + intelligence * 0.6f;
    }

    // Multiplier on awareness decay (lower is better). Range: [0.7, 1.3].
    float awarenessDecayMultiplier() const {
        return 1.3f - intelligence * 0.6f;
    }

    // Multiplier on memory capacity. Range: [0.5, 1.5].
    float memoryCapacityMultiplier() const {
        return 0.5f + intelligence;
    }

    // Multiplier on memory importance decay. Range: [0.7, 1.3].
    float memoryDecayMultiplier() const {
        return 1.3f - intelligence * 0.6f;
    }

    // Heal threshold: HP% at which NPC should heal. Range: [0.3, 0.7].
    float healThreshold() const {
        return 0.3f + intelligence * 0.4f;
    }

    // Multiplier on threat awareness. Range: [0.7, 1.3].
    float threatAwarenessMultiplier() const {
        return 0.7f + intelligence * 0.6f;
    }

    // Emotional contagion sensitivity. Range: [0.2, 1.0].
    // High sociability = more susceptible to others' emotions.
    float empathyMultiplier() const {
        return 0.2f + sociability * 0.8f;
    }

    // --- Display ---

    std::string toString() const {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1)
           << "COU=" << courage
           << " SOC=" << sociability
           << " GRE=" << greed
           << " PAT=" << patience
           << " INT=" << intelligence;
        return ss.str();
    }

    std::string traitSummary() const {
        auto describe = [](float v, const char* low, const char* mid, const char* high) {
            if (v < 0.3f) return low;
            if (v > 0.7f) return high;
            return mid;
        };
        std::ostringstream ss;
        ss << describe(courage,      "Cowardly",  "Steady",   "Fearless")  << ", "
           << describe(sociability,  "Introvert", "Balanced", "Extrovert") << ", "
           << describe(greed,        "Generous",  "Fair",     "Greedy")    << ", "
           << describe(patience,     "Impulsive", "Measured", "Calm")      << ", "
           << describe(intelligence, "Simple",    "Average",  "Clever");
        return ss.str();
    }

    // --- Preset Factory Functions ---

    static PersonalityTraits neutral() {
        return {};
    }

    static PersonalityTraits guard() {
        return {0.9f, 0.5f, 0.2f, 0.6f, 0.7f};
    }

    static PersonalityTraits blacksmith() {
        return {0.7f, 0.6f, 0.3f, 0.8f, 0.6f};
    }

    static PersonalityTraits merchant() {
        return {0.2f, 0.7f, 0.8f, 0.5f, 0.8f};
    }

    static PersonalityTraits innkeeper() {
        return {0.4f, 0.9f, 0.3f, 0.7f, 0.6f};
    }

    static PersonalityTraits farmer() {
        return {0.3f, 0.5f, 0.4f, 0.9f, 0.4f};
    }
};

} // namespace npc
