#pragma once
#include "../core/types.hpp"
#include "../event/event_system.hpp"
#include <string>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <optional>
#include <sstream>

namespace npc {

// ─── Diplomatic stance ────────────────────────────────────────────────

enum class FactionStance : uint8_t {
    Peace,      // default — no special obligation
    Alliance,   // mutual defense pact
    War,        // active armed conflict
    Trade,      // economic partnership (no military obligation)
    Vassal,     // weaker party follows overlord's wars/alliances
    Truce,      // post-war cooling period — can't re-declare war until expired
};

inline const char* stanceName(FactionStance s) {
    switch (s) {
        case FactionStance::Peace:    return "Peace";
        case FactionStance::Alliance: return "Alliance";
        case FactionStance::War:      return "War";
        case FactionStance::Trade:    return "Trade";
        case FactionStance::Vassal:   return "Vassal";
        case FactionStance::Truce:    return "Truce";
    }
    return "Unknown";
}

// ─── Diplomatic relation record ───────────────────────────────────────

struct FactionRelation {
    FactionStance stance       = FactionStance::Peace;
    float         value        = 0.f;   // -100..100 fine-grained within stance
    float         stanceAt     = 0.f;   // sim time when stance was set
    float         truceEndsAt  = -1.f;  // only used for Truce
    std::string   reason;
};

// ─── Event types published to EventBus ───────────────────────────────

struct FactionStanceChangedEvent {
    FactionId     faction1, faction2;
    FactionStance oldStance, newStance;
    std::string   reason;
};

// ─── Diplomatic history entry ─────────────────────────────────────────

struct DiplomaticEvent {
    float         time;
    FactionId     initiator, target;
    FactionStance newStance;
    std::string   reason;
};

// ─── Coalition result ─────────────────────────────────────────────────

struct Coalition {
    FactionId              aggressor;
    FactionId              defender;
    std::vector<FactionId> aggressorSide;
    std::vector<FactionId> defenderSide;
};

// ═══════════════════════════════════════════════════════════════════════
// Faction
// ═══════════════════════════════════════════════════════════════════════

struct Faction {
    FactionId          id;
    std::string        name;
    std::set<EntityId> members;
    FactionId          overlordId   = NO_FACTION; // 0 = independent
    float              reputation   = 0.f;        // global -100..100
    std::string        description;
};

// ═══════════════════════════════════════════════════════════════════════
// FactionSystem
// ═══════════════════════════════════════════════════════════════════════

class FactionSystem {
public:
    // ── Faction management ───────────────────────────────────────────

    void addFaction(FactionId id, const std::string& name,
                    FactionId overlordId = NO_FACTION) {
        factions_[id] = {id, name, {}, overlordId, 0.f, ""};
    }

    void addMember(FactionId fid, EntityId eid) {
        if (auto* f = mut(fid)) { f->members.insert(eid); entityFaction_[eid] = fid; }
    }
    void removeMember(FactionId fid, EntityId eid) {
        if (auto* f = mut(fid)) { f->members.erase(eid); entityFaction_.erase(eid); }
    }

    FactionId factionOf(EntityId eid) const {
        auto it = entityFaction_.find(eid);
        return it != entityFaction_.end() ? it->second : NO_FACTION;
    }
    // backward-compat alias
    FactionId getFactionOf(EntityId eid) const { return factionOf(eid); }

    const Faction* faction(FactionId id) const {
        auto it = factions_.find(id);
        return it != factions_.end() ? &it->second : nullptr;
    }
    const std::map<FactionId, Faction>& factions() const { return factions_; }

    // ── Reputation ───────────────────────────────────────────────────

    void  modifyReputation(FactionId id, float delta) {
        if (auto* f = mut(id))
            f->reputation = std::clamp(f->reputation + delta, -100.f, 100.f);
    }
    float reputation(FactionId id) const {
        auto* f = faction(id); return f ? f->reputation : 0.f;
    }

    // ── Low-level relation value ─────────────────────────────────────

    float getRelation(FactionId a, FactionId b) const {
        if (a == b) return 100.f;
        auto it = relations_.find(key(a, b));
        return it != relations_.end() ? it->second.value : 0.f;
    }
    void setRelation(FactionId a, FactionId b, float v) {
        relations_[key(a,b)].value = std::clamp(v, -100.f, 100.f);
    }
    void modifyRelation(FactionId a, FactionId b, float delta) {
        setRelation(a, b, getRelation(a, b) + delta);
    }

    // ── Stance ───────────────────────────────────────────────────────

