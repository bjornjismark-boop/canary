/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_dialogue.hpp"

class Player;

using BotQuestId = uint32_t;
using BotMissionId = uint32_t;

enum class BotMissionState : uint8_t { Unavailable, Available, Started, InProgress, ObjectiveComplete, TurnInRequired, Completed, Failed, Unknown, Stale };
enum class BotQuestPrerequisiteType : uint8_t { MinimumLevel, Vocation, PreviousMissionComplete, ItemPresent, MoneyAvailable, StoragePredicate, LocationReached, NpcAvailable, ObjectiveCount };
enum class BotQuestObjectiveType : uint8_t { TalkToNpc, VisitLocation, UseObject, KillCreatureType, CollectItem, DeliverItem, AcquireMoney, CompletePreviousMission };
enum class BotQuestRewardType : uint8_t { ItemReceived, ItemRemoved, MoneyChanged, ExperienceIncreased, StorageChanged, MissionStateChanged, AccessObserved, TeleportObserved, QuestLogChanged };
enum class BotQuestFailure : uint8_t { Eligible, InvalidLifecycle, LevelTooLow, WrongVocation, PreviousMissionIncomplete, MissingItem, InsufficientMoney, StorageConditionNotMet, NpcUnavailable, LocationUnavailable, ObjectiveIncomplete, ObservationStale, PolicyRejected, EvaluationBudgetExceeded };
enum class BotQuestVerificationResult : uint8_t { Verified, Pending, NoChange, Partial, UnexpectedReward, ExpectedItemMissing, ExpectedExperienceMissing, MissionStateUnchanged, ObservationStale, TimedOut, Cancelled };

struct BotStoragePredicate {
	uint32_t key = 0;
	int32_t minimum = -1;
	int32_t maximum = INT32_MAX;
	auto operator<=>(const BotStoragePredicate &) const = default;
};

struct BotQuestPrerequisite {
	BotQuestPrerequisiteType type = BotQuestPrerequisiteType::MinimumLevel;
	uint32_t value = 0;
	uint16_t itemTypeId = 0;
	BotMissionId missionId = 0;
	BotStoragePredicate storage;
	Position position;
	uint8_t radius = 0;
	std::string configuredNpcName;
	auto operator<=>(const BotQuestPrerequisite &) const = default;
};

struct BotQuestObjective {
	BotQuestObjectiveType type = BotQuestObjectiveType::TalkToNpc;
	uint32_t requiredCount = 1;
	uint16_t itemTypeId = 0;
	uint32_t creatureTypeId = 0;
	BotMissionId missionId = 0;
	Position position;
	uint8_t radius = 0;
	std::string configuredNpcName;
	std::string requiredDialogueToken;
	auto operator<=>(const BotQuestObjective &) const = default;
};

struct BotQuestRewardExpectation {
	BotQuestRewardType type = BotQuestRewardType::MissionStateChanged;
	uint16_t itemTypeId = 0;
	uint64_t minimumDelta = 1;
	auto operator<=>(const BotQuestRewardExpectation &) const = default;
};

struct BotMissionDefinition {
	BotMissionId id = 0;
	std::string visibleName;
	std::string visibleDescription;
	BotStoragePredicate stateStorage;
	int32_t availableValue = -1;
	int32_t startedValue = 0;
	int32_t objectiveCompleteValue = 0;
	int32_t completedValue = 0;
	bool turnInRequired = false;
	std::vector<BotQuestPrerequisite> prerequisites;
	std::vector<BotQuestObjective> objectives;
	std::vector<BotQuestRewardExpectation> rewards;
	auto operator<=>(const BotMissionDefinition &) const = default;
};

struct BotQuestDefinition {
	BotQuestId id = 0;
	std::string visibleName;
	uint64_t revision = 1;
	std::vector<BotMissionDefinition> missions;
	auto operator<=>(const BotQuestDefinition &) const = default;
};

