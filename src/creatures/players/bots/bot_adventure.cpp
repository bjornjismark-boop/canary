/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_adventure.hpp"

namespace {
bool supplyStops(BotSupplyIntent intent) {
	return intent == BotSupplyIntent::StopHunt || intent == BotSupplyIntent::ReturnRequired || intent == BotSupplyIntent::NoHealingSupplies || intent == BotSupplyIntent::NoAmmunition || intent == BotSupplyIntent::CapacityFull;
}

bool policyReserveStops(const BotAdventureObservation &observation, const BotAdventurePolicy &policy) {
	return observation.healingReserve < policy.minimumHealingReserve || observation.ammunitionReserve < policy.minimumAmmunitionReserve || observation.usedCapacity >= policy.maximumUsedCapacity;
}

void transition(BotAdventureProgress &progress, BotAdventureState state, BotAdventureIntent intent, std::chrono::milliseconds now) {
	if (BotAdventure::legalTransition(progress.state, state)) {
		progress.state = state;
		progress.intent = intent;
		progress.lastProgressAt = now;
	}
}
}

bool BotAdventure::contains(const BotAdventureRegion &region, const Position &position) {
	return region.center.z == position.z && std::max(Position::getDistanceX(region.center, position), Position::getDistanceY(region.center, position)) <= region.radius;
}

bool BotAdventure::legalTransition(BotAdventureState from, BotAdventureState to) {
	if (from == to) return true;
	if (to == BotAdventureState::Dead) return from != BotAdventureState::Cancelled && from != BotAdventureState::Completed;
	if (to == BotAdventureState::Cancelled) return from != BotAdventureState::Dead && from != BotAdventureState::Completed;
	if (to == BotAdventureState::Failed) return from != BotAdventureState::Dead && from != BotAdventureState::Cancelled && from != BotAdventureState::Completed;
	if (to == BotAdventureState::Completed) return from != BotAdventureState::Dead && from != BotAdventureState::Cancelled && from != BotAdventureState::Failed;
	if (from == BotAdventureState::Dead || from == BotAdventureState::Cancelled || from == BotAdventureState::Completed || from == BotAdventureState::Failed) return false;
	switch (from) {
		case BotAdventureState::Idle: return to == BotAdventureState::Preparing;
		case BotAdventureState::Preparing: return to == BotAdventureState::TravelingToArea || to == BotAdventureState::Searching || to == BotAdventureState::Recovering || to == BotAdventureState::Returning;
		case BotAdventureState::TravelingToArea: return to == BotAdventureState::Searching || to == BotAdventureState::Recovering || to == BotAdventureState::Returning;
		case BotAdventureState::Searching: return to == BotAdventureState::Engaging || to == BotAdventureState::Recovering || to == BotAdventureState::Returning;
		case BotAdventureState::Engaging: return to == BotAdventureState::Fighting || to == BotAdventureState::Searching || to == BotAdventureState::Recovering || to == BotAdventureState::Returning;
		case BotAdventureState::Fighting: return to == BotAdventureState::Looting || to == BotAdventureState::EvaluatingSupplies || to == BotAdventureState::Recovering || to == BotAdventureState::Returning;
		case BotAdventureState::Recovering: return to == BotAdventureState::Searching || to == BotAdventureState::Returning;
		case BotAdventureState::Looting: return to == BotAdventureState::EvaluatingSupplies || to == BotAdventureState::Recovering || to == BotAdventureState::Returning;
		case BotAdventureState::EvaluatingSupplies: return to == BotAdventureState::Searching || to == BotAdventureState::Returning || to == BotAdventureState::Completed;
		case BotAdventureState::Returning: return to == BotAdventureState::Completed || to == BotAdventureState::Recovering;
		default: return false;
	}
}

