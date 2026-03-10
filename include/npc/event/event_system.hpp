#pragma once

#include <functional>
#include <unordered_map>
#include <vector>
#include <typeindex>
#include <any>
#include <cstdint>
#include <string>
#include <queue>
#include <deque>
#include <optional>
#include <memory>
#include <algorithm>
#include "../core/types.hpp"
#include "../core/vec2.hpp"

namespace npc {

using SubscriptionId = uint32_t;
static constexpr SubscriptionId kInvalidSubscription = 0;

// ─── Priority ────────────────────────────────────────────────────────
// Lower integer  =  higher dispatch priority (Critical fired first).

enum class EventPriority : int {
    Critical = 0,
    High     = 100,
    Normal   = 200,
    Low      = 300,
    Idle     = 400,
};

// ═══════════════════════════════════════════════════════════════════════
// BUILT-IN EVENT TYPES  (unchanged from original + new additions)
// ═══════════════════════════════════════════════════════════════════════

struct CombatEvent {
    EntityId attacker;
    EntityId defender;
    float    damage;
    bool     killed;
    Vec2     location;
};

struct PerceptionEvent {
    EntityId     observer;
    EntityId     target;
    AwarenessLevel level;
    Vec2         location;
};

struct DialogEvent {
    EntityId    speaker;
    EntityId    listener;
    std::string dialogId;
    std::string chosenOption;
};

struct TradeEvent {
    EntityId buyer;
    EntityId seller;
    ItemId   item;
    int      quantity;
    float    price;
};

struct TimeEvent {
    float      currentHour;
    TimeOfDay  timeOfDay;
    int        day;
};

struct DeathEvent {
    EntityId deceased;
    EntityId killer;
    Vec2     location;
};

struct FactionEvent {
    FactionId faction1;
    FactionId faction2;
    float     relationChange;
};

struct WorldEvent {
    std::string eventType;
    std::string description;
    Vec2        location;
    float       severity; // 0-1
};

// ─── EventRecord  (history entry) ────────────────────────────────────

struct EventRecord {
    std::type_index type      = typeid(void);
    std::any        data;
    float           timestamp = 0.f;
    EventPriority   priority  = EventPriority::Normal;

    template<typename T>
    bool is() const { return type == std::type_index(typeid(T)); }

    template<typename T>
    const T* as() const {
        if (!is<T>()) return nullptr;
        return std::any_cast<T>(&data);
    }
};

// ═══════════════════════════════════════════════════════════════════════
// FORWARD DECLARATIONS
// ═══════════════════════════════════════════════════════════════════════

class EventBus;

// ─── ScopedSubscription  (RAII auto-unsubscribe) ─────────────────────

class ScopedSubscription {
public:
    ScopedSubscription() = default;

    ScopedSubscription(EventBus& bus, SubscriptionId id)
        : bus_(&bus), id_(id) {}

    ~ScopedSubscription() { unsubscribe(); }

    // Non-copyable, moveable
    ScopedSubscription(const ScopedSubscription&)            = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;

    ScopedSubscription(ScopedSubscription&& o) noexcept
        : bus_(o.bus_), id_(o.id_) { o.bus_ = nullptr; o.id_ = kInvalidSubscription; }

    ScopedSubscription& operator=(ScopedSubscription&& o) noexcept {
        if (this != &o) {
            unsubscribe();
            bus_ = o.bus_; id_ = o.id_;
            o.bus_ = nullptr; o.id_ = kInvalidSubscription;
        }
        return *this;
    }

    // Detach without unsubscribing (caller takes ownership of lifetime)
    void release() { bus_ = nullptr; id_ = kInvalidSubscription; }

    SubscriptionId id()    const { return id_; }
    bool           valid() const { return id_ != kInvalidSubscription && bus_; }

private:
    void unsubscribe();   // defined after EventBus

