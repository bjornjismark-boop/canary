/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_quest_execution.hpp"

BotQuestExecutionState BotQuestExecution::stateFor(BotQuestStepType type) {
	switch (type) {
		case BotQuestStepType::ApproachNpc: case BotQuestStepType::TravelRegion: return BotQuestExecutionState::Traveling;
		case BotQuestStepType::SendDialogue: return BotQuestExecutionState::StartingDialogue;
		case BotQuestStepType::AwaitDialogue: return BotQuestExecutionState::AwaitingDialogue;
		case BotQuestStepType::UseObject: return BotQuestExecutionState::UsingObject;
		case BotQuestStepType::KillCreature: return BotQuestExecutionState::EngagingTarget;
		case BotQuestStepType::CollectItem: case BotQuestStepType::BuyItem: case BotQuestStepType::WithdrawItem: return BotQuestExecutionState::CollectingItem;
		case BotQuestStepType::DeliverItem: return BotQuestExecutionState::DeliveringItem;
		case BotQuestStepType::VerifyReward: return BotQuestExecutionState::VerifyingReward;
		case BotQuestStepType::Finish: return BotQuestExecutionState::Checkpointing;
		case BotQuestStepType::Cancel: return BotQuestExecutionState::Cancelled;
		case BotQuestStepType::VerifyProgress: return BotQuestExecutionState::VerifyingProgress;
	}
	return BotQuestExecutionState::Failed;
}

BotQuestExecutionResult BotQuestExecution::start(const BotQuestPlan &plan, const BotQuestObservation &observation, const BotQuestEligibility &eligibility, const BotQuestExecutionPolicy &policy) {
	BotQuestExecutionResult result { .state = BotQuestExecutionState::CheckingPrerequisites };
	if (observation.revision == 0) { result.state = BotQuestExecutionState::Failed; result.stepResult = BotQuestStepResult::PrerequisiteLost; result.failure = BotQuestExecutionFailure::InvalidLifecycle; return result; }
	if (!eligibility.eligible()) { result.state = BotQuestExecutionState::Failed; result.stepResult = BotQuestStepResult::PrerequisiteLost; result.failure = BotQuestExecutionFailure::PolicyRejected; return result; }
	if (plan.questId != observation.questId || plan.missionId == 0 || plan.revision == 0 || plan.steps.empty()) { result.state = BotQuestExecutionState::Failed; result.failure = BotQuestExecutionFailure::InvalidPlan; return result; }
	if (plan.steps.size() > policy.maximumSteps) { result.state = BotQuestExecutionState::Failed; result.failure = BotQuestExecutionFailure::BudgetExceeded; return result; }
	result.checkpoint = { .questId = plan.questId, .missionId = plan.missionId, .missionState = BotMissionState::Available, .planRevision = plan.revision, .observationRevision = observation.revision };
	result.state = stateFor(plan.steps.front().type); result.stepResult = BotQuestStepResult::Pending; return result;
}

BotQuestExecutionResult BotQuestExecution::retry(BotQuestExecutionResult result, BotQuestStepResult failure, const BotQuestExecutionPolicy &policy, uint64_t now) {
	if (result.checkpoint.retryCount >= policy.maximumRetries) { result.state = BotQuestExecutionState::Failed; result.stepResult = BotQuestStepResult::RetryExhausted; result.failure = BotQuestExecutionFailure::RetryExhausted; return result; }
	++result.checkpoint.retryCount; const auto shift=std::min<uint8_t>(result.checkpoint.retryCount-1,8);const auto delay=std::min<uint64_t>(policy.maximumBackoff,policy.retryBackoff>UINT64_MAX>>shift?policy.maximumBackoff:policy.retryBackoff<<shift);result.checkpoint.retryDeadline=now>UINT64_MAX-delay?UINT64_MAX:now+delay;result.state = BotQuestExecutionState::Backoff; result.stepResult = BotQuestStepResult::RetryScheduled; result.failure = BotQuestExecutionFailure::None; (void)failure; return result;
}

