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
#include "npc/world/lod_system.hpp"    // pulls in npc.hpp → NPC class
#include "npc/serialization/json.hpp"
#include "npc/event/event_system.hpp"
#include "npc/ai/blackboard.hpp"

#include <vector>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// JSON Parser / Writer
// ─────────────────────────────────────────────────────────────────────────────

TEST("JSON: parse null") {
    auto v = npc::serial::parse("null");
    ASSERT_TRUE(v.isNull());
}

TEST("JSON: parse bool true") {
    ASSERT_EQ(npc::serial::parse("true").asBool(), true);
}

TEST("JSON: parse bool false") {
    ASSERT_EQ(npc::serial::parse("false").asBool(), false);
}

TEST("JSON: parse integer") {
    ASSERT_EQ(npc::serial::parse("42").asInt(), int64_t(42));
}

TEST("JSON: parse negative integer") {
    ASSERT_EQ(npc::serial::parse("-17").asInt(), int64_t(-17));
}

TEST("JSON: parse double") {
    ASSERT_NEAR(npc::serial::parse("3.14").asDouble(), 3.14, 1e-9);
}

TEST("JSON: parse string") {
    ASSERT_EQ(npc::serial::parse("\"hello world\"").asString(), "hello world");
}

TEST("JSON: parse empty array") {
    auto v = npc::serial::parse("[]");
    ASSERT_TRUE(v.isArray());
    ASSERT_EQ(v.asArray().size(), std::size_t(0));
}

TEST("JSON: parse array of ints") {
    auto v   = npc::serial::parse("[1,2,3]");
    auto& arr = v.asArray();
    ASSERT_EQ(arr.size(), std::size_t(3));
    ASSERT_EQ(arr[0].asInt(), int64_t(1));
    ASSERT_EQ(arr[2].asInt(), int64_t(3));
}

TEST("JSON: parse nested object") {
    auto v = npc::serial::parse("{\"name\":\"Aria\",\"level\":5}");
    ASSERT_EQ(v["name"].asString(), "Aria");
    ASSERT_EQ(v["level"].asInt(), int64_t(5));
}

TEST("JSON: roundtrip") {
    auto v  = npc::serial::parse("{\"a\":1,\"b\":[2,3],\"c\":null}");
    auto v2 = npc::serial::parse(npc::serial::toString(v));
    ASSERT_EQ(npc::serial::toString(v), npc::serial::toString(v2));
}

