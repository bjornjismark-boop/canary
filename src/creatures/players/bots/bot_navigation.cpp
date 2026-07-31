/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_navigation.hpp"

#include "utils/tools.hpp"

namespace {
	void mix(uint64_t &value, uint64_t part) {
		value ^= part;
		value *= 1099511628211ULL;
	}

	constexpr std::array<Direction, 8> routeDirections {
		DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST,
		DIRECTION_NORTHEAST, DIRECTION_SOUTHEAST, DIRECTION_SOUTHWEST, DIRECTION_NORTHWEST
	};

	uint32_t heuristic(const Position &from, const Position &to) {
		const auto dx = static_cast<uint32_t>(std::abs(Position::getOffsetX(from, to)));
		const auto dy = static_cast<uint32_t>(std::abs(Position::getOffsetY(from, to)));
		return (dx + dy) * BotNavigation::CardinalCost;
	}
}

bool BotNavigation::isLocalDirection(Direction direction) {
	return direction >= DIRECTION_NORTH && direction <= DIRECTION_LAST;
}

uint64_t BotNavigation::signature(const BotTileObservation &tile) {
	uint64_t value = 1469598103934665603ULL;
	mix(value, tile.position.x);
	mix(value, tile.position.y);
	mix(value, tile.position.z);
	mix(value, tile.groundTypeId);
	mix(value, tile.blockingItemTypeId);
	mix(value, tile.blockingCreatureId);
	mix(value, tile.harmfulFieldCombatType);
	mix(value, tile.hasGround);
	mix(value, tile.terrainBlocked);
	mix(value, tile.hazardous);
	return value;
}

BotWalkabilityResult BotNavigation::assess(const BotObservation &observation, Direction direction) {
	BotMovementCandidate candidate {
		.origin = observation.position,
		.destination = observation.position,
		.direction = direction,
	};
	if (!isLocalDirection(direction)) {
		return { .candidate = candidate, .outcome = BotWalkability::InvalidDirection, .movementCost = BlockedCost };
	}
	candidate.destination = getNextPosition(direction, observation.position);
	return assess(observation, candidate.destination);
}

BotWalkabilityResult BotNavigation::assess(const BotObservation &observation, const Position &destination) {
	BotMovementCandidate candidate {
		.origin = observation.position,
		.destination = destination,
		.direction = getDirectionTo(observation.position, destination),
	};
	if (destination.z != observation.position.z) {
		return { .candidate = candidate, .outcome = BotWalkability::DifferentFloor, .movementCost = BlockedCost };
	}
	if (destination == observation.position) {
		candidate.direction = DIRECTION_NONE;
		return { .candidate = candidate, .outcome = BotWalkability::InvalidDirection, .movementCost = BlockedCost };
	}
	if (!Position::areInRange<1, 1, 0>(observation.position, destination)) {
		return { .candidate = candidate, .outcome = BotWalkability::OutsideKnownOrVisibleArea, .movementCost = BlockedCost };
	}
	if (!isLocalDirection(candidate.direction)) {
		candidate.direction = DIRECTION_NONE;
		return { .candidate = candidate, .outcome = BotWalkability::InvalidDirection, .movementCost = BlockedCost };
	}

	const auto tile = std::ranges::find(observation.visibleTiles, destination, &BotTileObservation::position);
	if (tile == observation.visibleTiles.end()) {
		return { .candidate = candidate, .outcome = BotWalkability::OutsideKnownOrVisibleArea, .movementCost = BlockedCost };
	}
	candidate.observationSignature = signature(*tile);
	const uint32_t baseCost = (candidate.direction & DIRECTION_DIAGONAL_MASK) != 0 ? DiagonalCost : CardinalCost;
	if (!tile->hasGround || tile->terrainBlocked) {
		return { .candidate = candidate, .outcome = BotWalkability::BlockedByTerrain, .movementCost = BlockedCost, .evaluatedTiles = 1 };
	}
	if (tile->blockingItemTypeId != 0) {
		return { .candidate = candidate, .outcome = BotWalkability::BlockedByItem, .movementCost = BlockedCost, .blockingItemTypeId = tile->blockingItemTypeId, .evaluatedTiles = 1 };
	}
	if (tile->blockingCreatureId != 0) {
		return { .candidate = candidate, .outcome = BotWalkability::BlockedByCreature, .movementCost = BlockedCost, .blockingCreatureId = tile->blockingCreatureId, .evaluatedTiles = 1 };
	}
	if (tile->hazardous) {
		return { .candidate = candidate, .outcome = BotWalkability::WalkableWithRisk, .movementCost = baseCost + HazardCost, .harmfulFieldCombatType = tile->harmfulFieldCombatType, .evaluatedTiles = 1 };
	}
	return { .candidate = candidate, .outcome = BotWalkability::Walkable, .movementCost = baseCost, .evaluatedTiles = 1 };
}

