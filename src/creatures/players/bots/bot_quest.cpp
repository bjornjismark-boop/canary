/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_quest.hpp"

#include "creatures/players/player.hpp"
#include "items/containers/container.hpp"

namespace {
uint64_t saturatingAdd(uint64_t left, uint64_t right) { return right > UINT64_MAX - left ? UINT64_MAX : left + right; }
uint32_t itemCount(const std::vector<std::pair<uint16_t, uint32_t>> &items, uint16_t id) { const auto it = std::ranges::find(items, id, &std::pair<uint16_t, uint32_t>::first); return it == items.end() ? 0 : it->second; }
int32_t storageValue(const BotQuestObservation &observation, uint32_t key) { const auto it = std::ranges::find(observation.configuredStorages, key, &std::pair<uint32_t, int32_t>::first); return it == observation.configuredStorages.end() ? -1 : it->second; }
bool inRegion(const Position &value, const Position &center, uint8_t radius) { return value.z == center.z && Position::areInRange<UINT16_MAX, UINT16_MAX, 0>(value, center) && std::max(Position::getDistanceX(value, center), Position::getDistanceY(value, center)) <= radius; }
}

uint64_t BotQuest::dialogueTokenHash(std::string_view value) { uint64_t hash = 1469598103934665603ULL; for (const auto c : value) { hash ^= static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c))); hash *= 1099511628211ULL; } return hash; }

BotMissionState BotQuest::normalize(const BotMissionDefinition &definition, int32_t value, bool objectivesComplete, bool stale) {
	if (stale) return BotMissionState::Stale;
	if (value < definition.availableValue) return BotMissionState::Unavailable;
	if (value < definition.startedValue) return BotMissionState::Available;
	if (value >= definition.completedValue) return BotMissionState::Completed;
	if (objectivesComplete) return definition.turnInRequired ? BotMissionState::TurnInRequired : BotMissionState::ObjectiveComplete;
	return value == definition.startedValue ? BotMissionState::Started : BotMissionState::InProgress;
}

BotQuestObservation BotQuest::observe(const std::shared_ptr<Player> &player, const BotQuestDefinition &definition, std::vector<BotQuestEvidence> evidence, const BotQuestBounds &bounds) {
	BotQuestObservation result { .questId = definition.id, .visibleName = definition.visibleName, .definitionRevision = definition.revision };
	if (!player || !player->isBotControlled() || player->isRemoved() || !player->getTile() || bounds.maximumQuests == 0 || definition.missions.size() > bounds.maximumMissionsPerQuest) return result;
	result.level = player->getLevel(); result.vocationId = player->getVocationId(); result.money = saturatingAdd(player->getMoney(), player->getBankBalance()); result.experience = player->getExperience(); result.position = player->getPosition();
	std::vector<uint16_t> itemIds; std::vector<uint32_t> storageKeys;
	for (const auto &mission : definition.missions) {
		if (mission.prerequisites.size() > bounds.maximumPrerequisites || mission.objectives.size() > bounds.maximumObjectives || mission.rewards.size() > bounds.maximumRewardChecks) result.truncated = true;
		storageKeys.push_back(mission.stateStorage.key);
		for (const auto &p : mission.prerequisites) { if (p.itemTypeId) itemIds.push_back(p.itemTypeId); if (p.type == BotQuestPrerequisiteType::StoragePredicate) storageKeys.push_back(p.storage.key); }
		for (const auto &o : mission.objectives) if (o.itemTypeId) itemIds.push_back(o.itemTypeId);
		for (const auto &r : mission.rewards) if (r.itemTypeId) itemIds.push_back(r.itemTypeId);
	}
	std::ranges::sort(itemIds); itemIds.erase(std::unique(itemIds.begin(), itemIds.end()), itemIds.end());
	std::ranges::sort(storageKeys); storageKeys.erase(std::unique(storageKeys.begin(), storageKeys.end()), storageKeys.end());
	if (storageKeys.size() > bounds.maximumStoragePredicates) { storageKeys.resize(bounds.maximumStoragePredicates); result.truncated = true; }
	for (const auto key : storageKeys) result.configuredStorages.emplace_back(key, player->getStorageValue(key));
	for (const auto id : itemIds) result.items.emplace_back(id, std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(id));
	std::ranges::sort(evidence, [](const auto &a, const auto &b) { return std::tie(a.revision, a.type, a.subjectId, a.normalizedSubjectName, a.count, a.position) < std::tie(b.revision, b.type, b.subjectId, b.normalizedSubjectName, b.count, b.position); });
	if (evidence.size() > bounds.maximumEvidence) { evidence.resize(bounds.maximumEvidence); result.truncated = true; }
	result.evidence = std::move(evidence);
	uint64_t revision = definition.revision;
	for (const auto &mission : definition.missions) {
		BotMissionObservation observed { .missionId = mission.id, .configuredStorageValue = storageValue(result, mission.stateStorage.key), .visibleName = mission.visibleName, .visibleDescription = mission.visibleDescription };
		bool complete = true;
		for (const auto &objective : mission.objectives | std::views::take(bounds.maximumObjectives)) {
			uint64_t count = 0;
			if (objective.type == BotQuestObjectiveType::CollectItem || objective.type == BotQuestObjectiveType::DeliverItem) count = itemCount(result.items, objective.itemTypeId);
			else if (objective.type == BotQuestObjectiveType::AcquireMoney) count = result.money;
			else if (objective.type == BotQuestObjectiveType::VisitLocation) count = inRegion(result.position, objective.position, objective.radius) ? 1 : 0;
			else for (const auto &entry : result.evidence) {
				if (entry.type != objective.type) continue;
				if (objective.type == BotQuestObjectiveType::KillCreatureType && objective.creatureTypeId != 0 && entry.subjectId != objective.creatureTypeId) continue;
				if (objective.type == BotQuestObjectiveType::TalkToNpc && !objective.configuredNpcName.empty() && entry.normalizedSubjectName != objective.configuredNpcName) continue;
				if (objective.type == BotQuestObjectiveType::TalkToNpc && !objective.requiredDialogueToken.empty() && entry.tokenHash != dialogueTokenHash(objective.requiredDialogueToken)) continue;
				count = saturatingAdd(count, entry.count);
			}
			const auto bounded = static_cast<uint32_t>(std::min<uint64_t>(count, UINT32_MAX)); observed.progress.push_back({ objective.type, bounded, objective.requiredCount, bounded >= objective.requiredCount }); complete &= bounded >= objective.requiredCount;
		}
		observed.state = normalize(mission, observed.configuredStorageValue, complete && !mission.objectives.empty()); observed.revision = ++revision; result.missions.push_back(std::move(observed));
	}
	result.revision = revision; return result;
}

