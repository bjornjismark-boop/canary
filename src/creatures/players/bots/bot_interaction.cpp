/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_interaction.hpp"

bool BotDestinationRegion::contains(const Position &position) const {
	return position.x >= minimum.x && position.x <= maximum.x
		&& position.y >= minimum.y && position.y <= maximum.y
		&& position.z >= minimum.z && position.z <= maximum.z;
}

uint64_t BotInteraction::signature(const BotInteractionTarget &target) {
	uint64_t value = 1469598103934665603ULL;
	for (const uint64_t part : { uint64_t(target.position.x), uint64_t(target.position.y), uint64_t(target.position.z),
		uint64_t(target.stackPosition), uint64_t(target.itemTypeId), uint64_t(target.category) }) {
		value ^= part; value *= 1099511628211ULL;
	}
	return value;
}

bool BotInteraction::destinationAllowed(const BotTransitionRequest &request, const Position &before, const Position &after) {
	if (after == before || (!request.allowDifferentFloor && after.z != before.z)) return false;
	if (request.expectedDestination) return after == *request.expectedDestination;
	if (request.expectedRegion) return request.expectedRegion->contains(after);
	return true;
}

std::chrono::milliseconds BotInteraction::backoff(const BotTransitionRequest &request, uint32_t attempt) {
	if (attempt == 0) return {};
	const auto exponent = std::min<uint32_t>(attempt - 1, 10);
	return std::min(request.maximumBackoff, request.initialBackoff * (1U << exponent));
}

bool BotInteraction::supported(BotInteractionType type) {
	return type == BotInteractionType::UseDoor || type == BotInteractionType::UseLadder
		|| type == BotInteractionType::WalkOntoTransition || type == BotInteractionType::UseTeleportOrPortal
		|| type == BotInteractionType::UseGenericWorldObject;
}
