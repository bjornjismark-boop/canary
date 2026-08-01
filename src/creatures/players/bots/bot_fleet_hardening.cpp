/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_fleet_hardening.hpp"

bool BotFleetHardening::validate(const BotFleetResourcePolicy &p) {
	return p.maximumManagedBots <= AbsoluteMaximumManagedBots && p.maximumPendingLogins <= BotFleet::AbsoluteMaximumRate && p.maximumPendingLogouts <= BotFleet::AbsoluteMaximumRate && p.maximumCommandsQueued <= AbsoluteMaximumCommandQueue && p.maximumPlannerActionsPerTick <= AbsoluteMaximumPlannerActionsPerTick && p.maximumReconciliationWorkPerTick <= AbsoluteMaximumReconciliationWorkPerTick && p.recoveryObservations > 0;
}

BotFleetSoakPolicy BotFleetHardening::profile(BotFleetScaleProfile profile) {
	switch (profile) {
		case BotFleetScaleProfile::Smoke: return { profile, 2, 100, 10, 1, 1 };
		case BotFleetScaleProfile::Small: return { profile, 8, 1000, 100, 3, 7 };
		case BotFleetScaleProfile::Medium: return { profile, 32, 10000, 500, 3, 17 };
		case BotFleetScaleProfile::Large: return { profile, 128, 100000, 1000, 5, 29 };
		case BotFleetScaleProfile::ReleaseCandidate: return { profile, 256, 1000000, 5000, 10, 47 };
	}
	return {};
}

BotFleetLoadSheddingDecision BotFleetHardening::observe(const BotFleetResourcePolicy &p, const BotFleetResourceObservation &o, BotFleetPressureState previous, uint16_t healthy) {
	BotFleetLoadSheddingDecision d;
	const bool hard = o.managedBots > p.maximumManagedBots || o.pendingLogins > p.maximumPendingLogins || o.pendingLogouts > p.maximumPendingLogouts || o.commandsQueued > p.maximumCommandsQueued || o.plannerActions > p.maximumPlannerActionsPerTick || o.reconciliationWork > p.maximumReconciliationWorkPerTick;
	const bool categoryCritical = o.dispatcherPressure == BotFleetPressureState::Critical || o.schedulerPressure == BotFleetPressureState::Critical;
	const bool overloaded = categoryCritical || o.dispatcherPressure == BotFleetPressureState::Overloaded || o.schedulerPressure == BotFleetPressureState::Overloaded || o.dispatcherBacklog > p.maximumDispatcherBacklog || o.schedulerBacklog > p.maximumSchedulerBacklog || !o.databaseAvailable || !o.ordinaryPlayerResponsive;
	const bool elevated = o.dispatcherPressure == BotFleetPressureState::Elevated || o.schedulerPressure == BotFleetPressureState::Elevated || o.dispatcherBacklog > p.maximumDispatcherBacklog / 2 || o.schedulerBacklog > p.maximumSchedulerBacklog / 2;
	if (hard || categoryCritical || !o.ordinaryPlayerResponsive) d.pressure = BotFleetPressureState::Critical;
	else if (overloaded) d.pressure = BotFleetPressureState::Overloaded;
	else if (previous == BotFleetPressureState::Critical || previous == BotFleetPressureState::Overloaded || previous == BotFleetPressureState::Recovering) d.pressure = healthy >= p.recoveryObservations ? BotFleetPressureState::Normal : BotFleetPressureState::Recovering;
	else d.pressure = elevated ? BotFleetPressureState::Elevated : BotFleetPressureState::Normal;
	d.reconciliationWork = std::min<uint16_t>(p.maximumReconciliationWorkPerTick, d.pressure == BotFleetPressureState::Normal ? p.maximumReconciliationWorkPerTick : d.pressure == BotFleetPressureState::Elevated ? std::max<uint16_t>(1, p.maximumReconciliationWorkPerTick / 2) : std::max<uint16_t>(1, p.maximumReconciliationWorkPerTick / 4));
	d.permitBotLogin = d.pressure == BotFleetPressureState::Normal || d.pressure == BotFleetPressureState::Elevated;
	d.permitPlannerExpansion = d.pressure == BotFleetPressureState::Normal;
	d.permitCoordinationWork = d.pressure != BotFleetPressureState::Critical;
	d.reduceTelemetryDetail = d.pressure != BotFleetPressureState::Normal;
	d.permitSafeDrain = d.pressure == BotFleetPressureState::Overloaded || d.pressure == BotFleetPressureState::Critical;
	return d;
}

BotFleetReleaseBlocker BotFleetHardening::invariant(const BotFleetSoakPolicy &p, const BotFleetSoakObservation &o) {
	if (o.placed > p.desiredPopulation || o.uniqueSessions != o.placed || o.commandQueueDepth > AbsoluteMaximumCommandQueue || o.callbacksAfterDestruction != 0 || o.plannerRetries > BotFleet::AbsoluteMaximumRetries || !o.cleanShutdown) return BotFleetReleaseBlocker::InvariantFailure;
	if (!o.ordinaryPlayerResponsive) return BotFleetReleaseBlocker::OrdinaryPlayerStarvation;
	return BotFleetReleaseBlocker::None;
}

BotFleetSoakResult BotFleetHardening::run(const BotFleetSoakPolicy &p, const std::vector<BotFleetSoakObservation> &observations) {
	BotFleetSoakResult result { .profile = p.profile };
	if (p.tickBudget == 0 || p.tickBudget > AbsoluteMaximumSoakTicks || p.summaryIntervalTicks == 0 || observations.empty()) { result.blocker = BotFleetReleaseBlocker::SoakIncomplete; return result; }
	for (const auto &observation : observations) {
		if (observation.tick > p.tickBudget) break;
		if (const auto blocker = invariant(p, observation); blocker != BotFleetReleaseBlocker::None) { result.blocker = blocker; result.ticksCompleted = observation.tick; return result; }
		result.ticksCompleted = observation.tick;
		result.summaries += observation.tick % p.summaryIntervalTicks == 0;
	}
	result.restartCycles = p.restartCount;
	result.passed = result.ticksCompleted == p.tickBudget;
	if (!result.passed) result.blocker = BotFleetReleaseBlocker::SoakIncomplete;
	return result;
}

BotFleetFaultResult BotFleetHardening::fault(BotFleetFaultScenario scenario, uint32_t attempts, bool recovered) {
	BotFleetFaultResult result { scenario, BotFleetFailure::RetryExhausted, std::min<uint32_t>(attempts, BotFleet::AbsoluteMaximumRetries), attempts <= BotFleet::AbsoluteMaximumRetries, recovered };
	if (scenario == BotFleetFaultScenario::DispatcherPressure || scenario == BotFleetFaultScenario::SchedulerPressure || scenario == BotFleetFaultScenario::CommandQueueSaturation) result.normalizedFailure = BotFleetFailure::BudgetReached;
	else if (scenario == BotFleetFaultScenario::AuthenticationFailure || scenario == BotFleetFaultScenario::AdminTimeout || scenario == BotFleetFaultScenario::InvalidConfigurationReload) result.normalizedFailure = BotFleetFailure::InvalidPolicy;
	else if (scenario == BotFleetFaultScenario::StopDuringLogin || scenario == BotFleetFaultScenario::StopDuringDrain || scenario == BotFleetFaultScenario::ManagerDestroyedPendingReconciliation || scenario == BotFleetFaultScenario::SessionDestroyedDuringCoordination) result.normalizedFailure = BotFleetFailure::UnsafeBoundary;
	return result;
}
