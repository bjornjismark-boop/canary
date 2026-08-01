/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_loot.hpp"

#include "creatures/players/player.hpp"
#include "game/game.hpp"
#include "items/containers/container.hpp"

namespace {
uint64_t mix(uint64_t hash, uint64_t value) {
	hash ^= value;
	return hash * 1099511628211ULL;
}

BotLootFailure failureFor(BotLootEligibility eligibility) {
	return static_cast<BotLootFailure>(eligibility);
}

uint16_t distance(const Position &from, const Position &to) {
	return static_cast<uint16_t>(std::max(Position::getDistanceX(from, to), Position::getDistanceY(from, to)));
}

const BotLootRule* findRule(const BotLootPolicy &policy, uint16_t itemTypeId) {
	const auto it = std::ranges::find(policy.rules, itemTypeId, &BotLootRule::itemTypeId);
	return it == policy.rules.end() ? nullptr : &*it;
}
}

BotCorpseSignature BotLoot::signature(const BotCorpseObservation &corpse) {
	uint64_t hash = 1469598103934665603ULL;
	hash = mix(hash, corpse.position.x);
	hash = mix(hash, corpse.position.y);
	hash = mix(hash, corpse.position.z);
	hash = mix(hash, corpse.corpseItemTypeId);
	hash = mix(hash, corpse.sourceCreatureId);
	hash = mix(hash, corpse.ownerCreatureId);
	hash = mix(hash, corpse.containerCapacity);
	hash = mix(hash, corpse.containerSize);
	for (const auto &item : corpse.items) {
		hash = mix(hash, item.itemTypeId);
		hash = mix(hash, item.count);
		hash = mix(hash, item.weight);
		hash = mix(hash, item.stackPosition);
		hash = mix(hash, item.depth);
		hash = mix(hash, item.stackable);
		hash = mix(hash, item.nestedContainer);
	}
	return { hash };
}

BotLootSelectionResult BotLoot::select(const BotCorpseObservation &corpse, uint32_t freeCapacity, const BotLootPolicy &policy) {
	BotLootSelectionResult result;
	result.corpse = corpse;
	if (corpse.corpseItemTypeId == 0) {
		return result;
	}
	if (corpse.remainingDecayMilliseconds == 0) {
		result.eligibility = BotLootEligibility::CorpseExpired;
		result.failure = BotLootFailure::CorpseExpired;
		return result;
	}
	if (corpse.ownership == BotCorpseOwnership::Denied) {
		result.eligibility = BotLootEligibility::NoLootRights;
		result.failure = BotLootFailure::NoLootRights;
		return result;
	}
	const auto limit = std::min<size_t>(policy.maxItemCandidates, corpse.items.size());
	result.evaluatedItems = static_cast<uint16_t>(limit);
	result.candidates.reserve(limit);
	for (size_t index = 0; index < limit; ++index) {
		const auto &item = corpse.items[index];
		BotLootCandidate candidate;
		candidate.item = item;
		candidate.projectedWeight = item.weight;
		const auto rule = findRule(policy, item.itemTypeId);
		if (rule) {
			candidate.valueCategory = rule->valueCategory;
			candidate.priority = rule->priority;
		}
		if (!rule && policy.rejectUnconfiguredItems) {
			candidate.eligibility = BotLootEligibility::PolicyRejected;
		} else if (item.weight > freeCapacity || item.weight > policy.maxProjectedWeight) {
			candidate.eligibility = BotLootEligibility::CapacityInsufficient;
		} else {
			candidate.eligibility = BotLootEligibility::Eligible;
			candidate.score = static_cast<uint64_t>(candidate.priority) * 1000000ULL + static_cast<uint64_t>(candidate.valueCategory) * 10000ULL + (std::numeric_limits<uint16_t>::max() - item.itemTypeId) * 10ULL + (std::numeric_limits<uint16_t>::max() - item.stackPosition);
		}
		result.candidates.push_back(candidate);
	}
	if (corpse.items.size() > limit) {
		result.eligibility = BotLootEligibility::EvaluationBudgetExceeded;
		result.failure = BotLootFailure::EvaluationBudgetExceeded;
		return result;
	}
	const auto best = std::ranges::max_element(result.candidates, [](const auto &left, const auto &right) {
		return std::tuple(left.eligibility == BotLootEligibility::Eligible, left.score) < std::tuple(right.eligibility == BotLootEligibility::Eligible, right.score);
	});
	if (best != result.candidates.end() && best->eligibility == BotLootEligibility::Eligible) {
		result.selected = *best;
		result.eligibility = BotLootEligibility::Eligible;
		result.failure = BotLootFailure::None;
		return result;
	}
	result.eligibility = std::ranges::any_of(result.candidates, [](const auto &candidate) { return candidate.eligibility == BotLootEligibility::CapacityInsufficient; }) ? BotLootEligibility::CapacityInsufficient : BotLootEligibility::PolicyRejected;
	result.failure = failureFor(result.eligibility);
	return result;
}

