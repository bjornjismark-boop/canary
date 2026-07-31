/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_controller.hpp"

#include "creatures/players/player.hpp"
#include "game/game.hpp"

BotController::BotController(Game &game, const std::shared_ptr<Player> &player) :
	game(game), player(player) {
}

ReturnValue BotController::move(Direction direction) const {
	const auto controlledPlayer = player.lock();
	if (!controlledPlayer || !controlledPlayer->isBotControlled() || controlledPlayer->isRemoved() || !controlledPlayer->getTile()) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	return game.internalMoveCreature(controlledPlayer, direction);
}
