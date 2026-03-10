#pragma once
// Thread Safety — primitives and thread-safe wrappers for NPC systems.
//
// Threading model assumed:
//   Main thread   — Active NPC ticks, input, rendering, EventBus callbacks
//   Worker thread(s) — Background/Dormant ticks, pathfinding, autosave
//
// Contents:
//   SpinLock                  CAS spin lock — fast, short critical sections
//   ReadWriteLock             std::shared_mutex RAII wrapper
//   ThreadSafeQueue<T>        MPMC unbounded queue
//   DeferredDispatcher        Cross-thread event accumulator → drain on main
//   ThreadSafeEventBus        EventBus + deferred cross-thread publish
//   ThreadSafeSpatialIndex    SpatialIndex + RW lock
//   ThreadSafeSharedBlackboard SharedBlackboard + RW lock
//   TaskScheduler             Fixed worker thread pool + priority queue
//   ParallelNPCTicker         Distributes Background/Dormant ticks to workers

#include "../event/event_system.hpp"
#include "../world/spatial_index.hpp"
#include "../ai/shared_blackboard.hpp"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <queue>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <cassert>
#include <string>
#include <sstream>

namespace npc {

// ═══════════════════════════════════════════════════════════════════════
// SpinLock — CAS-based, best for very short critical sections
// ═══════════════════════════════════════════════════════════════════════

class SpinLock {
public:
    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire))
            std::this_thread::yield();
    }
    void unlock() noexcept { flag_.clear(std::memory_order_release); }
    bool try_lock() noexcept {
        return !flag_.test_and_set(std::memory_order_acquire);
    }
private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// ─── RAII guards ──────────────────────────────────────────────────────

template<typename Mutex>
struct LockGuard {
    explicit LockGuard(Mutex& m) : m_(m) { m_.lock(); }
    ~LockGuard()                          { m_.unlock(); }
    LockGuard(const LockGuard&)            = delete;
    LockGuard& operator=(const LockGuard&) = delete;
private: Mutex& m_;
};

struct ReadLock {
    explicit ReadLock(std::shared_mutex& m) : m_(m) { m_.lock_shared(); }
    ~ReadLock()                                      { m_.unlock_shared(); }
    ReadLock(const ReadLock&)            = delete;
    ReadLock& operator=(const ReadLock&) = delete;
private: std::shared_mutex& m_;
};

struct WriteLock {
    explicit WriteLock(std::shared_mutex& m) : m_(m) { m_.lock(); }
    ~WriteLock()                                      { m_.unlock(); }
    WriteLock(const WriteLock&)            = delete;
    WriteLock& operator=(const WriteLock&) = delete;
private: std::shared_mutex& m_;
};

// ═══════════════════════════════════════════════════════════════════════
// ThreadSafeQueue<T> — MPMC unbounded queue
// ═══════════════════════════════════════════════════════════════════════

template<typename T>
class ThreadSafeQueue {
public:
    void push(T item) {
        {
            LockGuard<std::mutex> g(mu_);
            q_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Non-blocking pop — returns false if empty
    bool pop(T& out) {
        LockGuard<std::mutex> g(mu_);
        if (q_.empty()) return false;
        out = std::move(q_.front()); q_.pop();
        return true;
    }

    // Blocking pop with timeout (milliseconds)
    bool popWait(T& out, int timeoutMs = 100) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                          [this]{ return !q_.empty(); }))
            return false;
        out = std::move(q_.front()); q_.pop();
        return true;
    }

    // Drain all items into vector (non-blocking)
    std::vector<T> drainAll() {
        LockGuard<std::mutex> g(mu_);
        std::vector<T> out;
        out.reserve(q_.size());
        while (!q_.empty()) { out.push_back(std::move(q_.front())); q_.pop(); }
        return out;
    }

    bool   empty() const { LockGuard<std::mutex> g(mu_); return q_.empty(); }
    size_t size()  const { LockGuard<std::mutex> g(mu_); return q_.size(); }

    void notifyAll() { cv_.notify_all(); }

private:
    mutable std::mutex        mu_;
    std::condition_variable   cv_;
    std::queue<T>             q_;
};

// ═══════════════════════════════════════════════════════════════════════
// DeferredDispatcher — accumulate events from any thread;
//                      flush to EventBus on main thread.
// ═══════════════════════════════════════════════════════════════════════
// Pattern:
//   Worker calls: dispatcher.publish<CombatEvent>(e)  ← thread-safe
//   Main calls:   dispatcher.flush(eventBus)          ← dispatches all queued events

