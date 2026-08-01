#pragma once

#include "creatures/players/bots/bot_adventure.hpp"
#include "creatures/players/bots/bot_equipment.hpp"
#include "utils/utils_definitions.hpp"

class Player;
using BotHuntAreaId = uint32_t;

enum class BotSkillType : uint8_t { Sword, Axe, Club, Distance, Shielding, MagicLevel };
enum class BotProgressionState : uint8_t { Invalid, Inapplicable, BelowTarget, Progressing, Attained, Blocked, ObservationStale };
enum class BotHuntAreaSuitability : uint8_t { Unknown, Suitable, TooDangerous, TooWeak, NoEligibleTargets, RouteUnreliable, SupplyInefficient, RepeatedSurvivalFailure, RepeatedDeath, ProgressTooSlow, PolicyMismatch, ObservationInsufficient, Stale };
enum class BotHuntAreaFailure : uint8_t { None, InsufficientEvidence, PolicyMismatch, NoTargets, Danger, WeakProgress, RouteFailure, SupplyFailure, SurvivalFailure, Death, StaleObservation };
enum class BotAreaSwitchResult : uint8_t { Stay, WaitForBoundary, Switch, Exhausted, Cooldown, InvalidPolicy };

struct BotLevelTarget { uint32_t level = 0; auto operator<=>(const BotLevelTarget &) const = default; };
struct BotSkillTarget { BotSkillType type = BotSkillType::Sword; uint16_t level = 0; uint16_t requiredVocationId = 0; WeaponType_t requiredWeaponType = WEAPON_NONE; auto operator<=>(const BotSkillTarget &) const = default; };
struct BotProgressionTarget { BotLevelTarget level; std::vector<BotSkillTarget> skills; uint64_t policyRevision = 0; bool containsWorldOwnership = false; auto operator<=>(const BotProgressionTarget &) const = default; };
struct BotSkillObservation { BotSkillType type = BotSkillType::Sword; uint16_t level = 0; uint8_t percent = 0; uint64_t tries = 0; auto operator<=>(const BotSkillObservation &) const = default; };
struct BotProgressionObservation { uint64_t revision = 0; uint32_t level = 0; uint64_t experience = 0; uint16_t vocationId = 0; WeaponType_t weaponType = WEAPON_NONE; std::vector<BotSkillObservation> skills; bool containsWorldOwnership = false; auto operator<=>(const BotProgressionObservation &) const = default; };
struct BotProgressionAssessment { BotProgressionState levelState = BotProgressionState::Invalid; uint32_t levelProgressPermille = 0; std::vector<BotProgressionState> skillStates; bool terminal = false; bool startProgressionCombat = false; uint64_t observationRevision = 0; auto operator<=>(const BotProgressionAssessment &) const = default; };

struct BotHuntAreaPolicy { BotHuntAreaId id = 0; BotAdventureRegion region; uint32_t minimumLevel = 1; uint32_t maximumLevel = UINT32_MAX; std::vector<uint16_t> vocationIds; uint64_t revision = 0; auto operator<=>(const BotHuntAreaPolicy &) const = default; };
struct BotHuntAreaObservation { BotHuntAreaId areaId = 0; uint64_t revision = 0; uint64_t policyRevision = 0; uint16_t encounters = 0; uint16_t successfulKills = 0; uint16_t failedCombats = 0; uint16_t fleeCount = 0; uint16_t deathCount = 0; uint16_t healingUses = 0; uint16_t supplyUses = 0; uint16_t noTargetCycles = 0; uint16_t routeFailures = 0; uint64_t observedExperience = 0; uint32_t elapsedTicks = 0; bool containsWorldOwnership = false; auto operator<=>(const BotHuntAreaObservation &) const = default; };
struct BotHuntAreaAssessment { BotHuntAreaSuitability suitability = BotHuntAreaSuitability::Unknown; BotHuntAreaFailure failure = BotHuntAreaFailure::None; int32_t score = 0; uint16_t sampleCount = 0; uint64_t observationRevision = 0; auto operator<=>(const BotHuntAreaAssessment &) const = default; };
struct BotHuntAreaWindow { uint16_t maximumSamples = 16; std::vector<BotHuntAreaObservation> samples; void record(BotHuntAreaObservation); void clearFor(BotHuntAreaId); };
struct BotAreaSwitchPolicy { std::vector<BotHuntAreaPolicy> areas; uint16_t minimumEvidence = 3; uint8_t maximumRetriesPerArea = 1; uint16_t cooldownObservations = 2; uint64_t revision = 0; };
struct BotAreaSwitchState { BotHuntAreaId activeAreaId = 0; BotHuntAreaId previousAreaId = 0; uint64_t lastObservationRevision = 0; uint64_t policyRevision = 0; uint16_t cooldownRemaining = 0; std::vector<std::pair<BotHuntAreaId, uint8_t>> failures; bool terminal = false; bool containsWorldOwnership = false; auto operator<=>(const BotAreaSwitchState &) const = default; };
struct BotAreaSwitchDecision { BotAreaSwitchResult result = BotAreaSwitchResult::Stay; BotHuntAreaId fromAreaId = 0; BotHuntAreaId toAreaId = 0; BotHuntAreaFailure failure = BotHuntAreaFailure::None; bool freshObservationRequired = false; auto operator<=>(const BotAreaSwitchDecision &) const = default; };

class BotProgression final {
public:
	static BotProgressionObservation observe(const std::shared_ptr<Player> &, uint64_t, WeaponType_t = WEAPON_NONE);
	static BotProgressionAssessment assess(const BotProgressionTarget &, const BotProgressionObservation &, uint64_t previousRevision = 0);
	static BotHuntAreaAssessment assessArea(const BotHuntAreaPolicy &, const BotHuntAreaWindow &, uint32_t playerLevel, uint16_t vocationId);
	static BotAreaSwitchDecision decideSwitch(BotAreaSwitchState &, const BotAreaSwitchPolicy &, const BotHuntAreaAssessment &, bool authoritativeActionPending);
};