BotQuestExecutionResult BotQuestExecution::advance(const BotQuestPlan &plan, BotQuestExecutionResult result, const BotQuestStepObservation &observation, const BotQuestExecutionPolicy &policy) {
	if (result.terminal()) return result;
	if (++result.operations > policy.maximumOperations) { result.state = BotQuestExecutionState::Failed; result.failure = BotQuestExecutionFailure::BudgetExceeded; return result; }
	if (observation.cancelled) { result.state = BotQuestExecutionState::Cancelled; result.stepResult = BotQuestStepResult::Cancelled; result.failure = BotQuestExecutionFailure::Cancelled; return result; }
	if (observation.dead) { result.state = BotQuestExecutionState::Dead; result.stepResult = BotQuestStepResult::Dead; result.failure = BotQuestExecutionFailure::Death; return result; }
	if (observation.survivalRequired) { result.state = BotQuestExecutionState::Suspended; result.stepResult = BotQuestStepResult::Suspended; return result; }
	if (result.state == BotQuestExecutionState::Backoff && observation.clock < result.checkpoint.retryDeadline) { result.stepResult = BotQuestStepResult::RetryScheduled; return result; }
	if (!observation.placed || !observation.prerequisitesEligible) { result.state = BotQuestExecutionState::Failed; result.stepResult = BotQuestStepResult::PrerequisiteLost; result.failure = BotQuestExecutionFailure::InvalidLifecycle; return result; }
	if (observation.observationAge > policy.maximumObservationAge) { result.state = BotQuestExecutionState::Failed; result.failure = BotQuestExecutionFailure::ObservationStale; return result; }
	if (observation.revision <= result.checkpoint.observationRevision) { result.state = BotQuestExecutionState::Failed; result.failure = BotQuestExecutionFailure::ObservationStale; return result; }
	if (result.checkpoint.planRevision != plan.revision || result.checkpoint.verifiedStepIndex >= plan.steps.size()) { result.state = BotQuestExecutionState::Failed; result.failure = BotQuestExecutionFailure::InvalidPlan; return result; }
	const auto &step = plan.steps[result.checkpoint.verifiedStepIndex];
	if (!observation.targetAvailable) {
		const auto unavailable = step.type == BotQuestStepType::ApproachNpc || step.type == BotQuestStepType::SendDialogue || step.type == BotQuestStepType::AwaitDialogue ? BotQuestStepResult::NpcUnavailable : step.type == BotQuestStepType::TravelRegion ? BotQuestStepResult::RouteUnavailable : step.type == BotQuestStepType::UseObject ? BotQuestStepResult::ObjectUnavailable : step.type == BotQuestStepType::KillCreature ? BotQuestStepResult::TargetUnavailable : BotQuestStepResult::ItemUnavailable;
		return retry(result, unavailable, policy, observation.clock);
	}
	if (observation.unexpectedProgress) { result.stepResult = BotQuestStepResult::UnexpectedProgress; return result; }
	bool verified = false;
	switch (step.type) {
		case BotQuestStepType::ApproachNpc: case BotQuestStepType::TravelRegion: verified = observation.regionReached; break;
		case BotQuestStepType::SendDialogue: verified = observation.visibleDialogueResponse; break;
		case BotQuestStepType::AwaitDialogue: verified = observation.visibleDialogueResponse; break;
		case BotQuestStepType::UseObject: case BotQuestStepType::VerifyProgress: verified = observation.authoritativeProgress || observation.missionState == step.expectedMissionState; break;
		case BotQuestStepType::KillCreature: verified = observation.deathObserved && observation.authoritativeProgress; break;
		case BotQuestStepType::CollectItem: case BotQuestStepType::BuyItem: case BotQuestStepType::WithdrawItem: verified = observation.itemAdded; break;
		case BotQuestStepType::DeliverItem: verified = observation.itemRemoved && observation.authoritativeProgress; break;
		case BotQuestStepType::VerifyReward: verified = observation.reward == BotQuestVerificationResult::Verified; break;
		case BotQuestStepType::Finish: verified = observation.authoritativeProgress; break;
		case BotQuestStepType::Cancel: result.state = BotQuestExecutionState::Cancelled; result.stepResult = BotQuestStepResult::Cancelled; return result;
	}
	if (!verified) { result.state = stateFor(step.type); result.stepResult = observation.actionAccepted ? BotQuestStepResult::Pending : BotQuestStepResult::NoProgress; return result; }
	result.checkpoint.observationRevision = observation.revision; result.checkpoint.missionState = observation.missionState; result.checkpoint.retryCount = 0; ++result.checkpoint.verifiedStepIndex; result.stepResult = BotQuestStepResult::Succeeded;
	if (result.checkpoint.verifiedStepIndex == plan.steps.size()) result.state = BotQuestExecutionState::Completed;
	else result.state = stateFor(plan.steps[result.checkpoint.verifiedStepIndex].type);
	return result;
}
