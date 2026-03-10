#pragma once

#include "../core/types.hpp"
#include "../core/vec2.hpp"
#include <vector>
#include <map>
#include <functional>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace npc {

// ─── Tactical Role ────────────────────────────────────────────────────────────
enum class TacticalRole {
    Leader,    // issues orders, high-value target
    Vanguard,  // front line, absorbs damage
    Flanker,   // encirclement / side attack
    Support,   // healer / buffer, protected position
    Archer     // ranged, holds back behind vanguard
};

inline std::string tacticalRoleToString(TacticalRole r) {
    switch (r) {
        case TacticalRole::Leader:   return "Leader";
        case TacticalRole::Vanguard: return "Vanguard";
        case TacticalRole::Flanker:  return "Flanker";
        case TacticalRole::Support:  return "Support";
        case TacticalRole::Archer:   return "Archer";
    }
    return "Unknown";
}

// ─── Tactical State ───────────────────────────────────────────────────────────
enum class TacticalState {
    Idle,
    Advancing,   // moving toward objective in formation
    Holding,     // defensive stance, hold ground
    Flanking,    // executing flanking maneuver
    Encircling,  // surrounding a target
    Retreating,  // ordered withdrawal to rally point
    Routing,     // morale broken — uncontrolled flee
    Rallying     // recovering morale at rally point
};

inline std::string tacticalStateToString(TacticalState s) {
    switch (s) {
        case TacticalState::Idle:       return "Idle";
        case TacticalState::Advancing:  return "Advancing";
        case TacticalState::Holding:    return "Holding";
        case TacticalState::Flanking:   return "Flanking";
        case TacticalState::Encircling: return "Encircling";
        case TacticalState::Retreating: return "Retreating";
        case TacticalState::Routing:    return "Routing";
        case TacticalState::Rallying:   return "Rallying";
    }
    return "Unknown";
}

// ─── Group Morale ─────────────────────────────────────────────────────────────
struct GroupMorale {
    float value            = 100.0f;  // 0–100
    float breakThreshold   =  25.0f;  // below → Routing
    float waverThreshold   =  50.0f;  // below → penalties, hesitation
    float rallyThreshold   =  65.0f;  // above → exit Routing/Rallying

    // Morale event magnitudes
    static constexpr float ALLY_KILLED_HIT    = -15.0f;
    static constexpr float LEADER_KILLED_HIT  = -30.0f;
    static constexpr float ENEMY_KILLED_BOOST =  +8.0f;
    static constexpr float OUTNUMBERED_TICK   =  -1.5f;  // per update, per ratio point above 1
    static constexpr float FLANK_ATTACKED_HIT = -10.0f;
    static constexpr float RALLY_BOOST        = +20.0f;
    static constexpr float PASSIVE_RECOVERY   =  +0.5f;  // per update while Rallying

    bool isBroken()    const { return value <= breakThreshold; }
    bool isWavering()  const { return value <= waverThreshold; }
    bool hasRallied()  const { return value >= rallyThreshold; }

    void apply(float delta) {
        value = std::clamp(value + delta, 0.0f, 100.0f);
    }

    // Speed multiplier: broken morale → members move faster (panic), wavering → slower
    float movementModifier() const {
        if (isBroken())   return 1.4f;  // panicked sprint
        if (isWavering()) return 0.8f;  // hesitant
        return 1.0f;
    }

    // Combat effectiveness multiplier
    float combatModifier() const {
        if (isBroken())   return 0.4f;
        if (isWavering()) return 0.75f;
        return 1.0f;
    }
};

// ─── Member State ─────────────────────────────────────────────────────────────
struct MemberState {
    EntityId     id;
    TacticalRole role          = TacticalRole::Vanguard;
    bool         isAlive       = true;
    Vec2         assignedPos   = {};   // current tactical target position
    int          retreatOrder  = 0;    // lower = retreats first
};