    FactionStance getStance(FactionId a, FactionId b) const {
        if (a == b) return FactionStance::Alliance;
        auto it = relations_.find(key(a,b));
        return it != relations_.end() ? it->second.stance : FactionStance::Peace;
    }

    void setStance(FactionId a, FactionId b, FactionStance s,
                   const std::string& reason = "",
                   float simTime = 0.f,
                   EventBus* bus = nullptr)
    {
        auto& rel  = relations_[key(a,b)];
        auto  old  = rel.stance;
        rel.stance = s;
        rel.stanceAt = simTime;
        rel.reason   = reason;

        history_.push_back({simTime, a, b, s, reason});

        if (bus && old != s)
            bus->publish(FactionStanceChangedEvent{a, b, old, s, reason});
    }

    // ── Diplomatic actions (with optional cascade) ────────────────────

    void declareWar(FactionId a, FactionId b,
                    const std::string& reason = "",
                    float simTime = 0.f,
                    bool  cascade  = true,
                    EventBus* bus  = nullptr)
    {
        setStance(a, b, FactionStance::War, reason, simTime, bus);
        setRelation(a, b, std::min(getRelation(a,b), -60.f));

        if (!cascade) return;

        // Allies of B auto-join if they are in Alliance with B
        for (auto& ally : alliesOf(b)) {
            if (ally == a) continue;
            if (getStance(a, ally) != FactionStance::War)
                setStance(a, ally, FactionStance::War,
                          "Allied with " + factionName(b), simTime, bus);
        }
        // Vassals of A join A's war
        for (auto& vassal : vassalsOf(a)) {
            if (getStance(vassal, b) != FactionStance::War)
                setStance(vassal, b, FactionStance::War,
                          "Vassal of " + factionName(a), simTime, bus);
        }
    }

    void declarePeace(FactionId a, FactionId b,
                      const std::string& reason = "",
                      float simTime = 0.f,
                      float truceDuration = 168.f, // 7 sim days in hours
                      EventBus* bus = nullptr)
    {
        auto& rel       = relations_[key(a,b)];
        rel.stance      = FactionStance::Truce;
        rel.stanceAt    = simTime;
        rel.truceEndsAt = truceDuration > 0.f ? simTime + truceDuration : -1.f;
        rel.reason      = reason;
        history_.push_back({simTime, a, b, FactionStance::Truce, reason});
        if (bus)
            bus->publish(FactionStanceChangedEvent{
                a, b, FactionStance::War, FactionStance::Truce, reason});
    }

    void formAlliance(FactionId a, FactionId b,
                      const std::string& reason = "",
                      float simTime = 0.f,
                      EventBus* bus = nullptr)
    {
        setStance(a, b, FactionStance::Alliance, reason, simTime, bus);
        setRelation(a, b, std::max(getRelation(a,b), 60.f));
    }

    void breakAlliance(FactionId a, FactionId b,
                       const std::string& reason = "",
                       float simTime = 0.f,
                       EventBus* bus = nullptr)
    {
        setStance(a, b, FactionStance::Peace, reason, simTime, bus);
        modifyRelation(a, b, -30.f);
    }

    void formVassal(FactionId vassal, FactionId overlord,
                    const std::string& reason = "",
                    float simTime = 0.f)
    {
        if (auto* f = mut(vassal)) f->overlordId = overlord;
        setStance(vassal, overlord, FactionStance::Vassal, reason, simTime);
    }

    // Advance truce timers — call periodically
    void update(float simTime, EventBus* bus = nullptr) {
        for (auto& [k, rel] : relations_) {
            if (rel.stance == FactionStance::Truce &&
                rel.truceEndsAt > 0.f && simTime >= rel.truceEndsAt) {
                auto [a, b] = unkey(k);
                setStance(a, b, FactionStance::Peace,
                          "Truce expired", simTime, bus);
            }
        }
    }

    // ── Convenience predicates ───────────────────────────────────────

    bool areAllied  (FactionId a, FactionId b) const { return getStance(a,b)==FactionStance::Alliance; }
    bool atWar      (FactionId a, FactionId b) const { return getStance(a,b)==FactionStance::War; }
    bool areHostile (FactionId a, FactionId b) const { return atWar(a,b)||getRelation(a,b)<-50.f; }
    bool areNeutral (FactionId a, FactionId b) const {
        auto s=getStance(a,b); return s==FactionStance::Peace||s==FactionStance::Truce; }

    bool areSameFaction(EntityId a, EntityId b) const {
        auto fa=factionOf(a); return fa!=NO_FACTION && fa==factionOf(b); }
    bool areEntitiesHostile(EntityId a, EntityId b) const {
        auto fa=factionOf(a), fb=factionOf(b);
        return fa!=NO_FACTION && fb!=NO_FACTION && areHostile(fa,fb); }

