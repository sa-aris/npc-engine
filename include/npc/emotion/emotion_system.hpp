#pragma once

#include "../core/types.hpp"
#include "../personality/personality_traits.hpp"
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

namespace npc {

struct Need {
    NeedType type;
    float value = 80.0f;
    float decayRate = 2.0f;
    float urgencyThreshold = 30.0f;
    float criticalThreshold = 10.0f;

    bool isUrgent() const { return value <= urgencyThreshold; }
    bool isCritical() const { return value <= criticalThreshold; }

    float urgency() const {
        return 1.0f - std::clamp(value / 100.0f, 0.0f, 1.0f);
    }
};

struct EmotionState {
    EmotionType type;
    float intensity = 0.5f;
    float duration = 1.0f;
    float elapsed = 0.0f;
};

struct EmotionConfig {
    struct NeedDefaults {
        float hunger  = 80.0f, hungerDecay  = 4.0f, hungerUrgency  = 30.0f, hungerCritical  = 10.0f;
        float thirst  = 85.0f, thirstDecay  = 5.0f, thirstUrgency  = 30.0f, thirstCritical  = 10.0f;
        float sleep   = 90.0f, sleepDecay   = 3.0f, sleepUrgency   = 25.0f, sleepCritical   =  8.0f;
        float social  = 70.0f, socialDecay  = 1.5f, socialUrgency  = 25.0f, socialCritical  = 10.0f;
        float fun     = 60.0f, funDecay     = 1.0f, funUrgency     = 20.0f, funCritical     =  5.0f;
        float safety  =100.0f, safetyDecay  = 0.5f, safetyUrgency  = 40.0f, safetyCritical  = 20.0f;
        float comfort = 75.0f, comfortDecay = 0.8f, comfortUrgency = 20.0f, comfortCritical =  5.0f;
    } needs;

    // Combat modifier
    float combatSafetyBase = 0.5f;
    float combatSafetyScale = 0.5f;
    float angerCombatBoost = 0.3f;
    float fearCombatPenalty = 0.4f;
    float combatModMin = 0.1f;
    float combatModMax = 2.0f;

    // Social modifier
    float socialBase = 0.5f;
    float socialScale = 0.5f;
    float positiveMoodThreshold = 0.2f;
    float positiveMoodBoost = 1.2f;
    float negativeMoodThreshold = -0.3f;
    float negativeMoodPenalty = 0.7f;
    float socialModMin = 0.1f;
    float socialModMax = 2.0f;

    // Flee modifier
    float fleeSafetyWeight = 0.5f;

    // Mood calculation
    float moodSafetyWeight = 2.0f;
    float moodHungerSleepWeight = 1.5f;
    float baseEmotionWeight = 0.4f;

    // Mood thresholds
    float joyfulThreshold = 0.5f;
    float contentThreshold = 0.2f;
    float neutralThreshold = -0.2f;
    float uneasyThreshold = -0.5f;

    // Emotion intensification
    float emotionIntensifyFactor = 0.5f;
};

class EmotionSystem {
public:
    EmotionConfig config_;

    EmotionSystem() : EmotionSystem(EmotionConfig{}) {}

    explicit EmotionSystem(const EmotionConfig& cfg) : config_(cfg) {
        auto& n = config_.needs;
        needs_[NeedType::Hunger]  = {NeedType::Hunger,  n.hunger,  n.hungerDecay,  n.hungerUrgency,  n.hungerCritical};
        needs_[NeedType::Thirst]  = {NeedType::Thirst,  n.thirst,  n.thirstDecay,  n.thirstUrgency,  n.thirstCritical};
        needs_[NeedType::Sleep]   = {NeedType::Sleep,   n.sleep,   n.sleepDecay,   n.sleepUrgency,   n.sleepCritical};
        needs_[NeedType::Social]  = {NeedType::Social,  n.social,  n.socialDecay,  n.socialUrgency,  n.socialCritical};
        needs_[NeedType::Fun]     = {NeedType::Fun,     n.fun,     n.funDecay,     n.funUrgency,     n.funCritical};
        needs_[NeedType::Safety]  = {NeedType::Safety,  n.safety,  n.safetyDecay,  n.safetyUrgency,  n.safetyCritical};
        needs_[NeedType::Comfort] = {NeedType::Comfort, n.comfort, n.comfortDecay, n.comfortUrgency, n.comfortCritical};
    }

