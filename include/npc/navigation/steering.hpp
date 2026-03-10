#pragma once

#include "../core/types.hpp"
#include "../core/vec2.hpp"
#include <vector>
#include <functional>
#include <algorithm>
#include <optional>
#include <cmath>

namespace npc {

// ─── Steering Agent ───────────────────────────────────────────────────────────
struct SteeringAgent {
    EntityId id;
    Vec2     position;
    Vec2     velocity;
    float    radius    = 0.5f;
    float    maxSpeed  = 3.0f;
    float    maxForce  = 10.0f;
    int      priority  = 0;      // higher = others yield to this agent
};

// ─── Static Obstacle ──────────────────────────────────────────────────────────
struct SteeringObstacle {
    Vec2  center;
    float radius;
};

// ─── Per-agent Output ─────────────────────────────────────────────────────────
struct SteeringOutput {
    EntityId id;
    Vec2     steeringForce;
    Vec2     desiredVelocity;  // clamped to actualMaxSpeed
    float    actualMaxSpeed;   // throttled by crowd density
    bool     isBlocked  = false;
    bool     inQueue    = false;
    int      queueDepth = 0;   // 0 = leader, 1 = second in line, etc.
};

// ─── Post-step Position Correction ───────────────────────────────────────────
// Returned by resolveOverlaps(); caller moves NPCs by these deltas.
struct OverlapCorrection {
    EntityId id;
    Vec2     delta;   // push NPC by this amount to eliminate overlap
};

// ─── Config ───────────────────────────────────────────────────────────────────
struct SteeringConfig {
    // Arrival
    float arrivalSlowRadius  = 2.5f;
    float arrivalStopRadius  = 0.25f;

    // Separation (soft force)
    float separationRadius   = 1.8f;   // multiplier on sum-of-radii
    float separationWeight   = 2.0f;

    // TTC avoidance
    float ttcHorizon         = 3.0f;   // look-ahead seconds for collision
    float ttcWeight          = 3.0f;

    // Obstacle avoidance
    float obstacleProbeLen   = 2.5f;
    float obstacleWeight     = 3.5f;

    // Crowd / queue
    float crowdRadius        = 2.5f;   // density sampling radius
    float crowdSpeedFloor    = 0.15f;  // minimum speed fraction in dense crowd
    float queueFollowDist    = 1.2f;   // target gap to agent-ahead in queue (× radii)
    float queueFrontAngle    = 0.85f;  // cos(angle): agent counts as "ahead" if dot > this

    // Priority yielding
    float yieldBias          = 0.65f;

    // Hard overlap correction
    float overlapSlop        = 0.02f;  // ignore overlaps smaller than this
    float overlapCorrectFrac = 0.5f;   // push each overlapping pair apart by this fraction
};

// ─── SteeringSystem ───────────────────────────────────────────────────────────
class SteeringSystem {
public:
    explicit SteeringSystem(SteeringConfig cfg = {}) : cfg_(cfg) {}
    const SteeringConfig& config() const { return cfg_; }

    // ── 1. Arrive ─────────────────────────────────────────────────────────────
    Vec2 arrive(Vec2 pos, Vec2 goal, float maxSpeed) const {
        Vec2  d    = goal - pos;
        float dist = d.length();
        if (dist < cfg_.arrivalStopRadius) return {};
        float speed = (dist < cfg_.arrivalSlowRadius)
                      ? maxSpeed * (dist / cfg_.arrivalSlowRadius)
                      : maxSpeed;
        return d.normalized() * speed;
    }

    // ── 2. Separation (soft spring force) ────────────────────────────────────
    Vec2 separate(const SteeringAgent& self,
                  const std::vector<SteeringAgent>& others) const {
        Vec2 force;
        for (const auto& o : others) {
            if (o.id == self.id) continue;
            Vec2  diff    = self.position - o.position;
            float dist    = diff.length();
            float minDist = (self.radius + o.radius) * cfg_.separationRadius;
            if (dist >= minDist || dist < 1e-5f) continue;
            float strength = std::pow((minDist - dist) / minDist, 2.0f);
            force += diff.normalized() * strength * self.maxForce;
        }
        return force * cfg_.separationWeight;
    }