class DeferredDispatcher {
public:
    // Enqueue an event from any thread
    template<typename EventT>
    void publish(const EventT& event) {
        auto wrapper = [ev = event](EventBus& bus) {
            bus.publish(ev);
        };
        queue_.push(std::move(wrapper));
    }

    // Flush all queued events to the EventBus (call from main thread only)
    int flush(EventBus& bus) {
        auto items = queue_.drainAll();
        for (auto& fn : items) fn(bus);
        return static_cast<int>(items.size());
    }

    bool   empty()    const { return queue_.empty(); }
    size_t pending()  const { return queue_.size(); }

private:
    ThreadSafeQueue<std::function<void(EventBus&)>> queue_;
};

// ═══════════════════════════════════════════════════════════════════════
// ThreadSafeEventBus — EventBus + thread-safe publish + deferred dispatch
// ═══════════════════════════════════════════════════════════════════════
// subscribe() and unsubscribe() must be called from the main thread only
// (subscription list is not lock-protected for read — main thread owns it).
// publish() from any thread:
//   • Main thread: direct dispatch (fast path)
//   • Worker thread: deferred into DeferredDispatcher (safe)
// drainDeferred() must be called from main thread each frame.

class ThreadSafeEventBus {
public:
    explicit ThreadSafeEventBus() : mainThreadId_(std::this_thread::get_id()) {}

    // ── Subscribe (main thread only) ────────────────────────────────

    template<typename EventT>
    SubscriptionId subscribe(std::function<void(const EventT&)> cb,
                              EventPriority priority = EventPriority::Normal,
                              std::function<bool(const EventT&)> filter = nullptr) {
        assert(isMainThread() && "subscribe() must be called from main thread");
        return bus_.subscribe<EventT>(std::move(cb), priority, std::move(filter));
    }

    template<typename EventT>
    ScopedSubscription subscribeScoped(
        std::function<void(const EventT&)> cb,
        EventPriority priority = EventPriority::Normal,
        std::function<bool(const EventT&)> filter = nullptr)
    {
        assert(isMainThread());
        return bus_.subscribeScoped<EventT>(std::move(cb), priority, std::move(filter));
    }

    void unsubscribe(SubscriptionId id) {
        assert(isMainThread());
        bus_.unsubscribe(id);
    }

    // ── Publish (any thread) ─────────────────────────────────────────

    template<typename EventT>
    void publish(const EventT& event,
                 EventPriority priority = EventPriority::Normal) {
        if (isMainThread()) {
            bus_.publish(event, priority);
        } else {
            deferred_.publish(event);
        }
    }

    // ── Delayed publish (main thread only) ──────────────────────────

    template<typename EventT>
    void publishDelayed(const EventT& event, float delaySecs,
                         EventPriority priority = EventPriority::Normal) {
        assert(isMainThread());
        bus_.publishDelayed(event, delaySecs, priority);
    }

    // ── Flush deferred + advance delayed queue (main thread) ─────────

    void update(float currentTime) {
        assert(isMainThread());
        deferred_.flush(bus_);
        bus_.update(currentTime);
    }

    // ── History / chain (main thread only) ──────────────────────────

    template<typename EventA, typename EventB>
    void addChain(std::function<std::optional<EventB>(const EventA&)> transform) {
        assert(isMainThread());
        bus_.addChain<EventA,EventB>(std::move(transform));
    }

    template<typename EventT>
    std::vector<const EventRecord*> getHistory() const { return bus_.getHistory<EventT>(); }
    template<typename EventT>
    const EventT* lastEvent() const { return bus_.lastEvent<EventT>(); }

    void setHistoryCapacity(size_t n) { bus_.setHistoryCapacity(n); }

    // ── Access underlying bus (main thread) ──────────────────────────

    EventBus&       inner()       { return bus_; }
    const EventBus& inner() const { return bus_; }

    // DeferredDispatcher for worker threads that want to enqueue events
    DeferredDispatcher& dispatcher() { return deferred_; }

    size_t pendingDeferred() const { return deferred_.pending(); }

private:
    bool isMainThread() const {
        return std::this_thread::get_id() == mainThreadId_;
    }

    EventBus           bus_;
    DeferredDispatcher deferred_;
    std::thread::id    mainThreadId_;
};

// ═══════════════════════════════════════════════════════════════════════
// ThreadSafeSpatialIndex — SpatialIndex + RW lock
// ═══════════════════════════════════════════════════════════════════════
// Multiple readers OK, single writer exclusive.
// update() / remove() — write lock (typically called from main thread)
// nearby() / closest() etc. — read lock (can be called from workers)