BotAdventureProgress BotAdventure::advance(BotAdventureProgress progress, const BotAdventureObservation &observation, std::chrono::milliseconds now, const BotAdventurePolicy &policy) {
	if (progress.state == BotAdventureState::Dead || progress.state == BotAdventureState::Cancelled || progress.state == BotAdventureState::Completed || progress.state == BotAdventureState::Failed) return progress;
	if (observation.dead) {
		++progress.deathCount;
		transition(progress, BotAdventureState::Dead, BotAdventureIntent::None, now);
		progress.failure = BotAdventureFailure::Dead;
		return progress;
	}
	if (observation.revision == 0 || (progress.lastObservationRevision && observation.revision == progress.lastObservationRevision)) {
		progress.intent = BotAdventureIntent::Reobserve;
		progress.failure = BotAdventureFailure::StaleObservation;
		return progress;
	}
	progress.lastObservationRevision = observation.revision;
	progress.failure = BotAdventureFailure::None;
	if (progress.state == BotAdventureState::Idle) progress.startedAt = now;
	if (now - progress.startedAt >= policy.maximumDuration) {
		transition(progress, BotAdventureState::Failed, BotAdventureIntent::None, now);
		progress.failure = BotAdventureFailure::DurationLimit;
		return progress;
	}
	if ((policy.targetLevel && observation.level >= policy.targetLevel) || (policy.targetExperience && observation.experience >= policy.targetExperience)) {
		transition(progress, BotAdventureState::Completed, BotAdventureIntent::Finish, now);
		return progress;
	}
	if (observation.actionFailed || observation.dynamicBlocker) {
		progress.repeatedFailures = static_cast<uint8_t>(std::min<uint16_t>(UINT8_MAX, progress.repeatedFailures + 1));
		if (progress.repeatedFailures > policy.maximumRepeatedFailures) {
			transition(progress, BotAdventureState::Failed, BotAdventureIntent::None, now);
			progress.failure = BotAdventureFailure::RepeatedFailure;
			return progress;
		}
	} else {
		progress.repeatedFailures = 0;
	}
	if (observation.healthPercent < policy.minimumHealthPercent || observation.survivalUrgency >= BotSurvivalUrgency::High) {
		transition(progress, BotAdventureState::Recovering, BotAdventureIntent::Recover, now);
		return progress;
	}
	if (supplyStops(observation.supplyIntent) || policyReserveStops(observation, policy)) {
		transition(progress, BotAdventureState::Returning, BotAdventureIntent::Return, now);
		return progress;
	}

	switch (progress.state) {
		case BotAdventureState::Idle: transition(progress, BotAdventureState::Preparing, BotAdventureIntent::Prepare, now); break;
		case BotAdventureState::Preparing:
			transition(progress, contains(policy.huntRegion, observation.position) ? BotAdventureState::Searching : BotAdventureState::TravelingToArea, contains(policy.huntRegion, observation.position) ? BotAdventureIntent::Search : BotAdventureIntent::Travel, now); break;
		case BotAdventureState::TravelingToArea:
			if (contains(policy.huntRegion, observation.position)) transition(progress, BotAdventureState::Searching, BotAdventureIntent::Search, now); else progress.intent = BotAdventureIntent::Travel;
			break;
		case BotAdventureState::Searching:
			if (observation.visibleTargetId) transition(progress, BotAdventureState::Engaging, BotAdventureIntent::Engage, now); else progress.intent = BotAdventureIntent::Search;
			break;
		case BotAdventureState::Engaging:
			if (observation.combatActive) transition(progress, BotAdventureState::Fighting, BotAdventureIntent::WaitForCombat, now); else if (!observation.visibleTargetId) transition(progress, BotAdventureState::Searching, BotAdventureIntent::Search, now); else progress.intent = BotAdventureIntent::Engage;
			break;
		case BotAdventureState::Fighting:
			if (observation.targetDefeated) {
				++progress.combatCount;
				transition(progress, observation.corpseAvailable ? BotAdventureState::Looting : BotAdventureState::EvaluatingSupplies, observation.corpseAvailable ? BotAdventureIntent::Loot : BotAdventureIntent::EvaluateSupplies, now);
			} else progress.intent = BotAdventureIntent::WaitForCombat;
			break;
		case BotAdventureState::Recovering:
			transition(progress, BotAdventureState::Searching, BotAdventureIntent::Search, now);
			break;
		case BotAdventureState::Looting:
			if (observation.lootComplete) transition(progress, BotAdventureState::EvaluatingSupplies, BotAdventureIntent::EvaluateSupplies, now); else progress.intent = BotAdventureIntent::Loot;
			break;
		case BotAdventureState::EvaluatingSupplies:
			if (progress.combatCount >= policy.maximumCombatCount) {
				transition(progress, BotAdventureState::Completed, BotAdventureIntent::Finish, now);
				progress.failure = BotAdventureFailure::CombatLimit;
			} else transition(progress, BotAdventureState::Searching, BotAdventureIntent::Search, now);
			break;
		case BotAdventureState::Returning:
			if (contains(policy.returnRegion, observation.position)) transition(progress, BotAdventureState::Completed, BotAdventureIntent::Finish, now); else progress.intent = BotAdventureIntent::Return;
			break;
		default: break;
	}
	return progress;
}

BotAdventureProgress BotAdventure::cancel(BotAdventureProgress progress) {
	if (legalTransition(progress.state, BotAdventureState::Cancelled)) {
		progress.state = BotAdventureState::Cancelled;
		progress.intent = BotAdventureIntent::Cancel;
		progress.failure = BotAdventureFailure::Cancelled;
	}
	return progress;
}