    // ── 3. TTC (Time-To-Collision) avoidance ──────────────────────────────────
    // Predicts future positions and steers away before overlap occurs.
    Vec2 ttcAvoid(const SteeringAgent& self,
                  const std::vector<SteeringAgent>& others) const {
        Vec2  force;
        for (const auto& o : others) {
            if (o.id == self.id) continue;
            float ttc = timeToCollision(self, o);
            if (ttc < 0.0f || ttc > cfg_.ttcHorizon) continue;

            // Urgency: closer to collision = stronger force
            float urgency = 1.0f - (ttc / cfg_.ttcHorizon);

            // Push direction: perpendicular to relative velocity, away from other
            Vec2 relVel  = self.velocity - o.velocity;
            Vec2 toOther = o.position - self.position;

            // Choose side that moves us most away from other's future position
            Vec2 futureOther = o.position + o.velocity * ttc;
            Vec2 awayDir     = (self.position - futureOther).normalized();

            // If zero (same position), pick perpendicular to velocity
            if (awayDir.lengthSquared() < 1e-6f) {
                awayDir = Vec2{-self.velocity.y, self.velocity.x}.normalized();
            }

            force += awayDir * urgency * self.maxForce * cfg_.ttcWeight;
        }
        return force;
    }

    // ── 4. Obstacle avoidance ─────────────────────────────────────────────────
    Vec2 avoidObstacles(const SteeringAgent& agent,
                        const std::vector<SteeringObstacle>& obstacles) const {
        if (agent.velocity.lengthSquared() < 1e-6f) return {};
        Vec2  dir        = agent.velocity.normalized();
        Vec2  ahead      = agent.position + dir * cfg_.obstacleProbeLen;
        Vec2  aheadMid   = agent.position + dir * (cfg_.obstacleProbeLen * 0.5f);

        float bestDist   = 1e9f;
        const SteeringObstacle* threat = nullptr;

        for (const auto& obs : obstacles) {
            float r = obs.radius + agent.radius;
            float d = std::min(ahead.distanceTo(obs.center),
                               aheadMid.distanceTo(obs.center));
            if (d < r && obs.center.distanceTo(agent.position) < bestDist) {
                bestDist = obs.center.distanceTo(agent.position);
                threat   = &obs;
            }
        }
        if (!threat) return {};
        Vec2 avoidDir = (ahead - threat->center).normalized();
        return avoidDir * agent.maxForce * cfg_.obstacleWeight;
    }

    // ── 5. Priority yielding ──────────────────────────────────────────────────
    Vec2 yieldToPriority(const SteeringAgent& self,
                         const std::vector<SteeringAgent>& others) const {
        Vec2 force;
        for (const auto& o : others) {
            if (o.id == self.id || o.priority <= self.priority) continue;
            Vec2  diff    = self.position - o.position;
            float dist    = diff.length();
            float minDist = (self.radius + o.radius) * cfg_.separationRadius * 1.5f;
            if (dist >= minDist || dist < 1e-5f) continue;
            float strength = (minDist - dist) / minDist * cfg_.yieldBias;
            force += diff.normalized() * strength * self.maxForce;
        }
        return force;
    }

    // ── 6. Crowd density → speed throttle ────────────────────────────────────
    // Counts agents within crowdRadius; more agents = slower max speed.
    float crowdThrottledSpeed(const SteeringAgent& self,
                               const std::vector<SteeringAgent>& others) const {
        int nearby = 0;
        for (const auto& o : others) {
            if (o.id == self.id) continue;
            if (self.position.distanceTo(o.position) < cfg_.crowdRadius)
                ++nearby;
        }
        if (nearby == 0) return self.maxSpeed;
        // Each extra neighbor reduces speed; floor at crowdSpeedFloor
        float density = std::min(static_cast<float>(nearby) / 6.0f, 1.0f);
        float throttle = 1.0f - density * (1.0f - cfg_.crowdSpeedFloor);
        return self.maxSpeed * throttle;
    }

    // ── 7. Queue / single-file detection ─────────────────────────────────────
    // Returns the closest agent that is between self and the goal (the "leader").
    struct QueueInfo {
        bool     inQueue    = false;
        EntityId leaderId   = INVALID_ENTITY;
        int      depth      = 0;   // position in the queue
        float    followSpeed= 0.0f;
    };

