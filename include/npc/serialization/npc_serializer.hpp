#pragma once
// NPC state serialization — save / load all runtime state to JSON.
// Depends on: serialization/json.hpp  (must be included first via npc.hpp or directly)
//
// Usage:
//   NpcSerializer::save(npc, "save/npc_42.json");
//   NpcSerializer::load(npc, "save/npc_42.json");
//   NpcSerializer::saveWorld(npcs, "save/world.json");
//   auto snapshots = NpcSerializer::loadWorld("save/world.json");
//   NpcSerializer::applySnapshot(npc, snapshots[0]);

#include "json.hpp"
#include "../npc.hpp"

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <filesystem>

namespace npc {

// ═══════════════════════════════════════════════════════════════════════
// NpcSerializer
// (declared as struct so friend declarations in subsystems resolve)
// ═══════════════════════════════════════════════════════════════════════

struct NpcSerializer {
    using J = serial::JsonValue;
    using O = serial::JsonObject;
    using A = serial::JsonArray;

    // ══════════════════════════════════════════════════════════════════
    // PUBLIC API
    // ══════════════════════════════════════════════════════════════════

    // Serialize full NPC state → JsonValue
    static J toJson(const NPC& npc) {
        O root;
        root["version"]     = 1;
        root["id"]          = static_cast<int64_t>(npc.id);
        root["name"]        = npc.name;
        root["type"]        = static_cast<int64_t>(npc.type);
        root["faction"]     = static_cast<int64_t>(npc.factionId);
        root["pos"]         = vec2Json(npc.position);
        root["facing"]      = vec2Json(npc.facing);
        root["moveSpeed"]   = npc.moveSpeed;
        root["isMoving"]    = npc.isMoving;
        root["moveTarget"]  = vec2Json(npc.moveTarget);
        root["personality"] = serializePersonality(npc.personality);
        root["combat"]      = serializeCombat(npc.combat);
        root["emotions"]    = serializeEmotions(npc.emotions);
        root["memory"]      = serializeMemory(npc.memory);
        root["skills"]      = serializeSkills(npc.skills);
        return root;
    }

    // Deserialize into an existing NPC (identity fields overwritten)
    static void fromJson(NPC& npc, const J& j) {
        npc.id        = static_cast<EntityId>(j["id"].asInt());
        npc.name      = j["name"].asString();
        npc.type      = static_cast<NPCType>(j["type"].asInt());
        npc.factionId = static_cast<FactionId>(j["faction"].asInt());
        npc.position  = vec2From(j["pos"]);
        npc.facing    = vec2From(j["facing"]);
        npc.moveSpeed = j["moveSpeed"].asFloat(3.0f);
        npc.isMoving  = j["isMoving"].asBool();
        npc.moveTarget= vec2From(j["moveTarget"]);
        deserializePersonality(npc.personality, j["personality"]);
        deserializeCombat(npc.combat,   j["combat"]);
        deserializeEmotions(npc.emotions, j["emotions"]);
        deserializeMemory(npc.memory,   j["memory"]);
        deserializeSkills(npc.skills,   j["skills"]);
    }

    // ── File helpers ─────────────────────────────────────────────────

    static bool save(const NPC& npc, const std::string& path) {
        ensureDir(path);
        return serial::saveFile(toJson(npc), path);
    }

    static bool load(NPC& npc, const std::string& path) {
        serial::JsonValue j;
        if (!serial::tryLoadFile(path, j)) return false;
        fromJson(npc, j);
        return true;
    }

    // ── World snapshot (multiple NPCs) ───────────────────────────────

    // Snapshot: lightweight per-NPC data bundle for world save/load.
    struct NpcSnapshot {
        EntityId    id;
        std::string name;
        NPCType     type;
        FactionId   factionId;
        J           data;
    };

    static bool saveWorld(const std::vector<std::shared_ptr<NPC>>& npcs,
                          const std::string& path)
    {
        O root;
        root["version"] = 1;
        A arr;
        for (auto& n : npcs) if (n) arr.push_back(toJson(*n));
        root["npcs"] = std::move(arr);
        ensureDir(path);
        return serial::saveFile(root, path);
    }

