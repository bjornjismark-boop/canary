/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_quest.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <cstdint>
	#include <string>
	#include <vector>
#endif

enum class BotQuestStepType : uint8_t { ApproachNpc, SendDialogue, AwaitDialogue, TravelRegion, UseObject, KillCreature, CollectItem, DeliverItem, BuyItem, WithdrawItem, VerifyProgress, VerifyReward, Finish, Cancel };
enum class BotQuestExecutionState : uint8_t { Idle, CheckingPrerequisites, Traveling, StartingDialogue, AwaitingDialogue, UsingObject, EngagingTarget, CollectingItem, DeliveringItem, VerifyingProgress, VerifyingReward, Checkpointing, Completed, Suspended, Backoff, Failed, Cancelled, Dead };
enum class BotQuestStepResult : uint8_t { Succeeded, Pending, NoProgress, UnexpectedProgress, PrerequisiteLost, NpcUnavailable, DialogueRejected, RouteUnavailable, ObjectUnavailable, TargetUnavailable, ItemUnavailable, DeliveryRejected, RewardUnverified, TimedOut, RetryScheduled, RetryExhausted, Suspended, Dead, Cancelled };
enum class BotQuestExecutionFailure : uint8_t { None, InvalidLifecycle, ObservationStale, InvalidPlan, PolicyRejected, BudgetExceeded, RetryExhausted, Death, Cancelled };

struct BotQuestStep {
	BotQuestStepType type = BotQuestStepType::VerifyProgress;
	BotMissionId missionId = 0;
	uint32_t stableTargetId = 0;
	uint16_t itemTypeId = 0;
	uint32_t requiredCount = 0;
	std::string configuredName;
	std::string configuredPhrase;
	Position region;
	uint8_t radius = 0;
	BotMissionState expectedMissionState = BotMissionState::Unknown;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	auto operator<=>(const BotQuestStep &) const = default;
};

struct BotQuestPlan {
	BotQuestId questId = 0;
	BotMissionId missionId = 0;
	uint64_t revision = 0;
	std::vector<BotQuestStep> steps;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};

struct BotQuestExecutionPolicy {
	uint16_t maximumSteps = 32;
	uint16_t maximumOperations = 128;
	uint8_t maximumRetries = 3;
	uint64_t maximumObservationAge = 8;
	uint64_t retryBackoff = 1;
	uint64_t maximumBackoff = 16;
};

struct BotQuestCheckpoint {
	BotQuestId questId = 0;
	BotMissionId missionId = 0;
	uint16_t verifiedStepIndex = 0;
	BotMissionState missionState = BotMissionState::Unknown;
	uint64_t planRevision = 0;
	uint64_t observationRevision = 0;
	uint8_t retryCount = 0;
	uint64_t retryDeadline = 0;
	BotQuestExecutionFailure failure = BotQuestExecutionFailure::None;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	auto operator<=>(const BotQuestCheckpoint &) const = default;
};

struct BotQuestStepObservation {
	uint64_t revision = 0;
	uint64_t clock = 0;
	uint64_t observationAge = 0;
	bool placed = true;
	bool survivalRequired = false;
	bool dead = false;
	bool cancelled = false;
	bool prerequisitesEligible = true;
	bool targetAvailable = true;
	bool actionAccepted = false;
	bool authoritativeProgress = false;
	bool unexpectedProgress = false;
	bool visibleDialogueResponse = false;
	bool regionReached = false;
	bool itemAdded = false;
	bool itemRemoved = false;
	bool deathObserved = false;
	BotMissionState missionState = BotMissionState::Unknown;
	BotQuestVerificationResult reward = BotQuestVerificationResult::Pending;
};

struct BotQuestExecutionResult {
	BotQuestExecutionState state = BotQuestExecutionState::Idle;
	BotQuestStepResult stepResult = BotQuestStepResult::Pending;
	BotQuestExecutionFailure failure = BotQuestExecutionFailure::None;
	BotQuestCheckpoint checkpoint;
	uint16_t operations = 0;
	[[nodiscard]] bool terminal() const { return state == BotQuestExecutionState::Completed || state == BotQuestExecutionState::Failed || state == BotQuestExecutionState::Cancelled || state == BotQuestExecutionState::Dead; }
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	auto operator<=>(const BotQuestExecutionResult &) const = default;
};

class BotQuestExecution final {
public:
	[[nodiscard]] static BotQuestExecutionResult start(const BotQuestPlan &, const BotQuestObservation &, const BotQuestEligibility &, const BotQuestExecutionPolicy & = {});
	[[nodiscard]] static BotQuestExecutionResult advance(const BotQuestPlan &, BotQuestExecutionResult, const BotQuestStepObservation &, const BotQuestExecutionPolicy & = {});
	[[nodiscard]] static BotQuestExecutionResult reconstruct(const BotQuestPlan &, BotQuestCheckpoint, const BotQuestObservation &, const BotQuestExecutionPolicy & = {});
	[[nodiscard]] static BotQuestExecutionResult retry(BotQuestExecutionResult, BotQuestStepResult, const BotQuestExecutionPolicy & = {}, uint64_t now = 0);
	[[nodiscard]] static BotQuestExecutionState stateFor(BotQuestStepType);
};