// ─── Per-member order produced each update ────────────────────────────────────
struct MemberOrder {
    EntityId    id;
    Vec2        targetPos;
    bool        shouldAttack  = false;
    bool        shouldRetreat = false;
    bool        shouldHold    = false;
    TacticalRole role         = TacticalRole::Vanguard;
    float       priorityScore = 0.0f;  // higher = more urgent
};

// ─── GroupBehavior ────────────────────────────────────────────────────────────
class GroupBehavior {
public:
    static constexpr float FORMATION_SPACING     = 2.0f;
    static constexpr float ENCIRCLE_RADIUS       = 4.0f;
    static constexpr float FLANK_OFFSET_DISTANCE = 5.0f;
    static constexpr float RETREAT_SPREAD        = 1.5f;

    // ── Setup ─────────────────────────────────────────────────────────────────
    void setLeader(EntityId id) {
        leader_ = id;
        if (auto* m = findMember(id)) m->role = TacticalRole::Leader;
    }
    EntityId leader() const { return leader_; }

    void addMember(EntityId id, TacticalRole role = TacticalRole::Vanguard) {
        if (findMember(id)) return;
        MemberState ms;
        ms.id   = id;
        ms.role = (id == leader_) ? TacticalRole::Leader : role;
        members_.push_back(ms);
        recomputeRetreatOrder();
    }

    void removeMember(EntityId id) {
        members_.erase(
            std::remove_if(members_.begin(), members_.end(),
                [id](const MemberState& m){ return m.id == id; }),
            members_.end());
    }

    void setMemberRole(EntityId id, TacticalRole role) {
        if (auto* m = findMember(id)) {
            m->role = role;
            recomputeRetreatOrder();
        }
    }

    void setFormation(FormationType type) { formation_ = type; }
    FormationType formation() const { return formation_; }

    const std::vector<MemberState>& members() const { return members_; }
    int  size()       const { return static_cast<int>(members_.size()); }
    bool isEmpty()    const { return members_.empty(); }

    TacticalState tacticalState() const { return state_; }
    const GroupMorale& morale()   const { return morale_; }

    // ── Morale events ─────────────────────────────────────────────────────────
    void onAllyKilled(EntityId id) {
        if (auto* m = findMember(id)) m->isAlive = false;
        morale_.apply(GroupMorale::ALLY_KILLED_HIT);
        checkMoraleBreak();
    }

    void onLeaderKilled() {
        morale_.apply(GroupMorale::LEADER_KILLED_HIT);
        // Promote highest-priority surviving member to leader
        for (auto& m : members_) {
            if (m.isAlive && m.id != leader_) {
                leader_  = m.id;
                m.role   = TacticalRole::Leader;
                break;
            }
        }
        checkMoraleBreak();
    }

    void onEnemyKilled() {
        morale_.apply(GroupMorale::ENEMY_KILLED_BOOST);
        if (state_ == TacticalState::Routing || state_ == TacticalState::Rallying)
            if (morale_.hasRallied()) state_ = TacticalState::Idle;
    }

    void onFlankAttacked() {
        morale_.apply(GroupMorale::FLANK_ATTACKED_HIT);
        checkMoraleBreak();
    }

    void onOutnumbered(float ratioEnemyToGroup) {
        if (ratioEnemyToGroup > 1.0f)
            morale_.apply(GroupMorale::OUTNUMBERED_TICK * (ratioEnemyToGroup - 1.0f));
        checkMoraleBreak();
    }

    void rally(float boost = GroupMorale::RALLY_BOOST) {
        morale_.apply(boost);
        if (morale_.hasRallied() && state_ == TacticalState::Routing)
            state_ = TacticalState::Rallying;
    }

    bool isRouting()   const { return state_ == TacticalState::Routing; }
    bool isWavering()  const { return morale_.isWavering(); }

    // ── Order issuing ─────────────────────────────────────────────────────────
    struct GroupOrder {
        enum Type { MoveTo, Attack, Defend, Retreat, Regroup, Encircle, Flank };
        Type     type         = Type::MoveTo;
        Vec2     targetPos    = {};
        EntityId targetEntity = INVALID_ENTITY;
    };

