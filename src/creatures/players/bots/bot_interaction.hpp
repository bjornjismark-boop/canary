/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_runtime.hpp"

enum class BotInteractionType : uint8_t {
	UseDoor, UseLadder, UseRopeSpot, UseHoleOrFloorOpening,
	WalkOntoTransition, UseTeleportOrPortal, UseGenericWorldObject
};

enum class BotTransitionFailure : uint8_t {
	None, InteractionRequired, NoTransition, Blocked, AccessDenied, MissingRequiredItem,
	InvalidTarget, StaleObservation, UnexpectedDestination, DifferentFloorNotAllowed,
	WorldRejected, TimedOut, RetryExhausted, InvalidLifecycle, UnsupportedInteraction, Cancelled
};

enum class BotInteractionOutcome : uint8_t {
	Succeeded, Pending, InteractionRequired, TransitionObserved, NoTransition, Blocked,
	AccessDenied, MissingRequiredItem, InvalidTarget, StaleObservation, UnexpectedDestination,
	DifferentFloorNotAllowed, WorldRejected, TimedOut, RetryScheduled, RetryExhausted,
	InvalidLifecycle, UnsupportedInteraction, Cancelled
};

enum class BotTransitionState : uint8_t {
	Idle, Approaching, InteractionPending, AwaitingTransition, VerifyingResult,
	Completed, ReplanRequired, Backoff, Failed, Cancelled
};

struct BotInteractionTarget {
	Position position;
	uint8_t stackPosition = 0;
	uint16_t itemTypeId = 0;
	uint64_t signature = 0;
	BotInteractionType category = BotInteractionType::UseGenericWorldObject;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	auto operator<=>(const BotInteractionTarget &) const = default;
};

struct BotDestinationRegion {
	Position minimum;
	Position maximum;
	[[nodiscard]] bool contains(const Position &position) const;
	auto operator<=>(const BotDestinationRegion &) const = default;
};

struct BotTransitionObservation {
	Position playerPosition;
	BotInteractionTarget target;
	uint64_t topologyRevision = 0;
	bool targetWalkable = false;
	bool interactionSupported = false;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};

struct BotTransitionRequest {
	BotInteractionTarget target;
	std::optional<Position> expectedDestination;
	std::optional<BotDestinationRegion> expectedRegion;
	bool allowDifferentFloor = true;
	uint16_t requiredItemTypeId = 0;
	uint32_t maxAttempts = 3;
	std::chrono::milliseconds timeout { 1000 };
	std::chrono::milliseconds initialBackoff { 100 };
	std::chrono::milliseconds maximumBackoff { 800 };
};

struct BotInteractionResult {
	BotInteractionOutcome outcome = BotInteractionOutcome::InvalidTarget;
	BotTransitionFailure failure = BotTransitionFailure::InvalidTarget;
	uint16_t worldReturnValue = 0;
	Position positionBefore;
	Position positionAfter;
	uint32_t attempts = 0;
	std::chrono::milliseconds retryAfter { 0 };
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};

struct BotTransitionResult : BotInteractionResult {
	BotTransitionState state = BotTransitionState::Idle;
	bool oldRouteInvalidated = false;
	bool newObservationRequired = false;
};

struct BotTransitionEdge {
	Position source;
	BotInteractionType interaction = BotInteractionType::WalkOntoTransition;
	uint16_t requiredItemTypeId = 0;
	std::optional<Position> expectedDestination;
	std::optional<BotDestinationRegion> expectedRegion;
	uint32_t cost = 0;
	uint8_t confidence = 0;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};

struct BotTransitionProgress {
	BotTransitionState state = BotTransitionState::Idle;
	BotTransitionRequest request;
	BotTransitionResult result;
	Position positionBefore;
	uint32_t attempts = 0;
	std::chrono::milliseconds startedAt { 0 };
	std::chrono::milliseconds nextAttemptAt { 0 };
};

class BotInteraction final {
public:
	[[nodiscard]] static uint64_t signature(const BotInteractionTarget &target);
	[[nodiscard]] static bool destinationAllowed(const BotTransitionRequest &request, const Position &before, const Position &after);
	[[nodiscard]] static std::chrono::milliseconds backoff(const BotTransitionRequest &request, uint32_t attempt);
	[[nodiscard]] static bool supported(BotInteractionType type);
};
