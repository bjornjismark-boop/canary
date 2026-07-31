/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_runtime.hpp"

#include "creatures/creature.hpp"
#include "creatures/players/player.hpp"
#include "map/spectators.hpp"

std::optional<BotObservation> BotPerception::observe(const std::shared_ptr<Player> &player) {
	if (!player || !player->isBotControlled() || player->isRemoved() || !player->getTile()) {
		return std::nullopt;
	}

	BotObservation observation {
		.playerGuid = player->getGUID(),
		.playerCreatureId = player->getID(),
		.position = player->getPosition(),
		.health = player->getHealth(),
		.maxHealth = player->getMaxHealth(),
		.mana = player->getMana(),
		.maxMana = player->getMaxMana(),
		.level = player->getLevel(),
		.visibleCreatures = {},
	};

	for (const auto &creature : Spectators().find<Creature>(player->getPosition())) {
		if (!creature || creature == player || creature->isRemoved() || !player->canSeeCreature(creature)) {
			continue;
		}
		BotCreatureKind kind = BotCreatureKind::Other;
		switch (creature->getType()) {
			case CREATURETYPE_PLAYER: kind = BotCreatureKind::Player; break;
			case CREATURETYPE_MONSTER: kind = BotCreatureKind::Monster; break;
			case CREATURETYPE_NPC: kind = BotCreatureKind::Npc; break;
			default: break;
		}
		const auto maximumHealth = std::max<int32_t>(creature->getMaxHealth(), 1);
		observation.visibleCreatures.emplace_back(BotCreatureObservation {
			.id = creature->getID(),
			.kind = kind,
			.position = creature->getPosition(),
			.healthPercent = static_cast<uint8_t>(std::clamp<int32_t>(creature->getHealth() * 100 / maximumHealth, 0, 100)),
		});
	}
	std::ranges::sort(observation.visibleCreatures, {}, &BotCreatureObservation::id);
	return observation;
}
