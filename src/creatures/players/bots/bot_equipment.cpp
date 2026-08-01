/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_equipment.hpp"

#include "creatures/players/player.hpp"
#include "creatures/players/vocations/vocation.hpp"
#include "items/containers/container.hpp"
#include "items/item.hpp"
#include "items/items.hpp"
#include "utils/tools.hpp"

namespace {
int32_t clampScore(int64_t value) { return static_cast<int32_t>(std::clamp<int64_t>(value, -1000000, 1000000)); }

BotEquipmentSlot slotFor(const ItemType &type, uint8_t actualSlot) {
	if (actualSlot >= CONST_SLOT_FIRST && actualSlot <= CONST_SLOT_LAST) return static_cast<BotEquipmentSlot>(actualSlot);
	if (type.slotPosition & SLOTP_HEAD) return BotEquipmentSlot::Head;
	if (type.slotPosition & SLOTP_NECKLACE) return BotEquipmentSlot::Necklace;
	if (type.slotPosition & SLOTP_BACKPACK) return BotEquipmentSlot::Backpack;
	if (type.slotPosition & SLOTP_ARMOR) return BotEquipmentSlot::Armor;
	if (type.slotPosition & SLOTP_LEGS) return BotEquipmentSlot::Legs;
	if (type.slotPosition & SLOTP_FEET) return BotEquipmentSlot::Feet;
	if (type.slotPosition & SLOTP_RING) return BotEquipmentSlot::Ring;
	if (type.slotPosition & SLOTP_AMMO) return BotEquipmentSlot::Ammunition;
	if (type.weaponType == WEAPON_SHIELD || (type.slotPosition & SLOTP_RIGHT)) return BotEquipmentSlot::RightHand;
	if (type.weaponType != WEAPON_NONE || (type.slotPosition & (SLOTP_LEFT | SLOTP_TWO_HAND))) return BotEquipmentSlot::LeftHand;
	return BotEquipmentSlot::None;
}

BotItemCategory categoryFor(const ItemType &type) {
	if (type.isShield()) return BotItemCategory::Shield;
	if (type.isWeapon()) return BotItemCategory::Weapon;
	if (type.isHelmet()) return BotItemCategory::Helmet;
	if (type.isArmor()) return BotItemCategory::Armor;
	if (type.isLegs()) return BotItemCategory::Legs;
	if (type.isBoots()) return BotItemCategory::Boots;
	if (type.isAmulet()) return BotItemCategory::Amulet;
	if (type.isRing()) return BotItemCategory::Ring;
	if (type.isContainer()) return BotItemCategory::Backpack;
	if (type.isAmmo()) return BotItemCategory::Ammunition;
	return BotItemCategory::Unknown;
}

bool vocationAllows(const Player &player, const ItemType &type) {
	if (!(type.wieldInfo & WIELDINFO_VOCREQ) || type.vocationString.empty()) return true;
	const auto vocation = player.getVocation();
	return vocation && asLowerCaseString(type.vocationString).find(asLowerCaseString(vocation->getVocName())) != std::string::npos;
}

BotItemObservation makeObservation(const Player &player, const std::shared_ptr<Item> &item, BotInventoryPath path, uint8_t actualSlot, const BotEquipmentPolicy &policy) {
	const auto &type = Item::items[item->getID()];
	BotItemObservation result;
	result.itemTypeId = item->getID();
	result.countOrCharges = item->getSubType() > 0 ? item->getSubType() : item->getItemCount();
	result.category = categoryFor(type);
	result.slot = slotFor(type, actualSlot);
	result.path = std::move(path);
	result.weight = item->getWeight();
	result.minimumLevel = type.minReqLevel;
	result.vocationRequirement = type.vocationString;
	result.weaponType = static_cast<uint8_t>(type.weaponType);
	result.attack = item->getAttack();
	result.defense = item->getDefense() + item->getExtraDefense();
	result.armor = item->getArmor();
	result.range = item->getShootRange();
	result.speed = type.getSpeed();
	result.durationMilliseconds = item->hasAttribute(ItemAttribute_t::DURATION) ? static_cast<uint32_t>(std::max<int64_t>(0, item->getDuration())) : type.decayTime * 1000;
	result.temporary = type.decayTime != 0 || type.wearOut || type.expire || type.charges != 0;
	result.equipped = actualSlot >= CONST_SLOT_FIRST && actualSlot <= CONST_SLOT_LAST;
	result.vocationCompatible = vocationAllows(player, type);
	result.levelCompatible = player.getLevel() >= type.minReqLevel;
	result.supply = std::ranges::find(policy.supplyItemTypeIds, result.itemTypeId) != policy.supplyItemTypeIds.end();
	if (result.supply && result.category == BotItemCategory::Unknown) result.category = BotItemCategory::Supply;
	if (type.abilities) {
		result.skillModifiers.assign(std::begin(type.abilities->skills), std::end(type.abilities->skills));
		result.resistances.assign(std::begin(type.abilities->absorbPercent), std::end(type.abilities->absorbPercent));
	}
	if (const auto known = std::ranges::find(policy.knownPrices, result.itemTypeId, &BotKnownPrice::itemTypeId); known != policy.knownPrices.end()) {
		if (known->npcBuyPrice) result.knownNpcBuyPrice = known->npcBuyPrice;
		if (known->npcSellPrice) result.knownNpcSellPrice = known->npcSellPrice;
		result.priceSource = known->source;
	}
	result.signature = BotEquipment::signature(result);
	return result;
}
}

