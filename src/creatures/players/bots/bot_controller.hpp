/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#ifndef USE_PRECOMPILED_HEADERS
	#include <cstdint>
	#include <memory>
#endif

enum Direction : uint8_t;
enum ReturnValue : uint16_t;

class Game;
class Player;

class BotController final {
public:
	BotController(Game &game, const std::shared_ptr<Player> &player);

	[[nodiscard]] ReturnValue move(Direction direction) const;

private:
	Game &game;
	std::weak_ptr<Player> player;
};