    void update(float dt) {
        for (auto& [type, need] : needs_) {
            need.value -= need.decayRate * dt;
            need.value = std::max(0.0f, need.value);
        }

        for (auto& e : emotions_) {
            e.elapsed += dt;
        }
        emotions_.erase(
            std::remove_if(emotions_.begin(), emotions_.end(),
                [](const EmotionState& e) { return e.elapsed >= e.duration; }),
            emotions_.end()
        );

        updateMood();
    }

    void addEmotion(EmotionType type, float intensity = 0.5f, float duration = 1.0f) {
        float adjustedIntensity = intensity;
        if (type == EmotionType::Fearful) {
            adjustedIntensity *= fearIntensityMod_;
        } else if (type == EmotionType::Angry) {
            adjustedIntensity *= angerIntensityMod_;
        }
        adjustedIntensity = std::clamp(adjustedIntensity, 0.0f, 1.0f);

        for (auto& e : emotions_) {
            if (e.type == type) {
                e.intensity = std::min(1.0f, e.intensity + adjustedIntensity * config_.emotionIntensifyFactor);
                e.duration = std::max(e.duration - e.elapsed, duration);
                e.elapsed = 0.0f;
                return;
            }
        }
        emotions_.push_back({type, adjustedIntensity, duration, 0.0f});
    }

    void satisfyNeed(NeedType type, float amount) {
        auto it = needs_.find(type);
        if (it != needs_.end()) {
            it->second.value = std::min(100.0f, it->second.value + amount);
        }
    }

    void depletNeed(NeedType type, float amount) {
        auto it = needs_.find(type);
        if (it != needs_.end()) {
            it->second.value = std::max(0.0f, it->second.value - amount);
        }
    }

    NeedType getMostUrgentNeed() const {
        NeedType most = NeedType::Hunger;
        float highestUrgency = -1.0f;
        for (const auto& [type, need] : needs_) {
            float u = need.urgency();
            if (u > highestUrgency) {
                highestUrgency = u;
                most = type;
            }
        }
        return most;
    }

    bool hasUrgentNeed() const {
        for (const auto& [type, need] : needs_) {
            if (need.isUrgent()) return true;
        }
        return false;
    }

    bool hasCriticalNeed() const {
        for (const auto& [type, need] : needs_) {
            if (need.isCritical()) return true;
        }
        return false;
    }

    float getMood() const { return mood_; }

    EmotionType getDominantEmotion() const {
        if (emotions_.empty()) return EmotionType::Neutral;
        auto it = std::max_element(emotions_.begin(), emotions_.end(),
            [](const EmotionState& a, const EmotionState& b) {
                return a.intensity < b.intensity;
            });
        return it->type;
    }

    std::string getMoodString() const {
        if (mood_ > config_.joyfulThreshold)  return "Joyful";
        if (mood_ > config_.contentThreshold) return "Content";
        if (mood_ > config_.neutralThreshold) return "Neutral";
        if (mood_ > config_.uneasyThreshold)  return "Uneasy";
        return "Distressed";
    }

    const Need& getNeed(NeedType type) const { return needs_.at(type); }
    Need& getNeed(NeedType type) { return needs_.at(type); }
    const std::map<NeedType, Need>& needs() const { return needs_; }
    const std::vector<EmotionState>& emotions() const { return emotions_; }

    // ─── Emotional Contagion ────────────────────────────────────────
    struct EmotionalAura {
        EmotionType type = EmotionType::Neutral;
        float intensity = 0.0f;
    };

    // Returns this NPC's dominant emotional aura for contagion
    EmotionalAura getEmotionalAura() const {
        if (emotions_.empty()) return {EmotionType::Neutral, 0.0f};
        const EmotionState* strongest = &emotions_[0];
        for (const auto& e : emotions_) {
            if (e.intensity > strongest->intensity) strongest = &e;
        }
        return {strongest->type, strongest->intensity};
    }

