/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_runtime.hpp"

#include "creatures/creature.hpp"
#include "creatures/combat/combat.hpp"
#include "creatures/players/player.hpp"
#include "game/game.hpp"
#include "items/item.hpp"
#include "map/spectators.hpp"
#include "utils/tools.hpp"

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
		.visibleTiles = {},
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
	for (uint8_t rawDirection = DIRECTION_NORTH; rawDirection <= DIRECTION_LAST; ++rawDirection) {
		const auto position = getNextPosition(static_cast<Direction>(rawDirection), player->getPosition());
		if (!Position::areInRange<8, 6, 0>(player->getPosition(), position)) {
			continue;
		}
		BotTileObservation tileObservation { .position = position };
		const auto tile = g_game().map.getTile(position);
		if (tile) {
			const auto &ground = tile->getGround();
			tileObservation.hasGround = ground != nullptr;
			if (ground) {
				tileObservation.groundTypeId = ground->getID();
				tileObservation.terrainBlocked = ground->isBlocking();
			}
			if (const auto* items = tile->getItemList()) {
				for (const auto &item : *items) {
					if (item && item->isBlocking()) {
						tileObservation.blockingItemTypeId = item->getID();
						break;
					}
				}
			}
			if (const auto &field = tile->getFieldItem(); field && !field->isBlocking() && field->getDamage() > 0) {
				tileObservation.hazardous = true;
				tileObservation.harmfulFieldCombatType = static_cast<uint8_t>(field->getCombatType());
			}
			if (const auto* creatures = tile->getCreatures()) {
				for (const auto &creature : *creatures) {
					if (creature && creature != player && !creature->isRemoved() && player->canSeeCreature(creature) && !player->canWalkthrough(creature)) {
						tileObservation.blockingCreatureId = creature->getID();
						break;
					}
				}
			}
		}
		observation.visibleTiles.emplace_back(tileObservation);
	}
	std::ranges::sort(observation.visibleTiles, [](const auto &left, const auto &right) {
		return left.position < right.position;
	});
	return observation;
}