    void issueOrder(const GroupOrder& order) {
        currentOrder_  = order;
        hasOrder_      = true;

        switch (order.type) {
            case GroupOrder::MoveTo:   state_ = TacticalState::Advancing;  break;
            case GroupOrder::Attack:   state_ = TacticalState::Advancing;  break;
            case GroupOrder::Defend:   state_ = TacticalState::Holding;    break;
            case GroupOrder::Retreat:  beginRetreat(order.targetPos);      break;
            case GroupOrder::Regroup:  state_ = TacticalState::Rallying;   break;
            case GroupOrder::Encircle: beginEncircle(order.targetPos);     break;
            case GroupOrder::Flank:    beginFlank(order.targetPos);        break;
        }
    }

    bool             hasOrder()     const { return hasOrder_; }
    const GroupOrder& currentOrder() const { return currentOrder_; }
    void             clearOrder()         { hasOrder_ = false; }

    // ── Formation positions ───────────────────────────────────────────────────
    // Returns target world position for a member at the given index.
    Vec2 getFormationPosition(int memberIndex, Vec2 leaderPos, Vec2 leaderFacing) const {
        if (memberIndex < 0 || members_.empty()) return leaderPos;

        Vec2  right   = {-leaderFacing.y, leaderFacing.x};
        float spacing = FORMATION_SPACING;

        switch (formation_) {
            case FormationType::Line: {
                int   half   = size() / 2;
                float offset = (memberIndex - half) * spacing;
                return leaderPos + right * offset;
            }
            case FormationType::Column: {
                Vec2 back = leaderFacing * -1.0f;
                return leaderPos + back * (spacing * (memberIndex + 1));
            }
            case FormationType::Circle: {
                float angle  = (2.0f * kPi * memberIndex) / std::max(1, size());
                float radius = spacing * 2.0f;
                return leaderPos + Vec2{std::cos(angle), std::sin(angle)} * radius;
            }
            case FormationType::Wedge: {
                int  side = (memberIndex % 2 == 0) ? 1 : -1;
                int  row  = (memberIndex / 2) + 1;
                Vec2 back = leaderFacing * -1.0f;
                return leaderPos
                    + back  * (spacing * row)
                    + right * (spacing * row * side * 0.5f);
            }
        }
        return leaderPos;
    }

    // ── Encirclement positions ────────────────────────────────────────────────
    // Distribute alive members around targetPos. Returns assigned positions.
    std::vector<std::pair<EntityId, Vec2>>
    computeEncirclementPositions(Vec2 targetPos, Vec2 approachDir) const {
        std::vector<std::pair<EntityId, Vec2>> result;
        auto alive = aliveMembers();
        if (alive.empty()) return result;

        int n = static_cast<int>(alive.size());
        for (int i = 0; i < n; ++i) {
            float angle = (2.0f * kPi * i) / n;
            Vec2  pos   = targetPos + Vec2{std::cos(angle), std::sin(angle)} * ENCIRCLE_RADIUS;
            result.push_back({alive[i]->id, pos});
        }
        return result;
    }

    // ── Flanking positions ────────────────────────────────────────────────────
    // Half the members go left-flank, half go right-flank around the target.
    std::vector<std::pair<EntityId, Vec2>>
    computeFlankPositions(Vec2 targetPos, Vec2 approachDir) const {
        std::vector<std::pair<EntityId, Vec2>> result;
        auto alive = aliveMembers();
        if (alive.empty()) return result;

        Vec2 right  = Vec2{-approachDir.y,  approachDir.x}.normalized();
        Vec2 left   = Vec2{ approachDir.y, -approachDir.x}.normalized();
        Vec2 behind = approachDir * -1.0f;

        int n     = static_cast<int>(alive.size());
        int split = n / 2;

        for (int i = 0; i < n; ++i) {
            Vec2 side = (i < split) ? right : left;
            float depth = (i % split + 1) * FORMATION_SPACING;
            Vec2 pos  = targetPos
                        + side   * FLANK_OFFSET_DISTANCE
                        + behind * depth;
            result.push_back({alive[i]->id, pos});
        }
        return result;
    }

