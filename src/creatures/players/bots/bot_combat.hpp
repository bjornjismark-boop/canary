/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_navigation.hpp"

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
enum class BotAttackState : uint8_t {
	Idle, TargetSelected, Validating, AcquiringTarget, InRange, AttackPending, Cooldown,
	RepositionRequired, Repositioning, TargetLost, Failed, Cancelled
};
enum class BotAttackFailure : uint8_t {
	None, InvalidLifecycle, InvalidTarget, StaleObservation, NotVisible, DifferentFloor,
	OutOfRange, LineOfSightBlocked, Unreachable, PolicyRejected, ProtectedByZone,
	SecureModeRejected, MissingWeapon, MissingAmmunition, WorldRejected, TimedOut,
	RetryExhausted, Cancelled
};
enum class BotCombatExecutionOutcome : uint8_t {
	Succeeded, Pending, TargetAcquired, AlreadyTargeting, CooldownActive, RepositionRequired,
	RepositionStarted, TargetLost, InvalidLifecycle, InvalidTarget, StaleObservation, NotVisible,
	DifferentFloor, OutOfRange, LineOfSightBlocked, Unreachable, PolicyRejected, ProtectedByZone,
	SecureModeRejected, MissingWeapon, MissingAmmunition, WorldRejected, TimedOut,
	RetryScheduled, RetryExhausted, Cancelled
};
enum class BotRangeAssessment : uint8_t { InRange, TooClose, TooFar, DifferentFloor, LineOfSightBlocked, UnknownWeaponRange, Unreachable, UnsafePosition };

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
	uint32_t attackSpeed = 0;
	bool ammunitionAvailable = true;
	uint32_t attackedCreatureId = 0;
	uint32_t followedCreatureId = 0;
	BotRecentDamageObservation recentDamage;
	uint64_t revision = 0;
	auto operator<=>(const BotCombatSelfState &) const = default;
};

struct BotCombatExecutionPolicy {
	BotCombatPolicy targetPolicy;
	BotRouteLimits routeLimits { .maxExpandedNodes = 128, .maxRouteLength = 16, .maxPlanningOperations = 2048, .maxReplans = 3, .maxNoProgress = 2 };
	uint32_t maxRepositionAttempts = 3;
	uint16_t maxDistanceFromOrigin = 8;
	uint32_t maxNoProgress = 2;
	uint32_t maxAssignmentRetries = 3;
	std::chrono::milliseconds reassessmentInterval { 100 };
	std::chrono::milliseconds initialRetryBackoff { 100 };
	std::chrono::milliseconds maximumRetryBackoff { 800 };
	std::chrono::milliseconds timeout { 5000 };
};

struct BotCombatExecutionRequest {
	uint32_t targetCreatureId = 0;
	uint64_t observationRevision = 0;
	uint64_t targetSignature = 0;
	Position combatOrigin;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	auto operator<=>(const BotCombatExecutionRequest &) const = default;
};

struct BotAttackTiming {
	std::chrono::milliseconds lastAssignmentAttempt { 0 };
	std::chrono::milliseconds lastAcceptedAssignment { 0 };
	std::chrono::milliseconds nextPermittedReassessment { 0 };
	std::chrono::milliseconds observedNextAttackTime { 0 };
	std::chrono::milliseconds retryDeadline { 0 };
	std::chrono::milliseconds timeoutDeadline { 0 };
	uint32_t retryCount = 0;
};

struct BotCombatPositionRequest {
	uint32_t targetCreatureId = 0;
	Position origin;
	Position targetPosition;
	Position destination;
	uint8_t minimumRange = 1;
	uint8_t maximumRange = 1;
	BotRouteLimits routeLimits;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};

struct BotCombatPositionResult {
	BotCombatExecutionOutcome outcome = BotCombatExecutionOutcome::InvalidTarget;
	BotAttackFailure failure = BotAttackFailure::InvalidTarget;
	BotRangeAssessment range = BotRangeAssessment::UnknownWeaponRange;
	BotCombatPositionRequest request;
	BotRouteResult route;
	uint32_t evaluatedPositions = 0;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};

struct BotCombatExecutionResult {
	BotCombatExecutionOutcome outcome = BotCombatExecutionOutcome::InvalidTarget;
	BotAttackFailure failure = BotAttackFailure::InvalidTarget;
	BotAttackState state = BotAttackState::Idle;
	BotRangeAssessment range = BotRangeAssessment::UnknownWeaponRange;
	uint32_t targetCreatureId = 0;
	uint16_t worldReturnValue = 0;
	uint32_t attempts = 0;
	std::chrono::milliseconds retryAfter { 0 };
	std::optional<BotCombatPositionResult> positioning;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
};

struct BotAttackExecutionState {
	BotAttackState state = BotAttackState::Idle;
	BotCombatExecutionRequest request;
	BotAttackTiming timing;
	Position lastObservedPosition;
	Position lastTargetPosition;
	uint32_t assignmentAttempts = 0;
	uint32_t repositionAttempts = 0;
	uint32_t noProgressCount = 0;
	void cancel() { *this = {}; state = BotAttackState::Cancelled; }
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
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
	[[nodiscard]] static BotRangeAssessment assessRange(const BotCombatObservation &observation, const BotCombatCreatureObservation &creature, bool lineOfSightClear);
	[[nodiscard]] static BotCombatPositionResult position(const BotObservation &observation, const BotCombatObservation &combat, const BotCombatCreatureObservation &creature, const BotCombatExecutionPolicy &policy, bool requireLineOfSight);
	[[nodiscard]] static std::chrono::milliseconds executionBackoff(const BotCombatExecutionPolicy &policy, uint32_t attempt);
};
