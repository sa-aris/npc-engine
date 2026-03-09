#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <functional>
#include <algorithm>
#include <queue>
#include <optional>
#include "blackboard.hpp"

namespace npc {

// ─── World State ────────────────────────────────────────────────────
// Key-value pairs representing conditions about the world

using GOAPValue = std::variant<bool, int, float>;
using GOAPState = std::unordered_map<std::string, GOAPValue>;

inline bool statesSatisfied(const GOAPState& required, const GOAPState& current) {
    for (const auto& [key, val] : required) {
        auto it = current.find(key);
        if (it == current.end()) return false;
        if (it->second != val) return false;
    }
    return true;
}

// ─── GOAP Goal ──────────────────────────────────────────────────────

struct GOAPGoal {
    std::string name;
    float priority = 1.0f;
    GOAPState desiredState;

    // Optional dynamic priority based on blackboard
    std::function<float(const Blackboard&)> priorityFn;

    float getPriority(const Blackboard& bb) const {
        return priorityFn ? priorityFn(bb) : priority;
    }

    bool isSatisfied(const GOAPState& current) const {
        return statesSatisfied(desiredState, current);
    }
};

// ─── GOAP Action ────────────────────────────────────────────────────

struct GOAPAction {
    std::string name;
    float cost = 1.0f;
    GOAPState preconditions;
    GOAPState effects;

    // Which FSM state to enter when executing this action
    std::string fsmState;

    // Optional: dynamic cost modifier
    std::function<float(const Blackboard&)> costFn;

    float getCost(const Blackboard& bb) const {
        return costFn ? costFn(bb) : cost;
    }

    bool canExecute(const GOAPState& current) const {
        return statesSatisfied(preconditions, current);
    }

    GOAPState applyEffects(const GOAPState& current) const {
        GOAPState result = current;
        for (const auto& [key, val] : effects) {
            result[key] = val;
        }
        return result;
    }
};

// ─── GOAP Plan ──────────────────────────────────────────────────────

struct GOAPPlan {
    std::string goalName;
    std::vector<const GOAPAction*> actions;
    size_t currentStep = 0;
    float totalCost = 0.0f;

    bool isComplete() const { return currentStep >= actions.size(); }

    const GOAPAction* currentAction() const {
        if (currentStep < actions.size()) return actions[currentStep];
        return nullptr;
    }

    void advanceStep() {
        if (currentStep < actions.size()) ++currentStep;
    }
};

// ─── GOAP Planner (A* backward search) ─────────────────────────────

class GOAPPlanner {
public:
    std::optional<GOAPPlan> plan(
            const GOAPState& currentState,
            const GOAPGoal& goal,
            const std::vector<GOAPAction>& availableActions,
            const Blackboard& bb,
            int maxDepth = 10) const {

        if (goal.isSatisfied(currentState)) return std::nullopt;

        // A* node: world state + path taken
        struct Node {
            GOAPState state;
            float gCost = 0.0f;
            float hCost = 0.0f;
            float fCost() const { return gCost + hCost; }
            std::vector<int> actionIndices; // indices into availableActions

            bool operator>(const Node& o) const { return fCost() > o.fCost(); }
        };

        // Heuristic: count unsatisfied conditions
        auto heuristic = [&goal](const GOAPState& state) -> float {
            float h = 0.0f;
            for (const auto& [key, val] : goal.desiredState) {
                auto it = state.find(key);
                if (it == state.end() || it->second != val) h += 1.0f;
            }
            return h;
        };

        std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

        Node start;
        start.state = currentState;
        start.gCost = 0.0f;
        start.hCost = heuristic(currentState);
        open.push(start);

        int iterations = 0;
        constexpr int maxIterations = 200;

        while (!open.empty() && iterations++ < maxIterations) {
            Node current = open.top();
            open.pop();

            // Goal reached?
            if (goal.isSatisfied(current.state)) {
                GOAPPlan result;
                result.goalName = goal.name;
                result.totalCost = current.gCost;
                for (int idx : current.actionIndices) {
                    result.actions.push_back(&availableActions[idx]);
                }
                return result;
            }

            // Max depth check
            if (static_cast<int>(current.actionIndices.size()) >= maxDepth) continue;

            // Expand neighbors
            for (int i = 0; i < static_cast<int>(availableActions.size()); ++i) {
                const auto& action = availableActions[i];
                if (!action.canExecute(current.state)) continue;

                GOAPState newState = action.applyEffects(current.state);
                float newG = current.gCost + action.getCost(bb);

                Node next;
                next.state = std::move(newState);
                next.gCost = newG;
                next.hCost = heuristic(next.state);
                next.actionIndices = current.actionIndices;
                next.actionIndices.push_back(i);
                open.push(std::move(next));
            }
        }

        return std::nullopt; // no plan found
    }

