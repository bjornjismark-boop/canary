/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_navigation.hpp"

class Item;
class Player;

enum class BotCorpseOwnership : uint8_t { Unrestricted, Self, Party, RewardEligible, Denied };
enum class BotLootEligibility : uint8_t { Eligible, InvalidLifecycle, InvalidCorpse, StaleObservation, NotVisible, OutsideKnownArea, TooFar, Unreachable, NoLootRights, CorpseExpired, ContainerUnavailable, PolicyRejected, CapacityInsufficient, EvaluationBudgetExceeded };
enum class BotLootFailure : uint8_t { None, InvalidLifecycle, InvalidCorpse, StaleObservation, NotVisible, OutsideKnownArea, TooFar, Unreachable, NoLootRights, CorpseExpired, ContainerUnavailable, PolicyRejected, CapacityInsufficient, EvaluationBudgetExceeded };

struct BotCorpseSignature {
	uint64_t value = 0;
	auto operator<=>(const BotCorpseSignature &) const = default;
};

struct BotLootItemObservation {
	uint16_t itemTypeId = 0;
	uint32_t count = 0;
	uint32_t weight = 0;
	uint16_t stackPosition = 0;
	uint8_t depth = 0;
	bool stackable = false;
	bool nestedContainer = false;
	uint64_t signature = 0;
	auto operator<=>(const BotLootItemObservation &) const = default;
};

struct BotCorpseObservation {
	uint64_t observationRevision = 0;
	Position position;
	uint16_t corpseItemTypeId = 0;
	BotCorpseSignature signature;
	uint32_t sourceCreatureId = 0;
	uint32_t ownerCreatureId = 0;
	BotCorpseOwnership ownership = BotCorpseOwnership::Denied;
	uint32_t remainingDecayMilliseconds = 0;
	uint16_t containerCapacity = 0;
	uint16_t containerSize = 0;
	std::vector<BotLootItemObservation> items;
	bool containsWorldOwnership = false;
	auto operator<=>(const BotCorpseObservation &) const = default;
};

struct BotLootRule {
	uint16_t itemTypeId = 0;
	uint8_t valueCategory = 0;
	uint16_t priority = 0;
	auto operator<=>(const BotLootRule &) const = default;
};

struct BotLootPolicy {
	std::vector<BotLootRule> rules;
	uint16_t maxDistance = 8;
	uint16_t maxCorpseCandidates = 16;
	uint16_t maxItemCandidates = 64;
	uint8_t maxNestedDepth = 1;
	uint32_t maxProjectedWeight = std::numeric_limits<uint32_t>::max();
	bool rejectUnconfiguredItems = true;
};

struct BotLootCandidate {
	BotLootItemObservation item;
	BotLootEligibility eligibility = BotLootEligibility::PolicyRejected;
	uint8_t valueCategory = 0;
	uint16_t priority = 0;
	uint32_t projectedWeight = 0;
	uint64_t score = 0;
	auto operator<=>(const BotLootCandidate &) const = default;
};

struct BotLootSelectionResult {
	BotLootEligibility eligibility = BotLootEligibility::InvalidCorpse;
	BotLootFailure failure = BotLootFailure::InvalidCorpse;
	BotCorpseObservation corpse;
	std::vector<BotLootCandidate> candidates;
	std::optional<BotLootCandidate> selected;
	uint16_t evaluatedItems = 0;
};

class BotLoot final {
public:
	static BotLootSelectionResult observe(const std::shared_ptr<Player> &player, const BotObservation &world, const Position &position, uint32_t sourceCreatureId, BotCorpseSignature expectedSignature, const BotLootPolicy &policy = {});
	static BotLootSelectionResult select(const BotCorpseObservation &corpse, uint32_t freeCapacity, const BotLootPolicy &policy = {});
	static BotCorpseSignature signature(const BotCorpseObservation &corpse);
};
