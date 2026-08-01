/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_loot.hpp"

class Player;

enum class BotSupplyCategory : uint8_t { HealthHealing, ManaRestoration, ConditionRemoval, Ammunition, OffensiveRune, Food, UtilityTool, FreeCapacity, UnknownConfigured };
enum class BotSupplyUrgency : uint8_t { None, Low, Moderate, High, Critical };
enum class BotSupplyIntent : uint8_t { Continue, Conserve, StopHunt, ReturnRequired, NoHealingSupplies, NoAmmunition, CapacityFull, ObservationStale, PolicyRejected };
enum class BotSupplyFailure : uint8_t { None, InvalidLifecycle, ObservationStale, EvaluationBudgetExceeded, PolicyRejected };

struct BotSupplyEntry {
	uint16_t itemTypeId = 0;
	BotSupplyCategory category = BotSupplyCategory::UnknownConfigured;
	uint32_t count = 0;
	uint32_t charges = 0;
	bool equipped = false;
	uint8_t depth = 0;
	uint64_t signature = 0;
	auto operator<=>(const BotSupplyEntry &) const = default;
};

struct BotSupplyObservation {
	uint64_t revision = 0;
	uint64_t inventorySignature = 0;
	uint32_t freeCapacity = 0;
	std::vector<BotSupplyEntry> entries;
	bool ammunitionRequired = false;
	bool containsWorldOwnership = false;
	auto operator<=>(const BotSupplyObservation &) const = default;
};

struct BotSupplyRule {
	uint16_t itemTypeId = 0;
	BotSupplyCategory category = BotSupplyCategory::UnknownConfigured;
	auto operator<=>(const BotSupplyRule &) const = default;
};

struct BotSupplyThreshold {
	BotSupplyCategory category = BotSupplyCategory::UnknownConfigured;
	uint32_t conserveBelow = 0;
	uint32_t stopBelow = 0;
	bool required = false;
	auto operator<=>(const BotSupplyThreshold &) const = default;
};

struct BotSupplyPolicy {
	std::vector<BotSupplyRule> rules;
	std::vector<BotSupplyThreshold> thresholds;
	uint32_t conserveFreeCapacityBelow = 2000;
	uint32_t stopFreeCapacityBelow = 100;
	uint16_t maxEntries = 128;
	uint8_t maxContainers = 16;
	uint8_t maxDepth = 2;
};

struct BotSupplyAssessment {
	BotSupplyUrgency urgency = BotSupplyUrgency::None;
	BotSupplyIntent intent = BotSupplyIntent::Continue;
	BotSupplyFailure failure = BotSupplyFailure::None;
	std::vector<std::pair<BotSupplyCategory, uint32_t>> totals;
	uint32_t evaluatedEntries = 0;
	uint64_t reasonScore = 0;
};

class BotSupply final {
public:
	static BotSupplyObservation observe(const std::shared_ptr<Player> &, const BotSupplyPolicy & = {});
	static BotSupplyAssessment assess(const BotSupplyObservation &, const BotSupplyPolicy & = {}, uint64_t expectedInventorySignature = 0);
};