BotLootSelectionResult BotLoot::observe(const std::shared_ptr<Player> &player, const BotObservation &world, const Position &position, uint32_t sourceCreatureId, BotCorpseSignature expectedSignature, const BotLootPolicy &policy) {
	BotLootSelectionResult result;
	if (!player || player->isRemoved() || player->getHealth() <= 0) {
		result.eligibility = BotLootEligibility::InvalidLifecycle;
		result.failure = BotLootFailure::InvalidLifecycle;
		return result;
	}
	const auto visibleTile = std::ranges::find(world.visibleTiles, position, &BotTileObservation::position);
	if (visibleTile == world.visibleTiles.end()) {
		result.eligibility = BotLootEligibility::OutsideKnownArea;
		result.failure = BotLootFailure::OutsideKnownArea;
		return result;
	}
	if (!player->canSee(position)) {
		result.eligibility = BotLootEligibility::NotVisible;
		result.failure = BotLootFailure::NotVisible;
		return result;
	}
	if (distance(player->getPosition(), position) > policy.maxDistance) {
		result.eligibility = BotLootEligibility::TooFar;
		result.failure = BotLootFailure::TooFar;
		return result;
	}
	if (player->getPosition() != position) {
		const BotRouteLimits limits { .maxExpandedNodes = 128, .maxRouteLength = policy.maxDistance, .maxPlanningOperations = 2048 };
		if (!BotNavigation::findRoute(world, { player->getPosition(), position, limits }).found()) {
			result.eligibility = BotLootEligibility::Unreachable;
			result.failure = BotLootFailure::Unreachable;
			return result;
		}
	}
	const auto tile = g_game().map.getTile(position);
	if (!tile || !tile->getItemList()) {
		return result;
	}
	std::shared_ptr<Item> corpse;
	for (const auto &item : *tile->getItemList()) {
		if (item && item->isCorpse() && item->getContainer()) {
			corpse = item;
			break;
		}
	}
	if (!corpse || !corpse->getParent()) {
		return result;
	}
	const auto container = corpse->getContainer();
	result.corpse.observationRevision = world.topologyRevision;
	result.corpse.position = position;
	result.corpse.corpseItemTypeId = corpse->getID();
	result.corpse.sourceCreatureId = sourceCreatureId;
	result.corpse.ownerCreatureId = corpse->getCorpseOwner();
	result.corpse.remainingDecayMilliseconds = static_cast<uint32_t>(std::max<int32_t>(corpse->getDuration(), 0));
	result.corpse.containerCapacity = static_cast<uint16_t>(std::min<uint32_t>(container->capacity(), std::numeric_limits<uint16_t>::max()));
	result.corpse.containerSize = static_cast<uint16_t>(std::min<size_t>(container->size(), std::numeric_limits<uint16_t>::max()));
	if (corpse->isRewardCorpse()) {
		result.corpse.ownership = BotCorpseOwnership::Denied;
	} else if (result.corpse.ownerCreatureId == 0) {
		result.corpse.ownership = BotCorpseOwnership::Unrestricted;
	} else if (result.corpse.ownerCreatureId == player->getID()) {
		result.corpse.ownership = BotCorpseOwnership::Self;
	} else if (player->canOpenCorpse(result.corpse.ownerCreatureId)) {
		result.corpse.ownership = BotCorpseOwnership::Party;
	} else {
		result.corpse.ownership = BotCorpseOwnership::Denied;
	}
	const auto itemLimit = std::min<size_t>(policy.maxItemCandidates + 1, container->getItemList().size());
	result.corpse.items.reserve(itemLimit);
	for (size_t index = 0; index < itemLimit; ++index) {
		const auto &item = container->getItemList()[index];
		if (!item) {
			continue;
		}
		BotLootItemObservation observed { .itemTypeId = item->getID(), .count = std::max<uint16_t>(item->getItemCount(), 1), .weight = item->getWeight(), .stackPosition = static_cast<uint16_t>(index), .depth = 0, .stackable = item->isStackable(), .nestedContainer = item->getContainer() != nullptr };
		observed.signature = mix(mix(mix(1469598103934665603ULL, observed.itemTypeId), observed.count), observed.stackPosition);
		result.corpse.items.push_back(observed);
	}
	result.corpse.signature = signature(result.corpse);
	if (expectedSignature.value != 0 && expectedSignature != result.corpse.signature) {
		result.eligibility = BotLootEligibility::StaleObservation;
		result.failure = BotLootFailure::StaleObservation;
		return result;
	}
	if (result.corpse.remainingDecayMilliseconds == 0 || corpse->isRemoved()) {
		result.eligibility = BotLootEligibility::CorpseExpired;
		result.failure = BotLootFailure::CorpseExpired;
		return result;
	}
	return select(result.corpse, player->getFreeCapacity(), policy);
}