struct BotQuestBounds {
	uint16_t maximumQuests = 32;
	uint16_t maximumMissionsPerQuest = 32;
	uint16_t maximumPrerequisites = 32;
	uint16_t maximumObjectives = 32;
	uint16_t maximumStoragePredicates = 16;
	uint16_t maximumRewardChecks = 16;
	uint32_t maximumOperations = 2048;
	uint16_t maximumEvidence = 64;
	uint64_t maximumObservationAge = 8;
};

struct BotQuestEvidence {
	BotQuestObjectiveType type = BotQuestObjectiveType::TalkToNpc;
	uint32_t subjectId = 0;
	std::string normalizedSubjectName;
	uint32_t count = 0;
	Position position;
	uint64_t revision = 0;
	uint64_t tokenHash = 0;
	auto operator<=>(const BotQuestEvidence &) const = default;
};

struct BotQuestProgress { BotQuestObjectiveType type = BotQuestObjectiveType::TalkToNpc; uint32_t observed = 0; uint32_t required = 0; bool complete = false; auto operator<=>(const BotQuestProgress &) const = default; };
struct BotMissionObservation {
	BotMissionId missionId = 0;
	BotMissionState state = BotMissionState::Unknown;
	int32_t configuredStorageValue = -1;
	std::string visibleName;
	std::string visibleDescription;
	std::vector<BotQuestProgress> progress;
	uint64_t revision = 0;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	auto operator<=>(const BotMissionObservation &) const = default;
};
struct BotQuestObservation {
	BotQuestId questId = 0;
	std::string visibleName;
	uint32_t level = 0;
	uint16_t vocationId = 0;
	uint64_t money = 0;
	uint64_t experience = 0;
	Position position;
	std::vector<std::pair<uint16_t, uint32_t>> items;
	std::vector<std::pair<uint32_t, int32_t>> configuredStorages;
	std::vector<BotMissionObservation> missions;
	std::vector<BotQuestEvidence> evidence;
	uint64_t revision = 0;
	uint64_t definitionRevision = 0;
	bool truncated = false;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	auto operator<=>(const BotQuestObservation &) const = default;
};
struct BotQuestEligibility { BotQuestFailure result = BotQuestFailure::Eligible; uint16_t prerequisiteIndex = 0; uint32_t operations = 0; bool eligible() const { return result == BotQuestFailure::Eligible; } auto operator<=>(const BotQuestEligibility &) const = default; };
struct BotQuestRewardObservation { uint64_t money = 0; uint64_t experience = 0; BotMissionState missionState = BotMissionState::Unknown; uint64_t questRevision = 0; std::vector<std::pair<uint16_t, uint32_t>> items; std::vector<std::pair<uint32_t, int32_t>> configuredStorages; [[nodiscard]] bool containsWorldOwnership() const { return false; } auto operator<=>(const BotQuestRewardObservation &) const = default; };
struct BotQuestVerification { BotQuestVerificationResult result = BotQuestVerificationResult::Pending; uint16_t verified = 0; uint16_t expected = 0; uint16_t unexpected = 0; std::vector<BotQuestRewardType> evidence; [[nodiscard]] bool containsWorldOwnership() const { return false; } auto operator<=>(const BotQuestVerification &) const = default; };

class BotQuest final {
public:
	static BotQuestObservation observe(const std::shared_ptr<Player> &, const BotQuestDefinition &, std::vector<BotQuestEvidence> = {}, const BotQuestBounds & = {});
	static BotMissionState normalize(const BotMissionDefinition &, int32_t storageValue, bool objectivesComplete, bool stale = false);
	static BotQuestEligibility evaluate(const BotQuestObservation &, const BotMissionDefinition &, const BotDialogueObservation & = {}, const BotQuestBounds & = {});
	static BotQuestRewardObservation rewardObservation(const BotQuestObservation &, BotMissionId);
	static BotQuestVerification verifyRewards(const BotQuestRewardObservation &, const BotQuestRewardObservation &, const std::vector<BotQuestRewardExpectation> &, bool stale = false, const BotQuestBounds & = {});
	static uint64_t dialogueTokenHash(std::string_view);
};