    // ── Retreat positions ─────────────────────────────────────────────────────
    // Returns ordered withdrawal assignments.
    // Support retreats first, then flankers, then vanguard (covering).
    std::vector<std::pair<EntityId, Vec2>>
    computeRetreatPositions(Vec2 rallyPoint) const {
        std::vector<std::pair<EntityId, Vec2>> result;

        // Sort alive members by retreatOrder ascending (low = retreats first)
        auto alive = aliveMembers();
        std::sort(alive.begin(), alive.end(),
            [](const MemberState* a, const MemberState* b){
                return a->retreatOrder < b->retreatOrder;
            });

        for (int i = 0; i < static_cast<int>(alive.size()); ++i) {
            // Stagger positions: early retreaters are closer to rally point
            float t = (alive.size() > 1)
                      ? static_cast<float>(i) / (alive.size() - 1)
                      : 1.0f;
            float spread = RETREAT_SPREAD * (alive.size() - i);
            float angle  = static_cast<float>(i) * 1.1f;  // slight spread fan
            Vec2  offset = Vec2{std::cos(angle), std::sin(angle)} * spread;
            Vec2  pos    = rallyPoint.lerp(rallyPoint + offset, 1.0f - t * 0.3f);
            result.push_back({alive[i]->id, pos});
        }
        return result;
    }

    // ── Main update ───────────────────────────────────────────────────────────
    // positionOf: callback returning current world position of an entity.
    // Returns per-member orders for this tick.
    std::vector<MemberOrder> update(
            float dt,
            const std::function<Vec2(EntityId)>& positionOf,
            Vec2 leaderFacing = {0.0f, 1.0f})
    {
        std::vector<MemberOrder> orders;
        if (members_.empty()) return orders;

        // Passive morale recovery while rallying
        if (state_ == TacticalState::Rallying) {
            morale_.apply(GroupMorale::PASSIVE_RECOVERY);
            if (morale_.hasRallied()) state_ = TacticalState::Idle;
        }

        Vec2 leaderPos = positionOf(leader_);

        for (auto& m : members_) {
            if (!m.isAlive) continue;

            MemberOrder order;
            order.id   = m.id;
            order.role = m.role;

            switch (state_) {
                case TacticalState::Routing: {
                    // Panic: flee away from last known threat (opposite of leader facing)
                    Vec2 fleeDir  = (leaderFacing * -1.0f).normalized();
                    Vec2 myPos    = positionOf(m.id);
                    Vec2 scatter  = Vec2{std::cos(m.id * 2.3f), std::sin(m.id * 1.7f)} * 3.0f;
                    order.targetPos    = myPos + fleeDir * 8.0f + scatter;
                    order.shouldRetreat= true;
                    order.priorityScore= 1.0f;
                    break;
                }
                case TacticalState::Retreating: {
                    order.targetPos    = m.assignedPos;
                    order.shouldRetreat= true;
                    // Rearguard (last in retreat order) holds briefly
                    order.shouldHold   = (m.retreatOrder == maxRetreatOrder());
                    order.priorityScore= 1.0f - (m.retreatOrder * 0.1f);
                    break;
                }
                case TacticalState::Holding: {
                    // Hold assigned position, face threat
                    order.targetPos    = m.assignedPos.lengthSquared() > 0.01f
                                         ? m.assignedPos
                                         : positionOf(m.id);
                    order.shouldHold   = true;
                    order.shouldAttack = true;
                    order.priorityScore= 0.8f;
                    break;
                }
                case TacticalState::Encircling:
                case TacticalState::Flanking: {
                    order.targetPos    = m.assignedPos;
                    order.shouldAttack = true;
                    order.priorityScore= 0.9f;
                    break;
                }
                case TacticalState::Advancing: {
                    // Build index for formation
                    int idx = memberIndex(m.id);
                    order.targetPos    = getFormationPosition(idx, leaderPos, leaderFacing);
                    order.shouldAttack = hasOrder_ &&
                                         currentOrder_.type == GroupOrder::Attack;
                    order.priorityScore= 0.7f;
                    break;
                }
                case TacticalState::Rallying:
                case TacticalState::Idle:
                default: {
                    int idx = memberIndex(m.id);
                    order.targetPos    = getFormationPosition(idx, leaderPos, leaderFacing);
                    order.priorityScore= 0.3f;
                    break;
                }
            }

            // Apply morale movement modifier as a hint via priorityScore
            order.priorityScore *= morale_.movementModifier();
            orders.push_back(order);
        }

        return orders;
    }