class ThreadSafeSpatialIndex {
public:
    explicit ThreadSafeSpatialIndex(float cellSize = 16.f) : idx_(cellSize) {}

    // ── Write operations ─────────────────────────────────────────────

    void update(EntityId id, Vec2 pos) {
        WriteLock lk(mu_);
        idx_.update(id, pos);
    }
    void remove(EntityId id) {
        WriteLock lk(mu_);
        idx_.remove(id);
    }
    void clear() {
        WriteLock lk(mu_);
        idx_.clear();
    }

    // ── Read operations (concurrent-safe) ────────────────────────────

    std::vector<EntityId> nearby(Vec2 center, float radius) const {
        ReadLock lk(mu_);
        return idx_.nearby(center, radius);
    }
    std::vector<EntityId> nearbyExcept(Vec2 center, float radius,
                                        EntityId ex) const {
        ReadLock lk(mu_);
        return idx_.nearbyExcept(center, radius, ex);
    }
    std::optional<SpatialHit> closest(Vec2 pos, float maxDist=1e9f) const {
        ReadLock lk(mu_);
        return idx_.closest(pos, maxDist);
    }
    std::vector<SpatialHit> nearbyWithDist(Vec2 center, float r) const {
        ReadLock lk(mu_);
        return idx_.nearbyWithDist(center, r);
    }
    std::vector<SpatialHit> nearestN(Vec2 pos, size_t n,
                                      float maxDist=1e9f) const {
        ReadLock lk(mu_);
        return idx_.nearestN(pos, n, maxDist);
    }
    size_t countNearby(Vec2 c, float r) const {
        ReadLock lk(mu_);
        return idx_.countNearby(c, r);
    }
    bool anyNearby(Vec2 c, float r) const {
        ReadLock lk(mu_);
        return idx_.anyNearby(c, r);
    }
    size_t size() const {
        ReadLock lk(mu_);
        return idx_.size();
    }

    // Unsafe direct access — caller holds external lock
    SpatialIndex& unsafeInner() { return idx_; }

private:
    mutable std::shared_mutex mu_;
    SpatialIndex              idx_;
};

// ═══════════════════════════════════════════════════════════════════════
// ThreadSafeSharedBlackboard — SharedBlackboard + RW lock
// ═══════════════════════════════════════════════════════════════════════

class ThreadSafeSharedBlackboard {
public:
    // ── Write ────────────────────────────────────────────────────────

    template<typename T>
    void set(const std::string& key, const T& value,
             float t=0.f, float ttl=-1.f, EntityId writer=INVALID_ENTITY) {
        WriteLock lk(mu_);
        bb_.set(key, value, t, ttl, writer);
    }

    template<typename T>
    bool setIfAbsent(const std::string& key, const T& value,
                     float t=0.f, float ttl=-1.f, EntityId writer=INVALID_ENTITY) {
        WriteLock lk(mu_);
        return bb_.setIfAbsent(key, value, t, ttl, writer);
    }

    void setAny(const std::string& key, const std::any& value,
                float t=0.f, float ttl=-1.f, EntityId writer=INVALID_ENTITY) {
        WriteLock lk(mu_);
        bb_.setAny(key, value, t, ttl, writer);
    }

    void remove(const std::string& key) {
        WriteLock lk(mu_);
        bb_.remove(key);
    }

    int pruneExpired(float t) {
        WriteLock lk(mu_);
        return bb_.pruneExpired(t);
    }

    void clear() { WriteLock lk(mu_); bb_.clear(); }

    // ── Read ─────────────────────────────────────────────────────────

    template<typename T>
    std::optional<T> get(const std::string& key, float t=0.f) const {
        ReadLock lk(mu_);
        return bb_.get<T>(key, t);
    }

    template<typename T>
    T getOr(const std::string& key, const T& def, float t=0.f) const {
        ReadLock lk(mu_);
        return bb_.getOr<T>(key, def, t);
    }

    bool has(const std::string& key, float t=0.f) const {
        ReadLock lk(mu_);
        return bb_.has(key, t);
    }

    uint64_t version(const std::string& key) const {
        ReadLock lk(mu_);
        return bb_.version(key);
    }

    std::vector<std::string> keysWithPrefix(const std::string& pfx,
                                             float t=0.f) const {
        ReadLock lk(mu_);
        return bb_.keysWithPrefix(pfx, t);
    }

    size_t size(float t=0.f) const {
        ReadLock lk(mu_);
        return bb_.size(t);
    }