    static std::vector<NpcSnapshot> loadWorld(const std::string& path) {
        J root;
        if (!serial::tryLoadFile(path, root)) return {};
        std::vector<NpcSnapshot> snaps;
        for (auto& jn : root["npcs"].asArray()) {
            NpcSnapshot s;
            s.id       = static_cast<EntityId>(jn["id"].asInt());
            s.name     = jn["name"].asString();
            s.type     = static_cast<NPCType>(jn["type"].asInt());
            s.factionId= static_cast<FactionId>(jn["faction"].asInt());
            s.data     = jn;
            snaps.push_back(std::move(s));
        }
        return snaps;
    }

    static void applySnapshot(NPC& npc, const NpcSnapshot& snap) {
        fromJson(npc, snap.data);
    }

    // ══════════════════════════════════════════════════════════════════
    // PER-SYSTEM SERIALIZERS
    // ══════════════════════════════════════════════════════════════════

    // ── Vec2 ─────────────────────────────────────────────────────────

    static J vec2Json(const Vec2& v) {
        O o; o["x"] = v.x; o["y"] = v.y; return o;
    }
    static Vec2 vec2From(const J& j) {
        return { j["x"].asFloat(), j["y"].asFloat() };
    }

    // ── PersonalityTraits ────────────────────────────────────────────

    static J serializePersonality(const PersonalityTraits& p) {
        O o;
        o["courage"]      = p.courage;
        o["sociability"]  = p.sociability;
        o["greed"]        = p.greed;
        o["patience"]     = p.patience;
        o["intelligence"] = p.intelligence;
        return o;
    }

    static void deserializePersonality(PersonalityTraits& p, const J& j) {
        p.courage      = j["courage"]     .asFloat(0.5f);
        p.sociability  = j["sociability"] .asFloat(0.5f);
        p.greed        = j["greed"]       .asFloat(0.5f);
        p.patience     = j["patience"]    .asFloat(0.5f);
        p.intelligence = j["intelligence"].asFloat(0.5f);
    }

    // ── CombatSystem ─────────────────────────────────────────────────

    static J serializeCombat(const CombatSystem& cs) {
        O o;
        const auto& s = cs.stats;
        o["health"]     = s.health;
        o["maxHealth"]  = s.maxHealth;
        o["attack"]     = s.attack;
        o["defense"]    = s.defense;
        o["speed"]      = s.speed;
        o["critChance"] = s.critChance;
        o["inCombat"]   = cs.inCombat;
        // Stamina / Mana pools
        O sta; sta["cur"] = s.stamina.current; sta["max"] = s.stamina.max;
        O mna; mna["cur"] = s.mana.current;    mna["max"] = s.mana.max;
        o["stamina"] = std::move(sta);
        o["mana"]    = std::move(mna);
        // Resistances
        O res;
        res["physical"] = s.resistances.physical;
        res["magical"]  = s.resistances.magical;
        res["fire"]     = s.resistances.fire;
        res["ice"]      = s.resistances.ice;
        res["poison"]   = s.resistances.poison;
        o["resistances"] = std::move(res);
        // Abilities (cooldowns only — definitions rebuilt by game)
        A abs;
        for (auto& ab : s.abilities) {
            O a;
            a["name"]    = ab.name;
            a["cooldown"]= ab.currentCooldown;
            abs.push_back(std::move(a));
        }
        o["abilities"] = std::move(abs);
        return o;
    }

    static void deserializeCombat(CombatSystem& cs, const J& j) {
        auto& s = cs.stats;
        s.health     = j["health"]    .asFloat(100.f);
        s.maxHealth  = j["maxHealth"] .asFloat(100.f);
        s.attack     = j["attack"]    .asFloat(15.f);
        s.defense    = j["defense"]   .asFloat(8.f);
        s.speed      = j["speed"]     .asFloat(5.f);
        s.critChance = j["critChance"].asFloat(0.1f);
        cs.inCombat  = j["inCombat"]  .asBool(false);

        s.stamina.current = j["stamina"]["cur"].asFloat(100.f);
        s.stamina.max     = j["stamina"]["max"].asFloat(100.f);
        s.mana.current    = j["mana"]["cur"]   .asFloat(0.f);
        s.mana.max        = j["mana"]["max"]   .asFloat(0.f);

        const auto& res = j["resistances"];
        s.resistances.physical = res["physical"].asFloat(1.f);
        s.resistances.magical  = res["magical"] .asFloat(1.f);
        s.resistances.fire     = res["fire"]    .asFloat(1.f);
        s.resistances.ice      = res["ice"]     .asFloat(1.f);
        s.resistances.poison   = res["poison"]  .asFloat(1.f);

        // Restore ability cooldowns by name (abilities list rebuilt elsewhere)
        for (auto& jab : j["abilities"].asArray()) {
            std::string nm = jab["name"].asString();
            float cd = jab["cooldown"].asFloat(0.f);
            for (auto& ab : s.abilities)
                if (ab.name == nm) { ab.currentCooldown = cd; break; }
        }
    }