    QueueInfo detectQueue(const SteeringAgent& self,
                          const std::vector<SteeringAgent>& others,
                          Vec2 goal) const {
        QueueInfo info;
        Vec2  toGoal  = (goal - self.position).normalized();
        float distGoal= self.position.distanceTo(goal);
        float closest = 1e9f;

        for (const auto& o : others) {
            if (o.id == self.id) continue;
            Vec2  toOther = o.position - self.position;
            float dist    = toOther.length();
            if (dist > distGoal + 0.5f) continue; // other is past the goal

            // Is other in front of self (toward goal)?
            float dot = toGoal.dot(toOther.normalized());
            if (dot < cfg_.queueFrontAngle) continue;

            // Close enough to be "in queue"
            float gapNeeded = (self.radius + o.radius) * cfg_.queueFollowDist;
            if (dist < gapNeeded * 3.0f && dist < closest) {
                closest          = dist;
                info.inQueue     = true;
                info.leaderId    = o.id;
                info.followSpeed = o.velocity.length();
            }
        }

        if (info.inQueue) {
            // Count depth (how many agents between self and goal)
            for (const auto& o : others) {
                if (o.id == self.id || o.id == info.leaderId) continue;
                Vec2 toO = o.position - self.position;
                if (toGoal.dot(toO.normalized()) >= cfg_.queueFrontAngle &&
                    toO.length() < closest)
                    ++info.depth;
            }
        }
        return info;
    }

    // Queue follow force: maintain follow-distance behind the leader.
    Vec2 followQueue(const SteeringAgent& self,
                     const SteeringAgent& leader,
                     float throttledSpeed) const {
        float gapTarget = (self.radius + leader.radius) * cfg_.queueFollowDist;
        Vec2  toLeader  = leader.position - self.position;
        float dist      = toLeader.length();

        if (dist < gapTarget * 0.5f) {
            // Too close — brake hard
            return -self.velocity.normalized() * self.maxForce;
        }
        // Match leader speed from gap distance
        float desiredSpeed = std::min(throttledSpeed, leader.velocity.length());
        return toLeader.normalized() * desiredSpeed;
    }

    // ── Main update ───────────────────────────────────────────────────────────
    std::vector<SteeringOutput> update(
            const std::vector<SteeringAgent>& agents,
            const std::function<Vec2(EntityId)>& goalOf,
            const std::vector<SteeringObstacle>& obstacles = {}) const
    {
        std::vector<SteeringOutput> out;
        out.reserve(agents.size());

        for (const auto& agent : agents) {
            Vec2 goal = goalOf(agent.id);

            // Crowd throttle
            float topSpeed = crowdThrottledSpeed(agent, agents);

            // Queue detection
            QueueInfo qi = detectQueue(agent, agents, goal);

            Vec2 primary;
            if (qi.inQueue) {
                // Find leader snapshot
                const SteeringAgent* leader = nullptr;
                for (const auto& o : agents)
                    if (o.id == qi.leaderId) { leader = &o; break; }

                if (leader) {
                    primary = followQueue(agent, *leader, topSpeed);
                } else {
                    primary = arrive(agent.position, goal, topSpeed);
                }
            } else {
                primary = arrive(agent.position, goal, topSpeed);
            }

            Vec2 sep    = separate(agent, agents);
            Vec2 ttc    = ttcAvoid(agent, agents);
            Vec2 obs    = avoidObstacles(agent, obstacles);
            Vec2 yield  = yieldToPriority(agent, agents);

            Vec2 total  = primary + sep + ttc + obs + yield;
            if (total.length() > agent.maxForce)
                total = total.normalized() * agent.maxForce;

            Vec2 newVel = agent.velocity + total;
            if (newVel.length() > topSpeed)
                newVel = newVel.normalized() * topSpeed;

            bool arrived = agent.position.distanceTo(goal) < cfg_.arrivalStopRadius;
            if (arrived) newVel = {};

            SteeringOutput o;
            o.id             = agent.id;
            o.steeringForce  = total;
            o.desiredVelocity= newVel;
            o.actualMaxSpeed = topSpeed;
            o.isBlocked      = !arrived && newVel.lengthSquared() < 1e-4f
                               && total.lengthSquared() > 0.1f;
            o.inQueue        = qi.inQueue;
            o.queueDepth     = qi.depth;
            out.push_back(o);
        }
        return out;
    }