uint64_t BotEquipment::signature(const BotItemObservation &item) {
	uint64_t value = 1469598103934665603ULL;
	auto add = [&value](uint64_t part) { value ^= part; value *= 1099511628211ULL; };
	add(item.itemTypeId); add(item.countOrCharges); add(static_cast<uint8_t>(item.category)); add(static_cast<uint8_t>(item.slot)); add(item.path.rootSlot);
	for (const auto index : item.path.childIndices) add(index);
	add(item.weight); add(static_cast<uint32_t>(item.attack)); add(static_cast<uint32_t>(item.defense)); add(static_cast<uint32_t>(item.armor)); add(item.range); add(item.durationMilliseconds);
	return value;
}

BotEquipmentObservation BotEquipment::observe(const std::shared_ptr<Player> &player, const BotEquipmentPolicy &policy) {
	BotEquipmentObservation result;
	if (!player) return result;
	result.freeCapacity = player->getFreeCapacity();
	result.playerLevel = player->getLevel();
	result.vocationId = player->getVocationId();
	struct Pending { std::shared_ptr<Container> container; BotInventoryPath path; uint8_t depth; };
	std::vector<Pending> pending;
	uint16_t seenItems = 0;
	uint8_t seenContainers = 0;
	for (uint8_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		const auto item = player->getInventoryItem(static_cast<Slots_t>(slot));
		if (!item) continue;
		if (seenItems++ >= policy.maximumItemsEvaluated) { result.itemBudgetExceeded = true; break; }
		BotInventoryPath path { slot, {} };
		result.items.push_back(makeObservation(*player, item, path, slot, policy));
		if (const auto container = item->getContainer()) pending.push_back({ container, path, 0 });
	}
	for (size_t cursor = 0; cursor < pending.size() && !result.itemBudgetExceeded; ++cursor) {
		if (seenContainers++ >= policy.maximumInventoryContainers) { result.containerBudgetExceeded = true; break; }
		const auto current = pending[cursor];
		for (uint16_t index = 0; index < current.container->size(); ++index) {
			if (seenItems++ >= policy.maximumItemsEvaluated) { result.itemBudgetExceeded = true; break; }
			const auto item = current.container->getItemByIndex(index);
			if (!item) continue;
			auto path = current.path; path.childIndices.push_back(index);
			result.items.push_back(makeObservation(*player, item, path, 0, policy));
			if (const auto child = item->getContainer(); child && current.depth < policy.maximumNestingDepth) pending.push_back({ child, path, static_cast<uint8_t>(current.depth + 1) });
		}
	}
	std::ranges::sort(result.items, {}, [](const BotItemObservation &item) { return std::tuple(item.path.rootSlot, item.path.childIndices, item.itemTypeId); });
	uint64_t revision = 1469598103934665603ULL;
	for (const auto &item : result.items) { revision ^= item.signature; revision *= 1099511628211ULL; }
	result.revision = revision;
	return result;
}