BotQuestEligibility BotQuest::evaluate(const BotQuestObservation &observation, const BotMissionDefinition &mission, const BotDialogueObservation &dialogue, const BotQuestBounds &bounds) {
	BotQuestEligibility result;
	if (observation.revision == 0) { result.result = BotQuestFailure::InvalidLifecycle; return result; }
	if (std::ranges::any_of(observation.missions, [](const auto &entry) { return entry.state == BotMissionState::Stale; })) { result.result = BotQuestFailure::ObservationStale; return result; }
	if (observation.truncated) { result.result = BotQuestFailure::EvaluationBudgetExceeded; return result; }
	const auto count = std::min<size_t>(mission.prerequisites.size(), bounds.maximumPrerequisites);
	for (size_t index = 0; index < count; ++index) {
		const auto &p = mission.prerequisites[index]; result.prerequisiteIndex = static_cast<uint16_t>(index); if (++result.operations > bounds.maximumOperations) { result.result = BotQuestFailure::EvaluationBudgetExceeded; return result; }
		switch (p.type) {
			case BotQuestPrerequisiteType::MinimumLevel: if (observation.level < p.value) result.result = BotQuestFailure::LevelTooLow; break;
			case BotQuestPrerequisiteType::Vocation: if (observation.vocationId != p.value) result.result = BotQuestFailure::WrongVocation; break;
			case BotQuestPrerequisiteType::PreviousMissionComplete: { const auto it = std::ranges::find(observation.missions, p.missionId, &BotMissionObservation::missionId); if (it == observation.missions.end() || it->state != BotMissionState::Completed) result.result = BotQuestFailure::PreviousMissionIncomplete; break; }
			case BotQuestPrerequisiteType::ItemPresent: if (itemCount(observation.items, p.itemTypeId) < p.value) result.result = BotQuestFailure::MissingItem; break;
			case BotQuestPrerequisiteType::MoneyAvailable: if (observation.money < p.value) result.result = BotQuestFailure::InsufficientMoney; break;
			case BotQuestPrerequisiteType::StoragePredicate: { const auto value = storageValue(observation, p.storage.key); if (value < p.storage.minimum || value > p.storage.maximum) result.result = BotQuestFailure::StorageConditionNotMet; break; }
			case BotQuestPrerequisiteType::LocationReached: if (!inRegion(observation.position, p.position, p.radius)) result.result = BotQuestFailure::LocationUnavailable; break;
			case BotQuestPrerequisiteType::NpcAvailable: if (std::ranges::none_of(dialogue.npcs, [&](const auto &npc) { return npc.visible && npc.sameFloor && npc.normalizedName == p.configuredNpcName; })) result.result = BotQuestFailure::NpcUnavailable; break;
			case BotQuestPrerequisiteType::ObjectiveCount: { const auto it = std::ranges::find(observation.missions, mission.id, &BotMissionObservation::missionId); if (it == observation.missions.end() || std::ranges::any_of(it->progress, [](const auto &progress) { return !progress.complete; })) result.result = BotQuestFailure::ObjectiveIncomplete; break; }
		}
		if (result.result != BotQuestFailure::Eligible) return result;
	}
	if (mission.prerequisites.size() > bounds.maximumPrerequisites) result.result = BotQuestFailure::EvaluationBudgetExceeded;
	return result;
}

