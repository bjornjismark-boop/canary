/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_quest_execution.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <cstdint>
	#include <string>
	#include <vector>
#endif

using BotGoalId = uint64_t;
enum class BotGoalType : uint8_t { Survive, Recover, Resupply, EquipUpgrade, CompleteConfiguredQuest, GainConfiguredProgress, HuntConfiguredRegion, SellConfiguredLoot, DepositConfiguredItems, ReturnHomeRegion, IdleSafely, Unsupported };
enum class BotGoalPriority : uint8_t { Idle, OptionalEconomy, Progression, Equipment, ActiveQuest, UrgentResupply, CriticalSurvival, DeathRecovery };
enum class BotGoalStatus : uint8_t { Unavailable, Eligible, Selected, Active, Blocked, Suspended, Completed, Failed, Cancelled, Stale };
enum class BotGoalPredicateType : uint8_t { Alive, SurvivalSafe, SupplyAvailable, QuestActive, RegionAvailable, EquipmentAvailable, ProgressBelowTarget, CheckpointValid };
enum class BotPlanStepType : uint8_t { Observe, Travel, Interact, Fight, Recover, Loot, Resupply, Equip, Converse, VerifyQuest, SaveLogout, Complete };
enum class BotPlannerFailure : uint8_t { None, InvalidLifecycle, ObservationStale, UnsupportedGoal, GoalLimit, DepthLimit, StepLimit, ReplanLimit, NoEligibleGoal, CheckpointInvalid, Cancelled };
enum class BotReplanReason : uint8_t { None, ObservationStale, SurvivalOverride, Death, SupplyDepleted, TargetUnavailable, RouteUnavailable, QuestStateChanged, NpcUnavailable, ItemUnavailable, PolicyChanged, CheckpointInvalid, RetryExhausted, Timeout };

struct BotGoalPredicate { BotGoalPredicateType type = BotGoalPredicateType::Alive; bool expected = true; uint64_t value = 0; auto operator<=>(const BotGoalPredicate &) const = default; };
struct BotGoal {
	BotGoalId id = 0; BotGoalType type = BotGoalType::IdleSafely; BotGoalPriority priority = BotGoalPriority::Idle; BotGoalStatus status = BotGoalStatus::Unavailable;
	uint64_t policyRevision = 0; uint64_t configuredTargetId = 0; Position configuredRegion; uint32_t targetValue = 0; uint8_t failures = 0; std::vector<BotGoalPredicate> predicates;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	auto operator<=>(const BotGoal &) const = default;
};
struct BotPlanStep { BotPlanStepType type = BotPlanStepType::Observe; uint8_t depth = 0; uint64_t configuredTargetId = 0; Position region; bool safeBoundary = false; [[nodiscard]] bool containsWorldOwnership() const { return false; } auto operator<=>(const BotPlanStep &) const = default; };
struct BotPlan { BotGoalId goalId = 0; BotGoalType goalType = BotGoalType::IdleSafely; uint64_t revision = 0; std::vector<BotPlanStep> steps; bool depthLimitExceeded = false; bool stepLimitExceeded = false; [[nodiscard]] bool containsWorldOwnership() const { return false; } auto operator<=>(const BotPlan &) const = default; };
struct BotPlanCheckpoint {
	BotGoalId goalId = 0; BotGoalType goalType = BotGoalType::IdleSafely; uint16_t verifiedStepIndex = 0; uint64_t policyRevision = 0; uint64_t observationRevision = 0; uint64_t sessionGeneration = 0; uint8_t retryCount = 0; uint8_t failureCount = 0; bool safeSaveBoundary = false;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	auto operator<=>(const BotPlanCheckpoint &) const = default;
};
struct BotPlannerPolicy { uint16_t maximumGoals = 32; uint8_t maximumPlanDepth = 4; uint16_t maximumSteps = 32; uint8_t maximumReplans = 4; uint8_t maximumGoalRetries = 3; uint16_t hysteresis = 10; uint64_t revision = 1; };
struct BotPlannerObservation {
	uint64_t revision = 0; uint64_t sessionGeneration = 0; bool placed = true; bool dead = false; bool survivalCritical = false; bool supplyUrgent = false; bool activeQuest = false; bool equipmentUpgrade = false; bool progressionConfigured = false; bool optionalLoot = false; bool regionAvailable = true; bool routeAvailable = true; bool checkpointValid = true;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};
struct BotPlannerDecision {
	BotGoalId goalId = 0; BotGoalType goalType = BotGoalType::IdleSafely; BotGoalStatus status = BotGoalStatus::Unavailable; BotPlan plan; BotPlanCheckpoint checkpoint; BotReplanReason replanReason = BotReplanReason::None; BotPlannerFailure failure = BotPlannerFailure::None; uint8_t replanCount = 0;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	[[nodiscard]] bool terminal() const { return status == BotGoalStatus::Completed || status == BotGoalStatus::Failed || status == BotGoalStatus::Cancelled; }
	auto operator<=>(const BotPlannerDecision &) const = default;
};

class BotPlanner final {
public:
	[[nodiscard]] static BotPlannerDecision select(std::vector<BotGoal>, const BotPlannerObservation &, const BotPlannerPolicy & = {}, const BotPlannerDecision *current = nullptr);
	[[nodiscard]] static BotPlan expand(const BotGoal &, const BotPlannerPolicy & = {});
	[[nodiscard]] static BotPlannerDecision verifyStep(BotPlannerDecision, uint16_t stepIndex, uint64_t observationRevision, bool verified);
	[[nodiscard]] static BotPlannerDecision replan(std::vector<BotGoal>, const BotPlannerObservation &, BotPlannerDecision, BotReplanReason, const BotPlannerPolicy & = {});
	[[nodiscard]] static BotPlannerDecision reconstruct(const BotPlannerDecision &, const BotPlannerObservation &, const BotPlannerPolicy & = {});
	[[nodiscard]] static uint16_t score(const BotGoal &, const BotPlannerObservation &, const BotPlannerPolicy & = {});
};
