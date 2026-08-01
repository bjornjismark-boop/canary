/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_navigation.hpp"
#include "creatures/players/bots/bot_survival.hpp"

class Item;
class Player;

enum class BotCorpseOwnership : uint8_t { Unrestricted, Self, Party, RewardEligible, Denied };
enum class BotLootEligibility : uint8_t { Eligible, InvalidLifecycle, InvalidCorpse, StaleObservation, NotVisible, OutsideKnownArea, TooFar, Unreachable, NoLootRights, CorpseExpired, ContainerUnavailable, PolicyRejected, CapacityInsufficient, EvaluationBudgetExceeded };
enum class BotLootFailure : uint8_t { None, InvalidLifecycle, InvalidCorpse, StaleObservation, NotVisible, OutsideKnownArea, TooFar, Unreachable, NoLootRights, CorpseExpired, ContainerUnavailable, PolicyRejected, CapacityInsufficient, EvaluationBudgetExceeded };

struct BotCorpseSignature {
	uint64_t value = 0;
	auto operator<=>(const BotCorpseSignature &) const = default;
};

struct BotLootItemObservation {
	uint16_t itemTypeId = 0;
	uint32_t count = 0;
	uint32_t weight = 0;
	uint16_t stackPosition = 0;
	uint8_t depth = 0;
	bool stackable = false;
	bool nestedContainer = false;
	uint64_t signature = 0;
	auto operator<=>(const BotLootItemObservation &) const = default;
};

struct BotCorpseObservation {
	uint64_t observationRevision = 0;
	Position position;
	uint16_t corpseItemTypeId = 0;
	BotCorpseSignature signature;
	uint32_t sourceCreatureId = 0;
	uint32_t ownerCreatureId = 0;
	BotCorpseOwnership ownership = BotCorpseOwnership::Denied;
	uint32_t remainingDecayMilliseconds = 0;
	uint16_t containerCapacity = 0;
	uint16_t containerSize = 0;
	std::vector<BotLootItemObservation> items;
	bool containsWorldOwnership = false;
	auto operator<=>(const BotCorpseObservation &) const = default;
};

struct BotLootRule {
	uint16_t itemTypeId = 0;
	uint8_t valueCategory = 0;
	uint16_t priority = 0;
	auto operator<=>(const BotLootRule &) const = default;
};

struct BotLootPolicy {
	std::vector<BotLootRule> rules;
	uint16_t maxDistance = 8;
	uint16_t maxCorpseCandidates = 16;
	uint16_t maxItemCandidates = 64;
	uint8_t maxNestedDepth = 1;
	uint32_t maxProjectedWeight = std::numeric_limits<uint32_t>::max();
	bool rejectUnconfiguredItems = true;
};

struct BotLootCandidate {
	BotLootItemObservation item;
	BotLootEligibility eligibility = BotLootEligibility::PolicyRejected;
	uint8_t valueCategory = 0;
	uint16_t priority = 0;
	uint32_t projectedWeight = 0;
	uint64_t score = 0;
	auto operator<=>(const BotLootCandidate &) const = default;
};

struct BotLootSelectionResult {
	BotLootEligibility eligibility = BotLootEligibility::InvalidCorpse;
	BotLootFailure failure = BotLootFailure::InvalidCorpse;
	BotCorpseObservation corpse;
	std::vector<BotLootCandidate> candidates;
	std::optional<BotLootCandidate> selected;
	uint16_t evaluatedItems = 0;
};

class BotLoot final {
public:
	static BotLootSelectionResult observe(const std::shared_ptr<Player> &player, const BotObservation &world, const Position &position, uint32_t sourceCreatureId, BotCorpseSignature expectedSignature, const BotLootPolicy &policy = {});
	static BotLootSelectionResult select(const BotCorpseObservation &corpse, uint32_t freeCapacity, const BotLootPolicy &policy = {});
	static BotCorpseSignature signature(const BotCorpseObservation &corpse);
};

enum class BotLootExecutionState : uint8_t { Idle, ApproachingCorpse, OpeningCorpse, ObservingContents, SelectingItem, TransferPending, VerifyingTransfer, CorpseEmpty, CapacityBlocked, Completed, RetryBackoff, Failed, Cancelled };
enum class BotLootPriorityState : uint8_t { LootActive, SurvivalInterruptRequested, WaitingForAuthoritativeBoundary, LootSuspended, HealingPriority, FleePriority, DeathOverride, FreshObservationRequired, LootResumeAllowed, LootAbandoned };
enum class BotLootTransferOutcome : uint8_t { Succeeded, Partial, Pending, NoEffect, CorpseOpened, AlreadyOpen, CapacityInsufficient, DestinationFull, NoLootRights, StaleCorpse, StaleItem, StaleDestination, ItemGone, CorpseExpired, WorldRejected, TimedOut, RetryScheduled, RetryExhausted, Cancelled };
enum class BotLootTransferFailure : uint8_t { None, InvalidLifecycle, CapacityInsufficient, DestinationFull, NoLootRights, StaleCorpse, StaleItem, StaleDestination, ItemGone, CorpseExpired, WorldRejected, TimedOut, RetryExhausted, Cancelled };