    // ── EmotionSystem ────────────────────────────────────────────────
    // (friend access to needs_ and emotions_)

    static J serializeEmotions(const EmotionSystem& es) {
        O o;
        // Needs
        A needArr;
        for (auto& [type, need] : es.needs_) {
            O n;
            n["type"]  = static_cast<int64_t>(type);
            n["value"] = need.value;
            needArr.push_back(std::move(n));
        }
        o["needs"] = std::move(needArr);
        // Active emotions
        A emoArr;
        for (auto& em : es.emotions_) {
            O e;
            e["type"]      = static_cast<int64_t>(em.type);
            e["intensity"] = em.intensity;
            e["duration"]  = em.duration;
            e["elapsed"]   = em.elapsed;
            emoArr.push_back(std::move(e));
        }
        o["emotions"] = std::move(emoArr);
        return o;
    }

    static void deserializeEmotions(EmotionSystem& es, const J& j) {
        for (auto& jn : j["needs"].asArray()) {
            auto type = static_cast<NeedType>(jn["type"].asInt());
            auto it = es.needs_.find(type);
            if (it != es.needs_.end())
                it->second.value = jn["value"].asFloat(80.f);
        }
        es.emotions_.clear();
        for (auto& je : j["emotions"].asArray()) {
            EmotionState em;
            em.type      = static_cast<EmotionType>(je["type"].asInt());
            em.intensity = je["intensity"].asFloat(0.5f);
            em.duration  = je["duration"] .asFloat(1.0f);
            em.elapsed   = je["elapsed"]  .asFloat(0.0f);
            es.emotions_.push_back(em);
        }
    }

    // ── MemorySystem ─────────────────────────────────────────────────
    // (friend access to memories_)

    static J serializeMemory(const MemorySystem& ms) {
        A arr;
        for (auto& m : ms.memories_) {
            O o;
            o["type"]           = static_cast<int64_t>(m.type);
            o["source"]         = static_cast<int64_t>(m.source);
            o["desc"]           = m.description;
            o["impact"]         = m.emotionalImpact;
            o["ts"]             = m.timestamp;
            o["importance"]     = m.importance;
            o["decayRate"]      = m.decayRate;
            o["strength"]       = m.currentStrength;
            o["reliability"]    = m.reliability;
            o["hopCount"]       = static_cast<int64_t>(m.hopCount);
            o["dayCreated"]     = static_cast<int64_t>(m.dayCreated);
            o["hasEntity"]      = m.entityId.has_value();
            o["entityId"]       = static_cast<int64_t>(m.entityId.value_or(0));
            o["hasSrcEntity"]   = m.sourceEntity.has_value();
            o["srcEntityId"]    = static_cast<int64_t>(m.sourceEntity.value_or(0));
            arr.push_back(std::move(o));
        }
        O root;
        root["maxMemories"] = static_cast<int64_t>(ms.maxMemories_);
        root["memories"]    = std::move(arr);
        return root;
    }

