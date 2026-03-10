#pragma once

#include "../core/types.hpp"
#include "../core/random.hpp"
#include "../event/event_system.hpp"
#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <cmath>

namespace npc {

// ─── Weather Types ────────────────────────────────────────────────────────────
enum class WeatherType {
    Clear,
    Cloudy,
    Rain,
    HeavyRain,
    Storm,      // thunder + lightning; outdoor work impossible
    Snow,
    Blizzard,   // extreme snow; most outdoor activity blocked
    Fog,
    HeavyFog
};

inline std::string weatherToString(WeatherType w) {
    switch (w) {
        case WeatherType::Clear:     return "Clear";
        case WeatherType::Cloudy:    return "Cloudy";
        case WeatherType::Rain:      return "Rain";
        case WeatherType::HeavyRain: return "HeavyRain";
        case WeatherType::Storm:     return "Storm";
        case WeatherType::Snow:      return "Snow";
        case WeatherType::Blizzard:  return "Blizzard";
        case WeatherType::Fog:       return "Fog";
        case WeatherType::HeavyFog:  return "HeavyFog";
    }
    return "Unknown";
}

// ─── Weather Event (published to EventBus) ────────────────────────────────────
struct WeatherChangedEvent {
    WeatherType previous;
    WeatherType current;
    float       intensity;
};

// ─── Compiled Effects ─────────────────────────────────────────────────────────
// All multipliers and flags derived from the current weather condition.
struct WeatherEffects {
    // Perception
    float sightRangeMul      = 1.0f;   // applied to PerceptionConfig::sightRange
    float hearingRangeMul    = 1.0f;   // rain masks sounds

    // Movement / steering
    float speedMul           = 1.0f;   // applied to SteeringAgent::maxSpeed
    float staminaDrainMul    = 1.0f;   // extra fatigue per hour outdoors

    // Activity blocking
    bool  blocksFarming      = false;  // Work at Farm
    bool  blocksOutdoorLeisure= false; // Leisure / Socialize outdoors
    bool  blocksPatrol       = false;  // Patrol (extreme only)
    bool  forcesIndoor       = false;  // Blizzard: everyone goes home

    // Mood / emotion
    float moodDelta          = 0.0f;   // per game-hour, applied to EmotionSystem
    EmotionType triggeredEmotion = EmotionType::Neutral;
    float emotionIntensity   = 0.0f;   // 0 = no trigger

    // Combat / perception
    float combatAccuracyMul  = 1.0f;   // rain/fog impairs ranged
    float awarenessDecayMul  = 1.0f;   // fog speeds up awareness decay

    bool  isOutdoorDangerous = false;  // true for Storm/Blizzard
};

// ─── Transition Entry ─────────────────────────────────────────────────────────
struct WeatherTransition {
    WeatherType from;
    WeatherType to;
    float       probability;  // relative weight
};

// ─── Weather Condition State ──────────────────────────────────────────────────
struct WeatherCondition {
    WeatherType type      = WeatherType::Clear;
    float       intensity = 1.0f;    // 0 (barely present) to 1 (maximum severity)
    float       duration  = 4.0f;    // planned duration in game hours
    float       elapsed   = 0.0f;    // hours since this weather started

    bool isExpired() const { return elapsed >= duration; }
    float progress() const { return std::clamp(elapsed / duration, 0.0f, 1.0f); }
};

// ─── WeatherSystem ────────────────────────────────────────────────────────────
class WeatherSystem {
public:
    explicit WeatherSystem(WeatherType initial = WeatherType::Clear)
        : current_({initial, 1.0f, 8.0f, 0.0f})
        , previous_(initial) {}

    // ── Accessors ─────────────────────────────────────────────────────────────
    WeatherType         type()      const { return current_.type; }
    const WeatherCondition& condition() const { return current_; }
    float               intensity() const { return current_.intensity; }

    // ── Compiled effects for current weather ──────────────────────────────────
    WeatherEffects effects() const { return compile(current_.type, current_.intensity); }

    // ── Per-system modifier helpers ───────────────────────────────────────────

    // Call with NPC's base sight range → returns weather-modified value
    float modifySightRange(float baseSight) const {
        return baseSight * compile(current_.type, current_.intensity).sightRangeMul;
    }