    EventBus*      bus_ = nullptr;
    SubscriptionId id_  = kInvalidSubscription;
};

// ─── SubscriptionGroup  (batch RAII lifecycle) ───────────────────────

class SubscriptionGroup {
public:
    void add(ScopedSubscription sub) { subs_.push_back(std::move(sub)); }
    void releaseAll()                { subs_.clear(); }
    bool empty()  const              { return subs_.empty(); }
    size_t size() const              { return subs_.size(); }

private:
    std::vector<ScopedSubscription> subs_;
};

// ═══════════════════════════════════════════════════════════════════════
// EVENT BUS
// ═══════════════════════════════════════════════════════════════════════

class EventBus {
public:
    // ── Subscribe ────────────────────────────────────────────────────

    // Subscribe with optional priority and optional filter predicate.
    // Returns raw SubscriptionId; caller is responsible for unsubscribe().
    template<typename EventT>
    SubscriptionId subscribe(
        std::function<void(const EventT&)> callback,
        EventPriority priority = EventPriority::Normal,
        std::function<bool(const EventT&)> filter = nullptr)
    {
        auto id = nextId_++;
        Subscriber sub;
        sub.id       = id;
        sub.priority = static_cast<int>(priority);
        sub.callback = [cb = std::move(callback)](const std::any& e) {
            cb(std::any_cast<const EventT&>(e));
        };
        if (filter) {
            sub.filter = [f = std::move(filter)](const std::any& e) -> bool {
                return f(std::any_cast<const EventT&>(e));
            };
        }

        auto& vec = subscribers_[std::type_index(typeid(EventT))];
        vec.push_back(std::move(sub));
        sortByPriority(vec);
        return id;
    }

    // RAII variant — auto-unsubscribes when ScopedSubscription is destroyed.
    template<typename EventT>
    ScopedSubscription subscribeScoped(
        std::function<void(const EventT&)> callback,
        EventPriority priority = EventPriority::Normal,
        std::function<bool(const EventT&)> filter = nullptr)
    {
        auto id = subscribe<EventT>(std::move(callback), priority, std::move(filter));
        return ScopedSubscription(*this, id);
    }

    // Add scoped subscription directly into a SubscriptionGroup.
    template<typename EventT>
    void subscribeInto(
        SubscriptionGroup& group,
        std::function<void(const EventT&)> callback,
        EventPriority priority = EventPriority::Normal,
        std::function<bool(const EventT&)> filter = nullptr)
    {
        group.add(subscribeScoped<EventT>(std::move(callback), priority, std::move(filter)));
    }

    void unsubscribe(SubscriptionId id) {
        if (id == kInvalidSubscription) return;
        for (auto& [type, subs] : subscribers_) {
            subs.erase(std::remove_if(subs.begin(), subs.end(),
                [id](const Subscriber& s) { return s.id == id; }), subs.end());
        }
    }

    // ── Publish (immediate) ──────────────────────────────────────────

    template<typename EventT>
    void publish(const EventT& event,
                 EventPriority priority = EventPriority::Normal)
    {
        recordHistory(event, priority);
        dispatchNow<EventT>(event);
        triggerChains<EventT>(event);
    }

    // ── Delayed publish ──────────────────────────────────────────────
    // Event is dispatched when update(t) is called with t >= triggerTime.

    template<typename EventT>
    void publishDelayed(const EventT& event,
                        float         delaySeconds,
                        EventPriority priority = EventPriority::Normal)
    {
        DelayedEvent de;
        de.triggerTime = currentTime_ + delaySeconds;
        de.priority    = static_cast<int>(priority);
        de.type        = std::type_index(typeid(EventT));
        de.data        = event;
        de.dispatch    = [this, ev = event, pr = priority]() {
            recordHistory(ev, pr);
            dispatchNow<EventT>(ev);
            triggerChains<EventT>(ev);
        };
        delayedQueue_.push(std::move(de));
    }

