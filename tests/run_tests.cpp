// run_tests.cpp — comprehensive test suite for NPC behavior system
// Compile: g++ -std=c++17 -I../include -O2 -o run_tests run_tests.cpp
// Run:     ./run_tests       (dot output)
//          ./run_tests -v    (verbose)

#include "test_runner.hpp"

#include "npc/social/faction_system.hpp"
#include "npc/social/relationship_system.hpp"
#include "npc/world/spatial_index.hpp"
#include "npc/navigation/pathfinding.hpp"
#include "npc/ai/shared_blackboard.hpp"
#include "npc/world/lod_system.hpp"    // also pulls in npc.hpp → NPC class
#include "npc/serialization/json.hpp"
#include "npc/event/event_system.hpp"
#include "npc/ai/blackboard.hpp"

#include <vector>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// JSON Parser / Writer
// ─────────────────────────────────────────────────────────────────────────────

TEST("JSON: parse null") {
    auto v = npc::json::parse("null");
    ASSERT_TRUE(std::holds_alternative<std::nullptr_t>(v));
}

TEST("JSON: parse bool true") {
    auto v = npc::json::parse("true");
    ASSERT_EQ(std::get<bool>(v), true);
}

TEST("JSON: parse bool false") {
    auto v = npc::json::parse("false");
    ASSERT_EQ(std::get<bool>(v), false);
}

TEST("JSON: parse integer") {
    auto v = npc::json::parse("42");
    ASSERT_EQ(std::get<int64_t>(v), int64_t(42));
}

TEST("JSON: parse negative integer") {
    auto v = npc::json::parse("-17");
    ASSERT_EQ(std::get<int64_t>(v), int64_t(-17));
}

TEST("JSON: parse double") {
    auto v = npc::json::parse("3.14");
    ASSERT_NEAR(std::get<double>(v), 3.14, 1e-9);
}

TEST("JSON: parse string") {
    auto v = npc::json::parse("\"hello world\"");
    ASSERT_EQ(std::get<std::string>(v), "hello world");
}

TEST("JSON: parse empty array") {
    auto v = npc::json::parse("[]");
    ASSERT_TRUE(std::holds_alternative<npc::json::JsonArray>(v));
    ASSERT_EQ(std::get<npc::json::JsonArray>(v).size(), std::size_t(0));
}

TEST("JSON: parse array of ints") {
    auto v = npc::json::parse("[1,2,3]");
    auto& arr = std::get<npc::json::JsonArray>(v);
    ASSERT_EQ(arr.size(), std::size_t(3));
    ASSERT_EQ(std::get<int64_t>(arr[0]), int64_t(1));
    ASSERT_EQ(std::get<int64_t>(arr[2]), int64_t(3));
}

TEST("JSON: parse empty object") {
    auto v = npc::json::parse("{}");
    ASSERT_TRUE(std::holds_alternative<npc::json::JsonObject>(v));
}

TEST("JSON: parse nested object") {
    auto v = npc::json::parse("{\"name\":\"Aria\",\"level\":5}");
    auto& obj = std::get<npc::json::JsonObject>(v);
    ASSERT_EQ(std::get<std::string>(obj.at("name")), "Aria");
    ASSERT_EQ(std::get<int64_t>(obj.at("level")), int64_t(5));
}

TEST("JSON: roundtrip") {
    std::string original = "{\"a\":1,\"b\":[2,3],\"c\":null}";
    auto v  = npc::json::parse(original);
    auto v2 = npc::json::parse(npc::json::toString(v));
    ASSERT_EQ(npc::json::toString(v), npc::json::toString(v2));
}