    float modifyHearingRange(float baseHearing) const {
        return baseHearing * compile(current_.type, current_.intensity).hearingRangeMul;
    }

    float modifySpeed(float baseSpeed) const {
        return baseSpeed * compile(current_.type, current_.intensity).speedMul;
    }

    // Is this activity blocked by current weather?
    bool isActivityBlocked(ActivityType act) const {
        auto fx = effects();
        if (fx.forcesIndoor)         return true;
        if (fx.blocksFarming && act == ActivityType::Work)     return true;  // caller checks location
        if (fx.blocksPatrol  && act == ActivityType::Patrol)   return true;
        if (fx.blocksOutdoorLeisure &&
            (act == ActivityType::Leisure || act == ActivityType::Socialize)) return true;
        return false;
    }

    // Mood delta per game-hour for this weather (add to NPC mood)
    float hourlyMoodDelta() const {
        return compile(current_.type, current_.intensity).moodDelta;
    }

    // ── Update ────────────────────────────────────────────────────────────────
    // dt in game hours. Publishes WeatherChangedEvent when weather transitions.
    void update(float dt, RandomGenerator& rng, EventBus* bus = nullptr) {
        current_.elapsed += dt;
        if (!current_.isExpired()) return;

        // Transition to next weather
        WeatherType next = pickNext(current_.type, rng);
        float       dur  = randomDuration(next, rng);
        float       intn = randomIntensity(next, rng);

        previous_ = current_.type;
        current_  = {next, intn, dur, 0.0f};

        if (bus) bus->publish(WeatherChangedEvent{previous_, next, intn});
    }

    // Force a specific weather (for scripted events, testing)
    void set(WeatherType type, float intensity = 1.0f,
             float duration = 4.0f, EventBus* bus = nullptr) {
        previous_ = current_.type;
        current_  = {type, std::clamp(intensity, 0.0f, 1.0f), duration, 0.0f};
        if (bus) bus->publish(WeatherChangedEvent{previous_, type, intensity});
    }

    // ── Subscribe to EventBus ──────────────────────────────────────────────────
    // Listens for WorldEvents that should trigger weather changes
    // ("storm_incoming", "clear_skies", etc.)
    SubscriptionId subscribeToEvents(EventBus& bus, EventBus* outBus = nullptr) {
        return bus.subscribe<WorldEvent>([this, outBus](const WorldEvent& ev) {
            handleWorldEvent(ev, outBus);
        });
    }

    // ── Season bias (optional) ────────────────────────────────────────────────
    // Set to prefer certain weather types (0=spring,1=summer,2=autumn,3=winter)
    void setSeason(int season) { season_ = std::clamp(season, 0, 3); }