    // Plan for highest-priority unsatisfied goal
    std::optional<GOAPPlan> planBest(
            const GOAPState& currentState,
            const std::vector<GOAPGoal>& goals,
            const std::vector<GOAPAction>& availableActions,
            const Blackboard& bb) const {

        // Sort goals by dynamic priority (descending)
        std::vector<const GOAPGoal*> sorted;
        for (const auto& g : goals) sorted.push_back(&g);
        std::sort(sorted.begin(), sorted.end(),
            [&bb](const GOAPGoal* a, const GOAPGoal* b) {
                return a->getPriority(bb) > b->getPriority(bb);
            });

        for (const auto* goal : sorted) {
            if (goal->isSatisfied(currentState)) continue;
            auto result = plan(currentState, *goal, availableActions, bb);
            if (result) return result;
        }
        return std::nullopt;
    }
};

// ─── GOAP Agent (manages goals, actions, active plan) ───────────────

class GOAPAgent {
public:
    std::vector<GOAPGoal> goals;
    std::vector<GOAPAction> actions;

    // Build world state from blackboard
    std::function<GOAPState(const Blackboard&)> worldStateBuilder;

    // Called when a new plan step begins — returns the FSM state to enter
    std::function<void(const std::string& fsmState, Blackboard&)> onActionStart;

    // Check if current action's effects are met (step complete)
    std::function<bool(const GOAPAction&, const Blackboard&)> isActionComplete;

    void update(Blackboard& bb) {
        if (!worldStateBuilder) return;

        GOAPState worldState = worldStateBuilder(bb);

        // Replan if no active plan or current plan's goal is already satisfied
        if (!activePlan_ || activePlan_->isComplete()) {
            replan(worldState, bb);
        }

        if (!activePlan_) return;

        const auto* action = activePlan_->currentAction();
        if (!action) return;

        // Check if current step is complete
        if (isActionComplete && isActionComplete(*action, bb)) {
            activePlan_->advanceStep();

            // If plan complete, try replanning for next goal
            if (activePlan_->isComplete()) {
                replan(worldState, bb);
                if (!activePlan_) return;
                action = activePlan_->currentAction();
                if (!action) return;
            }

            // Start next action
            if (onActionStart) {
                onActionStart(action->fsmState, bb);
            }
        }
    }

    void replan(const GOAPState& worldState, Blackboard& bb) {
        activePlan_ = planner_.planBest(worldState, goals, actions, bb);
        if (activePlan_ && onActionStart) {
            if (auto* action = activePlan_->currentAction()) {
                onActionStart(action->fsmState, bb);
            }
        }
    }

    void invalidatePlan() { activePlan_ = std::nullopt; }

    const std::optional<GOAPPlan>& activePlan() const { return activePlan_; }
    bool hasPlan() const { return activePlan_.has_value() && !activePlan_->isComplete(); }

    std::string currentGoalName() const {
        return activePlan_ ? activePlan_->goalName : "";
    }

    std::string currentActionName() const {
        if (activePlan_) {
            if (auto* a = activePlan_->currentAction()) return a->name;
        }
        return "";
    }

private:
    GOAPPlanner planner_;
    std::optional<GOAPPlan> activePlan_;
};

} // namespace npc
