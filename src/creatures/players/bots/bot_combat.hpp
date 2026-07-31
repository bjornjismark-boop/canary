/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_runtime.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <chrono>
	#include <cstdint>
	#include <optional>
	#include <vector>
#endif

enum class BotCombatCreatureKind : uint8_t { Monster, Player, Npc, Summon, Unknown };
enum class BotCombatVisibility : uint8_t { Visible, Hidden, OutsideKnownArea };
enum class BotCombatReachability : uint8_t { Reachable, Unreachable, Unknown, BudgetExceeded };
enum class BotWeaponCategory : uint8_t { None, Melee, Distance, Wand, Unknown };
enum class BotCombatArchetype : uint8_t { None, Melee, Distance, Magic, Unknown };
enum class BotCombatEligibility : uint8_t {
	Eligible, InvalidLifecycle, InvalidTarget, StaleObservation, NotVisible, DifferentFloor,
	OutsideKnownArea, Unreachable, Friendly, PlayerTargetDisallowed, NpcTargetDisallowed,
	OwnedSummonDisallowed, ProtectedByZone, SecureModeRejected, DeadOrRemoved,
	PolicyRejected, EvaluationBudgetExceeded,
};
enum class BotCombatIntent : uint8_t { NoCombat, Observe, HoldTarget, AcquireTarget, ReleaseTarget, RepositionNeeded, Unsafe, AwaitFreshObservation };
enum class BotCombatFailure : uint8_t { None, InvalidLifecycle, NoTarget, StaleObservation, EvaluationBudgetExceeded, Cancelled };

struct BotRecentDamageObservation {
	uint32_t attackerId = 0;
	uint32_t amount = 0;
	uint64_t revision = 0;
	auto operator<=>(const BotRecentDamageObservation &) const = default;
};

struct BotCombatSelfState {
	Position position;
	int32_t health = 0;
	int32_t maxHealth = 0;
	uint32_t mana = 0;
	uint32_t maxMana = 0;
	uint8_t healthPercent = 0;
	uint8_t manaPercent = 0;
	uint64_t activeConditions = 0;
	BotCombatArchetype archetype = BotCombatArchetype::Unknown;
	BotWeaponCategory weapon = BotWeaponCategory::Unknown;
	uint8_t attackRange = 1;
	uint32_t attackedCreatureId = 0;
	uint32_t followedCreatureId = 0;
	BotRecentDamageObservation recentDamage;
	uint64_t revision = 0;
	auto operator<=>(const BotCombatSelfState &) const = default;
};

struct BotCombatCreatureObservation {
	uint32_t id = 0;
	Position position;
	BotCombatCreatureKind kind = BotCombatCreatureKind::Unknown;
	uint8_t healthPercent = 0;
	uint32_t summonMasterId = 0;
	bool attackingBot = false;
	bool followingBot = false;
	bool recentlyDamagedBot = false;
	bool deadOrRemoved = false;
	bool friendly = false;
	bool protectedByZone = false;
	bool secureModeRejected = false;
	uint16_t directDistance = 0;
	uint32_t routeCost = 0;
	BotCombatReachability reachability = BotCombatReachability::Unknown;
	BotCombatVisibility visibility = BotCombatVisibility::Visible;
	uint64_t revision = 0;
	uint64_t signature = 0;
	auto operator<=>(const BotCombatCreatureObservation &) const = default;
};

struct BotCombatObservation {
	BotCombatSelfState self;
	std::vector<BotCombatCreatureObservation> creatures;
	uint64_t revision = 0;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	auto operator<=>(const BotCombatObservation &) const = default;
};

struct BotCombatPolicy {
	bool allowPlayers = false;
	bool allowNpcs = false;
	bool allowOwnedSummons = false;
	bool allowUnknown = false;
	uint32_t maxCandidates = 64;
	uint32_t maxScoreOperations = 1024;
	int32_t attackerWeight = 1000;
	int32_t recentDamageWeight = 800;
	int32_t followingWeight = 150;
	int32_t retainedTargetWeight = 300;
	int32_t distanceWeight = 20;
	int32_t routeCostWeight = 2;
	int32_t missingHealthWeight = 3;
	int32_t crowdRiskWeight = 25;
	int32_t switchThreshold = 200;
	int32_t scoreLimit = 1000000;
};

struct BotTargetCandidate { uint32_t creatureId = 0; BotCombatEligibility eligibility = BotCombatEligibility::InvalidTarget; uint32_t ordinal = 0; auto operator<=>(const BotTargetCandidate &) const = default; };
struct BotThreatAssessment {
	int32_t attacker = 0, recentDamage = 0, following = 0, distance = 0, route = 0, visibleHealth = 0, crowdRisk = 0, retention = 0, hysteresis = 0, total = 0;
	auto operator<=>(const BotThreatAssessment &) const = default;
};
struct BotTargetScore { uint32_t creatureId = 0; BotCombatEligibility eligibility = BotCombatEligibility::InvalidTarget; BotThreatAssessment breakdown; int32_t total = 0; auto operator<=>(const BotTargetScore &) const = default; };
struct BotTargetLock {
	uint32_t creatureId = 0; int32_t score = 0; uint64_t observationRevision = 0; uint64_t lockDeadline = 0; int32_t switchThreshold = 0; bool cancelled = false;
	void cancel() { *this = {}; cancelled = true; }
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};
struct BotTargetSelectionResult {
	BotCombatIntent intent = BotCombatIntent::NoCombat;
	BotCombatFailure failure = BotCombatFailure::None;
	BotCombatEligibility reason = BotCombatEligibility::InvalidTarget;
	uint32_t selectedCreatureId = 0;
	uint32_t evaluatedCandidates = 0;
	uint32_t scoreOperations = 0;
	bool truncated = false;
	std::vector<BotTargetScore> scores;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};

class Player;
class BotCombat final {
public:
	[[nodiscard]] static std::optional<BotCombatObservation> observe(const std::shared_ptr<Player> &player, const BotObservation &observation);
	[[nodiscard]] static BotCombatEligibility eligible(const BotCombatObservation &observation, const BotCombatCreatureObservation &creature, const BotCombatPolicy &policy);
	[[nodiscard]] static BotThreatAssessment assess(const BotCombatObservation &observation, const BotCombatCreatureObservation &creature, const BotCombatPolicy &policy, uint32_t eligibleHostiles, uint32_t retainedTargetId);
	[[nodiscard]] static BotTargetSelectionResult select(const BotCombatObservation &observation, const BotCombatPolicy &policy, BotTargetLock &lock, bool lifecyclePlaced = true);
	[[nodiscard]] static uint64_t signature(const BotCombatCreatureObservation &creature);
};
