/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_plan_execution.hpp"

BotPlanSubsystem BotPlanExecutor::subsystem(BotPlanStepType type) {
	switch (type) {
		case BotPlanStepType::Observe: return BotPlanSubsystem::Observation;
		case BotPlanStepType::Travel: return BotPlanSubsystem::Navigation;
		case BotPlanStepType::Fight: return BotPlanSubsystem::Combat;
		case BotPlanStepType::Loot: return BotPlanSubsystem::Loot;
		case BotPlanStepType::Recover: return BotPlanSubsystem::Survival;
		case BotPlanStepType::Resupply: return BotPlanSubsystem::Resupply;
		case BotPlanStepType::Equip: return BotPlanSubsystem::Equipment;
		case BotPlanStepType::Interact: case BotPlanStepType::Converse: return BotPlanSubsystem::Dialogue;
		case BotPlanStepType::VerifyQuest: return BotPlanSubsystem::Quest;
		case BotPlanStepType::SaveLogout: case BotPlanStepType::Complete: return BotPlanSubsystem::Lifecycle;
	}
	return BotPlanSubsystem::None;
}

BotPlanArbitration BotPlanExecutor::arbitrate(const BotPlanExecution &execution, const BotPlanExecutionObservation &o) {
	if (o.dead) return { BotPlanArbitrationReason::Death, false, true, o.pendingAuthoritativeAction };
	if (o.survivalCritical) return { BotPlanArbitrationReason::CriticalSurvival, true, false, o.pendingAuthoritativeAction };
	if (o.pendingAuthoritativeAction) return { BotPlanArbitrationReason::PendingBoundary, true, false, true };
	if (o.shutdownRequested) return { BotPlanArbitrationReason::Shutdown, false, true, false };
	if (o.mandatoryQuest) return { BotPlanArbitrationReason::MandatoryQuest, execution.decision.goalType != BotGoalType::CompleteConfiguredQuest, false, false };
	if (o.supplyUrgent) return { BotPlanArbitrationReason::UrgentResupply, execution.decision.goalType != BotGoalType::Resupply, false, false };
	if (execution.delegatedStep) return { BotPlanArbitrationReason::Verification, false, false, false };
	return { BotPlanArbitrationReason::Progression, false, false, false };
}

BotPlanExecution BotPlanExecutor::start(BotPlanExecutionId id, BotPlannerDecision decision, const BotPlanExecutionObservation &o, const BotPlanExecutionBudget &budget) {
	BotPlanExecution result;
	result.id = id; result.state = BotPlanExecutionState::PlanSelected; result.decision = std::move(decision); result.budget = budget; result.observationRevision = o.revision; result.sessionGeneration = o.sessionGeneration; result.freshObservationRequired = false;
	if (id == 0 || !o.placed || o.revision == 0 || result.decision.terminal()) { result.state = BotPlanExecutionState::Failed; result.failure = BotPlanFailure::InvalidLifecycle; }
	return result;
}

BotPlanExecution BotPlanExecutor::delegate(BotPlanExecution result, const BotPlanExecutionObservation &o) {
	if (result.terminal()) return result;
	result.arbitration = arbitrate(result, o);
	if (o.dead) { result.state = BotPlanExecutionState::Dead; result.failure = BotPlanFailure::SurvivalInterruption; result.delegatedStep.reset(); return result; }
	if (o.sessionGeneration != result.sessionGeneration) return recover(std::move(result), BotPlanFailure::SessionChanged);
	if (o.revision <= result.observationRevision || result.freshObservationRequired) { result.state = BotPlanExecutionState::Replanning; result.failure = BotPlanFailure::StaleCheckpoint; return result; }
	if (result.arbitration.requiresBoundary || result.arbitration.suspend) { result.state = BotPlanExecutionState::Suspended; result.freshObservationRequired = true; return result; }
	if (result.arbitration.cancel) return cancel(std::move(result));
	if (++result.ticks > result.budget.maximumTicks) { result.state = BotPlanExecutionState::Failed; result.failure = BotPlanFailure::BudgetExhausted; return result; }
	if (result.delegatedStep) { result.failure = BotPlanFailure::ConflictingIntent; return result; }
	const auto index = result.decision.checkpoint.verifiedStepIndex;
	if (!o.subsystemAvailable || !o.preconditionValid || index >= result.decision.plan.steps.size()) return recover(std::move(result), !o.subsystemAvailable ? BotPlanFailure::TargetUnavailable : BotPlanFailure::StaleCheckpoint);
	const auto &step = result.decision.plan.steps[index];
	result.delegatedStep = BotPlanStepExecution { .intent = { result.id, index, step.type, subsystem(step.type), step.configuredTargetId, step.region, o.revision, o.sessionGeneration } };
	result.state = BotPlanExecutionState::AwaitingSubsystem; result.observationRevision = o.revision;
	return result;
}