struct BotInventorySlotObservation {
	uint8_t slot = 0;
	uint16_t itemTypeId = 0;
	uint32_t count = 0;
	bool container = false;
	uint64_t signature = 0;
	auto operator<=>(const BotInventorySlotObservation &) const = default;
};

struct BotContainerObservation {
	uint16_t itemTypeId = 0;
	uint16_t capacity = 0;
	uint16_t size = 0;
	uint8_t depth = 0;
	uint64_t signature = 0;
	auto operator<=>(const BotContainerObservation &) const = default;
};

struct BotInventoryObservation {
	uint64_t revision = 0;
	uint64_t signature = 0;
	uint32_t freeCapacity = 0;
	std::vector<BotInventorySlotObservation> slots;
	std::vector<BotContainerObservation> containers;
	bool containsWorldOwnership = false;
	auto operator<=>(const BotInventoryObservation &) const = default;
};

struct BotCapacityAssessment {
	uint32_t freeCapacity = 0;
	uint32_t requestedWeight = 0;
	uint32_t movableCount = 0;
	bool sufficient = false;
	auto operator<=>(const BotCapacityAssessment &) const = default;
};

struct BotLootTransferRequest {
	Position corpsePosition;
	uint32_t sourceCreatureId = 0;
	BotCorpseSignature corpseSignature;
	uint16_t itemTypeId = 0;
	uint64_t itemSignature = 0;
	uint32_t count = 0;
	uint64_t destinationSignature = 0;
	auto operator<=>(const BotLootTransferRequest &) const = default;
};

struct BotLootTransferPolicy {
	uint8_t maxAttempts = 1;
	uint8_t maxInventoryContainers = 16;
	uint8_t maxInventoryDepth = 2;
	std::chrono::milliseconds timeout { 2000 };
	std::chrono::milliseconds initialBackoff { 200 };
	std::chrono::milliseconds maximumBackoff { 1600 };
};

struct BotLootTransferResult {
	BotLootTransferOutcome outcome = BotLootTransferOutcome::WorldRejected;
	BotLootTransferFailure failure = BotLootTransferFailure::None;
	BotLootExecutionState state = BotLootExecutionState::Idle;
	BotLootTransferRequest request;
	uint32_t sourceBefore = 0;
	uint32_t sourceAfter = 0;
	uint32_t destinationBefore = 0;
	uint32_t destinationAfter = 0;
	uint32_t movedCount = 0;
	uint8_t attempts = 0;
	bool ordinaryOpenAccepted = false;
	bool ordinaryMoveDispatched = false;
};

struct BotLootExecutionProgress {
	BotLootExecutionState state = BotLootExecutionState::Idle;
	std::optional<BotLootTransferRequest> request;
	uint32_t sourceBefore = 0;
	uint32_t destinationBefore = 0;
	uint8_t attempts = 0;
	std::chrono::milliseconds startedAt { 0 };
	std::chrono::milliseconds nextAttemptAt { 0 };
	bool containsWorldOwnership = false;
	BotLootPriorityState priority = BotLootPriorityState::LootActive;
	uint64_t corpseObservationRevision = 0;
	uint64_t inventoryObservationRevision = 0;
	bool freshCorpseRequired = false;
	bool freshInventoryRequired = false;
	BotLootTransferOutcome authoritativeBoundaryOutcome = BotLootTransferOutcome::Pending;
	uint32_t authoritativeBoundaryMovedCount = 0;
};

class BotLootTransfer final {
public:
	static BotInventoryObservation observeInventory(const std::shared_ptr<Player> &player, uint8_t maxContainers = 16, uint8_t maxDepth = 2);
	static BotCapacityAssessment assessCapacity(uint32_t freeCapacity, uint32_t unitWeight, uint32_t requestedCount);
	static std::chrono::milliseconds retryDelay(uint8_t attempt, const BotLootTransferPolicy &policy);
	static bool legalTransition(BotLootExecutionState from, BotLootExecutionState to);
	static BotLootPriorityState requestSurvivalInterrupt(BotLootExecutionProgress &, BotSurvivalDecision, bool dead = false);
	static bool mayDispatch(const BotLootExecutionProgress &);
	static bool observeFresh(BotLootExecutionProgress &, uint64_t corpseRevision, uint64_t inventoryRevision, bool corpseEligible);
};
