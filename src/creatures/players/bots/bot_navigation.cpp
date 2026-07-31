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
