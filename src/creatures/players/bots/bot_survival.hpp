/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_combat.hpp"

enum class BotSurvivalUrgency : uint8_t { None, Low, Moderate, High, Critical, Fatal };
enum class BotSurvivalState : uint8_t { Stable, Threatened, HealingRequired, HealingPending, Recovering, FleeRequired, FleePlanning, Fleeing, Safe, Exhausted, DeathDetected, Dead, Failed, Cancelled };
enum class BotHealingKind : uint8_t { None, HealthPotion, ManaPotion, HealingRune, SelfHealingSpell, ConditionRemoval, Regeneration };
enum class BotHealingOutcome : uint8_t { Succeeded, Pending, NoEffect, CooldownActive, Exhausted, MissingItem, MissingMana, RequirementNotMet, ConditionNotPresent, InvalidLifecycle, StaleObservation, WorldRejected, TimedOut, RetryScheduled, RetryExhausted, Dead, Cancelled };
enum class BotFleeOutcome : uint8_t { SafePositionSelected, FleeStarted, Progressing, Safe, NoSafeDestination, RouteUnavailable, Blocked, NoProgress, ThreatStillPresent, DistanceLimitExceeded, TimedOut, RetryScheduled, RetryExhausted, Dead, Cancelled };
enum class BotSurvivalDecision : uint8_t { None, Heal, Flee, Wait, Dead };

struct BotSurvivalObservation {
	uint64_t revision = 0;
	Position position;
	int32_t health = 0;
	int32_t maxHealth = 0;
	uint32_t mana = 0;
	uint32_t maxMana = 0;
	uint8_t healthPercent = 0;
	uint8_t manaPercent = 0;
	uint64_t harmfulConditions = 0;
	uint32_t recentDamage = 0;
	uint16_t visibleHostiles = 0;
	bool attacked = false;
	bool escapeBlocked = false;
	bool dead = false;
	auto operator<=>(const BotSurvivalObservation &) const = default;
};

struct BotSurvivalPolicy {
	uint8_t lowHealthPercent = 75;
	uint8_t moderateHealthPercent = 55;
	uint8_t highHealthPercent = 35;
	uint8_t criticalHealthPercent = 20;
	uint8_t fatalHealthPercent = 5;
	uint32_t recentDamageWeight = 2;
	uint32_t conditionWeight = 100;
	uint32_t hostileWeight = 40;
	uint32_t attackedWeight = 80;
	uint32_t blockedWeight = 80;
	uint32_t scoreLimit = 100000;
	uint16_t maxFleeRadius = 8;
	uint16_t maxRouteLength = 16;
	uint16_t maxCandidates = 128;
	uint8_t maxAttempts = 3;
	uint8_t maxNoProgress = 2;
	std::chrono::milliseconds timeout { 5000 };
	std::chrono::milliseconds initialBackoff { 200 };
	std::chrono::milliseconds maximumBackoff { 1600 };
	bool permitRegeneration = false;
};

struct BotSurvivalScore {
	int32_t health = 0, recentDamage = 0, conditions = 0, hostiles = 0, attacked = 0, blocked = 0, total = 0;
	auto operator<=>(const BotSurvivalScore &) const = default;
};

struct BotHealingOption {
	BotHealingKind kind = BotHealingKind::None;
	uint16_t itemTypeId = 0;
	std::string spell;
	uint32_t availableCount = 0;
	uint32_t manaCost = 0;
	uint32_t minimumHealing = 0;
	uint32_t maximumHealing = 0;
	uint64_t removesConditions = 0;
	bool cooldownActive = false;
	bool requirementsMet = true;
	auto operator<=>(const BotHealingOption &) const = default;
};

struct BotSurvivalAssessment {
	BotSurvivalUrgency urgency = BotSurvivalUrgency::None;
	BotSurvivalDecision decision = BotSurvivalDecision::None;
	BotSurvivalScore score;
	BotHealingOption healing;
	bool fleeAvailable = false;
};

struct BotHealingRequest { BotHealingOption option; uint64_t observationRevision = 0; int32_t preHealth = 0; uint32_t preMana = 0; uint64_t preConditions = 0; uint32_t preItemCount = 0; };
struct BotHealingResult { BotHealingOutcome outcome = BotHealingOutcome::WorldRejected; BotHealingRequest request; int32_t observedHealthDelta = 0; int32_t observedManaDelta = 0; uint64_t removedConditions = 0; int32_t observedItemDelta = 0; uint8_t attempts = 0; std::chrono::milliseconds retryAfter { 0 }; };
struct BotFleeRequest { Position origin; Position destination; BotRouteLimits limits; uint16_t evaluatedCandidates = 0; };
struct BotFleeResult { BotFleeOutcome outcome = BotFleeOutcome::NoSafeDestination; BotFleeRequest request; BotRouteResult route; uint8_t attempts = 0; uint8_t noProgress = 0; };
struct BotDeathObservation { uint64_t revision = 0; Position position; int32_t health = 0; bool authoritativeDead = false; };
struct BotDeathResult { BotSurvivalState state = BotSurvivalState::Stable; BotDeathObservation observation; bool combatCancelled = false; bool movementCancelled = false; };

struct BotSurvivalProgress {
	BotSurvivalState state = BotSurvivalState::Stable;
	std::optional<BotHealingRequest> healing;
	std::optional<BotFleeRequest> flee;
	uint8_t attempts = 0;
	uint8_t noProgress = 0;
	std::chrono::milliseconds startedAt { 0 }, nextActionAt { 0 };
	BotDeathObservation death;
	[[nodiscard]] bool terminal() const { return state == BotSurvivalState::Dead || state == BotSurvivalState::Failed || state == BotSurvivalState::Cancelled; }
};

class BotSurvival final {
public:
	static uint8_t percentage(uint64_t value, uint64_t maximum);
	static BotSurvivalAssessment assess(const BotSurvivalObservation &, const BotSurvivalPolicy &, std::vector<BotHealingOption>, bool fleeAvailable);
	static BotHealingOption selectHealing(const BotSurvivalObservation &, const BotSurvivalPolicy &, std::vector<BotHealingOption>);
	static BotFleeResult selectFlee(const BotObservation &, const BotCombatObservation &, const BotSurvivalPolicy &);
	static std::chrono::milliseconds backoff(const BotSurvivalPolicy &, uint32_t attempt);
	static bool legalTransition(BotSurvivalState from, BotSurvivalState to);
};