    // ── Alive member helpers ──────────────────────────────────────────────────
    int aliveCount() const {
        int n = 0;
        for (const auto& m : members_) if (m.isAlive) ++n;
        return n;
    }

    std::vector<EntityId> aliveMemberIds() const {
        std::vector<EntityId> out;
        for (const auto& m : members_) if (m.isAlive) out.push_back(m.id);
        return out;
    }

private:
    static constexpr float kPi = 3.14159265f;

    EntityId              leader_    = INVALID_ENTITY;
    std::vector<MemberState> members_;
    FormationType         formation_ = FormationType::Line;
    TacticalState         state_     = TacticalState::Idle;
    GroupMorale           morale_;
    GroupOrder            currentOrder_;
    bool                  hasOrder_  = false;
    Vec2                  rallyPoint_= {};

    // ── Internal helpers ──────────────────────────────────────────────────────
    MemberState* findMember(EntityId id) {
        for (auto& m : members_) if (m.id == id) return &m;
        return nullptr;
    }
    const MemberState* findMember(EntityId id) const {
        for (const auto& m : members_) if (m.id == id) return &m;
        return nullptr;
    }

    std::vector<const MemberState*> aliveMembers() const {
        std::vector<const MemberState*> out;
        for (const auto& m : members_) if (m.isAlive) out.push_back(&m);
        return out;
    }

    int memberIndex(EntityId id) const {
        for (int i = 0; i < static_cast<int>(members_.size()); ++i)
            if (members_[i].id == id) return i;
        return 0;
    }

    int maxRetreatOrder() const {
        int mx = 0;
        for (const auto& m : members_)
            if (m.isAlive) mx = std::max(mx, m.retreatOrder);
        return mx;
    }

    // Support retreats first (order 0), then Archers, Flankers, Vanguard (rearguard)
    void recomputeRetreatOrder() {
        auto priority = [](TacticalRole r) -> int {
            switch (r) {
                case TacticalRole::Support:  return 0;
                case TacticalRole::Archer:   return 1;
                case TacticalRole::Leader:   return 2;
                case TacticalRole::Flanker:  return 3;
                case TacticalRole::Vanguard: return 4;
            }
            return 5;
        };
        for (auto& m : members_)
            m.retreatOrder = priority(m.role);
    }

    void checkMoraleBreak() {
        if (morale_.isBroken() && state_ != TacticalState::Routing)
            state_ = TacticalState::Routing;
    }

    void beginRetreat(Vec2 rallyPoint) {
        rallyPoint_ = rallyPoint;
        state_      = TacticalState::Retreating;
        auto positions = computeRetreatPositions(rallyPoint);
        for (auto& [id, pos] : positions)
            if (auto* m = findMember(id)) m->assignedPos = pos;
    }

    void beginEncircle(Vec2 targetPos) {
        state_ = TacticalState::Encircling;
        Vec2 dir = (targetPos - Vec2{}).normalized();
        auto positions = computeEncirclementPositions(targetPos, dir);
        for (auto& [id, pos] : positions)
            if (auto* m = findMember(id)) m->assignedPos = pos;
    }

    void beginFlank(Vec2 targetPos) {
        state_ = TacticalState::Flanking;
        Vec2 dir = (targetPos - Vec2{}).normalized();
        auto positions = computeFlankPositions(targetPos, dir);
        for (auto& [id, pos] : positions)
            if (auto* m = findMember(id)) m->assignedPos = pos;
    }
};

} // namespace npc
