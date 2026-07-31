/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_runtime.hpp"

enum class BotRouteState : uint8_t { Idle, Planning, Ready, StepPending, ReplanRequired, Backoff, Arrived, Failed, Cancelled };
enum class BotRouteReason : uint8_t {
	None, RouteFound, AlreadyAtDestination, DestinationUnknown, DestinationOutsideKnownArea,
	NoRoute, NodeBudgetExceeded, RouteLengthExceeded, PlanningBudgetExceeded, StaleTopology,
	DynamicBlocker, MovementRejected, NoProgress, ReplanLimitExceeded, InvalidLifecycle, Cancelled
};

struct BotRouteLimits {
	uint32_t maxExpandedNodes = 128;
	uint32_t maxRouteLength = 32;
	uint32_t maxPlanningOperations = 4096;
	uint32_t maxReplans = 3;
	uint32_t maxNoProgress = 2;
	std::chrono::milliseconds initialBackoff { 100 };
	std::chrono::milliseconds maximumBackoff { 800 };
};

struct BotRouteRequest { Position origin; Position destination; BotRouteLimits limits; };
struct BotRouteResult {
	BotRouteReason reason = BotRouteReason::NoRoute;
	std::vector<Position> positions;
	std::vector<uint64_t> signatures;
	uint64_t topologyRevision = 0;
	uint32_t expandedNodes = 0;
	uint32_t planningOperations = 0;
	[[nodiscard]] bool found() const { return reason == BotRouteReason::RouteFound || reason == BotRouteReason::AlreadyAtDestination; }
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};

struct BotRouteProgress {
	BotRouteState state = BotRouteState::Idle;
	BotRouteReason reason = BotRouteReason::None;
	Position destination;
	Position expectedOrigin;
	Position expectedNext;
	Position actualPosition;
	size_t routeIndex = 0;
	uint32_t consecutiveNoProgress = 0;
	uint32_t totalReplans = 0;
	std::chrono::milliseconds lastProgressAt { 0 };
	std::chrono::milliseconds backoffDeadline { 0 };
};

class BotNavigation final {
public:
	static constexpr uint32_t CardinalCost = 10;
	static constexpr uint32_t DiagonalCost = 35;
	static constexpr uint32_t HazardCost = 180;
	static constexpr uint32_t BlockedCost = std::numeric_limits<uint32_t>::max();
	static constexpr uint8_t MaximumEvaluatedTiles = 1;

	[[nodiscard]] static bool isLocalDirection(Direction direction);
	[[nodiscard]] static BotWalkabilityResult assess(const BotObservation &observation, Direction direction);
	[[nodiscard]] static BotWalkabilityResult assess(const BotObservation &observation, const Position &destination);
	[[nodiscard]] static uint64_t signature(const BotTileObservation &tile);
	[[nodiscard]] static BotRouteResult findRoute(const BotObservation &observation, const BotRouteRequest &request);
	[[nodiscard]] static std::chrono::milliseconds backoff(const BotRouteLimits &limits, uint32_t replanAttempt);
	static void observeProgress(BotRouteProgress &progress, const Position &actual, std::chrono::milliseconds now, const BotRouteLimits &limits);
};