TEST("JSON: toString pretty smoke") {
    auto v = npc::serial::parse("[1,[2,3],{\"x\":true}]");
    std::string pretty = npc::serial::toString(v, true);
    ASSERT_FALSE(pretty.empty());
    ASSERT_TRUE(pretty.find('\n') != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Blackboard
// ─────────────────────────────────────────────────────────────────────────────

TEST("Blackboard: set and getOr") {
    npc::Blackboard bb;
    bb.set<int>("health", 100);
    ASSERT_EQ(bb.getOr<int>("health", -1), 100);
}

TEST("Blackboard: getOr missing returns default") {
    npc::Blackboard bb;
    ASSERT_EQ(bb.getOr<int>("missing", 0), 0);
}

TEST("Blackboard: get returns nullopt for missing") {
    npc::Blackboard bb;
    ASSERT_FALSE(bb.get<int>("missing").has_value());
}

TEST("Blackboard: overwrite value") {
    npc::Blackboard bb;
    bb.set<float>("x", 1.0f);
    bb.set<float>("x", 2.0f);
    ASSERT_NEAR(bb.getOr<float>("x", 0.f), 2.0f, 1e-6f);
}

TEST("Blackboard: has()") {
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
    ASSERT_EQ(bb.keys().size(), std::size_t(2));
}

// ─────────────────────────────────────────────────────────────────────────────
// SharedBlackboard / WorldBlackboard
// ─────────────────────────────────────────────────────────────────────────────

TEST("SharedBlackboard: set and get") {
    npc::SharedBlackboard sbb;
    sbb.set<int>("counter", 5, 0.0);
    ASSERT_EQ(sbb.getOr<int>("counter", -1), 5);
}

TEST("SharedBlackboard: TTL expiry") {
    npc::SharedBlackboard sbb;
    sbb.set<int>("temp", 99, 0.0, 1.0);
    ASSERT_EQ(sbb.getOr<int>("temp", -1), 99);
    sbb.pruneExpired(2.0);
    ASSERT_FALSE(sbb.has("temp"));
}

TEST("SharedBlackboard: setIfAbsent does not overwrite") {
    npc::SharedBlackboard sbb;
    sbb.set<int>("x", 10, 0.0);
    sbb.setIfAbsent<int>("x", 20, 0.0);
    ASSERT_EQ(sbb.getOr<int>("x", -1), 10);
}

TEST("SharedBlackboard: versioning increments") {
    npc::SharedBlackboard sbb;
    sbb.set<int>("v", 1, 0.0);
    uint64_t v1 = sbb.version("v");
    sbb.set<int>("v", 2, 0.0);
    ASSERT_GT((int)sbb.version("v"), (int)v1);
}

TEST("SharedBlackboard: watcher fires on set") {
    npc::SharedBlackboard sbb;
    int fired = 0;
    auto id = sbb.watch("ns/", [&](const std::string&, const std::any&, const npc::BBEntry&){
        ++fired;
    });
    sbb.set<int>("ns/value", 42, 0.0);
    ASSERT_EQ(fired, 1);
    sbb.unwatch(id);
}

TEST("WorldBlackboard: setTime and viewOf") {
    npc::SharedBlackboard _sbb;
    npc::WorldBlackboard wb(_sbb);
    wb.setTime(12.5f, 0.f);  // (hour, simTime)
    wb.setDay(5, 0.f);
    // faction alert uses uint32_t FactionId
    wb.setFactionAlert(1u, true, 0.f);
    // item price uses uint32_t ItemId
    wb.setItemPrice(42u, 45.0f, 0.f);
    auto view = wb.viewOf("faction/", 0.f);
    ASSERT_TRUE(view.has("1/alert"));
}

// ─────────────────────────────────────────────────────────────────────────────
// EventBus  (callback takes const EventT&)
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
    int tc = 0, oc = 0;
    bus.subscribe<TestEvent>([&](const TestEvent&){ ++tc; });
    bus.subscribe<OtherEvent>([&](const OtherEvent&){ ++oc; });
    bus.publish(TestEvent{1});
    ASSERT_EQ(tc, 1);
    ASSERT_EQ(oc, 0);
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
    ASSERT_EQ(count, 1); // not incremented after scope exit
}

TEST("EventBus: delayed event timing") {
    npc::EventBus bus;
    int received = 0;
    bus.subscribe<TestEvent>([&](const TestEvent& e){ received = e.value; });
    // Set time to 5.0, publish with 1s delay → fires at t=6
    bus.update(5.0f);
    bus.publishDelayed(TestEvent{99}, 1.0f);
    bus.update(5.5f);
    ASSERT_EQ(received, 0);  // not yet
    bus.update(6.5f);
    ASSERT_EQ(received, 99); // now fired
}

TEST("EventBus: history records events") {
    npc::EventBus bus;
    bus.publish(TestEvent{1});
    bus.publish(TestEvent{2});
    ASSERT_GE(bus.getHistory<TestEvent>().size(), std::size_t(2));
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

TEST("EventBus: addChain fires B when A published") {
    npc::EventBus bus;
    int chainFired = 0;
    bus.subscribe<OtherEvent>([&](const OtherEvent& e){
        if (e.msg == "chained") ++chainFired;
    });
    bus.addChain<TestEvent, OtherEvent>([](const TestEvent&) -> OtherEvent {
        return OtherEvent{"chained"};
    });
    bus.publish(TestEvent{1});
    ASSERT_EQ(chainFired, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// SpatialGrid / SpatialIndex
// ─────────────────────────────────────────────────────────────────────────────

TEST("SpatialGrid: insert and queryRadius") {
    npc::SpatialGrid grid(10.0f);
    grid.insert(1, {0.f, 0.f});
    grid.insert(2, {5.f, 0.f});
    grid.insert(3, {50.f, 50.f});
    ASSERT_EQ(grid.queryRadius({0.f, 0.f}, 8.0f).size(), std::size_t(2));
}

TEST("SpatialGrid: update moves entity") {
    npc::SpatialGrid grid(10.0f);
    grid.insert(1, {0.f, 0.f});
    grid.update(1, {100.f, 100.f});
    ASSERT_EQ(grid.queryRadius({0.f, 0.f},   5.0f).size(), std::size_t(0));
    ASSERT_EQ(grid.queryRadius({100.f,100.f}, 5.0f).size(), std::size_t(1));
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

TEST("SpatialIndex: nearbyExcept excludes self") {
    npc::SpatialIndex si(10.0f);
    si.update(1, {0.f, 0.f});
    si.update(2, {1.f, 0.f});
    auto hits = si.nearbyExcept({0.f, 0.f}, 5.0f, 1u);
    ASSERT_EQ(hits.size(), std::size_t(1));
    ASSERT_EQ(hits[0], 2u);
}

TEST("SpatialIndex: findClusters groups close entities") {
    npc::SpatialIndex si(5.0f);
    si.update(1, {0.f,   0.f});
    si.update(2, {1.f,   0.f});
    si.update(3, {0.f,   1.f});
    si.update(4, {100.f, 100.f});
    si.update(5, {101.f, 100.f});
    ASSERT_GE(si.findClusters(5.0f).size(), std::size_t(2));
}

// ─────────────────────────────────────────────────────────────────────────────
// Pathfinding  (Pathfinder constructor takes grid dims + walkable check)
// ─────────────────────────────────────────────────────────────────────────────

TEST("Pathfinding: simple straight path") {
    int W = 10, H = 10;
    std::vector<bool> walkable(W*H, true);
    npc::Pathfinder pf(W, H, [&](int x, int y){ return walkable[y*W+x]; });
    pf.buildRegions();
    auto result = pf.query({0.f,0.f}, {5.f,0.f});
    ASSERT_TRUE(result.complete);
    ASSERT_FALSE(result.waypoints.empty());
}

TEST("Pathfinding: NavRegions wall splits map") {
    int W = 6, H = 6;
    std::vector<bool> walkable(W*H, true);
    for (int y = 0; y < H; ++y) walkable[y*W+3] = false;
    npc::NavRegions nr;
    nr.rebuild(W, H, [&](int x, int y){ return walkable[y*W+x]; });
    ASSERT_FALSE(nr.isReachable({0.f,0.f}, {5.f,0.f}));
    ASSERT_TRUE(nr.isReachable({0.f,0.f},  {2.f,0.f}));
}

TEST("Pathfinding: PathCache stores and retrieves waypoints") {
    npc::PathCache cache;
    // PathCache stores vector<Vec2>, keyed by PathCacheKey{sx,sy,gx,gy}
    npc::PathCacheKey key{0, 0, 2, 0};
    std::vector<npc::Vec2> path = {{0.f,0.f},{1.f,0.f},{2.f,0.f}};
    cache.put(key, path);
    auto* got = cache.get(key);
    ASSERT_TRUE(got != nullptr);
    ASSERT_EQ(got->size(), std::size_t(3));
}

TEST("Pathfinding: PathCache miss returns nullptr") {
    npc::PathCache cache;
    ASSERT_TRUE(cache.get({99, 99, 100, 100}) == nullptr);
}

TEST("Pathfinding: second query uses cache") {
    int W = 10, H = 10;
    std::vector<bool> walkable(W*H, true);
    npc::Pathfinder pf(W, H, [&](int x, int y){ return walkable[y*W+x]; });
    pf.buildRegions();
    auto r1 = pf.query({0.f,0.f}, {9.f,0.f});
    ASSERT_FALSE(r1.fromCache);
    auto r2 = pf.query({0.f,0.f}, {9.f,0.f});
    ASSERT_TRUE(r2.fromCache);
}

TEST("Pathfinding: obstacle invalidates cache") {
    int W = 10, H = 10;
    std::vector<bool> walkable(W*H, true);
    npc::Pathfinder pf(W, H, [&](int x, int y){ return walkable[y*W+x]; });
    pf.buildRegions();
    pf.query({0.f,0.f}, {9.f,0.f}); // populate cache
    pf.clearObstacles();               // clearObstacles() also clears the path cache
    auto r3 = pf.query({0.f,0.f}, {9.f,0.f});
    ASSERT_FALSE(r3.fromCache);
}

// ─────────────────────────────────────────────────────────────────────────────
// FactionSystem  (FactionId = uint32_t, EntityId = uint32_t)
// ─────────────────────────────────────────────────────────────────────────────

TEST("FactionSystem: addFaction and faction() accessor") {
    npc::FactionSystem fs;
    fs.addFaction(1u, "Rebel Alliance");
    fs.addFaction(2u, "Galactic Empire");
    ASSERT_TRUE(fs.faction(1u) != nullptr);
    ASSERT_EQ(fs.faction(1u)->name, "Rebel Alliance");
}

TEST("FactionSystem: default stance is Peace") {
    npc::FactionSystem fs;
    fs.addFaction(1u,"A"); fs.addFaction(2u,"B");
    ASSERT_EQ(static_cast<int>(fs.getStance(1u,2u)), static_cast<int>(npc::FactionStance::Peace));
}

TEST("FactionSystem: declareWar sets stance") {
    npc::FactionSystem fs;
    fs.addFaction(1u,"A"); fs.addFaction(2u,"B");
    fs.declareWar(1u, 2u, "resources", 0.f);
    ASSERT_EQ(static_cast<int>(fs.getStance(1u,2u)), static_cast<int>(npc::FactionStance::War));
    ASSERT_EQ(static_cast<int>(fs.getStance(2u,1u)), static_cast<int>(npc::FactionStance::War));
}

TEST("FactionSystem: war is symmetric via areHostile") {
    npc::FactionSystem fs;
    fs.addFaction(1u,"X"); fs.addFaction(2u,"Y");
    fs.declareWar(1u, 2u, "", 0.f);
    ASSERT_TRUE(fs.areHostile(1u,2u));
    ASSERT_TRUE(fs.areHostile(2u,1u));
}

TEST("FactionSystem: formAlliance sets stance") {
    npc::FactionSystem fs;
    fs.addFaction(1u,"A"); fs.addFaction(2u,"B");
    fs.formAlliance(1u, 2u, "", 0.f);
    ASSERT_EQ(static_cast<int>(fs.getStance(1u,2u)), static_cast<int>(npc::FactionStance::Alliance));
    ASSERT_TRUE(fs.areAllied(1u,2u));
}

TEST("FactionSystem: declarePeace creates truce") {
    npc::FactionSystem fs;
    fs.addFaction(1u,"A"); fs.addFaction(2u,"B");
    fs.declareWar(1u, 2u, "", 0.f);
    fs.declarePeace(1u, 2u, "", 0.f, 100.f);
    ASSERT_EQ(static_cast<int>(fs.getStance(1u,2u)), static_cast<int>(npc::FactionStance::Truce));
}

TEST("FactionSystem: truce expires to Peace") {
    npc::FactionSystem fs;
    fs.addFaction(1u,"A"); fs.addFaction(2u,"B");
    fs.declareWar(1u, 2u, "", 0.f);
    fs.declarePeace(1u, 2u, "", 0.f, 50.f);
    fs.update(60.f);
    ASSERT_EQ(static_cast<int>(fs.getStance(1u,2u)), static_cast<int>(npc::FactionStance::Peace));
}

TEST("FactionSystem: alliance cascade on war") {
    npc::FactionSystem fs;
    fs.addFaction(1u,"A"); fs.addFaction(2u,"B"); fs.addFaction(5u,"AllyB");
    fs.formAlliance(2u, 5u, "", 0.f);
    fs.declareWar(1u, 2u, "", 0.f, true);
    ASSERT_EQ(static_cast<int>(fs.getStance(1u,5u)), static_cast<int>(npc::FactionStance::War));
}

TEST("FactionSystem: vassal joins overlord war") {
    npc::FactionSystem fs;
    fs.addFaction(6u,"Lord"); fs.addFaction(7u,"Vassal"); fs.addFaction(8u,"Enemy");
    fs.formVassal(7u, 6u, "", 0.f);
    fs.declareWar(6u, 8u, "", 0.f, true);
    ASSERT_EQ(static_cast<int>(fs.getStance(7u,8u)), static_cast<int>(npc::FactionStance::War));
}

TEST("FactionSystem: wouldDefend direct ally") {
    npc::FactionSystem fs;
    fs.addFaction(1u,"A"); fs.addFaction(2u,"B"); fs.addFaction(3u,"C");
    fs.formAlliance(2u, 3u, "", 0.f);
    ASSERT_TRUE(fs.wouldDefend(2u,3u));
    ASSERT_FALSE(fs.wouldDefend(1u,3u));
}

TEST("FactionSystem: resolveCoalition defender side") {
    npc::FactionSystem fs;
    fs.addFaction(9u,"Atk"); fs.addFaction(10u,"Tgt"); fs.addFaction(11u,"TgtAly");
    fs.formAlliance(10u, 11u, "", 0.f);
    auto c = fs.resolveCoalition(9u, 10u);
    ASSERT_EQ(c.aggressor, 9u);
    ASSERT_TRUE(std::find(c.defenderSide.begin(), c.defenderSide.end(),
                          11u) != c.defenderSide.end());
}

TEST("FactionSystem: addMember and getFactionOf") {
    npc::FactionSystem fs;
    fs.addFaction(12u, "Guild");
    fs.addMember(12u, 1001u);
    ASSERT_EQ(fs.getFactionOf(1001u), 12u);
}

TEST("FactionSystem: diplomatic history") {
    npc::FactionSystem fs;
    fs.addFaction(1u,"A"); fs.addFaction(2u,"B");
    fs.declareWar(1u, 2u, "land", 1.f);
    fs.declarePeace(1u, 2u, "", 10.f, 50.f);
    ASSERT_FALSE(fs.diplomaticSummary(1u,2u).empty());
    ASSERT_GE(fs.history().size(), std::size_t(2));
}

// ─────────────────────────────────────────────────────────────────────────────
// RelationshipSystem  (string IDs, standalone system)
// ─────────────────────────────────────────────────────────────────────────────

TEST("RelationshipSystem: default value neutral") {
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

TEST("RelationshipSystem: does not remember before since") {
    npc::RelationshipSystem rs;
    rs.recordEvent("hero","npc", npc::RelationshipEventType::Saved, 5.0);
    ASSERT_FALSE(rs.remembers("npc","hero", npc::RelationshipEventType::Saved, 10.0));
}

TEST("RelationshipSystem: narrative non-empty") {
    npc::RelationshipSystem rs;
    rs.recordEvent("hero","npc", npc::RelationshipEventType::Saved, 0.0);
    rs.recordEvent("hero","npc", npc::RelationshipEventType::Gifted, 5.0);
    std::string n = rs.narrative("hero","npc", 10.0);
    ASSERT_FALSE(n.empty());
    ASSERT_TRUE(n.find("hero") != std::string::npos);
}

TEST("RelationshipSystem: recallSentence contains event type") {
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
// LOD System  (update takes vector<shared_ptr<NPC>>)
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
    for (int i = 0; i < 10; ++i)
        lod.update(npcs, static_cast<float>(i) * 0.016f, 0.016f);
    ASSERT_EQ(static_cast<int>(lod.tier(1u)), static_cast<int>(npc::LODTier::Active));
}

TEST("LODSystem: very distant NPC becomes Dormant") {
    npc::LODSystem lod;
    npc::LODConfig cfg;
    cfg.activeRadius     = 50.0f;
    cfg.backgroundRadius = 100.0f;
    cfg.minDwellSecs     = 0.0f;
    lod.setConfig(cfg);
    auto npc1 = std::make_shared<npc::NPC>(1u, "NPC1", npc::NPCType::Villager);
    npc1->position = {500.0f, 0.0f};
    lod.registerNPC(1u);
    lod.setPlayerPosition({0.f, 0.f});
    std::vector<std::shared_ptr<npc::NPC>> npcs = {npc1};
    for (int i = 0; i < 30; ++i)
        lod.update(npcs, static_cast<float>(i) * 0.1f, 0.1f);
    ASSERT_EQ(static_cast<int>(lod.tier(1u)), static_cast<int>(npc::LODTier::Dormant));
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
    lod.pin(1u, npc::LODTier::Active);  // pin(id, tier)
    lod.setPlayerPosition({0.f, 0.f});
    std::vector<std::shared_ptr<npc::NPC>> npcs = {npc1};
    for (int i = 0; i < 50; ++i)
        lod.update(npcs, static_cast<float>(i) * 0.1f, 0.1f);
    ASSERT_EQ(static_cast<int>(lod.tier(1u)), static_cast<int>(npc::LODTier::Active));
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
    ASSERT_GE(lod.toTickThisFrame(npc::LODTier::Active).size(), std::size_t(1));
}

// ─────────────────────────────────────────────────────────────────────────────
// RelationshipData decay edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST("RelationshipEvent: decayedWeight at t=0 is 1") {
    npc::RelationshipEvent ev; ev.simTime = 100.0;
    ASSERT_NEAR(ev.decayedWeight(100.0, 168.0f), 1.0f, 1e-5f);
}

TEST("RelationshipEvent: decayedWeight at half-life is 0.5") {
    npc::RelationshipEvent ev; ev.simTime = 0.0;
    ASSERT_NEAR(ev.decayedWeight(168.0, 168.0f), 0.5f, 1e-4f);
}

TEST("RelationshipData: history capped at MAX_HISTORY") {
    npc::RelationshipData d;
    for (int i = 0; i < 100; ++i) {
        npc::RelationshipEvent ev; ev.simTime = i; d.addEvent(ev);
    }
    ASSERT_EQ(d.history.size(), npc::RelationshipData::MAX_HISTORY);
}

TEST("RelationshipData: worstEvent finds min delta") {
    npc::RelationshipData d;
    npc::RelationshipEvent g; g.delta =  20.f; d.addEvent(g);
    npc::RelationshipEvent b; b.delta = -50.f; d.addEvent(b);
    npc::RelationshipEvent m; m.delta =   5.f; d.addEvent(m);
    ASSERT_NEAR(d.worstEvent()->delta, -50.f, 1e-5f);
}

TEST("RelationshipData: bestEvent finds max delta") {
    npc::RelationshipData d;
    npc::RelationshipEvent e1; e1.delta = 10.f; d.addEvent(e1);
    npc::RelationshipEvent e2; e2.delta = 35.f; d.addEvent(e2);
    ASSERT_NEAR(d.bestEvent()->delta, 35.f, 1e-5f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Integration smoke tests
// ─────────────────────────────────────────────────────────────────────────────

TEST("Integration: faction war → NPC-level hostility") {
    npc::FactionSystem  fs;
    npc::RelationshipSystem rs;
    fs.addFaction(1u, "Knights");
    fs.addFaction(2u, "Bandits");
    fs.addMember(1u, 101u);
    fs.addMember(2u, 202u);
    fs.declareWar(1u, 2u, "territorial", 0.f);
    if (fs.areHostile(1u, 2u)) {
        rs.recordEvent("rogue","roland", npc::RelationshipEventType::Attacked, 0.0);
        rs.recordEvent("roland","rogue", npc::RelationshipEventType::Attacked, 0.0);
    }
    ASSERT_TRUE(rs.areHostile("roland","rogue"));
}

TEST("Integration: saved event remembered in dialogue") {
    npc::RelationshipSystem rs;
    rs.recordEvent("hero","merchant", npc::RelationshipEventType::Saved, 10.0);
    auto recall = rs.recallSentence("merchant","hero",
                                    npc::RelationshipEventType::Saved, 34.0);
    ASSERT_TRUE(recall.has_value());
    ASSERT_TRUE(recall->find("saved") != std::string::npos);
}

TEST("Integration: WorldBlackboard faction key format") {
    npc::SharedBlackboard _sbb2;
    npc::WorldBlackboard wb(_sbb2);
    wb.setFactionAlert(42u, true, 0.f);
    auto view = wb.viewOf("faction/", 0.f);
    ASSERT_TRUE(view.has("42/alert"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    bool verbose = (argc > 1 && std::string(argv[1]) == "-v");
    return npc::test::run_all(verbose);
}