    WeatherType previousType() const { return previous_; }

private:
    // ── Static effect table ───────────────────────────────────────────────────
    static WeatherEffects compile(WeatherType type, float intensity) {
        WeatherEffects fx;
        switch (type) {
            case WeatherType::Clear:
                fx.moodDelta       = +0.08f * intensity;
                fx.triggeredEmotion= EmotionType::Happy;
                fx.emotionIntensity= 0.15f  * intensity;
                break;

            case WeatherType::Cloudy:
                fx.moodDelta       = -0.02f;
                fx.sightRangeMul   = 0.95f;
                break;

            case WeatherType::Rain:
                fx.sightRangeMul   = 0.80f  - 0.10f * intensity;
                fx.hearingRangeMul = 0.85f;
                fx.speedMul        = 0.90f  - 0.05f * intensity;
                fx.staminaDrainMul = 1.20f;
                fx.moodDelta       = -0.08f * intensity;
                fx.combatAccuracyMul = 0.85f;
                fx.blocksFarming   = intensity > 0.7f;
                fx.triggeredEmotion= EmotionType::Sad;
                fx.emotionIntensity= 0.20f * intensity;
                break;

            case WeatherType::HeavyRain:
                fx.sightRangeMul   = 0.55f;
                fx.hearingRangeMul = 0.65f;
                fx.speedMul        = 0.80f;
                fx.staminaDrainMul = 1.50f;
                fx.moodDelta       = -0.15f;
                fx.combatAccuracyMul = 0.70f;
                fx.blocksFarming   = true;
                fx.blocksOutdoorLeisure = true;
                fx.triggeredEmotion= EmotionType::Sad;
                fx.emotionIntensity= 0.35f;
                break;

            case WeatherType::Storm:
                fx.sightRangeMul   = 0.50f;
                fx.hearingRangeMul = 0.50f;
                fx.speedMul        = 0.65f;
                fx.staminaDrainMul = 2.00f;
                fx.moodDelta       = -0.25f;
                fx.combatAccuracyMul = 0.55f;
                fx.awarenessDecayMul = 1.40f;
                fx.blocksFarming   = true;
                fx.blocksOutdoorLeisure = true;
                fx.isOutdoorDangerous = true;
                fx.triggeredEmotion= EmotionType::Fearful;
                fx.emotionIntensity= 0.50f;
                break;

            case WeatherType::Snow:
                fx.sightRangeMul   = 0.80f;
                fx.speedMul        = 0.75f - 0.15f * intensity;
                fx.staminaDrainMul = 1.40f;
                fx.moodDelta       = -0.05f + 0.10f * (1.0f - intensity); // light snow = pleasant
                fx.blocksFarming   = intensity > 0.6f;
                fx.combatAccuracyMul = 0.85f;
                fx.triggeredEmotion= EmotionType::Surprised;
                fx.emotionIntensity= 0.15f;
                break;

            case WeatherType::Blizzard:
                fx.sightRangeMul   = 0.20f;
                fx.hearingRangeMul = 0.60f;
                fx.speedMul        = 0.35f;
                fx.staminaDrainMul = 3.00f;
                fx.moodDelta       = -0.35f;
                fx.combatAccuracyMul = 0.45f;
                fx.awarenessDecayMul = 2.00f;
                fx.blocksFarming   = true;
                fx.blocksPatrol    = true;
                fx.blocksOutdoorLeisure = true;
                fx.forcesIndoor    = true;
                fx.isOutdoorDangerous = true;
                fx.triggeredEmotion= EmotionType::Fearful;
                fx.emotionIntensity= 0.65f;
                break;

            case WeatherType::Fog:
                fx.sightRangeMul   = 0.35f - 0.10f * intensity;
                fx.speedMul        = 0.90f;
                fx.moodDelta       = -0.06f;
                fx.awarenessDecayMul = 1.50f;
                fx.combatAccuracyMul = 0.75f;
                fx.triggeredEmotion= EmotionType::Fearful;
                fx.emotionIntensity= 0.20f * intensity;
                break;

            case WeatherType::HeavyFog:
                fx.sightRangeMul   = 0.12f;
                fx.speedMul        = 0.80f;
                fx.moodDelta       = -0.12f;
                fx.awarenessDecayMul = 2.50f;
                fx.combatAccuracyMul = 0.50f;
                fx.blocksPatrol    = true;
                fx.triggeredEmotion= EmotionType::Fearful;
                fx.emotionIntensity= 0.40f;
                break;
        }
        return fx;
    }

