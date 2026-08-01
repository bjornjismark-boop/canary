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

enum class BotCampaignState : uint8_t { Idle, Running, Combat, Loot, Supply, DeathDetected, Recovering, Reconstructing, Resumed, SaveBoundary, LogoutReady, Completed, Failed, Cancelled };
enum class BotCampaignFailure : uint8_t { None, UnknownRegion, NoReachableRegion, KillAttemptLimit, CombatFailureLimit, RecoveryLimit, DurationLimit, UnsafeSaveBoundary, StaleObservation, Cancelled };
struct BotConfiguredRegion { std::string id; BotAdventureRegion area; uint16_t maximumTravelRange = 16; auto operator<=>(const BotConfiguredRegion &) const = default; };
struct BotCampaignPolicy {
	std::vector<BotConfiguredRegion> allowedRegions;
	uint16_t targetKillCount = 10;
	uint16_t maximumKillAttempts = 20;
	uint8_t maximumCombatFailures = 3;
	uint8_t maximumRecoveryAttempts = 2;
	uint16_t maximumRouteLength = 32;
	std::chrono::milliseconds maximumDuration { 300000 };
	uint8_t minimumHealthPercent = 40;
	uint32_t minimumHealingReserve = 1;
};
struct BotCampaignProgress {
	BotCampaignState state = BotCampaignState::Idle;
	BotCampaignFailure failure = BotCampaignFailure::None;
	uint16_t authoritativeKills = 0;
	uint16_t killAttempts = 0;
	uint8_t combatFailures = 0;
	uint8_t recoveryAttempts = 0;
	uint64_t lastDeathEvidence = 0;
	uint64_t lastObservationRevision = 0;
	std::chrono::milliseconds startedAt { 0 };
	std::string selectedRegionId;
	bool freshObservationRequired = true;
	bool containsWorldOwnership = false;
	auto operator<=>(const BotCampaignProgress &) const = default;
};

class BotCampaign final {
public:
	static std::optional<BotConfiguredRegion> selectRegion(const BotCampaignPolicy &, const std::vector<std::string> &reachableRegionIds);
	static BotCampaignProgress begin(const BotCampaignPolicy &, std::chrono::milliseconds now = {});
	static BotCampaignProgress recordAttempt(BotCampaignProgress, const BotCampaignPolicy &, std::chrono::milliseconds now = {});
	static BotCampaignProgress recordAuthoritativeKill(BotCampaignProgress, uint64_t deathEvidence, const BotCampaignPolicy &);
	static BotCampaignProgress recordCombatFailure(BotCampaignProgress, const BotCampaignPolicy &);
	static BotCampaignProgress recordDeath(BotCampaignProgress, uint64_t deathEvidence, const BotCampaignPolicy &);
	static BotCampaignProgress reconstruct(BotCampaignProgress, uint64_t observationRevision, const Position &, const BotConfiguredRegion &, const BotCampaignPolicy &);
	static bool safeSaveBoundary(bool combatActive, bool movementActive, bool lootActive, bool survivalActive);
	static BotCampaignProgress cancel(BotCampaignProgress);
};