BotItemScore BotEquipment::score(const BotItemObservation &item, const BotEquipmentPolicy &policy, uint32_t &operations) {
	BotItemScore score;
	auto term = [&operations, &policy](int64_t value) { if (operations >= policy.maximumScoreOperations) return int32_t { 0 }; ++operations; return clampScore(value); };
	const bool ranged = policy.role == BotEquipmentRole::Distance;
	const bool magic = policy.role == BotEquipmentRole::Magic;
	score.attack = term(static_cast<int64_t>(item.attack) * (ranged ? 45 : magic ? 10 : 50));
	score.defense = term(static_cast<int64_t>(item.defense) * 30);
	score.armor = term(static_cast<int64_t>(item.armor) * 45);
	score.range = term(static_cast<int64_t>(item.range) * (ranged ? 100 : 5));
	int64_t modifierTotal = item.speed;
	for (const auto value : item.skillModifiers) modifierTotal += static_cast<int64_t>(value) * (magic ? 25 : 40);
	for (const auto value : item.resistances) modifierTotal += static_cast<int64_t>(value) * 10;
	score.modifiers = term(modifierTotal);
	score.durability = term(item.temporary ? std::min<uint32_t>(item.durationMilliseconds / 1000, 100) : 100);
	score.weight = term(-static_cast<int64_t>(item.weight) / 10);
	score.supplyCompatibility = term(item.supply ? 200 : 0);
	score.retention = term(item.equipped ? policy.retentionBonus : 0);
	score.total = clampScore(static_cast<int64_t>(score.attack) + score.defense + score.armor + score.range + score.modifiers + score.durability + score.weight + score.supplyCompatibility + score.retention);
	return score;
}

BotEquipmentComparison BotEquipment::compare(const BotItemObservation *current, const BotItemObservation &candidate, const BotEquipmentObservation &observation, const BotEquipmentPolicy &policy) {
	BotEquipmentComparison result;
	if (!candidate.vocationCompatible) { result.intent = BotEquipmentIntent::WrongVocation; result.failure = BotValuationFailure::WrongVocation; return result; }
	if (!candidate.levelCompatible) { result.intent = BotEquipmentIntent::MissingRequirement; result.failure = BotValuationFailure::MissingLevel; return result; }
	if (candidate.slot == BotEquipmentSlot::None) { result.intent = BotEquipmentIntent::WrongSlot; result.failure = BotValuationFailure::WrongSlot; return result; }
	if (!candidate.equipped && observation.freeCapacity < policy.minimumFreeCapacity + candidate.weight) { result.intent = BotEquipmentIntent::CapacityRisk; result.failure = BotValuationFailure::CapacityRisk; return result; }
	if (candidate.supply && candidate.category == BotItemCategory::Supply) { result.intent = BotEquipmentIntent::KeepForSupply; return result; }
	uint32_t operations = 0;
	result.candidateScore = score(candidate, policy, operations);
	if (current) result.currentScore = score(*current, policy, operations);
	result.scoreOperations = operations;
	if (operations >= policy.maximumScoreOperations) { result.failure = BotValuationFailure::ScoreBudgetExceeded; return result; }
	result.improvement = clampScore(static_cast<int64_t>(result.candidateScore.total) - result.currentScore.total);
	if (candidate.equipped) result.intent = BotEquipmentIntent::KeepEquipped;
	else if (!current || result.improvement >= policy.upgradeThreshold) result.intent = BotEquipmentIntent::UpgradeAvailable;
	else result.intent = BotEquipmentIntent::DowngradeRejected;
	return result;
}

std::vector<BotUpgradeCandidate> BotEquipment::upgrades(const BotEquipmentObservation &observation, const BotEquipmentPolicy &policy) {
	std::vector<BotUpgradeCandidate> result;
	for (const auto &candidate : observation.items) {
		if (candidate.equipped || candidate.slot == BotEquipmentSlot::None) continue;
		const BotItemObservation *current = nullptr;
		for (const auto &item : observation.items) if (item.equipped && item.slot == candidate.slot) { current = &item; break; }
		auto comparison = compare(current, candidate, observation, policy);
		if (comparison.intent == BotEquipmentIntent::UpgradeAvailable) result.push_back({ candidate.itemTypeId, candidate.path, candidate.slot, comparison, observation.revision });
	}
	std::ranges::sort(result, [](const auto &left, const auto &right) {
		return std::tuple(-left.comparison.improvement, left.itemTypeId, left.path.rootSlot, left.path.childIndices) < std::tuple(-right.comparison.improvement, right.itemTypeId, right.path.rootSlot, right.path.childIndices);
	});
	return result;
}