    // Apply emotional contagion from a nearby NPC
    // empathy: receiver's empathy multiplier (from personality)
    // proximity: 0.0 (far) to 1.0 (very close)
    void applyContagion(EmotionType type, float sourceIntensity,
                        float empathy, float proximity) {
        if (type == EmotionType::Neutral) return;
        if (sourceIntensity < 0.1f) return;

        // Contagion strength: source intensity * empathy * proximity falloff
        float strength = sourceIntensity * empathy * proximity * contagionScale_;
        if (strength < 0.01f) return;

        addEmotion(type, strength, 0.5f); // short-lived contagion emotions
    }

    float contagionScale_ = 0.3f; // global contagion dampener

    float getCombatModifier() const {
        float mod = 1.0f;
        float safety = needs_.at(NeedType::Safety).value / 100.0f;
        mod *= (config_.combatSafetyBase + safety * config_.combatSafetyScale);

        for (const auto& e : emotions_) {
            if (e.type == EmotionType::Angry) {
                mod *= (1.0f + e.intensity * config_.angerCombatBoost * angerIntensityMod_);
            }
            if (e.type == EmotionType::Fearful) {
                mod *= (1.0f - e.intensity * config_.fearCombatPenalty * fearIntensityMod_);
            }
        }
        return std::clamp(mod, config_.combatModMin, config_.combatModMax);
    }

    float getSocialModifier() const {
        float social = needs_.at(NeedType::Social).urgency();
        float mod = config_.socialBase + social * config_.socialScale;
        if (mood_ > config_.positiveMoodThreshold) mod *= config_.positiveMoodBoost;
        if (mood_ < config_.negativeMoodThreshold) mod *= config_.negativeMoodPenalty;
        return std::clamp(mod, config_.socialModMin, config_.socialModMax);
    }

    float getFleeModifier() const {
        float mod = 0.0f;
        for (const auto& e : emotions_) {
            if (e.type == EmotionType::Fearful) mod += e.intensity;
        }
        float safety = 1.0f - needs_.at(NeedType::Safety).value / 100.0f;
        mod += safety * config_.fleeSafetyWeight;
        mod *= fearIntensityMod_;
        return std::clamp(mod, 0.0f, 1.0f);
    }

private:
    void updateMood() {
        float needScore = 0.0f;
        float totalWeight = 0.0f;
        for (const auto& [type, need] : needs_) {
            float w = 1.0f;
            if (type == NeedType::Safety) w = config_.moodSafetyWeight;
            if (type == NeedType::Hunger || type == NeedType::Sleep) w = config_.moodHungerSleepWeight;
            needScore += (need.value / 100.0f) * w;
            totalWeight += w;
        }
        needScore = (needScore / totalWeight) * 2.0f - 1.0f;

        float emotionScore = 0.0f;
        for (const auto& e : emotions_) {
            float val = 0.0f;
            switch (e.type) {
                case EmotionType::Happy:     val =  1.0f; break;
                case EmotionType::Surprised: val =  0.2f; break;
                case EmotionType::Neutral:   val =  0.0f; break;
                case EmotionType::Sad:       val = -0.5f; break;
                case EmotionType::Angry:     val = -0.3f; break;
                case EmotionType::Fearful:   val = -0.7f; break;
                case EmotionType::Disgusted: val = -0.4f; break;
            }
            emotionScore += val * e.intensity;
        }
        if (!emotions_.empty()) {
            emotionScore /= static_cast<float>(emotions_.size());
        }

        float emotionWeight = config_.baseEmotionWeight * moodStabilityMod_;
        float needWeight = 1.0f - emotionWeight;
        mood_ = std::clamp(needScore * needWeight + emotionScore * emotionWeight, -1.0f, 1.0f);
    }

    std::map<NeedType, Need> needs_;
    std::vector<EmotionState> emotions_;
    float mood_ = 0.5f;

    // Personality influence modifiers
    float fearIntensityMod_ = 1.0f;
    float angerIntensityMod_ = 1.0f;
    float socialDecayMod_ = 1.0f;
    float moodStabilityMod_ = 1.0f;

public:
    void applyPersonality(const PersonalityTraits& p) {
        fearIntensityMod_ = p.fearIntensityMultiplier();
        angerIntensityMod_ = p.angerIntensityMultiplier();
        socialDecayMod_ = p.socialDecayMultiplier();
        moodStabilityMod_ = p.moodStabilityMultiplier();
        needs_[NeedType::Social].decayRate *= socialDecayMod_;
    }
};

} // namespace npc