    // Cancel all pending delayed events of a given type.
    template<typename EventT>
    void cancelDelayed() {
        auto target = std::type_index(typeid(EventT));
        // Rebuild queue excluding matching type
        std::priority_queue<DelayedEvent,
                            std::vector<DelayedEvent>,
                            DelayedCmp> newQ;
        while (!delayedQueue_.empty()) {
            auto& top = delayedQueue_.top();
            if (top.type != target)
                newQ.push(top);
            delayedQueue_.pop();
        }
        delayedQueue_ = std::move(newQ);
    }

    // ── Event chains ─────────────────────────────────────────────────
    // When EventA is published and predicate passes, EventB is automatically
    // published.  Chain dispatch does NOT recursively re-enter the same chain
    // to prevent infinite loops.

    // Chain with optional transform: A → optional<B>
    template<typename EventA, typename EventB>
    void addChain(std::function<std::optional<EventB>(const EventA&)> transform) {
        chains_[std::type_index(typeid(EventA))].push_back(
            [this, tr = std::move(transform)](const std::any& e) {
                auto res = tr(std::any_cast<const EventA&>(e));
                if (res) publish<EventB>(*res);
            });
    }

    // Unconditional chain: A always produces B
    template<typename EventA, typename EventB>
    void addChain(std::function<EventB(const EventA&)> transform) {
        chains_[std::type_index(typeid(EventA))].push_back(
            [this, tr = std::move(transform)](const std::any& e) {
                publish<EventB>(tr(std::any_cast<const EventA&>(e)));
            });
    }

    // Conditional passthrough chain: A fires B only when predicate holds
    template<typename EventA, typename EventB>
    void addChainIf(std::function<bool(const EventA&)>  predicate,
                    std::function<EventB(const EventA&)> transform)
    {
        chains_[std::type_index(typeid(EventA))].push_back(
            [this, pred = std::move(predicate),
                   tr   = std::move(transform)](const std::any& e) {
                const auto& ev = std::any_cast<const EventA&>(e);
                if (pred(ev)) publish<EventB>(tr(ev));
            });
    }

    // ── History ──────────────────────────────────────────────────────

    void setHistoryCapacity(size_t cap) {
        historyCapacity_ = cap;
        while (history_.size() > historyCapacity_) history_.pop_front();
    }

    size_t historyCapacity() const { return historyCapacity_; }

    // All events of type T (newest last)
    template<typename EventT>
    std::vector<const EventRecord*> getHistory() const {
        std::vector<const EventRecord*> out;
        auto target = std::type_index(typeid(EventT));
        for (auto& r : history_)
            if (r.type == target) out.push_back(&r);
        return out;
    }

    // Events of type T matching filter
    template<typename EventT>
    std::vector<const EventRecord*> getHistory(
        std::function<bool(const EventT&)> filter) const
    {
        std::vector<const EventRecord*> out;
        auto target = std::type_index(typeid(EventT));
        for (auto& r : history_) {
            if (r.type != target) continue;
            if (filter(*r.as<EventT>())) out.push_back(&r);
        }
        return out;
    }

    // Events of type T since a given simulation time
    template<typename EventT>
    std::vector<const EventRecord*> getHistorySince(float sinceTime) const {
        std::vector<const EventRecord*> out;
        auto target = std::type_index(typeid(EventT));
        for (auto& r : history_)
            if (r.type == target && r.timestamp >= sinceTime)
                out.push_back(&r);
        return out;
    }

    // Most recent event of type T, or nullptr
    template<typename EventT>
    const EventT* lastEvent() const {
        auto target = std::type_index(typeid(EventT));
        for (auto it = history_.rbegin(); it != history_.rend(); ++it)
            if (it->type == target) return it->as<EventT>();
        return nullptr;
    }

    const std::deque<EventRecord>& fullHistory() const { return history_; }
    void clearHistory()                                 { history_.clear(); }

    // ── Update — tick delayed queue ───────────────────────────────────