    // Watchers — subscribe from any thread, callbacks run under write lock
    SharedBlackboard::WatcherId watch(const std::string& prefix,
                                       SharedBlackboard::ChangeCallback cb) {
        WriteLock lk(mu_);
        return bb_.watch(prefix, std::move(cb));
    }
    void unwatch(SharedBlackboard::WatcherId id) {
        WriteLock lk(mu_);
        bb_.unwatch(id);
    }

    // Unsafe direct access (caller must hold external lock)
    SharedBlackboard&       unsafeInner()       { return bb_; }
    const SharedBlackboard& unsafeInner() const { return bb_; }

    std::shared_mutex& mutex() { return mu_; }

private:
    mutable std::shared_mutex mu_;
    SharedBlackboard          bb_;
};

// ═══════════════════════════════════════════════════════════════════════
// TaskScheduler — fixed worker thread pool with priority queue
// ═══════════════════════════════════════════════════════════════════════

enum class TaskPriority : int { High = 0, Normal = 1, Low = 2 };

class TaskScheduler {
public:
    explicit TaskScheduler(size_t numThreads = 0) {
        size_t n = numThreads > 0
            ? numThreads
            : std::max(1u, std::thread::hardware_concurrency() - 1);
        running_ = true;
        workers_.reserve(n);
        for (size_t i = 0; i < n; ++i)
            workers_.emplace_back(&TaskScheduler::workerLoop, this);
    }

    ~TaskScheduler() { shutdown(); }

    // Non-copyable, non-moveable (owns threads)
    TaskScheduler(const TaskScheduler&)            = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    // ── Submit ───────────────────────────────────────────────────────

    // Submit a void task (fire-and-forget)
    void submit(std::function<void()> fn,
                TaskPriority priority = TaskPriority::Normal)
    {
        {
            std::unique_lock<std::mutex> lk(mu_);
            tasks_.push({std::move(fn), static_cast<int>(priority),
                         taskCounter_++});
        }
        cv_.notify_one();
    }

    // Submit a task returning T — get a std::future<T>
    template<typename F, typename R = std::invoke_result_t<F>>
    std::future<R> submitAsync(F&& fn,
                                TaskPriority priority = TaskPriority::Normal)
    {
        auto promise = std::make_shared<std::promise<R>>();
        auto future  = promise->get_future();

        submit([p = std::move(promise), f = std::forward<F>(fn)]() mutable {
            try {
                if constexpr (std::is_void_v<R>) {
                    f(); p->set_value();
                } else {
                    p->set_value(f());
                }
            } catch (...) {
                p->set_exception(std::current_exception());
            }
        }, priority);

        return future;
    }

    // ── Control ──────────────────────────────────────────────────────

    void shutdown() {
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (!running_) return;
            running_ = false;
        }
        cv_.notify_all();
        for (auto& t : workers_) if (t.joinable()) t.join();
        workers_.clear();
    }

    // Block until all currently queued tasks are done
    void waitAll() {
        std::unique_lock<std::mutex> lk(mu_);
        done_.wait(lk, [this]{ return tasks_.empty() && activeCount_==0; });
    }

    size_t pending()      const {
        std::unique_lock<std::mutex> lk(mu_);
        return tasks_.size();
    }
    size_t workerCount()  const { return workers_.size(); }
    bool   isRunning()    const { return running_.load(); }

private:
    struct Task {
        std::function<void()> fn;
        int                   priority;
        uint64_t              seq;       // FIFO tie-break
        bool operator>(const Task& o) const {
            return priority != o.priority ? priority > o.priority
                                          : seq     > o.seq;
        }
    };

    void workerLoop() {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this]{ return !tasks_.empty() || !running_; });
                if (!running_ && tasks_.empty()) return;
                task = std::move(const_cast<Task&>(tasks_.top()));
                tasks_.pop();
                ++activeCount_;
            }
            task.fn();
            {
                std::unique_lock<std::mutex> lk(mu_);
                --activeCount_;
            }
            done_.notify_all();
        }
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable done_;

    std::priority_queue<Task, std::vector<Task>, std::greater<Task>> tasks_;
    std::vector<std::thread> workers_;
    std::atomic<bool>        running_{false};
    uint64_t                 taskCounter_  = 0;
    int                      activeCount_  = 0;
};

// ═══════════════════════════════════════════════════════════════════════
// ParallelNPCTicker — distributes Background/Dormant ticks to workers
// ═══════════════════════════════════════════════════════════════════════
// Usage:
//   ParallelNPCTicker ticker(scheduler, world, dispatcher);
//   ticker.tickBackground(ids, accumDts);   // async
//   ticker.tickDormant(ids, accumDts);      // async
//   ticker.wait();                          // sync before frame end
//   dispatcher.flush(eventBus);             // deliver deferred events

