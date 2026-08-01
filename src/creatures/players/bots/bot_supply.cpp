/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/master/LICENSE
 */

#include "creatures/players/bots/bot_supply.hpp"

#include "creatures/players/player.hpp"
#include "items/containers/container.hpp"

namespace {
uint64_t supplyMix(uint64_t hash, uint64_t value) {
	return (hash ^ value) * 1099511628211ULL;
}

BotSupplyCategory categoryFor(const BotSupplyPolicy &policy, uint16_t id) {
	const auto it = std::ranges::find(policy.rules, id, &BotSupplyRule::itemTypeId);
	return it == policy.rules.end() ? BotSupplyCategory::UnknownConfigured : it->category;
}

void addSupply(std::vector<BotSupplyEntry> &entries, const std::shared_ptr<Item> &item, uint8_t depth, bool equipped, const BotSupplyPolicy &policy) {
	if (!item || entries.size() >= policy.maxEntries) {
		return;
	}

	BotSupplyEntry entry {
		.itemTypeId = item->getID(),
		.category = categoryFor(policy, item->getID()),
		.count = std::max<uint16_t>(item->getItemCount(), 1),
		.charges = item->getCharges(),
		.equipped = equipped,
		.depth = depth,
	};
	entry.signature = supplyMix(supplyMix(supplyMix(supplyMix(supplyMix(1469598103934665603ULL, entry.itemTypeId), entry.count), entry.charges), static_cast<uint8_t>(entry.category)), entry.depth);
	entries.push_back(entry);
}
}

BotSupplyObservation BotSupply::observe(const std::shared_ptr<Player> &player, const BotSupplyPolicy &policy) {
	BotSupplyObservation result;
	if (!player || player->isRemoved() || player->getHealth() <= 0) {
		return result;
	}

	result.freeCapacity = player->getFreeCapacity();
	std::vector<std::pair<std::shared_ptr<Container>, uint8_t>> pending;
	for (uint8_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST && result.entries.size() < policy.maxEntries; ++slot) {
		const auto item = player->getInventoryItem(static_cast<Slots_t>(slot));
		if (!item) {
			continue;
		}
		addSupply(result.entries, item, 0, true, policy);
		if (const auto container = item->getContainer(); container && pending.size() < policy.maxContainers) {
			pending.emplace_back(container, 0);
		}
	}

	for (size_t i = 0; i < pending.size() && i < policy.maxContainers && result.entries.size() < policy.maxEntries; ++i) {
		const auto &[container, depth] = pending[i];
		if (!container) {
			continue;
		}
		for (const auto &item : container->getItemList()) {
			addSupply(result.entries, item, depth, false, policy);
			if (depth < policy.maxDepth) {
				if (const auto child = item ? item->getContainer() : nullptr; child && pending.size() < policy.maxContainers) {
					pending.emplace_back(child, static_cast<uint8_t>(depth + 1));
				}
			}
			if (result.entries.size() >= policy.maxEntries) {
				break;
			}
		}
	}

	// Ignore ammunition resolution here: the equipped launcher defines whether
	// ammunition is required even when no compatible ammunition is carried.
	const auto weapon = player->getWeapon(true);
	result.ammunitionRequired = weapon && weapon->getWeaponType() == WEAPON_DISTANCE && weapon->getAmmoType() != AMMO_NONE;
	uint64_t signature = supplyMix(1469598103934665603ULL, result.freeCapacity);
	for (const auto &entry : result.entries) {
		signature = supplyMix(signature, entry.signature);
	}
	result.inventorySignature = signature;
	result.revision = signature ? signature : 1;
	return result;
}

BotSupplyAssessment BotSupply::assess(const BotSupplyObservation &observation, const BotSupplyPolicy &policy, uint64_t expectedInventorySignature) {
	BotSupplyAssessment result;
	if (observation.revision == 0) {
		result.intent = BotSupplyIntent::ObservationStale;
		result.failure = BotSupplyFailure::InvalidLifecycle;
		result.urgency = BotSupplyUrgency::Critical;
		return result;
	}
	if (expectedInventorySignature && expectedInventorySignature != observation.inventorySignature) {
		result.intent = BotSupplyIntent::ObservationStale;
		result.failure = BotSupplyFailure::ObservationStale;
		result.urgency = BotSupplyUrgency::High;
		return result;
	}

	result.evaluatedEntries = static_cast<uint32_t>(observation.entries.size());
	if (observation.entries.size() >= policy.maxEntries) {
		result.failure = BotSupplyFailure::EvaluationBudgetExceeded;
	}
	std::map<BotSupplyCategory, uint64_t> totals;
	for (const auto &entry : observation.entries) {
		const uint64_t amount = entry.charges ? entry.charges : entry.count;
		totals[entry.category] = std::min<uint64_t>(UINT32_MAX, totals[entry.category] + amount);
	}
	for (const auto &[category, total] : totals) {
		result.totals.emplace_back(category, static_cast<uint32_t>(total));
	}

	if (observation.freeCapacity <= policy.stopFreeCapacityBelow) {
		result.intent = BotSupplyIntent::CapacityFull;
		result.urgency = BotSupplyUrgency::Critical;
		result.reasonScore = 500;
		return result;
	}
	for (const auto &threshold : policy.thresholds) {
		const auto total = static_cast<uint32_t>(totals[threshold.category]);
		if (threshold.required && total <= threshold.stopBelow) {
			result.intent = threshold.category == BotSupplyCategory::HealthHealing ? BotSupplyIntent::NoHealingSupplies : threshold.category == BotSupplyCategory::Ammunition ? BotSupplyIntent::NoAmmunition : BotSupplyIntent::ReturnRequired;
			result.urgency = BotSupplyUrgency::Critical;
			result.reasonScore = 400 + threshold.stopBelow - total;
			return result;
		}
		if (total <= threshold.conserveBelow) {
			result.intent = BotSupplyIntent::Conserve;
			result.urgency = std::max(result.urgency, BotSupplyUrgency::Moderate);
			result.reasonScore = std::max<uint64_t>(result.reasonScore, 200 + threshold.conserveBelow - total);
		}
	}
	if (observation.ammunitionRequired && totals[BotSupplyCategory::Ammunition] == 0) {
		result.intent = BotSupplyIntent::NoAmmunition;
		result.urgency = BotSupplyUrgency::Critical;
		result.reasonScore = 450;
		return result;
	}
	if (observation.freeCapacity <= policy.conserveFreeCapacityBelow) {
		result.intent = BotSupplyIntent::Conserve;
		result.urgency = std::max(result.urgency, BotSupplyUrgency::Low);
		result.reasonScore = std::max<uint64_t>(result.reasonScore, 100);
	}
	return result;
}
