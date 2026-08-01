/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_planner.hpp"

using BotPlanExecutionId = uint64_t;

enum class BotPlanExecutionState : uint8_t { Idle, PlanSelected, PreparingStep, Delegating, AwaitingSubsystem, VerifyingStep, Checkpointing, Suspended, Cancelling, Recovering, Replanning, Completed, Failed, Dead, Cancelled };
enum class BotPlanSubsystem : uint8_t { None, Observation, Navigation, Combat, Loot, Survival, Resupply, Equipment, Dialogue, Quest, Lifecycle };
enum class BotPlanStepOutcome : uint8_t { Succeeded, Pending, Suspended, NoEffect, Partial, UnexpectedState, PreconditionLost, SubsystemUnavailable, SubsystemRejected, ObservationStale, TimedOut, RetryScheduled, RetryExhausted, RecoveryScheduled, ReplanRequired, Dead, Cancelled, Failed };
enum class BotPlanArbitrationReason : uint8_t { Idle, OptionalWork, Progression, Verification, UrgentResupply, MandatoryQuest, Shutdown, PendingBoundary, CriticalSurvival, Death };
enum class BotPlanFailure : uint8_t { None, RouteUnavailable, DynamicBlocker, TargetUnavailable, CombatFailure, SurvivalInterruption, CorpseUnavailable, SupplyDepletion, ShopUnavailable, DepotUnavailable, NpcUnavailable, QuestStateChanged, ItemUnavailable, StaleCheckpoint, SessionChanged, Timeout, RetryExhausted, InvalidLifecycle, ConflictingIntent, BudgetExhausted };
enum class BotPlanRecoveryDecision : uint8_t { None, RetryStep, ChooseAlternative, SuspendPlan, ReplacePlan, ReturnSafeRegion, SaveLogout, TerminalFailure };

struct BotPlanExecutionBudget { uint16_t maximumTicks = 1024; uint8_t maximumRetries = 3; uint8_t maximumAlternatives = 2; uint8_t maximumRecoveries = 4; uint8_t maximumGoalSwitches = 4; uint8_t failureMemoryLimit = 8; };
struct BotPlanStepIntent { BotPlanExecutionId executionId = 0; uint16_t stepIndex = 0; BotPlanStepType type = BotPlanStepType::Observe; BotPlanSubsystem subsystem = BotPlanSubsystem::None; uint64_t configuredTargetId = 0; Position region; uint64_t observationRevision = 0; uint64_t sessionGeneration = 0; bool operator==(const BotPlanStepIntent &) const = default; };
struct BotPlanStepExecution { BotPlanStepIntent intent; BotPlanStepOutcome outcome = BotPlanStepOutcome::Pending; uint8_t retries = 0; uint8_t alternatives = 0; bool accepted = false; bool terminalObserved = false; bool postconditionVerified = false; bool operator==(const BotPlanStepExecution &) const = default; };
struct BotPlanArbitration { BotPlanArbitrationReason reason = BotPlanArbitrationReason::Idle; bool suspend = false; bool cancel = false; bool requiresBoundary = false; auto operator<=>(const BotPlanArbitration &) const = default; };
struct BotPlanFailureRecord { BotPlanFailure failure = BotPlanFailure::None; uint64_t targetSignature = 0; uint8_t count = 0; auto operator<=>(const BotPlanFailureRecord &) const = default; };
struct BotPlanExecutionObservation { uint64_t revision = 0; uint64_t sessionGeneration = 0; bool placed = false; bool dead = false; bool survivalCritical = false; bool pendingAuthoritativeAction = false; bool shutdownRequested = false; bool mandatoryQuest = false; bool supplyUrgent = false; bool subsystemAvailable = true; bool preconditionValid = true; auto operator<=>(const BotPlanExecutionObservation &) const = default; };
struct BotPlanExecution {
	BotPlanExecutionId id = 0; BotPlanExecutionState state = BotPlanExecutionState::Idle; BotPlannerDecision decision; std::optional<BotPlanStepExecution> delegatedStep; BotPlanArbitration arbitration; BotPlanFailure failure = BotPlanFailure::None; BotPlanRecoveryDecision recovery = BotPlanRecoveryDecision::None; BotPlanExecutionBudget budget; std::vector<BotPlanFailureRecord> failureMemory; uint16_t ticks = 0; uint8_t recoveries = 0; uint8_t goalSwitches = 0; uint64_t observationRevision = 0; uint64_t sessionGeneration = 0; bool freshObservationRequired = true;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	[[nodiscard]] bool terminal() const { return state == BotPlanExecutionState::Completed || state == BotPlanExecutionState::Failed || state == BotPlanExecutionState::Dead || state == BotPlanExecutionState::Cancelled; }
};

class BotPlanExecutor final {
public:
	[[nodiscard]] static BotPlanExecution start(BotPlanExecutionId, BotPlannerDecision, const BotPlanExecutionObservation &, const BotPlanExecutionBudget & = {});
	[[nodiscard]] static BotPlanArbitration arbitrate(const BotPlanExecution &, const BotPlanExecutionObservation &);
	[[nodiscard]] static BotPlanExecution delegate(BotPlanExecution, const BotPlanExecutionObservation &);
	[[nodiscard]] static BotPlanExecution observe(BotPlanExecution, BotPlanStepOutcome, uint64_t observationRevision, bool postconditionVerified);
	[[nodiscard]] static BotPlanExecution recover(BotPlanExecution, BotPlanFailure, uint64_t targetSignature = 0);
	[[nodiscard]] static BotPlanExecution cancel(BotPlanExecution);
	[[nodiscard]] static BotPlanSubsystem subsystem(BotPlanStepType);
};