class ParallelNPCTicker {
public:
    using TickFn = std::function<void(NPC&, float dt)>;

    ParallelNPCTicker(TaskScheduler&     scheduler,
                       GameWorld&          world,
                       DeferredDispatcher& dispatcher)
        : scheduler_(scheduler)
        , world_(world)
        , dispatcher_(dispatcher)
    {}

    // Set custom tick functions (defaults: emotions + movement)
    void setBackgroundFn(TickFn fn) { backgroundFn_ = std::move(fn); }
    void setDormantFn   (TickFn fn) { dormantFn_    = std::move(fn); }

    // Enqueue background ticks (non-blocking)
    void tickBackground(const std::vector<EntityId>& ids,
                        const std::unordered_map<EntityId,float>& accumDts)
    {
        for (EntityId id : ids) {
            float dt = 0.f;
            auto it = accumDts.find(id);
            if (it != accumDts.end()) dt = it->second;

            futures_.push_back(scheduler_.submitAsync<void>(
                [this, id, dt]() {
                    NPC* npc = world_.findNPC(id);
                    if (!npc) return;
                    if (backgroundFn_) {
                        backgroundFn_(*npc, dt);
                    } else {
                        defaultBackgroundTick(*npc, dt);
                    }
                },
                TaskPriority::Normal));
        }
    }

    // Enqueue dormant ticks (non-blocking)
    void tickDormant(const std::vector<EntityId>& ids,
                     const std::unordered_map<EntityId,float>& accumDts)
    {
        for (EntityId id : ids) {
            float dt = 0.f;
            auto it = accumDts.find(id);
            if (it != accumDts.end()) dt = it->second;

            futures_.push_back(scheduler_.submitAsync<void>(
                [this, id, dt]() {
                    NPC* npc = world_.findNPC(id);
                    if (!npc) return;
                    if (dormantFn_) {
                        dormantFn_(*npc, dt);
                    } else {
                        defaultDormantTick(*npc, dt);
                    }
                },
                TaskPriority::Low));
        }
    }

    // Block until all submitted ticks have finished
    void wait() {
        for (auto& f : futures_) {
            try { f.get(); } catch (...) {}
        }
        futures_.clear();
    }

    bool allDone() const { return futures_.empty(); }

private:
    // Default tick logic — must be safe to run concurrently
    // (no shared mutable state beyond the NPC's own systems)
    static void defaultBackgroundTick(NPC& npc, float dt) {
        npc.emotions.update(dt);
        npc.updateMovement(dt);
        npc.schedule.updateFatigue(dt);
    }

    static void defaultDormantTick(NPC& npc, float dt) {
        npc.emotions.update(dt);
    }

    TaskScheduler&      scheduler_;
    GameWorld&          world_;
    DeferredDispatcher& dispatcher_;
    TickFn              backgroundFn_;
    TickFn              dormantFn_;
    std::vector<std::future<void>> futures_;
};

// ═══════════════════════════════════════════════════════════════════════
// ThreadSafetyAudit — debug helper to detect races at runtime
// ═══════════════════════════════════════════════════════════════════════
// In debug builds, wrap critical sections with OwnershipGuard to assert
// that a resource is only accessed from one thread at a time.

#ifndef NDEBUG
class OwnershipGuard {
public:
    void acquire(const char* location = "") {
        std::thread::id expected{};
        std::thread::id current = std::this_thread::get_id();
        if (!owner_.compare_exchange_strong(expected, current,
                                             std::memory_order_acq_rel)) {
            if (expected != current) {
                std::ostringstream ss;
                ss << "RACE DETECTED at " << location
                   << ": owned by thread " << expected
                   << " but accessed by " << current;
                throw std::runtime_error(ss.str());
            }
        }
        ++depth_;
    }
    void release() {
        if (--depth_ == 0)
            owner_.store({}, std::memory_order_release);
    }
    struct Scope {
        Scope(OwnershipGuard& g, const char* loc) : g_(g) { g_.acquire(loc); }
        ~Scope() { g_.release(); }
    private: OwnershipGuard& g_;
    };
private:
    std::atomic<std::thread::id> owner_{};
    int depth_ = 0;
};
#define NPC_THREAD_GUARD(guard) OwnershipGuard::Scope _tg_##__LINE__((guard), __func__)
#else
struct OwnershipGuard {
    struct Scope { Scope(OwnershipGuard&, const char*) {} };
};
#define NPC_THREAD_GUARD(guard) (void)0
#endif

} // namespace npc