    static void deserializeMemory(MemorySystem& ms, const J& j) {
        ms.memories_.clear();
        ms.maxMemories_ = static_cast<size_t>(j["maxMemories"].asInt(100));
        for (auto& jm : j["memories"].asArray()) {
            Memory m;
            m.type              = static_cast<MemoryType>(jm["type"].asInt());
            m.source            = static_cast<MemorySource>(jm["source"].asInt());
            m.description       = jm["desc"].asString();
            m.emotionalImpact   = jm["impact"]     .asFloat();
            m.timestamp         = jm["ts"]         .asFloat();
            m.importance        = jm["importance"] .asFloat(0.5f);
            m.decayRate         = jm["decayRate"]  .asFloat(0.01f);
            m.currentStrength   = jm["strength"]   .asFloat(1.0f);
            m.reliability       = jm["reliability"].asFloat(1.0f);
            m.hopCount          = static_cast<int>(jm["hopCount"].asInt());
            m.dayCreated        = static_cast<int>(jm["dayCreated"].asInt(1));
            if (jm["hasEntity"].asBool())
                m.entityId = static_cast<EntityId>(jm["entityId"].asInt());
            if (jm["hasSrcEntity"].asBool())
                m.sourceEntity = static_cast<EntityId>(jm["srcEntityId"].asInt());
            ms.memories_.push_back(std::move(m));
        }
    }

    // ── SkillSystem ──────────────────────────────────────────────────
    // (friend access to skills_ and unlockedPerks_)

    static J serializeSkills(const SkillSystem& ss) {
        O o;
        o["owner"] = static_cast<int64_t>(ss.owner_);
        // Per-domain progress
        A skills;
        for (auto& [domain, sk] : ss.skills_) {
            O d;
            d["domain"] = static_cast<int64_t>(domain);
            d["level"]  = static_cast<int64_t>(sk.level);
            d["xp"]     = sk.xp;
            skills.push_back(std::move(d));
        }
        o["skills"] = std::move(skills);
        // Unlocked perks
        A perks;
        for (auto& pid : ss.unlockedPerks_)
            perks.push_back(pid);
        o["perks"] = std::move(perks);
        return o;
    }

    static void deserializeSkills(SkillSystem& ss, const J& j) {
        ss.owner_ = static_cast<EntityId>(j["owner"].asInt());
        for (auto& jd : j["skills"].asArray()) {
            auto domain = static_cast<SkillDomain>(jd["domain"].asInt());
            auto it = ss.skills_.find(domain);
            if (it != ss.skills_.end()) {
                it->second.level = static_cast<int>(jd["level"].asInt());
                it->second.xp    = jd["xp"].asFloat();
            }
        }
        ss.unlockedPerks_.clear();
        for (auto& jp : j["perks"].asArray())
            ss.unlockedPerks_.insert(jp.asString());
        ss.rebuildBonuses();
    }

    // ══════════════════════════════════════════════════════════════════
    // DIFF / PATCH  (for incremental network sync or autosave)
    // ══════════════════════════════════════════════════════════════════

    // Returns a minimal JSON patch containing only fields that changed
    // between two full snapshots.  Apply with patchApply().
    static J diff(const J& base, const J& updated) {
        if (!base.isObject() || !updated.isObject()) return updated;
        O patch;
        for (auto& [k, v] : updated.asObject()) {
            if (!base.has(k)) { patch[k] = v; continue; }
            if (jsEqual(base[k], v)) continue;
            if (v.isObject() && base[k].isObject())
                patch[k] = diff(base[k], v);
            else
                patch[k] = v;
        }
        return patch.empty() ? J(nullptr) : J(std::move(patch));
    }

    // Apply a diff patch to a base snapshot.
    static J patch(const J& base, const J& delta) {
        if (delta.isNull()) return base;
        if (!base.isObject() || !delta.isObject()) return delta;
        O result = base.asObject();
        for (auto& [k, v] : delta.asObject()) {
            if (result.count(k) && result[k].isObject() && v.isObject())
                result[k] = patch(result[k], v);
            else
                result[k] = v;
        }
        return J(std::move(result));
    }

private:
    // ── Internal helpers ─────────────────────────────────────────────

    static bool jsEqual(const serial::JsonValue& a, const serial::JsonValue& b) {
        // Quick structural equality for diff (same type + same primitive value)
        if (a.var().index() != b.var().index()) return false;
        if (a.isNull())   return true;
        if (a.isBool())   return a.asBool()   == b.asBool();
        if (a.isInt())    return a.asInt()    == b.asInt();
        if (a.isDouble()) return std::abs(a.asDouble()-b.asDouble()) < 1e-9;
        if (a.isString()) return a.asString() == b.asString();
        // Arrays/objects: not deep-compared in diff (treated as changed)
        return false;
    }

    static void ensureDir(const std::string& filepath) {
        std::filesystem::path p(filepath);
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path());
    }
};

} // namespace npc
