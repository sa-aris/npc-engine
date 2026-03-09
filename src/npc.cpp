#include "npc/npc.hpp"
#include "npc/world/world.hpp"

namespace npc {

void NPC::update(float dt, GameWorld& world) {
    float currentTime = world.time().totalHours();
    std::string timeStr = world.time().formatClock();

    // ─── 1. Update needs & emotions ──────────────────────────────────
    emotions.update(dt);

    // ─── 2. Update memory ────────────────────────────────────────────
    memory.update(dt);

    // ─── 3. Update perception ────────────────────────────────────────
    // Lazy-init line-of-sight checkers from pathfinder
    if (pathfinder && !perception.losChecker) {
        auto pf = pathfinder;
        perception.losChecker = [pf](Vec2 a, Vec2 b) { return pf->hasLineOfSight(a, b); };
        perception.wallCounter = [pf](Vec2 a, Vec2 b) { return pf->countWallsOnLine(a, b); };
    }

    std::vector<SensoryInput> sensoryInputs;
    for (const auto& other : world.npcs()) {
        if (other->id == id) continue;
        if (!other->combat.stats.isAlive()) continue; // Skip dead NPCs
        SensoryInput si;
        si.entityId = other->id;
        si.position = other->position;
        si.noiseLevel = other->combat.inCombat ? perception.config.inCombatNoise : perception.config.defaultNoise;
        si.isHostile = (other->type == NPCType::Enemy);
        sensoryInputs.push_back(si);
    }
    perception.update(position, facing, sensoryInputs, currentTime, dt);

    // ─── 4. Update combat threat evaluation ──────────────────────────
    // Remove dead entities from perception
    for (const auto& other : world.npcs()) {
        if (!other->combat.stats.isAlive() && perception.hasPerceivedEntity(other->id)) {
            perception.forgetEntity(other->id);
        }
    }
    auto threats = perception.getThreats();
    std::vector<PerceivedEntity> perceivedVec;
    for (const auto& [eid, pe] : perception.perceived()) {
        perceivedVec.push_back(pe);
    }
    combat.evaluateThreats(perceivedVec, position);
    combat.update(dt);

    // ─── 5. Populate blackboard for AI ───────────────────────────────
    auto& bb = fsm.blackboard();
    bb.set<float>("_time", currentTime);
    bb.set<float>("health_pct", combat.stats.healthPercent());
    bb.set<float>("stamina_pct", combat.stats.stamina.percent());
    bb.set<float>("mana_pct", combat.stats.mana.percent());
    bb.set<bool>("in_combat", combat.inCombat);
    bb.set<bool>("has_threats", !threats.empty());
    bb.set<int>("threat_count", combat.threatCount());
    bb.set<bool>("should_flee", combat.shouldFlee());
    bb.set<bool>("has_urgent_need", emotions.hasUrgentNeed());
    bb.set<float>("mood", emotions.getMood());
    bb.set<std::string>("dominant_emotion", emotionToString(emotions.getDominantEmotion()));
    bb.set<float>("flee_modifier", emotions.getFleeModifier());

    // Need urgencies for Utility AI
    bb.set<float>("hunger_urgency", emotions.getNeed(NeedType::Hunger).urgency());
    bb.set<float>("sleep_urgency", emotions.getNeed(NeedType::Sleep).urgency());
    bb.set<float>("social_urgency", emotions.getNeed(NeedType::Social).urgency());
    bb.set<float>("safety_value", emotions.getNeed(NeedType::Safety).value / 100.0f);

    // Personality traits for AI scoring
    bb.set<float>("trait_courage", personality.courage);
    bb.set<float>("trait_sociability", personality.sociability);
    bb.set<float>("trait_greed", personality.greed);
    bb.set<float>("trait_patience", personality.patience);
    bb.set<float>("trait_intelligence", personality.intelligence);

    // Schedule info
    auto currentActivity = schedule.getCurrentActivity(world.time().currentHour());
    if (currentActivity) {
        bb.set<std::string>("scheduled_activity", activityToString(currentActivity->activity));
        bb.set<std::string>("scheduled_location", currentActivity->location);
    }

    // ─── 6. AI Decision & Behavior ──────────────────────────────────
    if (useUtilityAI) {
        // 6a. Utility AI evaluates all actions and picks the best
        auto decision = utilityAI.evaluate(bb);
        if (decision) {
            // 6b. The action's callback sets "desired_state" on the blackboard
            // which the FSM transitions pick up
            bb.set<std::string>("utility_decision", decision->actionName);
            bb.set<float>("utility_score", decision->score);
        }
    }

    // 6c. FSM update (uses blackboard data for transitions)
    fsm.update(dt);

    // 6d. Behavior Trees tick in specific states
    // Note: Combat BT is ticked externally by runCombatRound() to avoid
    // double-ticking and dead entity race conditions
    std::string state = fsm.currentState();
    if (state == "Socialize") {
        socializeBT.tick(bb);
    }

    // ─── 7. Resource regen while resting ────────────────────────────
    if (state == "Sleep" || state == "Idle" || state == "Eat") {
        combat.stats.stamina.regen(combat.stats.stamina.restRegenRate * dt);
        combat.stats.mana.regen(combat.stats.mana.restRegenRate * dt);
    }

    // ─── 8. Movement ─────────────────────────────────────────────────
    updateMovement(dt);
}

} // namespace npc
