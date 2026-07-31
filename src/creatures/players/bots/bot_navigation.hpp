/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_runtime.hpp"

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
};
