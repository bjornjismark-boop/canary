/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "game/movement/position.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <chrono>
	#include <cstdint>
	#include <optional>
	#include <string>
	#include <vector>
#endif

class Game;
class Player;

enum class BotCreatureKind : uint8_t {
	Player,
	Monster,
	Npc,
	Other,
};

struct BotCreatureObservation {
	uint32_t id = 0;
	BotCreatureKind kind = BotCreatureKind::Other;
	Position position;
	uint8_t healthPercent = 0;

	auto operator<=>(const BotCreatureObservation &) const = default;
};

struct BotItemReference {
	uint16_t typeId = 0;
	Position position;
	uint8_t stackPosition = 0;

	auto operator<=>(const BotItemReference &) const = default;
};

struct BotTileObservation {
	Position position;
	uint16_t groundTypeId = 0;
	uint16_t blockingItemTypeId = 0;
	uint32_t blockingCreatureId = 0;
	uint8_t harmfulFieldCombatType = 0;
	bool hasGround = false;
	bool terrainBlocked = false;
	bool hazardous = false;

	auto operator<=>(const BotTileObservation &) const = default;
};

struct BotObservation {
	uint32_t playerGuid = 0;
	uint32_t playerCreatureId = 0;
	Position position;
	int32_t health = 0;
	int32_t maxHealth = 0;
	uint32_t mana = 0;
	uint32_t maxMana = 0;
	uint32_t level = 0;
	uint64_t topologyRevision = 0;
	std::vector<BotCreatureObservation> visibleCreatures;
	std::vector<BotTileObservation> visibleTiles;

	auto operator<=>(const BotObservation &) const = default;
};

class BotPerception final {
public:
	[[nodiscard]] static std::optional<BotObservation> observe(const std::shared_ptr<Player> &player);
};

enum class BotActionType : uint8_t {
	Wait,
	Move,
	InspectCreature,
};

enum class BotActionReason : uint8_t {
	Idle,
	ObserveTarget,
	Retry,
};

struct BotAction {
	BotActionType type = BotActionType::Wait;
	BotActionReason reason = BotActionReason::Idle;
	uint32_t targetCreatureId = 0;
	Direction direction = DIRECTION_NONE;
	Position observedOrigin;
	Position observedDestination;
	uint64_t observationSignature = 0;

	auto operator<=>(const BotAction &) const = default;
};

enum class BotActionStatus : uint8_t {
	Succeeded,
	Pending,
	Rejected,
	RetryScheduled,
};

enum class BotActionFailure : uint8_t {
	None,
	InvalidLifecycle,
	InvalidTarget,
	RateLimited,
	TimedOut,
	WorldRejected,
	TickBudgetExceeded,
	InvalidDirection,
	DifferentFloor,
	OutsideKnownOrVisibleArea,
	StaleObservation,
};

enum class BotWalkability : uint8_t {
	Walkable,
	WalkableWithRisk,
	BlockedByTerrain,
	BlockedByItem,
	BlockedByCreature,
	InvalidDirection,
	DifferentFloor,
	OutsideKnownOrVisibleArea,
	StaleObservation,
	WorldRejected,
};

struct BotMovementCandidate {
	Position origin;
	Position destination;
	Direction direction = DIRECTION_NONE;
	uint64_t observationSignature = 0;

	auto operator<=>(const BotMovementCandidate &) const = default;
};

struct BotWalkabilityResult {
	BotMovementCandidate candidate;
	BotWalkability outcome = BotWalkability::InvalidDirection;
	uint32_t movementCost = 0;
	uint16_t blockingItemTypeId = 0;
	uint32_t blockingCreatureId = 0;
	uint8_t harmfulFieldCombatType = 0;
	uint16_t worldReturnValue = 0;
	uint8_t evaluatedTiles = 0;

	[[nodiscard]] bool walkable() const {
		return outcome == BotWalkability::Walkable || outcome == BotWalkability::WalkableWithRisk;
	}
	[[nodiscard]] bool containsWorldOwnership() const { return false; }

	auto operator<=>(const BotWalkabilityResult &) const = default;
};

struct BotActionResult {
	BotActionStatus status = BotActionStatus::Rejected;
	BotActionFailure failure = BotActionFailure::None;
	uint32_t attempts = 0;
	std::chrono::milliseconds retryAfter { 0 };
	std::optional<BotWalkabilityResult> walkability;

	[[nodiscard]] bool succeeded() const { return status == BotActionStatus::Succeeded; }
};

enum class BotRootState : uint8_t {
	Idle,
	Observe,
	Recover,
};

struct BotBlackboard {
	BotRootState state = BotRootState::Idle;
	std::optional<BotAction> pendingAction;
	uint32_t attempts = 0;
	std::chrono::milliseconds actionStartedAt { 0 };
	std::chrono::milliseconds nextActionAt { 0 };
	BotActionFailure lastFailure = BotActionFailure::None;
};

struct BotRuntimeLimits {
	std::chrono::milliseconds tickInterval { 100 };
	std::chrono::milliseconds actionInterval { 250 };
	std::chrono::milliseconds actionTimeout { 1000 };
	std::chrono::milliseconds initialBackoff { 200 };
	uint32_t maxAttempts = 3;
	size_t maxCreaturesPerTick = 64;
};