    // ── Markov transition table ───────────────────────────────────────────────
    // Returns next weather type based on probabilities and current season.
    WeatherType pickNext(WeatherType current, RandomGenerator& rng) const {
        // Transition weights [from][to] tuned per season
        struct Row { WeatherType to; float weight; };
        std::vector<Row> options;

        bool winter = (season_ == 3);
        bool summer = (season_ == 1);

        switch (current) {
            case WeatherType::Clear:
                options = {{WeatherType::Clear,    summer ? 0.50f : 0.35f},
                           {WeatherType::Cloudy,   0.30f},
                           {WeatherType::Fog,      0.10f},
                           {WeatherType::Rain,     summer ? 0.05f : 0.15f},
                           {WeatherType::Snow,     winter ? 0.10f : 0.00f}};
                break;
            case WeatherType::Cloudy:
                options = {{WeatherType::Clear,    0.25f},
                           {WeatherType::Cloudy,   0.20f},
                           {WeatherType::Rain,     0.25f},
                           {WeatherType::HeavyRain,0.10f},
                           {WeatherType::Fog,      0.10f},
                           {WeatherType::Snow,     winter ? 0.10f : 0.00f}};
                break;
            case WeatherType::Rain:
                options = {{WeatherType::Rain,     0.30f},
                           {WeatherType::HeavyRain,0.20f},
                           {WeatherType::Storm,    0.10f},
                           {WeatherType::Cloudy,   0.25f},
                           {WeatherType::Clear,    0.15f}};
                break;
            case WeatherType::HeavyRain:
                options = {{WeatherType::Rain,     0.35f},
                           {WeatherType::Storm,    0.25f},
                           {WeatherType::Cloudy,   0.30f},
                           {WeatherType::Clear,    0.10f}};
                break;
            case WeatherType::Storm:
                options = {{WeatherType::HeavyRain,0.40f},
                           {WeatherType::Rain,     0.30f},
                           {WeatherType::Cloudy,   0.20f},
                           {WeatherType::Clear,    0.10f}};
                break;
            case WeatherType::Snow:
                options = {{WeatherType::Snow,     0.40f},
                           {WeatherType::Blizzard, 0.15f},
                           {WeatherType::Cloudy,   0.25f},
                           {WeatherType::Clear,    0.20f}};
                break;
            case WeatherType::Blizzard:
                options = {{WeatherType::Snow,     0.50f},
                           {WeatherType::Cloudy,   0.30f},
                           {WeatherType::Clear,    0.20f}};
                break;
            case WeatherType::Fog:
                options = {{WeatherType::Fog,      0.20f},
                           {WeatherType::HeavyFog, 0.15f},
                           {WeatherType::Cloudy,   0.40f},
                           {WeatherType::Clear,    0.25f}};
                break;
            case WeatherType::HeavyFog:
                options = {{WeatherType::Fog,      0.40f},
                           {WeatherType::Cloudy,   0.40f},
                           {WeatherType::Clear,    0.20f}};
                break;
        }

        float total = 0.0f;
        for (const auto& r : options) total += r.weight;
        float roll = rng.randomFloat(0.0f, total);
        float acc  = 0.0f;
        for (const auto& r : options) {
            acc += r.weight;
            if (roll <= acc) return r.to;
        }
        return options.back().to;
    }

    static float randomDuration(WeatherType type, RandomGenerator& rng) {
        switch (type) {
            case WeatherType::Clear:     return rng.randomFloat(4.0f, 12.0f);
            case WeatherType::Cloudy:    return rng.randomFloat(2.0f,  8.0f);
            case WeatherType::Rain:      return rng.randomFloat(1.5f,  5.0f);
            case WeatherType::HeavyRain: return rng.randomFloat(1.0f,  3.0f);
            case WeatherType::Storm:     return rng.randomFloat(0.5f,  2.0f);
            case WeatherType::Snow:      return rng.randomFloat(2.0f,  8.0f);
            case WeatherType::Blizzard:  return rng.randomFloat(1.0f,  4.0f);
            case WeatherType::Fog:       return rng.randomFloat(1.0f,  4.0f);
            case WeatherType::HeavyFog:  return rng.randomFloat(0.5f,  2.0f);
        }
        return 2.0f;
    }

    static float randomIntensity(WeatherType type, RandomGenerator& rng) {
        // Clear always full; extremes push toward high intensity
        switch (type) {
            case WeatherType::Clear:    return 1.0f;
            case WeatherType::Blizzard:
            case WeatherType::Storm:    return rng.randomFloat(0.7f, 1.0f);
            case WeatherType::HeavyFog:
            case WeatherType::HeavyRain:return rng.randomFloat(0.6f, 1.0f);
            default:                    return rng.randomFloat(0.3f, 1.0f);
        }
    }

    void handleWorldEvent(const WorldEvent& ev, EventBus* bus) {
        if (ev.eventType == "storm_incoming")
            set(WeatherType::Storm, ev.severity, 3.0f, bus);
        else if (ev.eventType == "clear_skies")
            set(WeatherType::Clear, 1.0f, 8.0f, bus);
        else if (ev.eventType == "blizzard_warning")
            set(WeatherType::Blizzard, ev.severity, 4.0f, bus);
        else if (ev.eventType == "fog_rolls_in")
            set(WeatherType::HeavyFog, ev.severity, 2.0f, bus);
    }

    WeatherCondition current_;
    WeatherType      previous_;
    int              season_ = 0;  // 0=spring,1=summer,2=autumn,3=winter
};

} // namespace npc
