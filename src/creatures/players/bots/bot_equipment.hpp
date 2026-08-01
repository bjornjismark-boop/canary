/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "enums/item_attribute.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <cstdint>
	#include <optional>
	#include <string>
	#include <vector>
#endif

class Player;

enum class BotItemCategory : uint8_t { Unknown, Weapon, Shield, Helmet, Armor, Legs, Boots, Amulet, Ring, Backpack, Ammunition, Supply };
enum class BotEquipmentSlot : uint8_t { None, Head, Necklace, Backpack, Armor, RightHand, LeftHand, Legs, Feet, Ring, Ammunition };
enum class BotItemValueSource : uint8_t { Unknown, IntrinsicMetadata, PlayerBotPolicy, NpcObservation };
enum class BotEquipmentIntent : uint8_t { KeepEquipped, UpgradeAvailable, DowngradeRejected, MissingRequirement, WrongVocation, WrongSlot, CapacityRisk, SellCandidate, KeepForSupply, UnknownValue, FreshObservationRequired };
enum class BotValuationFailure : uint8_t { None, InvalidLifecycle, InvalidItem, StaleObservation, WrongVocation, MissingLevel, WrongSlot, CapacityRisk, ContainerBudgetExceeded, ItemBudgetExceeded, ScoreBudgetExceeded };
enum class BotEquipmentRole : uint8_t { Melee, Distance, Magic };

struct BotInventoryPath {
	uint8_t rootSlot = 0;
	std::vector<uint16_t> childIndices;
	auto operator<=>(const BotInventoryPath &) const = default;
};

struct BotKnownPrice {
	uint16_t itemTypeId = 0;
	uint32_t npcBuyPrice = 0;
	uint32_t npcSellPrice = 0;
	BotItemValueSource source = BotItemValueSource::Unknown;
	auto operator<=>(const BotKnownPrice &) const = default;
};

struct BotItemObservation {
	uint16_t itemTypeId = 0;
	uint32_t countOrCharges = 0;
	BotItemCategory category = BotItemCategory::Unknown;
	BotEquipmentSlot slot = BotEquipmentSlot::None;
	BotInventoryPath path;
	uint32_t weight = 0;
	uint32_t minimumLevel = 0;
	std::string vocationRequirement;
	uint8_t weaponType = 0;
	int32_t attack = 0;
	int32_t defense = 0;
	int32_t armor = 0;
	uint8_t range = 0;
	int32_t speed = 0;
	std::vector<int32_t> skillModifiers;
	std::vector<int16_t> resistances;
	uint32_t durationMilliseconds = 0;
	bool temporary = false;
	bool equipped = false;
	bool vocationCompatible = true;
	bool levelCompatible = true;
	bool supply = false;
	std::optional<uint32_t> knownNpcBuyPrice;
	std::optional<uint32_t> knownNpcSellPrice;
	BotItemValueSource priceSource = BotItemValueSource::Unknown;
	uint64_t signature = 0;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	auto operator<=>(const BotItemObservation &) const = default;
};

struct BotEquipmentObservation {
	uint64_t revision = 0;
	uint32_t freeCapacity = 0;
	uint32_t playerLevel = 0;
	uint16_t vocationId = 0;
	std::vector<BotItemObservation> items;
	bool containerBudgetExceeded = false;
	bool itemBudgetExceeded = false;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};

struct BotEquipmentPolicy {
	BotEquipmentRole role = BotEquipmentRole::Melee;
	std::vector<BotKnownPrice> knownPrices;
	std::vector<uint16_t> supplyItemTypeIds;
	uint8_t maximumInventoryContainers = 16;
	uint8_t maximumNestingDepth = 2;
	uint16_t maximumItemsEvaluated = 128;
	uint16_t maximumScoreOperations = 1024;
	uint32_t minimumFreeCapacity = 0;
	int32_t upgradeThreshold = 100;
	int32_t retentionBonus = 75;
};

struct BotItemScore {
	int32_t attack = 0;
	int32_t defense = 0;
	int32_t armor = 0;
	int32_t range = 0;
	int32_t modifiers = 0;
	int32_t durability = 0;
	int32_t weight = 0;
	int32_t supplyCompatibility = 0;
	int32_t retention = 0;
	int32_t total = 0;
	auto operator<=>(const BotItemScore &) const = default;
};

struct BotEquipmentComparison {
	BotEquipmentIntent intent = BotEquipmentIntent::UnknownValue;
	BotValuationFailure failure = BotValuationFailure::None;
	BotItemScore currentScore;
	BotItemScore candidateScore;
	int32_t improvement = 0;
	uint32_t scoreOperations = 0;
	auto operator<=>(const BotEquipmentComparison &) const = default;
};

struct BotUpgradeCandidate {
	uint16_t itemTypeId = 0;
	BotInventoryPath path;
	BotEquipmentSlot slot = BotEquipmentSlot::None;
	BotEquipmentComparison comparison;
	uint64_t observationRevision = 0;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};

class BotEquipment final {
public:
	static BotEquipmentObservation observe(const std::shared_ptr<Player> &, const BotEquipmentPolicy & = {});
	static BotItemScore score(const BotItemObservation &, const BotEquipmentPolicy &, uint32_t &operations);
	static BotEquipmentComparison compare(const BotItemObservation *current, const BotItemObservation &, const BotEquipmentObservation &, const BotEquipmentPolicy & = {});
	static std::vector<BotUpgradeCandidate> upgrades(const BotEquipmentObservation &, const BotEquipmentPolicy & = {});
	static uint64_t signature(const BotItemObservation &);
};