    // ── Alliance network ─────────────────────────────────────────────

    std::vector<FactionId> alliesOf(FactionId id) const {
        std::vector<FactionId> out;
        for (auto& [k, rel] : relations_) {
            if (rel.stance != FactionStance::Alliance) continue;
            auto [a, b] = unkey(k);
            if (a == id) out.push_back(b);
            else if (b == id) out.push_back(a);
        }
        return out;
    }

    std::vector<FactionId> enemiesOf(FactionId id) const {
        std::vector<FactionId> out;
        for (auto& [k, rel] : relations_) {
            if (rel.stance != FactionStance::War) continue;
            auto [a, b] = unkey(k);
            if (a == id) out.push_back(b);
            else if (b == id) out.push_back(a);
        }
        return out;
    }

    std::vector<FactionId> vassalsOf(FactionId overlord) const {
        std::vector<FactionId> out;
        for (auto& [fid, f] : factions_)
            if (f.overlordId == overlord) out.push_back(fid);
        return out;
    }

    FactionId overlordOf(FactionId vassal) const {
        auto* f = faction(vassal); return f ? f->overlordId : NO_FACTION;
    }
    bool isVassal(FactionId v, FactionId o) const { return overlordOf(v)==o; }

    // Would `defender` come to aid of `attacked` via alliances?
    bool wouldDefend(FactionId defender, FactionId attacked) const {
        if (defender == attacked) return true;
        if (areAllied(defender, attacked)) return true;
        // Transitive: defender allied with someone allied with attacked
        for (auto& mid : alliesOf(defender))
            if (areAllied(mid, attacked)) return true;
        return false;
    }

    // Full coalition resolution
    Coalition resolveCoalition(FactionId attacker, FactionId target) const {
        Coalition c{attacker, target, {attacker}, {target}};

        auto addSide = [&](std::vector<FactionId>& side, FactionId core) {
            for (auto& [fid, f] : factions_) {
                if (fid == attacker || fid == target) continue;
                if (wouldDefend(fid, core) &&
                    std::find(side.begin(), side.end(), fid) == side.end())
                    side.push_back(fid);
            }
        };
        addSide(c.aggressorSide, attacker);
        addSide(c.defenderSide,  target);
        return c;
    }

    // ── History & narrative ──────────────────────────────────────────

    const std::vector<DiplomaticEvent>& history() const { return history_; }

    std::string diplomaticSummary(FactionId a, FactionId b) const {
        std::ostringstream ss;
        ss << factionName(a) << " ↔ " << factionName(b)
           << ": " << stanceName(getStance(a,b))
           << " (value=" << static_cast<int>(getRelation(a,b)) << ")";
        // Recent history
        int shown = 0;
        for (auto it = history_.rbegin(); it != history_.rend() && shown < 3; ++it) {
            if ((it->initiator==a && it->target==b) ||
                (it->initiator==b && it->target==a)) {
                ss << "\n  [t=" << static_cast<int>(it->time) << "] "
                   << factionName(it->initiator) << " → "
                   << stanceName(it->newStance)
                   << (it->reason.empty() ? "" : ": " + it->reason);
                ++shown;
            }
        }
        return ss.str();
    }

    // backward-compat aliases
    float         getRelation(FactionId a, FactionId b, int) const { return getRelation(a,b); }
    bool          areEntitiesHostile(EntityId a, EntityId b, int) const { return areEntitiesHostile(a,b); }
    std::set<EntityId> getFactionMembers(FactionId id) const {
        auto* f = faction(id); return f ? f->members : std::set<EntityId>{}; }

private:
    using Key = uint64_t;

    static Key key(FactionId a, FactionId b) {
        auto lo=std::min(a,b), hi=std::max(a,b);
        return (uint64_t(lo)<<32)|hi;
    }
    static std::pair<FactionId,FactionId> unkey(Key k) {
        return {FactionId(k>>32), FactionId(k&0xFFFFFFFF)};
    }

    Faction* mut(FactionId id) {
        auto it = factions_.find(id); return it!=factions_.end()?&it->second:nullptr; }

    std::string factionName(FactionId id) const {
        auto* f=faction(id); return f?f->name:"Faction#"+std::to_string(id); }

    std::map<FactionId, Faction>    factions_;
    std::map<Key, FactionRelation>  relations_;
    std::map<EntityId, FactionId>   entityFaction_;
    std::vector<DiplomaticEvent>    history_;
};

} // namespace npc