BotPlanExecution BotPlanExecutor::observe(BotPlanExecution result, BotPlanStepOutcome outcome, uint64_t revision, bool verified) {
	if (result.terminal() || !result.delegatedStep) return result;
	auto &step = *result.delegatedStep; step.outcome = outcome; step.terminalObserved = outcome != BotPlanStepOutcome::Pending && outcome != BotPlanStepOutcome::RetryScheduled; step.postconditionVerified = verified;
	if (revision <= result.observationRevision) { result.state = BotPlanExecutionState::Replanning; result.failure = BotPlanFailure::StaleCheckpoint; result.freshObservationRequired = true; return result; }
	result.observationRevision = revision;
	if (outcome == BotPlanStepOutcome::Dead) { result.state = BotPlanExecutionState::Dead; result.delegatedStep.reset(); return result; }
	if (outcome == BotPlanStepOutcome::Cancelled) return cancel(std::move(result));
	if (outcome == BotPlanStepOutcome::Succeeded && verified) {
		result.decision = BotPlanner::verifyStep(std::move(result.decision), step.intent.stepIndex, revision, true);
		result.delegatedStep.reset(); result.state = result.decision.status == BotGoalStatus::Completed ? BotPlanExecutionState::Completed : BotPlanExecutionState::Checkpointing; return result;
	}
	if (outcome == BotPlanStepOutcome::Pending || outcome == BotPlanStepOutcome::RetryScheduled) return result;
	if (outcome == BotPlanStepOutcome::UnexpectedState || outcome == BotPlanStepOutcome::ObservationStale) { result.state = BotPlanExecutionState::Replanning; result.freshObservationRequired = true; return result; }
	return recover(std::move(result), outcome == BotPlanStepOutcome::PreconditionLost ? BotPlanFailure::StaleCheckpoint : BotPlanFailure::TargetUnavailable);
}

BotPlanExecution BotPlanExecutor::recover(BotPlanExecution result, BotPlanFailure failure, uint64_t signature) {
	result.failure = failure; result.state = BotPlanExecutionState::Recovering; result.freshObservationRequired = true;
	auto it = std::ranges::find(result.failureMemory, signature, &BotPlanFailureRecord::targetSignature);
	if (it == result.failureMemory.end()) {
		if (result.budget.failureMemoryLimit == 0) { result.state = BotPlanExecutionState::Failed; result.recovery = BotPlanRecoveryDecision::TerminalFailure; return result; }
		if (result.failureMemory.size() >= result.budget.failureMemoryLimit) result.failureMemory.erase(result.failureMemory.begin());
		result.failureMemory.push_back({ failure, signature, 1 }); it = std::prev(result.failureMemory.end());
	} else if (it->count != UINT8_MAX) ++it->count;
	if (result.recoveries >= result.budget.maximumRecoveries) { result.state = BotPlanExecutionState::Failed; result.recovery = BotPlanRecoveryDecision::TerminalFailure; return result; }
	++result.recoveries;
	if (it->count <= result.budget.maximumRetries) result.recovery = BotPlanRecoveryDecision::RetryStep;
	else if (it->count <= static_cast<uint16_t>(result.budget.maximumRetries) + result.budget.maximumAlternatives) result.recovery = BotPlanRecoveryDecision::ChooseAlternative;
	else { result.state = BotPlanExecutionState::Replanning; result.recovery = BotPlanRecoveryDecision::ReplacePlan; }
	result.delegatedStep.reset(); return result;
}

BotPlanExecution BotPlanExecutor::cancel(BotPlanExecution result) { result.delegatedStep.reset(); result.state = BotPlanExecutionState::Cancelled; result.failure = BotPlanFailure::None; return result; }