    void update(float currentTime) {
        currentTime_ = currentTime;
        while (!delayedQueue_.empty() &&
               delayedQueue_.top().triggerTime <= currentTime_) {
            // Copy dispatch fn out before pop (top is const)
            auto dispatch = delayedQueue_.top().dispatch;
            delayedQueue_.pop();
            dispatch();
        }
    }

    // ── Housekeeping ─────────────────────────────────────────────────

    void clear() {
        subscribers_.clear();
        chains_.clear();
        while (!delayedQueue_.empty()) delayedQueue_.pop();
        history_.clear();
    }

    // Remove all subscribers for a given type
    template<typename EventT>
    void clearSubscribers() {
        subscribers_.erase(std::type_index(typeid(EventT)));
    }

    // Number of pending delayed events
    size_t pendingDelayedCount() const { return delayedQueue_.size(); }

    float currentTime() const { return currentTime_; }

private:
    // ── Internal types ───────────────────────────────────────────────

    struct Subscriber {
        SubscriptionId id       = 0;
        int            priority = static_cast<int>(EventPriority::Normal);
        std::function<void(const std::any&)> callback;
        std::function<bool(const std::any&)> filter; // may be null
    };

    struct DelayedEvent {
        float           triggerTime = 0.f;
        int             priority    = static_cast<int>(EventPriority::Normal);
        std::type_index type        = typeid(void);
        std::any        data;
        std::function<void()> dispatch;
    };

    struct DelayedCmp {
        bool operator()(const DelayedEvent& a, const DelayedEvent& b) const {
            if (a.triggerTime != b.triggerTime)
                return a.triggerTime > b.triggerTime;   // min-heap by time
            return a.priority > b.priority;             // tie-break: lower int wins
        }
    };

    // ── Helpers ──────────────────────────────────────────────────────

    static void sortByPriority(std::vector<Subscriber>& vec) {
        std::stable_sort(vec.begin(), vec.end(),
            [](const Subscriber& a, const Subscriber& b) {
                return a.priority < b.priority;
            });
    }

    template<typename EventT>
    void dispatchNow(const EventT& event) {
        auto it = subscribers_.find(std::type_index(typeid(EventT)));
        if (it == subscribers_.end()) return;
        std::any wrapped = event;
        // Snapshot: copy sub list to tolerate unsubscribe-during-callback
        auto subs = it->second;
        for (auto& sub : subs) {
            if (sub.filter && !sub.filter(wrapped)) continue;
            sub.callback(wrapped);
        }
    }

    template<typename EventT>
    void triggerChains(const EventT& event) {
        auto it = chains_.find(std::type_index(typeid(EventT)));
        if (it == chains_.end()) return;
        std::any wrapped = event;
        for (auto& chain : it->second) chain(wrapped);
    }

    template<typename EventT>
    void recordHistory(const EventT& event, EventPriority priority) {
        if (historyCapacity_ == 0) return;
        if (history_.size() >= historyCapacity_) history_.pop_front();
        EventRecord rec;
        rec.type      = std::type_index(typeid(EventT));
        rec.data      = event;
        rec.timestamp = currentTime_;
        rec.priority  = priority;
        history_.push_back(std::move(rec));
    }

    // ── Members ──────────────────────────────────────────────────────

    SubscriptionId nextId_ = 1;

    std::unordered_map<std::type_index,
                       std::vector<Subscriber>> subscribers_;

    std::unordered_map<std::type_index,
                       std::vector<std::function<void(const std::any&)>>> chains_;

    std::priority_queue<DelayedEvent,
                        std::vector<DelayedEvent>,
                        DelayedCmp> delayedQueue_;

    std::deque<EventRecord> history_;
    size_t historyCapacity_ = 256;
    float  currentTime_     = 0.f;
};

// ─── ScopedSubscription — method bodies (need full EventBus) ─────────

inline void ScopedSubscription::unsubscribe() {
    if (bus_ && id_ != kInvalidSubscription) {
        bus_->unsubscribe(id_);
        id_  = kInvalidSubscription;
        bus_ = nullptr;
    }
}

} // namespace npc
