/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_supply.hpp"
#include "creatures/players/bots/bot_survival.hpp"

enum class BotAdventureState : uint8_t { Idle, Preparing, TravelingToArea, Searching, Engaging, Fighting, Recovering, Looting, EvaluatingSupplies, Returning, Completed, Failed, Cancelled, Dead };
enum class BotAdventureIntent : uint8_t { None, Prepare, Travel, Search, Engage, WaitForCombat, Recover, Loot, EvaluateSupplies, Return, Finish, Reobserve, Cancel };
enum class BotAdventureFailure : uint8_t { None, InvalidLifecycle, StaleObservation, DurationLimit, CombatLimit, RepeatedFailure, Dead, Cancelled };

struct BotAdventureRegion {
	Position center;
	uint8_t radius = 0;
	auto operator<=>(const BotAdventureRegion &) const = default;
};

struct BotAdventurePolicy {
	BotAdventureRegion startRegion;
	BotAdventureRegion huntRegion;
	BotAdventureRegion returnRegion;
	std::chrono::milliseconds maximumDuration { 300000 };
	uint16_t maximumCombatCount = 20;
	uint8_t maximumDeathCount = 0;
	uint8_t maximumRepeatedFailures = 3;
	uint8_t minimumHealthPercent = 40;
	uint32_t minimumHealingReserve = 1;
	uint32_t minimumAmmunitionReserve = 0;
	uint32_t maximumUsedCapacity = UINT32_MAX;
	uint32_t targetLevel = 0;
	uint64_t targetExperience = 0;
};

struct BotAdventureObservation {
	uint64_t revision = 0;
	Position position;
	uint32_t level = 0;
	uint64_t experience = 0;
	uint8_t healthPercent = 100;
	uint32_t healingReserve = UINT32_MAX;
	uint32_t ammunitionReserve = UINT32_MAX;
	uint32_t usedCapacity = 0;
	BotSurvivalUrgency survivalUrgency = BotSurvivalUrgency::None;
	BotSupplyIntent supplyIntent = BotSupplyIntent::Continue;
	uint32_t visibleTargetId = 0;
	bool combatActive = false;
	bool targetDefeated = false;
	bool corpseAvailable = false;
	bool lootComplete = false;
	bool actionFailed = false;
	bool dynamicBlocker = false;
	bool dead = false;
	bool containsWorldOwnership = false;
	auto operator<=>(const BotAdventureObservation &) const = default;
};

struct BotAdventureProgress {
	BotAdventureState state = BotAdventureState::Idle;
	BotAdventureIntent intent = BotAdventureIntent::None;
	BotAdventureFailure failure = BotAdventureFailure::None;
	uint16_t combatCount = 0;
	uint8_t deathCount = 0;
	uint8_t repeatedFailures = 0;
	uint64_t lastObservationRevision = 0;
	std::chrono::milliseconds startedAt { 0 };
	std::chrono::milliseconds lastProgressAt { 0 };
	bool containsWorldOwnership = false;
	auto operator<=>(const BotAdventureProgress &) const = default;
};

class BotAdventure final {
public:
	static BotAdventureProgress advance(BotAdventureProgress, const BotAdventureObservation &, std::chrono::milliseconds now, const BotAdventurePolicy & = {});
	static BotAdventureProgress cancel(BotAdventureProgress);
	static bool legalTransition(BotAdventureState, BotAdventureState);
	static bool contains(const BotAdventureRegion &, const Position &);
};