TEST("JSON: toString pretty smoke") {
    auto v = npc::json::parse("[1,[2,3],{\"x\":true}]");
    std::string pretty = npc::json::toString(v, true);
    ASSERT_FALSE(pretty.empty());
    ASSERT_TRUE(pretty.find('\n') != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Blackboard
// ─────────────────────────────────────────────────────────────────────────────

TEST("Blackboard: set and getOr typed") {
    npc::Blackboard bb;
    bb.set<int>("health", 100);
    ASSERT_EQ(bb.getOr<int>("health", -1), 100);
}

TEST("Blackboard: getOr missing returns default") {
    npc::Blackboard bb;
    ASSERT_EQ(bb.getOr<int>("missing", 0), 0);
}

TEST("Blackboard: get optional returns nullopt for missing") {
    npc::Blackboard bb;
    ASSERT_FALSE(bb.get<int>("missing").has_value());
}

TEST("Blackboard: overwrite value") {
    npc::Blackboard bb;
    bb.set<float>("x", 1.0f);
    bb.set<float>("x", 2.0f);
    ASSERT_NEAR(bb.getOr<float>("x", 0.f), 2.0f, 1e-6f);
}

TEST("Blackboard: has() works") {
    npc::Blackboard bb;
    ASSERT_FALSE(bb.has("key"));
    bb.set<std::string>("key", "val");
    ASSERT_TRUE(bb.has("key"));
}

TEST("Blackboard: remove()") {
    npc::Blackboard bb;
    bb.set<int>("n", 42);
    bb.remove("n");
    ASSERT_FALSE(bb.has("n"));
}

TEST("Blackboard: snapshot and restore") {
    npc::Blackboard bb;
    bb.set<int>("a", 1);
    bb.set<int>("b", 2);
    auto snap = bb.snapshot();
    bb.set<int>("a", 99);
    bb.restore(snap);
    ASSERT_EQ(bb.getOr<int>("a", -1), 1);
}

TEST("Blackboard: keys() returns all keys") {
    npc::Blackboard bb;
    bb.set<int>("x", 1);
    bb.set<int>("y", 2);
    auto ks = bb.keys();
    ASSERT_EQ(ks.size(), std::size_t(2));
}

// ─────────────────────────────────────────────────────────────────────────────
// SharedBlackboard
// ─────────────────────────────────────────────────────────────────────────────

TEST("SharedBlackboard: set and get") {
    npc::SharedBlackboard sbb;
    sbb.set<int>("counter", 5, 0.0);
    ASSERT_EQ(sbb.get<int>("counter"), 5);
}

TEST("SharedBlackboard: TTL expiry") {
    npc::SharedBlackboard sbb;
    sbb.set<int>("temp", 99, 0.0, 1.0);
    ASSERT_EQ(sbb.get<int>("temp"), 99);
    sbb.pruneExpired(2.0);
    ASSERT_FALSE(sbb.has("temp"));
}

TEST("SharedBlackboard: setIfAbsent does not overwrite") {
    npc::SharedBlackboard sbb;
    sbb.set<int>("x", 10, 0.0);
    sbb.setIfAbsent<int>("x", 20, 0.0);
    ASSERT_EQ(sbb.get<int>("x"), 10);
}

TEST("SharedBlackboard: versioning increments") {
    npc::SharedBlackboard sbb;
    sbb.set<int>("v", 1, 0.0);
    uint64_t v1 = sbb.version("v");
    sbb.set<int>("v", 2, 0.0);
    uint64_t v2 = sbb.version("v");
    ASSERT_GT(v2, v1);
}

TEST("SharedBlackboard: watcher fires on set") {
    npc::SharedBlackboard sbb;
    int fired = 0;
    auto id = sbb.watch("test/", [&](const std::string&, const npc::BBEntry&){
        ++fired;
    });
    sbb.set<int>("test/value", 42, 0.0);
    ASSERT_EQ(fired, 1);
    sbb.unwatch(id);
}

// ─────────────────────────────────────────────────────────────────────────────
// EventBus  (callback takes const EventT&, not EventRecord)
// ─────────────────────────────────────────────────────────────────────────────

struct TestEvent { int value; };
struct OtherEvent { std::string msg; };

TEST("EventBus: basic subscribe and publish") {
    npc::EventBus bus;
    int received = 0;
    bus.subscribe<TestEvent>([&](const TestEvent& e){ received = e.value; });
    bus.publish(TestEvent{42});
    ASSERT_EQ(received, 42);
}

TEST("EventBus: multiple subscribers") {
    npc::EventBus bus;
    int count = 0;
    bus.subscribe<TestEvent>([&](const TestEvent&){ ++count; });
    bus.subscribe<TestEvent>([&](const TestEvent&){ ++count; });
    bus.publish(TestEvent{1});
    ASSERT_EQ(count, 2);
}

TEST("EventBus: different event types don't cross") {
    npc::EventBus bus;
    int testCount = 0, otherCount = 0;
    bus.subscribe<TestEvent>([&](const TestEvent&){ ++testCount; });
    bus.subscribe<OtherEvent>([&](const OtherEvent&){ ++otherCount; });
    bus.publish(TestEvent{1});
    ASSERT_EQ(testCount, 1);
    ASSERT_EQ(otherCount, 0);
}

TEST("EventBus: ScopedSubscription auto-unsubscribes") {
    npc::EventBus bus;
    int count = 0;
    {
        auto sub = bus.subscribeScoped<TestEvent>([&](const TestEvent&){ ++count; });
        bus.publish(TestEvent{1});
        ASSERT_EQ(count, 1);
    }
    bus.publish(TestEvent{2});
    ASSERT_EQ(count, 1);
}

TEST("EventBus: delayed event fires after update") {
    npc::EventBus bus;
    int received = 0;
    bus.subscribe<TestEvent>([&](const TestEvent& e){ received = e.value; });
    // Set bus time to 5.0, then schedule with 1s delay (fires at t=6)
    bus.update(5.0f);
    bus.publishDelayed(TestEvent{99}, 1.0f);
    bus.update(5.5f); // before deadline
    ASSERT_EQ(received, 0);
    bus.update(6.5f); // past deadline
    ASSERT_EQ(received, 99);
}

TEST("EventBus: delayed event does not fire before time") {
    npc::EventBus bus;
    int received = 0;
    bus.subscribe<TestEvent>([&](const TestEvent& e){ received = e.value; });
    bus.publishDelayed(TestEvent{77}, 10.0f); // fires at t=10
    bus.update(5.0f);
    ASSERT_EQ(received, 0);
}

TEST("EventBus: history records events") {
    npc::EventBus bus;
    bus.publish(TestEvent{1});
    bus.publish(TestEvent{2});
    auto hist = bus.getHistory<TestEvent>();
    ASSERT_GE(hist.size(), std::size_t(2));
}

TEST("EventBus: priority ordering") {
    npc::EventBus bus;
    std::vector<int> order;
    bus.subscribe<TestEvent>([&](const TestEvent&){ order.push_back(2); },
                             npc::EventPriority::Normal);
    bus.subscribe<TestEvent>([&](const TestEvent&){ order.push_back(1); },
                             npc::EventPriority::High);
    bus.subscribe<TestEvent>([&](const TestEvent&){ order.push_back(3); },
                             npc::EventPriority::Low);
    bus.publish(TestEvent{0});
    ASSERT_EQ(order.size(), std::size_t(3));
    ASSERT_EQ(order[0], 1);
    ASSERT_EQ(order[1], 2);
    ASSERT_EQ(order[2], 3);
}

TEST("EventBus: lastEvent returns most recent") {
    npc::EventBus bus;
    bus.publish(TestEvent{1});
    bus.publish(TestEvent{2});
    bus.publish(TestEvent{3});
    auto* last = bus.lastEvent<TestEvent>();
    ASSERT_TRUE(last != nullptr);
    ASSERT_EQ(last->value, 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// SpatialGrid / SpatialIndex
// ─────────────────────────────────────────────────────────────────────────────

TEST("SpatialGrid: insert and queryRadius") {
    npc::SpatialGrid grid(10.0f);
    grid.insert(1, {0.f, 0.f});
    grid.insert(2, {5.f, 0.f});
    grid.insert(3, {50.f, 50.f});
    auto hits = grid.queryRadius({0.f, 0.f}, 8.0f);
    ASSERT_EQ(hits.size(), std::size_t(2));
}

TEST("SpatialGrid: update moves entity") {
    npc::SpatialGrid grid(10.0f);
    grid.insert(1, {0.f, 0.f});
    grid.update(1, {100.f, 100.f});
    ASSERT_EQ(grid.queryRadius({0.f, 0.f}, 5.0f).size(), std::size_t(0));
    ASSERT_EQ(grid.queryRadius({100.f, 100.f}, 5.0f).size(), std::size_t(1));
}

TEST("SpatialGrid: remove clears entity") {
    npc::SpatialGrid grid(10.0f);
    grid.insert(1, {0.f, 0.f});
    grid.remove(1);
    ASSERT_EQ(grid.queryRadius({0.f, 0.f}, 5.0f).size(), std::size_t(0));
}

TEST("SpatialGrid: nearest returns closest") {
    npc::SpatialGrid grid(10.0f);
    grid.insert(1, {0.f, 0.f});
    grid.insert(2, {3.f, 0.f});
    grid.insert(3, {10.f, 0.f});
    auto hits = grid.nearest({0.f, 0.f}, 1, 20.0f);
    ASSERT_EQ(hits.size(), std::size_t(1));
    ASSERT_EQ(hits[0].id, 1u);
}

TEST("SpatialIndex: nearby excludes self") {
    npc::SpatialIndex si(10.0f);
    si.insert(1, {0.f, 0.f});
    si.insert(2, {1.f, 0.f});
    auto hits = si.nearbyExcept(1u, {0.f, 0.f}, 5.0f);
    ASSERT_EQ(hits.size(), std::size_t(1));
    ASSERT_EQ(hits[0].id, 2u);
}

TEST("SpatialIndex: findClusters groups close entities") {
    npc::SpatialIndex si(5.0f);
    si.insert(1, {0.f,   0.f});
    si.insert(2, {1.f,   0.f});
    si.insert(3, {0.f,   1.f});
    si.insert(4, {100.f, 100.f});
    si.insert(5, {101.f, 100.f});
    auto clusters = si.findClusters(5.0f);
    ASSERT_GE(clusters.size(), std::size_t(2));
}

// ─────────────────────────────────────────────────────────────────────────────
// Pathfinding
// ─────────────────────────────────────────────────────────────────────────────

TEST("Pathfinding: simple straight path") {
    npc::Pathfinder pf;
    int W = 10, H = 10;
    std::vector<bool> walkable(W*H, true);
    pf.init(W, H, [&](int x, int y){ return walkable[y*W+x]; });
    pf.buildRegions();
    auto result = pf.query({0.f,0.f}, {5.f,0.f});
    ASSERT_TRUE(result.complete);
    ASSERT_FALSE(result.waypoints.empty());
}

TEST("Pathfinding: NavRegions wall splits map") {
    npc::NavRegions nr;
    int W = 6, H = 6;
    std::vector<bool> walkable(W*H, true);
    for (int y = 0; y < H; ++y) walkable[y*W+3] = false;
    nr.rebuild(W, H, [&](int x, int y){ return walkable[y*W+x]; });
    ASSERT_FALSE(nr.isReachable({0,0}, {5,0}));
    ASSERT_TRUE(nr.isReachable({0,0}, {2,0}));
}

TEST("Pathfinding: PathCache stores and retrieves") {
    npc::PathCache cache;
    npc::PathResult r;
    r.complete   = true;
    r.waypoints  = {{0.f,0.f},{1.f,0.f},{2.f,0.f}};
    r.cost       = 2.0f;
    cache.put(1, 2, r);
    auto* got = cache.get(1, 2);
    ASSERT_TRUE(got != nullptr);
    ASSERT_EQ(got->waypoints.size(), std::size_t(3));
}

TEST("Pathfinding: PathCache miss returns nullptr") {
    npc::PathCache cache;
    ASSERT_TRUE(cache.get(99, 100) == nullptr);
}

TEST("Pathfinding: second query uses cache") {
    npc::Pathfinder pf;
    int W = 10, H = 10;
    std::vector<bool> walkable(W*H, true);
    pf.init(W, H, [&](int x, int y){ return walkable[y*W+x]; });
    pf.buildRegions();
    auto r1 = pf.query({0.f,0.f}, {9.f,0.f});
    ASSERT_FALSE(r1.fromCache);
    auto r2 = pf.query({0.f,0.f}, {9.f,0.f});
    ASSERT_TRUE(r2.fromCache);
}

TEST("Pathfinding: obstacle invalidates cache") {
    npc::Pathfinder pf;
    int W = 10, H = 10;
    std::vector<bool> walkable(W*H, true);
    pf.init(W, H, [&](int x, int y){ return walkable[y*W+x]; });
    pf.buildRegions();
    pf.query({0.f,0.f}, {9.f,0.f});
    pf.addObstacle({5,0});
    auto r3 = pf.query({0.f,0.f}, {9.f,0.f});
    ASSERT_FALSE(r3.fromCache);
}

// ─────────────────────────────────────────────────────────────────────────────
// FactionSystem  (FactionId = uint32_t)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr npc::FactionId REBELS  = 1;
static constexpr npc::FactionId EMPIRE  = 2;
static constexpr npc::FactionId KNIGHTS = 3;
static constexpr npc::FactionId BANDITS = 4;
static constexpr npc::FactionId ALLY_B  = 5;
static constexpr npc::FactionId LORD    = 6;
static constexpr npc::FactionId VASSAL  = 7;
static constexpr npc::FactionId ENEMY   = 8;
static constexpr npc::FactionId ATCK    = 9;
static constexpr npc::FactionId TGT     = 10;
static constexpr npc::FactionId TGT_ALY = 11;
static constexpr npc::FactionId GUILD   = 12;

TEST("FactionSystem: create and query factions") {
    npc::FactionSystem fs;
    fs.addFaction(REBELS, "Rebel Alliance");
    fs.addFaction(EMPIRE, "Galactic Empire");
    ASSERT_TRUE(fs.faction(REBELS) != nullptr);
    ASSERT_TRUE(fs.faction(EMPIRE) != nullptr);
    ASSERT_EQ(fs.faction(REBELS)->name, "Rebel Alliance");
}

TEST("FactionSystem: default stance is Peace") {
    npc::FactionSystem fs;
    fs.addFaction(1, "A"); fs.addFaction(2, "B");
    ASSERT_EQ(fs.getStance(1, 2), npc::FactionStance::Peace);
}

TEST("FactionSystem: declareWar sets stance") {
    npc::FactionSystem fs;
    fs.addFaction(1, "A"); fs.addFaction(2, "B");
    fs.declareWar(1, 2, "resources", 0.f);
    ASSERT_EQ(fs.getStance(1, 2), npc::FactionStance::War);
    ASSERT_EQ(fs.getStance(2, 1), npc::FactionStance::War);
}

TEST("FactionSystem: war is symmetric") {
    npc::FactionSystem fs;
    fs.addFaction(1,"X"); fs.addFaction(2,"Y");
    fs.declareWar(1, 2, "test", 0.f);
    ASSERT_TRUE(fs.areHostile(1, 2));
    ASSERT_TRUE(fs.areHostile(2, 1));
}

TEST("FactionSystem: formAlliance sets stance") {
    npc::FactionSystem fs;
    fs.addFaction(1,"A"); fs.addFaction(2,"B");
    fs.formAlliance(1, 2, "", 0.f);
    ASSERT_EQ(fs.getStance(1, 2), npc::FactionStance::Alliance);
    ASSERT_TRUE(fs.areAllied(1, 2));
}

TEST("FactionSystem: declarePeace creates truce") {
    npc::FactionSystem fs;
    fs.addFaction(1,"A"); fs.addFaction(2,"B");
    fs.declareWar(1, 2, "greed", 0.f);
    fs.declarePeace(1, 2, "", 0.f, 100.f);
    ASSERT_EQ(fs.getStance(1, 2), npc::FactionStance::Truce);
}

TEST("FactionSystem: truce expires to Peace") {
    npc::FactionSystem fs;
    fs.addFaction(1,"A"); fs.addFaction(2,"B");
    fs.declareWar(1, 2, "test", 0.f);
    fs.declarePeace(1, 2, "", 0.f, 50.f);
    fs.update(60.f);
    ASSERT_EQ(fs.getStance(1, 2), npc::FactionStance::Peace);
}

TEST("FactionSystem: alliance cascade on war") {
    npc::FactionSystem fs;
    fs.addFaction(1, "A");
    fs.addFaction(2, "B");
    fs.addFaction(ALLY_B, "AllyB");
    fs.formAlliance(2, ALLY_B, "", 0.f);
    fs.declareWar(1, 2, "conquest", 0.f, true);
    ASSERT_EQ(fs.getStance(1, ALLY_B), npc::FactionStance::War);
}

TEST("FactionSystem: vassal joins overlord war") {
    npc::FactionSystem fs;
    fs.addFaction(LORD, "Lord");
    fs.addFaction(VASSAL, "Vassal");
    fs.addFaction(ENEMY, "Enemy");
    fs.formVassal(VASSAL, LORD, "", 0.f);
    fs.declareWar(LORD, ENEMY, "expansion", 0.f, true);
    ASSERT_EQ(fs.getStance(VASSAL, ENEMY), npc::FactionStance::War);
}

TEST("FactionSystem: wouldDefend direct ally") {
    npc::FactionSystem fs;
    fs.addFaction(1,"A"); fs.addFaction(2,"B"); fs.addFaction(3,"C");
    fs.formAlliance(2, 3, "", 0.f);
    ASSERT_TRUE(fs.wouldDefend(2, 3));
    ASSERT_FALSE(fs.wouldDefend(1, 3));
}

TEST("FactionSystem: resolveCoalition defender side") {
    npc::FactionSystem fs;
    fs.addFaction(ATCK,    "Attacker");
    fs.addFaction(TGT,     "Target");
    fs.addFaction(TGT_ALY, "TargetAlly");
    fs.formAlliance(TGT, TGT_ALY, "", 0.f);
    auto c = fs.resolveCoalition(ATCK, TGT);
    ASSERT_EQ(c.aggressor, ATCK);
    ASSERT_TRUE(std::find(c.defenderSide.begin(), c.defenderSide.end(),
                          TGT_ALY) != c.defenderSide.end());
}

TEST("FactionSystem: addMember and getFactionOf") {
    npc::FactionSystem fs;
    fs.addFaction(GUILD, "Guild");
    fs.addMember(GUILD, 1001u);
    ASSERT_EQ(fs.getFactionOf(1001u), GUILD);
}

TEST("FactionSystem: diplomatic history non-empty after events") {
    npc::FactionSystem fs;
    fs.addFaction(1,"A"); fs.addFaction(2,"B");
    fs.declareWar(1, 2, "land", 1.f);
    fs.declarePeace(1, 2, "", 10.f, 50.f);
    ASSERT_FALSE(fs.diplomaticSummary(1, 2).empty());
    ASSERT_GE(fs.history().size(), std::size_t(2));
}

// ─────────────────────────────────────────────────────────────────────────────
// RelationshipSystem  (string IDs)
// ─────────────────────────────────────────────────────────────────────────────

TEST("RelationshipSystem: default value is neutral") {
    npc::RelationshipSystem rs;
    ASSERT_NEAR(rs.getValue("aria","bard"), 0.0f, 1.0f);
}

TEST("RelationshipSystem: saved event increases value") {
    npc::RelationshipSystem rs;
    rs.recordEvent("aria","bard", npc::RelationshipEventType::Saved, 0.0);
    ASSERT_GT(rs.getValue("aria","bard"), 10.0f);
}

TEST("RelationshipSystem: betrayal reduces value") {
    npc::RelationshipSystem rs;
    rs.recordEvent("aria","bard", npc::RelationshipEventType::Betrayed, 0.0);
    ASSERT_LT(rs.getValue("aria","bard"), -20.0f);
}

TEST("RelationshipSystem: trust decreases on lie") {
    npc::RelationshipSystem rs;
    rs.recordEvent("aria","bard", npc::RelationshipEventType::Lied, 0.0);
    ASSERT_LT(rs.getTrust("aria","bard"), 50.0f);
}

TEST("RelationshipSystem: mutual event affects both") {
    npc::RelationshipSystem rs;
    rs.recordMutualEvent("a","b", npc::RelationshipEventType::Traded, 0.0);
    ASSERT_GT(rs.getValue("a","b"), 0.0f);
    ASSERT_GT(rs.getValue("b","a"), 0.0f);
}

TEST("RelationshipSystem: decay moves toward neutral") {
    npc::RelationshipSystem rs;
    rs.setValue("a","b", 50.0f);
    rs.update(0.0, 100.0f);
    float after = rs.getValue("a","b");
    ASSERT_LT(after, 50.0f);
    ASSERT_GT(after, 0.0f);
}

TEST("RelationshipSystem: remembers past event") {
    npc::RelationshipSystem rs;
    rs.recordEvent("hero","npc", npc::RelationshipEventType::Saved, 10.0);
    ASSERT_TRUE(rs.remembers("npc","hero", npc::RelationshipEventType::Saved));
}

TEST("RelationshipSystem: does not remember event before since") {
    npc::RelationshipSystem rs;
    rs.recordEvent("hero","npc", npc::RelationshipEventType::Saved, 5.0);
    ASSERT_FALSE(rs.remembers("npc","hero", npc::RelationshipEventType::Saved, 10.0));
}

TEST("RelationshipSystem: narrative non-empty after events") {
    npc::RelationshipSystem rs;
    rs.recordEvent("hero","npc", npc::RelationshipEventType::Saved, 0.0);
    rs.recordEvent("hero","npc", npc::RelationshipEventType::Gifted, 5.0);
    std::string n = rs.narrative("hero","npc", 10.0);
    ASSERT_FALSE(n.empty());
    ASSERT_TRUE(n.find("hero") != std::string::npos);
}

TEST("RelationshipSystem: recallSentence returns relevant event") {
    npc::RelationshipSystem rs;
    rs.recordEvent("hero","npc", npc::RelationshipEventType::Saved, 24.0);
    auto s = rs.recallSentence("npc","hero", npc::RelationshipEventType::Saved, 48.0);
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->find("saved") != std::string::npos);
}

TEST("RelationshipSystem: areHostile when both negative") {
    npc::RelationshipSystem rs;
    rs.setValue("a","b", -50.0f);
    rs.setValue("b","a", -50.0f);
    ASSERT_TRUE(rs.areHostile("a","b"));
}

TEST("RelationshipSystem: areFriendly when both positive") {
    npc::RelationshipSystem rs;
    rs.setValue("a","b", 30.0f);
    rs.setValue("b","a", 30.0f);
    ASSERT_TRUE(rs.areFriendly("a","b"));
}

TEST("RelationshipSystem: topFriends sorted descending") {
    npc::RelationshipSystem rs;
    rs.setValue("a","b", 80.0f);
    rs.setValue("a","c", 20.0f);
    rs.setValue("a","d", 60.0f);
    auto friends = rs.topFriends("a", 3);
    ASSERT_EQ(friends.size(), std::size_t(3));
    ASSERT_GE(friends[0].second, friends[1].second);
    ASSERT_GE(friends[1].second, friends[2].second);
}

TEST("RelationshipSystem: removeNPC cleans all pairs") {
    npc::RelationshipSystem rs;
    rs.recordEvent("a","b", npc::RelationshipEventType::Helped, 0.0);
    rs.recordEvent("a","c", npc::RelationshipEventType::Helped, 0.0);
    rs.removeNPC("a");
    ASSERT_EQ(rs.pairCount(), std::size_t(0));
}

// ─────────────────────────────────────────────────────────────────────────────
// LOD System  (uses shared_ptr<NPC>)
// ─────────────────────────────────────────────────────────────────────────────

TEST("LODSystem: close NPC promoted to Active") {
    npc::LODSystem lod;
    npc::LODConfig cfg;
    cfg.activeRadius     = 50.0f;
    cfg.backgroundRadius = 150.0f;
    lod.setConfig(cfg);

    auto npc1 = std::make_shared<npc::NPC>(1u, "NPC1", npc::NPCType::Villager);
    npc1->position = {10.0f, 0.0f};
    lod.registerNPC(1u);
    lod.setPlayerPosition({0.f, 0.f});

    std::vector<std::shared_ptr<npc::NPC>> npcs = {npc1};
    // Run enough frames for hysteresis to promote
    for (int i = 0; i < 10; ++i)
        lod.update(npcs, static_cast<float>(i) * 0.016f, 0.016f);

    ASSERT_EQ(lod.tier(1u), npc::LODTier::Active);
}

TEST("LODSystem: very distant NPC becomes Dormant") {
    npc::LODSystem lod;
    npc::LODConfig cfg;
    cfg.activeRadius     = 50.0f;
    cfg.backgroundRadius = 100.0f;
    cfg.minDwellSecs     = 0.0f; // disable dwell for fast test
    lod.setConfig(cfg);

    auto npc1 = std::make_shared<npc::NPC>(1u, "NPC1", npc::NPCType::Villager);
    npc1->position = {500.0f, 0.0f};
    lod.registerNPC(1u);
    lod.setPlayerPosition({0.f, 0.f});

    std::vector<std::shared_ptr<npc::NPC>> npcs = {npc1};
    for (int i = 0; i < 30; ++i)
        lod.update(npcs, static_cast<float>(i) * 0.1f, 0.1f);

    ASSERT_EQ(lod.tier(1u), npc::LODTier::Dormant);
}

TEST("LODSystem: pin prevents demotion") {
    npc::LODSystem lod;
    npc::LODConfig cfg;
    cfg.activeRadius     = 50.0f;
    cfg.backgroundRadius = 100.0f;
    cfg.minDwellSecs     = 0.0f;
    lod.setConfig(cfg);

    auto npc1 = std::make_shared<npc::NPC>(1u, "NPC1", npc::NPCType::Villager);
    npc1->position = {500.0f, 0.0f};
    lod.registerNPC(1u);
    lod.pin(1u, npc::LODTier::Active);
    lod.setPlayerPosition({0.f, 0.f});

    std::vector<std::shared_ptr<npc::NPC>> npcs = {npc1};
    for (int i = 0; i < 50; ++i)
        lod.update(npcs, static_cast<float>(i) * 0.1f, 0.1f);

    ASSERT_EQ(lod.tier(1u), npc::LODTier::Active);
}

TEST("LODSystem: toTickThisFrame returns registered NPCs") {
    npc::LODSystem lod;
    auto n1 = std::make_shared<npc::NPC>(1u, "A", npc::NPCType::Villager);
    auto n2 = std::make_shared<npc::NPC>(2u, "B", npc::NPCType::Villager);
    n1->position = {0.f, 0.f};
    n2->position = {5.f, 0.f};
    lod.registerNPC(1u); lod.registerNPC(2u);
    lod.setPlayerPosition({0.f, 0.f});
    std::vector<std::shared_ptr<npc::NPC>> npcs = {n1, n2};
    for (int i = 0; i < 10; ++i)
        lod.update(npcs, static_cast<float>(i)*0.016f, 0.016f);
    auto active = lod.toTickThisFrame(npc::LODTier::Active);
    ASSERT_GE(active.size(), std::size_t(1));
}

// ─────────────────────────────────────────────────────────────────────────────
// RelationshipData decay edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST("RelationshipData: decayedWeight at t=0 is 1") {
    npc::RelationshipEvent ev;
    ev.simTime = 100.0;
    ASSERT_NEAR(ev.decayedWeight(100.0, 168.0f), 1.0f, 1e-5f);
}

TEST("RelationshipData: decayedWeight at half-life is 0.5") {
    npc::RelationshipEvent ev;
    ev.simTime = 0.0;
    ASSERT_NEAR(ev.decayedWeight(168.0, 168.0f), 0.5f, 1e-4f);
}

TEST("RelationshipData: history capped at MAX_HISTORY") {
    npc::RelationshipData d;
    for (int i = 0; i < 100; ++i) {
        npc::RelationshipEvent ev;
        ev.simTime = static_cast<double>(i);
        d.addEvent(ev);
    }
    ASSERT_EQ(d.history.size(), npc::RelationshipData::MAX_HISTORY);
}

TEST("RelationshipData: worstEvent finds minimum delta") {
    npc::RelationshipData d;
    npc::RelationshipEvent good; good.delta =  20.0f; d.addEvent(good);
    npc::RelationshipEvent bad;  bad.delta  = -50.0f; d.addEvent(bad);
    npc::RelationshipEvent mid;  mid.delta  =   5.0f; d.addEvent(mid);
    auto* worst = d.worstEvent();
    ASSERT_TRUE(worst != nullptr);
    ASSERT_NEAR(worst->delta, -50.0f, 1e-5f);
}

TEST("RelationshipData: bestEvent finds maximum delta") {
    npc::RelationshipData d;
    npc::RelationshipEvent e1; e1.delta = 10.0f; d.addEvent(e1);
    npc::RelationshipEvent e2; e2.delta = 35.0f; d.addEvent(e2);
    auto* best = d.bestEvent();
    ASSERT_TRUE(best != nullptr);
    ASSERT_NEAR(best->delta, 35.0f, 1e-5f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Integration smoke tests
// ─────────────────────────────────────────────────────────────────────────────

TEST("Integration: faction war → member hostility via RelationshipSystem") {
    npc::FactionSystem  fs;
    npc::RelationshipSystem rs;

    fs.addFaction(1, "Knights");
    fs.addFaction(2, "Bandits");
    fs.addMember(1, 101u); // sir_roland
    fs.addMember(2, 202u); // rogue_mira

    fs.declareWar(1, 2, "territorial", 0.f);

    // Simulate NPC-level hostility when factions are at war
    if (fs.areHostile(1, 2)) {
        rs.recordEvent("rogue_mira","sir_roland",
                       npc::RelationshipEventType::Attacked, 0.0);
        rs.recordEvent("sir_roland","rogue_mira",
                       npc::RelationshipEventType::Attacked, 0.0);
    }

    ASSERT_TRUE(rs.areHostile("sir_roland","rogue_mira"));
    ASSERT_TRUE(rs.areHostile("rogue_mira","sir_roland"));
}

TEST("Integration: saved event remembered in dialogue") {
    npc::RelationshipSystem rs;
    rs.recordEvent("hero","merchant", npc::RelationshipEventType::Saved, 10.0);
    // 24h later NPC recalls
    auto recall = rs.recallSentence("merchant","hero",
                                    npc::RelationshipEventType::Saved, 34.0);
    ASSERT_TRUE(recall.has_value());
    ASSERT_TRUE(recall->find("saved") != std::string::npos);
}

TEST("Integration: SharedBlackboard world state view") {
    npc::WorldBlackboard wb;
    wb.setTime(12.5f, 5, 0.0);
    wb.setFactionAlert("undead", 0.9f, 0.0);
    wb.setItemPrice("iron_sword", 45.0f, 0.0);
    auto view = wb.viewOf("faction/", 0.0);
    ASSERT_TRUE(view.has("faction/undead/alert"));
}

TEST("Integration: EventBus chain A→B") {
    npc::EventBus bus;
    int chainFired = 0;
    bus.subscribe<OtherEvent>([&](const OtherEvent& e){
        if (e.msg == "from_chain") ++chainFired;
    });
    bus.addChain<TestEvent, OtherEvent>([](const TestEvent&) -> OtherEvent {
        return OtherEvent{"from_chain"};
    });
    bus.publish(TestEvent{1});
    ASSERT_EQ(chainFired, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    bool verbose = (argc > 1 && std::string(argv[1]) == "-v");
    return npc::test::run_all(verbose);
}