    // ── Hard overlap resolution (call AFTER integrating positions) ────────────
    // Pushes overlapping agents apart so they never visually intersect.
    // positions: mutable world positions, indexed same as agents.
    static std::vector<OverlapCorrection>
    resolveOverlaps(std::vector<Vec2>& positions,
                    const std::vector<SteeringAgent>& agents,
                    float slop = 0.02f, float fraction = 0.5f,
                    int iterations = 3)
    {
        std::vector<OverlapCorrection> corrections(agents.size(), {0, {}});
        for (int i = 0; i < static_cast<int>(agents.size()); ++i)
            corrections[i].id = agents[i].id;

        for (int iter = 0; iter < iterations; ++iter) {
            for (int i = 0; i < static_cast<int>(agents.size()); ++i) {
                for (int j = i + 1; j < static_cast<int>(agents.size()); ++j) {
                    Vec2  diff   = positions[i] - positions[j];
                    float dist   = diff.length();
                    float minD   = agents[i].radius + agents[j].radius;
                    float overlap= minD - dist;
                    if (overlap <= slop) continue;

                    Vec2 pushDir = (dist < 1e-5f)
                                   ? Vec2{1.0f, 0.0f}   // degenerate: push along X
                                   : diff.normalized();

                    // Higher priority agent moves less
                    float totalPrio = static_cast<float>(agents[i].priority +
                                                          agents[j].priority + 2);
                    float shareJ = (agents[i].priority + 1) / totalPrio;
                    float shareI = (agents[j].priority + 1) / totalPrio;

                    Vec2 pushI =  pushDir * overlap * shareI * fraction;
                    Vec2 pushJ = -pushDir * overlap * shareJ * fraction;

                    positions[i]       += pushI;
                    positions[j]       += pushJ;
                    corrections[i].delta += pushI;
                    corrections[j].delta += pushJ;
                }
            }
        }
        return corrections;
    }

    // ── Head-on resolution ────────────────────────────────────────────────────
    static std::pair<Vec2, Vec2> resolveHeadOn(
            const SteeringAgent& a, Vec2 goalA,
            const SteeringAgent& b, Vec2 goalB,
            float sideStep = 1.2f)
    {
        Vec2  dirA = (goalA - a.position).normalized();
        Vec2  dirB = (goalB - b.position).normalized();
        float dot  = dirA.dot(dirB * -1.0f);
        if (dot < 0.7f) return {goalA, goalB};

        Vec2 rightA = Vec2{-dirA.y,  dirA.x};
        Vec2 rightB = Vec2{ dirB.y, -dirB.x};
        return { goalA + rightA * sideStep, goalB + rightB * sideStep };
    }

    // ── Formation slot override ───────────────────────────────────────────────
    static Vec2 formationOverride(Vec2 agentPos, Vec2 slot,
                                   Vec2 force, float tolerance = 0.5f) {
        float d = agentPos.distanceTo(slot);
        if (d < tolerance)           return {};
        if (d < tolerance * 3.0f) {
            float blend = (d - tolerance) / (tolerance * 2.0f);
            return force * blend;
        }
        return force;
    }

private:
    // Time to collision between two agents moving at constant velocity.
    // Returns negative if no collision expected.
    static float timeToCollision(const SteeringAgent& a, const SteeringAgent& b) {
        Vec2  relPos = b.position - a.position;
        Vec2  relVel = b.velocity - a.velocity;
        float combinedR = a.radius + b.radius;

        float rvDotRv = relVel.dot(relVel);
        if (rvDotRv < 1e-6f) {
            // No relative movement — check static overlap
            return relPos.length() < combinedR ? 0.0f : -1.0f;
        }

        // Quadratic: |relPos + t*relVel|^2 = combinedR^2
        float rpDotRv = relPos.dot(relVel);
        float rpDotRp = relPos.dot(relPos);
        float disc = rpDotRv * rpDotRv - rvDotRv * (rpDotRp - combinedR * combinedR);
        if (disc < 0.0f) return -1.0f;  // no collision

        float t = -(rpDotRv + std::sqrt(disc)) / rvDotRv;
        return (t >= 0.0f) ? t : -1.0f;
    }

    SteeringConfig cfg_;
};

} // namespace npc
