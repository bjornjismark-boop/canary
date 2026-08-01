/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_fleet.hpp"

#ifndef USE_PRECOMPILED_HEADERS

	#include <cstdint>
	#include <string>
	#include <vector>
#endif

enum class BotFleetPressureState : uint8_t { Normal, Elevated, Overloaded, Critical, Recovering };
enum class BotFleetScaleProfile : uint8_t { Smoke, Small, Medium, Large, ReleaseCandidate };
enum class BotFleetFaultScenario : uint8_t { DatabaseUnavailableBeforeLogin, DatabaseFailureDuringSave, DelayedLogin, FailedPlacement, FailedSave, DelayedLogout, DispatcherPressure, SchedulerPressure, CommandQueueSaturation, TelemetryStale, AdminTimeout, StopDuringLogin, StopDuringDrain, ManagerDestroyedPendingReconciliation, SessionDestroyedDuringCoordination, PlannerPersistenceRejected, InvalidConfigurationReload, AuthenticationFailure };
enum class BotFleetReleaseBlocker : uint8_t { None, InvariantFailure, ResourceBudgetExceeded, FaultRecoveryFailed, RestartRecoveryFailed, OrdinaryPlayerStarvation, SoakIncomplete };

struct BotFleetResourcePolicy {
	uint32_t maximumManagedBots = 64;
	uint16_t maximumPendingLogins = 8;
	uint16_t maximumPendingLogouts = 8;
	uint16_t maximumCommandsQueued = 64;
	uint16_t maximumPlannerActionsPerTick = 64;
	uint16_t maximumReconciliationWorkPerTick = 64;
	uint32_t maximumDispatcherBacklog = 1024;
	uint32_t maximumSchedulerBacklog = 1024;
	uint16_t recoveryObservations = 3;
	bool operator==(const BotFleetResourcePolicy &) const = default;
};

struct BotFleetResourceObservation {
	uint32_t managedBots = 0;
	uint16_t pendingLogins = 0;
	uint16_t pendingLogouts = 0;
	uint16_t commandsQueued = 0;
	uint16_t plannerActions = 0;
	uint16_t reconciliationWork = 0;
	uint32_t dispatcherBacklog = 0;
	uint32_t schedulerBacklog = 0;
	uint64_t telemetryAgeTicks = 0;
	bool databaseAvailable = true;
	bool ordinaryPlayerResponsive = true;
	bool operator==(const BotFleetResourceObservation &) const = default;
};

struct BotFleetLoadSheddingDecision {
	BotFleetPressureState pressure = BotFleetPressureState::Normal;
	uint16_t reconciliationWork = 0;
	bool permitBotLogin = true;
	bool permitPlannerExpansion = true;
	bool permitCoordinationWork = true;
	bool reduceTelemetryDetail = false;
	bool permitSafeDrain = false;
	bool preserveSaveAndLogout = true;
	bool operator==(const BotFleetLoadSheddingDecision &) const = default;
};

struct BotFleetSoakPolicy {
	BotFleetScaleProfile profile = BotFleetScaleProfile::Smoke;
	uint32_t desiredPopulation = 2;
	uint32_t tickBudget = 100;
	uint32_t summaryIntervalTicks = 10;
	uint32_t restartCount = 1;
	uint64_t seed = 1;
	bool operator==(const BotFleetSoakPolicy &) const = default;
};

struct BotFleetSoakObservation {
	uint32_t tick = 0;
	uint32_t placed = 0;
	uint32_t uniqueSessions = 0;
	uint32_t commandQueueDepth = 0;
	uint32_t coordinationReservations = 0;
	uint32_t callbacksAfterDestruction = 0;
	uint32_t plannerRetries = 0;
	bool ordinaryPlayerResponsive = true;
	bool cleanShutdown = true;
};

struct BotFleetSoakResult {
	BotFleetScaleProfile profile = BotFleetScaleProfile::Smoke;
	BotFleetReleaseBlocker blocker = BotFleetReleaseBlocker::None;
	uint32_t ticksCompleted = 0;
	uint32_t summaries = 0;
	uint32_t restartCycles = 0;
	bool passed = false;
};

struct BotFleetFaultResult {
	BotFleetFaultScenario scenario = BotFleetFaultScenario::DatabaseUnavailableBeforeLogin;
	BotFleetFailure normalizedFailure = BotFleetFailure::None;
	uint32_t attempts = 0;
	bool bounded = false;
	bool recovered = false;
};

struct BotFleetReleaseReadiness {
	std::vector<BotFleetReleaseBlocker> blockers;
	bool deterministicSoakPassed = false;
	bool faultSuitePassed = false;
	bool threeRestartCyclesPassed = false;
	bool productionSoakPassed = false;
	[[nodiscard]] bool ready() const { return blockers.empty() && deterministicSoakPassed && faultSuitePassed && threeRestartCyclesPassed && productionSoakPassed; }
};

class BotFleetHardening final {
public:
	static constexpr uint32_t AbsoluteMaximumManagedBots = 1024;
	static constexpr uint32_t AbsoluteMaximumSoakTicks = 10000000;
	static constexpr uint32_t AbsoluteMaximumCommandQueue = 64;
	static constexpr uint32_t AbsoluteMaximumPlannerActionsPerTick = 256;
	static constexpr uint32_t AbsoluteMaximumReconciliationWorkPerTick = 256;
	[[nodiscard]] static bool validate(const BotFleetResourcePolicy &);
	[[nodiscard]] static BotFleetSoakPolicy profile(BotFleetScaleProfile);
	[[nodiscard]] static BotFleetLoadSheddingDecision observe(const BotFleetResourcePolicy &, const BotFleetResourceObservation &, BotFleetPressureState previous = BotFleetPressureState::Normal, uint16_t healthyObservations = 0);
	[[nodiscard]] static BotFleetReleaseBlocker invariant(const BotFleetSoakPolicy &, const BotFleetSoakObservation &);
	[[nodiscard]] static BotFleetSoakResult run(const BotFleetSoakPolicy &, const std::vector<BotFleetSoakObservation> &);
	[[nodiscard]] static BotFleetFaultResult fault(BotFleetFaultScenario, uint32_t attempts, bool recovered);
};
