# NPC Behavior System

[![CI](https://github.com/sa-aris/NPC/actions/workflows/ci.yml/badge.svg)](https://github.com/sa-aris/NPC/actions/workflows/ci.yml)
[![Live Demo](https://img.shields.io/badge/demo-GitHub_Pages-brightgreen)](https://sa-aris.github.io/NPC/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)](#integration)
[![WebAssembly](https://img.shields.io/badge/WebAssembly-WASM-654FF0)](https://webassembly.org/)
[![Lua](https://img.shields.io/badge/Lua-5.4-2C2D72?logo=lua)](https://www.lua.org/)

A self-contained NPC AI framework for games, written in C++17. Drop the `include/` directory into any project and you get 20 interconnected systems — from low-level pathfinding and spatial queries to high-level faction politics, relationship history, and narrative-aware dialogue hooks.

No dependencies. No engine lock-in. No runtime overhead you didn't ask for.

![NPC Behavior System demo](demo.gif)

---

## Live Demo

**[→ sa-aris.github.io/NPC](https://sa-aris.github.io/NPC/)** — compiled to WebAssembly via Emscripten, runs entirely in the browser.

A five-NPC village simulation plays out in real time. The demo visualises:
- Emotional state of each NPC with colour-coded contagion spreading between nearby characters
- Memory decay as NPCs gradually forget events (strength bars update live)
- Social influence chains as rumours and beliefs propagate across the social graph hop by hop

Controls: Play/Pause, Reset, Speed ×1–×8.

---

## What it does

A single `NPC` object composes every subsystem automatically. The systems talk to each other through a typed event bus — combat damage triggers emotional responses, emotional state influences trade pricing, daily schedules yield to perceived threats, faction stance changes cascade through alliance chains. Everything is connected, nothing is hardcoded.

```cpp
#include "npc/npc.hpp"
#include "npc/world/world.hpp"

auto guard = std::make_shared<npc::NPC>(1, "Aldric", npc::NPCType::Guard);
guard->personality = npc::PersonalityTraits::brave();
guard->combat.stats.maxHealth = 120.f;
guard->fsm.addState("patrol",  patrolBehavior);
guard->fsm.addState("combat",  combatBehavior);
guard->fsm.addState("wounded", woundedBehavior);
guard->fsm.setInitialState("patrol");

world.addNPC(guard);
guard->subscribeToEvents(world.events());
```

That's the whole setup. The guard will now patrol, react to threats, remember attacks, feel fear when wounded, and gossip about the wolf that bit him with anyone nearby.

---

## Systems

### AI Decision-Making

**Finite State Machine** — State-based behavior with guarded transitions, priority ordering, and per-state blackboard access. States can be nested or composed with behavior trees at the leaf level.

**Behavior Tree** — Full composite/decorator/leaf architecture with a fluent builder API. Includes `ServiceNode` for background polling, `TimeoutDecorator`, `RetryDecorator`, and a `RandomSelectorNode` with per-child weights. The tree exposes a `debugSnapshot()` that dumps the full execution trace with tick counts and last-known status — useful for editor tooling.

```cpp
auto bt = npc::BehaviorTreeBuilder()
    .selector()
        .sequence()
            .condition("enemy_visible", [](auto& bb){ return bb.getOr<bool>("enemy_in_sight", false); })
            .action("attack", attackFn)
        .end()
        .withTimeout(
            npc::BehaviorTreeBuilder().action("search", searchFn).build(),
            8.0f  // give up after 8 seconds
        )
        .action("patrol", patrolFn)
    .end()
    .build();
```

**Utility AI** — Score-based decision-making. Each action defines a set of considerations (linear, sigmoid, exponential, or bell-curve response curves) that are multiplied together to produce a final score. The highest-scoring action wins. Works standalone or as a fallback layer beneath the FSM.

**GOAP** — Goal-Oriented Action Planning. NPCs define world states, goals, and actions with preconditions and effects. The planner finds the cheapest action sequence via A* over the state space. Suitable for complex multi-step behaviors like "get food" → "earn money" → "buy from merchant" → "eat".

### Perception & Memory

**Perception** — Configurable sight cone (angle + range), hearing radius with noise levels, and line-of-sight checking via the pathfinder's Bresenham implementation. Outputs `PerceivedEntity` records with staleness tracking — NPCs remember where they last saw a threat even after it disappears.

**Memory** — Time-stamped episodic memories with emotional impact scores and importance weights. Memories decay based on importance (trivial events fade quickly, traumatic ones persist). Three decay stages emit narrative events: *Fading* (strength < 0.9), *Nearly Forgotten* (< 0.35), and *Forgotten* (0.0). Hearsay memories received through gossip decay four times faster than first-hand observations. Supports gossip propagation: an NPC can receive a memory second-hand with trust-based reliability degradation.

```cpp
// Heard from someone who heard from someone else
memory.receiveGossip(combatMemory, tellerId, tellerTrust, simTime);
// Hearsay decays 4× faster; reliability degrades per hop

// Drain narrative events each frame:
for (auto& evt : npc.memory.drainFadeEvents()) {
    if (evt.stage == MemoryFadeStage::Forgotten)
        fmt::print("{} no longer remembers: {}\n", npc.name, evt.snapshot.description);
}
```

### Emotion & Needs

**Emotion System** — Seven discrete emotion types (happy, sad, angry, fearful, disgusted, surprised, neutral) with intensity and duration. Emotions decay over time and influence downstream systems: fear reduces combat aggression, anger increases it, sadness lowers trade acceptance thresholds.

**Needs** — Sims-style need bars (hunger, thirst, sleep, social, fun, safety, comfort) that deplete over time and drive schedule priorities. An NPC with critically low safety need will refuse to leave a building regardless of their assigned work schedule.

**Emotional Contagion** — NPCs within hearing range share emotional states scaled by personality empathy coefficient and proximity. A frightened NPC running through a market can trigger a cascade of anxiety in nearby villagers. The terminal demo renders contagion live with ANSI-coloured intensity bars and a per-step propagation table.

### Navigation

**Pathfinding** — A* on a uniform grid with configurable node budget, tie-break weighting, and 8-directional movement. The `NavRegions` subsystem runs a flood-fill to precompute connected components, enabling O(1) reachability checks before A* is even attempted. Dynamic obstacles invalidate the LRU path cache automatically.

```
start ──── NavRegions check (O(1)) ──── same region? ──── A* query
                                              │
                                           no │ → return immediately (unreachable)
```

Supports partial paths (closest reachable point when goal is blocked), Catmull-Rom spline smoothing, and a `WaypointGraph` for sparse navigation over large open worlds. Path requests can be queued at three priority levels and processed in budget-limited batches per frame.

**Steering** — Separate steering layer for smooth movement: seek, flee, wander, arrival, obstacle avoidance, separation, cohesion, alignment. Output is a steering force that callers blend with their own movement logic.

### Social & Faction

**Faction System** — Multi-faction diplomacy with six stance types: Peace, Alliance, War, Trade, Vassal, and Truce. War declarations cascade through alliance chains — allies auto-join, vassals follow their overlord. Truces carry expiry timers and transition back to Peace automatically.

```cpp
// Kingdom declares war on Empire — Alliance members and Vassals auto-join
factions.declareWar(KINGDOM, EMPIRE, "border dispute", simTime, /*cascade=*/true);

// Full coalition resolution: who's on each side?
auto coalition = factions.resolveCoalition(KINGDOM, EMPIRE);
// coalition.aggressorSide = { KINGDOM, DUCHY, CITY_STATE }
// coalition.defenderSide  = { EMPIRE, PROTECTORATE }
```

**Relationship System** — Directed relationship graph where every interaction is recorded as a typed event (`Saved`, `Betrayed`, `Attacked`, `Gifted`, `Lied`, …). Each event carries a delta, magnitude, timestamp, and location. Values decay toward neutral over time; a separate trust channel degrades on betrayal and is harder to rebuild.

The key feature: NPCs can recall specific events and reason about them in dialogue.

```cpp
// Hero saved merchant 24 sim-hours ago
rs.recordEvent("hero", "merchant", RelationshipEventType::Saved, simTime);

// Later, merchant greets the player:
auto recall = rs.recallSentence("merchant", "hero", RelationshipEventType::Saved, now);
// → "hero saved merchant [1 day ago]"

// Full narrative:
rs.narrative("merchant", "hero", now);
// → "merchant feels Close Friend toward hero [72/100, High Trust].
//    Notably, hero saved merchant (1 day ago).
//    Recent interactions: hero gifted merchant (3h ago); hero helped merchant (5h ago)."
```

**Social Influence Chains** — Beliefs and rumours propagate organically through the social graph. An `InfluenceMessage` starts with an originator, a topic, a charge (−1 to +1), and a reliability of 1.0. Each hop degrades reliability by the receiver's empathy coefficient; charges mutate as the message passes through each personality. Below reliability 0.30 the content distorts. The system records the full chain (`"Alaric ⟶ Brina ⟶ Dagna ⟶ Gareth"`) and emits a hop record for every transfer — useful for debugging propaganda spread or building in-game rumour mechanics.

```cpp
g_influence.seed({msgId, "wolves at the gate", originatorId, "Alaric",
                  /*charge=*/-0.85f, /*reliability=*/1.0f, simTime});

// Each frame, pairs within social range probabilistically propagate:
// reliability *= receiver.personality.empathyMultiplier()
// charge      *= receiver.personality.empathyMultiplier()
// if reliability < 0.30 → charge gets random distortion
```

**Group Behavior** — Formation system (line, wedge, circle, column) with slot assignment, leader-follower command propagation, and tactical roles (Leader, Vanguard, Flanker, Support, Archer). Group morale aggregates individual emotional states and feeds back into member behavior.

### World Infrastructure

**Event Bus** — Typed publish-subscribe with priority ordering, delayed events (min-heap scheduler), event chains (A → B transforms), filter predicates, RAII subscription lifetime, and a circular history buffer. Everything in the system communicates through this bus.

**Shared Blackboard** — World-level key-value store with TTL expiry, per-key version counters, and prefix-scoped watcher callbacks. The `WorldBlackboard` layer provides typed accessors for standard namespaces (`world/*`, `market/*`, `faction/*`, `combat/*`, `event/*`). A `BlackboardSync` bridge lets NPCs pull relevant world state into their local blackboard each frame.

**Spatial Index** — Two-layer spatial query system. `SpatialGrid` is a flat hash-grid providing O(1) insert/update/remove and O(k) radius queries. `QuadTree` handles non-uniform distributions with adaptive subdivision. The `SpatialIndex` facade exposes unified queries: `nearby`, `nearestN`, `closestExcept`, `inRect`, `findClusters` (BFS-based).

**LOD System** — Three-tier level-of-detail scheduler (Active / Background / Dormant) with hysteresis to prevent tier flickering, importance scoring for quest NPCs and bosses, velocity prediction for early promotion of approaching entities, group-based tier elevation, and per-frame CPU budget tracking. Background and Dormant NPCs accumulate delta-time between ticks so physics-independent simulation stays accurate.

**Simulation Manager** — Orchestrates the full update pipeline each frame in the correct order: event bus drain → time system → weather → world events → LOD classification → AI ticks → spatial index sync → autosave. Handles NPC spawn/despawn with automatic event subscription cleanup.

**Serialization** — Zero-dependency JSON parser/writer with full spec support and a friend-accessor-based NPC serializer. Saves full NPC state including personality, combat stats, emotion intensities, skill levels, and memory content. Supports incremental diffs for bandwidth-efficient sync.

### Threading

**Thread-Safety Layer** — Optional thread-safe wrappers for the event bus, spatial index, and shared blackboard using `std::shared_mutex` for concurrent reads. `TaskScheduler` is a priority-aware thread pool exposing `submitAsync<T>() → std::future<T>`. `ParallelNPCTicker` distributes Background and Dormant ticks across worker threads while Active ticks remain on the main thread for safe world access.

### Other

**Combat** — Threat assessment and target selection, ability system with cooldowns, stamina/mana resource pools, damage type resistances, and automatic flee/heal decision thresholds driven by the personality system.

**Trade** — Supply/demand pricing with scarcity multipliers, personality-based markup/markdown, buy/sell/barter transactions, and relationship-based discount application.

**Schedule** — Time-of-day activity planner. Each NPC has a weekly template with named activities and location targets. High-priority needs and external events (combat, severe weather) can override scheduled activities.

**Dialogue** — Branching dialogue trees with condition evaluation, reputation-based text variants, and side-effect callbacks. Dialogue outcomes publish events to the bus, which other NPCs can observe.

**Quest** — Quest definition, assignment, progress tracking, condition evaluation, and completion/failure events with full EventBus integration.

**Skills** — Six skill domains (Combat, Trade, Farming, Crafting, Social, Leadership) with XP gain, level thresholds, perk unlocks, and bonus application to dependent subsystems. Skill XP is awarded automatically by subscribing to the event bus.

---

## Architecture

```
                          ┌──────────────────────────────────────────┐
                          │              GameWorld                   │
                          │   TimeSystem  WeatherSystem  EventBus    │
                          │   SpatialIndex  SharedBlackboard         │
                          └──────────────┬───────────────────────────┘
                                         │ SimulationManager
                          ┌──────────────▼───────────────────────────┐
                          │                LOD System                │
                          │  Active (full tick) │ Background │ Dormant│
                          └──────┬─────────────┴──────┬─────────────┘
                                 │                    │ (parallel workers)
                    ┌────────────▼──────────┐         │
                    │          NPC          │         │
                    ├───────────────────────┤         │
                    │  FSM ←→ BehaviorTree  │         │
                    │  UtilityAI   GOAP     │         │
                    │  Blackboard           │         │
                    ├───────────────────────┤         │
                    │  Perception  Memory   │         │
                    │  Emotion     Needs    │         │
                    │  Personality          │         │
                    ├───────────────────────┤         │
                    │  Combat  Trade        │         │
                    │  Dialog  Quest        │         │
                    │  Schedule  Skills     │         │
                    ├───────────────────────┤         │
                    │  Pathfinder  Steering │         │
                    └────────────┬──────────┘         │
                                 │                    │
                          ┌──────▼────────────────────▼──────────────┐
                          │    Typed EventBus (pub-sub, priority,    │
                          │    delayed dispatch, chain transforms)   │
                          └──────────────────────────────────────────┘
```

Every NPC is a composition of systems. No inheritance hierarchy, no virtual dispatch in the hot path. The NPC class itself is a plain aggregate with a main `update()` method — all behavior logic lives in the subsystems.

---

## Building

```bash
git clone https://github.com/sa-aris/NPC.git
cd NPC
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

**Requirements:** C++17 (GCC 9+, Clang 10+, MSVC 19.20+), CMake 3.16+

### Lua scripting bridge

Requires Lua 5.4. On Ubuntu: `sudo apt install liblua5.4-dev`

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNPC_LUA_BRIDGE=ON
cmake --build build --target lua_village
./build/lua_village examples/scripts
```

NPC behaviours are defined entirely in Lua — no C++ FSM lambdas required:

```lua
-- examples/scripts/guard.lua
function guard_enter_combat(npc)
    npc:log("Enemy spotted! Drawing sword!")
    npc:addEmotion("Angry", 0.9, 4.0)
    npc:depletNeed("Safety", 30.0)
    npc:rememberEvent("Entered combat", -0.6)
end

function guard_update_combat(npc, dt)
    if npc:getHealthPercent() <= 0.25 then
        npc:setState("flee")
    end
end
```

```cpp
// C++ side — one call wires the Lua function into the FSM
bridge.addLuaState(guard->fsm, "combat", guard.get(),
                   "guard_update_combat",
                   "guard_enter_combat",
                   "guard_exit_combat");
```

The bridge exposes a full NPC API to Lua: position, health, emotions, needs, blackboard keys, FSM transitions, memory, movement, and `world_time()` / `world_hour()` globals. If Lua 5.4 is not installed, the bridge target is silently skipped and the rest of the build is unaffected.

### Run the demo

```bash
./build/village_sim
```

Simulates a medieval village with six NPCs over one full day. Guards patrol, merchants trade, the blacksmith works her forge, and a wolf pack attacks at dusk — triggering combat, emotional responses, and shared fear. The simulation renders:

- **Emotion contagion table** — ANSI-coloured intensity bars updated each in-game hour, showing which NPCs are spreading their emotional state to nearby characters
- **Memory decay narrative** — stage-by-stage output as memories fade (`[FADING]`, `[NEARLY FORGOTTEN]`, `[FORGOTTEN]`) with end-of-day memory strength bars per NPC
- **Influence chain log** — hop-by-hop propagation of three seeded rumours (strange tracks, wolf attack, heroic defence), printed in full with reliability and charge at each step

### Run the tests

```bash
./build/run_tests         # compact output
./build/run_tests -v      # verbose with per-test results
ctest --test-dir build    # via CTest
```

---

## Integration

The entire framework is header-only. Copy `include/npc/` into your project and add the directory to your include path:

```cmake
target_include_directories(your_target PRIVATE path/to/NPC/include)
```

The `src/` directory contains thin `.cpp` translation units that prevent symbol duplication in multi-TU builds. If you're dropping the headers directly into a single-TU project, these are optional.

### Minimal example

```cpp
#include "npc/npc.hpp"
#include "npc/world/world.hpp"

int main() {
    npc::GameWorld world(64, 64);

    auto npc = std::make_shared<npc::NPC>(1, "Mira", npc::NPCType::Merchant);
    npc->position = {32.f, 32.f};

    // Give her something to do
    npc->fsm.addState("idle", [](npc::NPC& n, float dt, npc::GameWorld&){
        n.emotions.update(dt);
        n.memory.update(dt);
    });
    npc->fsm.setInitialState("idle");

    world.addNPC(npc);
    npc->subscribeToEvents(world.events());

    // Tick
    for (int i = 0; i < 100; ++i)
        world.update(0.016f);   // 60 Hz

    return 0;
}
```

### Using the Faction and Relationship systems

These are world-level systems, not per-NPC. Construct them once and pass references where needed:

```cpp
npc::FactionSystem  factions;
npc::RelationshipSystem rs;

factions.addFaction(1u, "Merchants Guild");
factions.addFaction(2u, "Thieves Guild");
factions.addMember(1u, merchant->id);

factions.declareWar(1u, 2u, "stolen shipment", simTime);

// Record that the thief attacked the merchant
rs.recordEvent("thief_01", "merchant_05",
               npc::RelationshipEventType::Attacked, simTime);

// Later — merchant remembers
auto sentence = rs.recallSentence("merchant_05", "thief_01",
                                   npc::RelationshipEventType::Attacked, now);
// → "thief_01 attacked merchant_05 [2 days ago]"
```

### Spatial queries

```cpp
npc::SpatialIndex spatial(10.f);  // 10-unit cell size

// Sync positions
for (auto& npc : world.npcs())
    spatial.insert(npc->id, npc->position);

// Query
auto threats = spatial.nearby(guardPos, 30.f);
auto nearest = spatial.closest(npc->position);
auto clusters = spatial.findClusters(5.f);  // BFS grouping
```

### LOD-aware simulation

```cpp
npc::LODSystem lod;
lod.setConfig({
    .activeRadius     = 60.f,
    .backgroundRadius = 200.f,
    .minDwellSecs     = 1.5f,
});
lod.setPlayerPosition(playerPos);

// Mark quest NPCs as important — larger effective active radius
lod.setImportance(questNPCId, 0.8f);
lod.pin(bossId, npc::LODTier::Active);  // always fully ticked

lod.update(worldNPCs, simTime, dt);

for (auto id : lod.toTickThisFrame(npc::LODTier::Active))
    npcs[id]->update(dt, world);

for (auto id : lod.toTickThisFrame(npc::LODTier::Background)) {
    float accum = lod.consumeAccumDt(id);
    npcs[id]->emotions.update(accum);
    npcs[id]->memory.update(accum);
}
// Dormant NPCs: emotions only, once every ~20 frames
```

---

## Performance

All figures on a single core, Intel i7-12700K, `-O2`:

| Scenario | NPCs | Frame time |
|----------|------|-----------|
| Full A* pathfind (10×10 grid) | — | ~0.08 ms |
| Full A* pathfind (64×64 grid) | — | ~0.6 ms |
| Radius query, SpatialGrid | 10 000 entities | ~0.02 ms |
| Active NPC full tick | 100 | ~1.4 ms |
| Background tick (emotion + movement) | 500 | ~0.9 ms |
| Dormant tick (emotion only) | 2000 | ~0.3 ms |

The LOD system is designed so that a world with 2000 NPCs consumes roughly the same CPU budget as one with 100, assuming typical player movement patterns.

---

## Project layout

```
NPC/
├── include/npc/
│   ├── core/           types, vec2, random
│   ├── event/          event_system — typed pub-sub bus
│   ├── ai/             fsm, behavior_tree, utility_ai, goap, blackboard, shared_blackboard
│   ├── perception/     sight cone, hearing, line-of-sight
│   ├── memory/         episodic memory, decay stages, gossip
│   ├── emotion/        emotion state, needs, contagion
│   ├── personality/    trait system, multipliers
│   ├── combat/         threat model, abilities, resources
│   ├── dialog/         branching trees, reputation variants
│   ├── trade/          dynamic pricing, transactions
│   ├── schedule/       daily routines, time-of-day planner
│   ├── quest/          definition, tracking, events
│   ├── skill/          XP, levels, perks, bonuses
│   ├── navigation/     A*, NavRegions, PathCache, WaypointGraph, steering
│   ├── social/         faction_system, relationship_system, group_behavior,
│   │                   influence_chain — rumour propagation, hop recording
│   ├── world/          world, time, weather, spatial_index, lod_system,
│   │                   simulation_manager, world_event_manager
│   ├── threading/      thread_safety, task_scheduler, parallel_ticker
│   ├── serialization/  json, npc_serializer
│   ├── scripting/      lua_bridge — Lua 5.4 scripting bridge
│   └── npc.hpp         main NPC composite class
├── src/
│   ├── npc.cpp
│   ├── world/world.cpp
│   └── wasm_api.cpp    — Emscripten C exports (npc_init / npc_step / npc_is_complete)
├── web/
│   └── index.html      — single-file browser demo (dark terminal theme, WebAssembly)
├── examples/
│   ├── village_sim.cpp        — full medieval village demo with contagion/decay/influence output
│   ├── lua_village.cpp        — Lua scripting demo (zero C++ FSM lambdas)
│   └── scripts/
│       ├── guard.lua           — guard FSM: patrol/alert/combat/flee/recover
│       └── merchant.lua        — merchant schedule: open/lunch/closed/worried
├── tests/
│   ├── test_runner.hpp — zero-dependency test framework
│   └── run_tests.cpp   — test suite (~75 tests)
└── .github/workflows/
    ├── ci.yml          — GCC 12 + Clang 15 + macOS matrix
    └── pages.yml       — Emscripten WASM build + GitHub Pages deploy
```

---

## License

[MIT](LICENSE)