BotRouteResult BotNavigation::findRoute(const BotObservation &observation, const BotRouteRequest &request) {
	BotRouteResult result { .topologyRevision = observation.topologyRevision };
	if (request.origin != observation.position || request.destination.z != request.origin.z
		|| !Position::areInRange<8, 6, 0>(request.origin, request.destination)) {
		result.reason = BotRouteReason::DestinationOutsideKnownArea;
		return result;
	}
	if (request.origin == request.destination) {
		result.reason = BotRouteReason::AlreadyAtDestination;
		return result;
	}
	const auto destinationTile = std::ranges::find(observation.visibleTiles, request.destination, &BotTileObservation::position);
	if (destinationTile == observation.visibleTiles.end() || !destinationTile->hasGround) {
		result.reason = BotRouteReason::DestinationUnknown;
		return result;
	}

	struct Node { Position position; uint32_t cost; uint32_t estimate; int32_t parent; uint32_t order; };
	std::vector<Node> nodes;
	nodes.push_back({ request.origin, 0, heuristic(request.origin, request.destination), -1, 0 });
	std::vector<size_t> open { 0 };
	std::vector<Position> closed;
	uint32_t order = 1;
	int32_t found = -1;
	while (!open.empty()) {
		if (++result.planningOperations > request.limits.maxPlanningOperations) {
			result.reason = BotRouteReason::PlanningBudgetExceeded;
			return result;
		}
		auto best = std::ranges::min_element(open, [&](size_t left, size_t right) {
			const auto &a = nodes[left]; const auto &b = nodes[right];
			return std::tie(a.estimate, a.cost, a.position.y, a.position.x, a.order)
				< std::tie(b.estimate, b.cost, b.position.y, b.position.x, b.order);
		});
		const size_t currentIndex = *best;
		open.erase(best);
		const auto current = nodes[currentIndex];
		if (current.position == request.destination) { found = static_cast<int32_t>(currentIndex); break; }
		if (++result.expandedNodes > request.limits.maxExpandedNodes) {
			result.reason = BotRouteReason::NodeBudgetExceeded;
			return result;
		}
		closed.push_back(current.position);
		for (const auto direction : routeDirections) {
			if (++result.planningOperations > request.limits.maxPlanningOperations) {
				result.reason = BotRouteReason::PlanningBudgetExceeded;
				return result;
			}
			const auto next = getNextPosition(direction, current.position);
			if (!Position::areInRange<8, 6, 0>(request.origin, next) || std::ranges::find(closed, next) != closed.end()) continue;
			const auto tile = std::ranges::find(observation.visibleTiles, next, &BotTileObservation::position);
			if (tile == observation.visibleTiles.end() || !tile->hasGround || tile->terrainBlocked || tile->blockingItemTypeId || tile->blockingCreatureId) continue;
			const uint32_t step = ((direction & DIRECTION_DIAGONAL_MASK) ? DiagonalCost : CardinalCost) + (tile->hazardous ? HazardCost : 0);
			const uint32_t cost = current.cost + step;
			auto existing = std::ranges::find(nodes, next, &Node::position);
			if (existing != nodes.end() && existing->cost <= cost) continue;
			if (existing != nodes.end()) {
				existing->cost = cost; existing->estimate = cost + heuristic(next, request.destination); existing->parent = static_cast<int32_t>(currentIndex);
				if (std::ranges::find(open, static_cast<size_t>(existing - nodes.begin())) == open.end()) open.push_back(existing - nodes.begin());
			} else {
				nodes.push_back({ next, cost, cost + heuristic(next, request.destination), static_cast<int32_t>(currentIndex), order++ });
				open.push_back(nodes.size() - 1);
			}
		}
	}
	if (found < 0) { result.reason = BotRouteReason::NoRoute; return result; }
	for (int32_t index = found; index > 0; index = nodes[index].parent) result.positions.push_back(nodes[index].position);
	std::ranges::reverse(result.positions);
	if (result.positions.size() > request.limits.maxRouteLength) {
		result.positions.clear(); result.reason = BotRouteReason::RouteLengthExceeded; return result;
	}
	for (const auto &position : result.positions) {
		const auto tile = std::ranges::find(observation.visibleTiles, position, &BotTileObservation::position);
		result.signatures.push_back(signature(*tile));
	}
	result.reason = BotRouteReason::RouteFound;
	return result;
}

std::chrono::milliseconds BotNavigation::backoff(const BotRouteLimits &limits, uint32_t replanAttempt) {
	const auto exponent = std::min<uint32_t>(replanAttempt == 0 ? 0 : replanAttempt - 1, 10);
	return std::min(limits.initialBackoff * (1U << exponent), limits.maximumBackoff);
}

void BotNavigation::observeProgress(BotRouteProgress &progress, const Position &actual, std::chrono::milliseconds now, const BotRouteLimits &limits) {
	progress.actualPosition = actual;
	if (actual == progress.expectedNext && actual != progress.expectedOrigin) {
		progress.consecutiveNoProgress = 0;
		progress.lastProgressAt = now;
		return;
	}
	++progress.consecutiveNoProgress;
	progress.reason = BotRouteReason::NoProgress;
	progress.state = progress.consecutiveNoProgress >= limits.maxNoProgress ? BotRouteState::Failed : BotRouteState::ReplanRequired;
}