BotQuestRewardObservation BotQuest::rewardObservation(const BotQuestObservation &observation, BotMissionId missionId) {
	BotQuestRewardObservation result { .money = observation.money, .experience = observation.experience, .questRevision = observation.revision, .items = observation.items, .configuredStorages = observation.configuredStorages };
	if (const auto it = std::ranges::find(observation.missions, missionId, &BotMissionObservation::missionId); it != observation.missions.end()) result.missionState = it->state;
	return result;
}

BotQuestVerification BotQuest::verifyRewards(const BotQuestRewardObservation &before, const BotQuestRewardObservation &after, const std::vector<BotQuestRewardExpectation> &expected, bool stale, const BotQuestBounds &bounds) {
	BotQuestVerification result { .expected = static_cast<uint16_t>(std::min<size_t>(expected.size(), bounds.maximumRewardChecks)) };
	if (stale || after.questRevision <= before.questRevision) { result.result = BotQuestVerificationResult::ObservationStale; return result; }
	for (const auto &check : expected | std::views::take(bounds.maximumRewardChecks)) {
		bool matched = false;
		switch (check.type) {
			case BotQuestRewardType::ItemReceived: { const auto oldCount = itemCount(before.items, check.itemTypeId), newCount = itemCount(after.items, check.itemTypeId); matched = newCount >= oldCount && static_cast<uint64_t>(newCount - oldCount) >= check.minimumDelta; break; }
			case BotQuestRewardType::ItemRemoved: { const auto oldCount = itemCount(before.items, check.itemTypeId), newCount = itemCount(after.items, check.itemTypeId); matched = oldCount >= newCount && static_cast<uint64_t>(oldCount - newCount) >= check.minimumDelta; break; }
			case BotQuestRewardType::MoneyChanged: matched = after.money != before.money; break;
			case BotQuestRewardType::ExperienceIncreased: matched = after.experience >= before.experience && after.experience - before.experience >= check.minimumDelta; break;
			case BotQuestRewardType::StorageChanged: matched = after.configuredStorages != before.configuredStorages; break;
			case BotQuestRewardType::MissionStateChanged: matched = after.missionState != before.missionState; break;
			case BotQuestRewardType::AccessObserved: case BotQuestRewardType::TeleportObserved: case BotQuestRewardType::QuestLogChanged: matched = after.questRevision != before.questRevision; break;
		}
		if (matched) { ++result.verified; result.evidence.push_back(check.type); }
	}
	const auto expectedType = [&](BotQuestRewardType type) { return std::ranges::any_of(expected | std::views::take(bounds.maximumRewardChecks), [=](const auto &entry) { return entry.type == type; }); };
	const bool unexpectedMoney = after.money != before.money && !expectedType(BotQuestRewardType::MoneyChanged);
	const bool unexpectedExperience = after.experience != before.experience && !expectedType(BotQuestRewardType::ExperienceIncreased);
	const bool unexpectedStorage = after.configuredStorages != before.configuredStorages && !expectedType(BotQuestRewardType::StorageChanged) && !expectedType(BotQuestRewardType::MissionStateChanged);
	const bool unexpectedMission = after.missionState != before.missionState && !expectedType(BotQuestRewardType::MissionStateChanged);
	bool unexpectedItem = false;
	for (const auto &[id, count] : after.items) {
		if (count == itemCount(before.items, id)) continue;
		const bool configured = std::ranges::any_of(expected | std::views::take(bounds.maximumRewardChecks), [=](const auto &entry) { return entry.itemTypeId == id && (entry.type == BotQuestRewardType::ItemReceived || entry.type == BotQuestRewardType::ItemRemoved); });
		unexpectedItem |= !configured;
	}
	if (unexpectedMoney || unexpectedExperience || unexpectedStorage || unexpectedMission || unexpectedItem) { result.unexpected = 1; result.result = BotQuestVerificationResult::UnexpectedReward; return result; }
	if (result.expected == 0) result.result = BotQuestVerificationResult::NoChange;
	else if (result.verified == result.expected) result.result = BotQuestVerificationResult::Verified;
	else if (result.verified != 0) result.result = BotQuestVerificationResult::Partial;
	else if (std::ranges::any_of(expected, [](const auto &e) { return e.type == BotQuestRewardType::ItemReceived; })) result.result = BotQuestVerificationResult::ExpectedItemMissing;
	else if (std::ranges::any_of(expected, [](const auto &e) { return e.type == BotQuestRewardType::ExperienceIncreased; })) result.result = BotQuestVerificationResult::ExpectedExperienceMissing;
	else if (std::ranges::any_of(expected, [](const auto &e) { return e.type == BotQuestRewardType::MissionStateChanged; })) result.result = BotQuestVerificationResult::MissionStateUnchanged;
	else result.result = BotQuestVerificationResult::NoChange;
	return result;
}
