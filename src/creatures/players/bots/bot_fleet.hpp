/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#ifndef USE_PRECOMPILED_HEADERS
	#include <cstdint>
	#include <string>
	#include <vector>
#endif

using BotFleetId = uint32_t;
using BotFleetMemberId = uint32_t;

enum class BotFleetMemberState : uint8_t { Disabled, Offline, LoginQueued, Loading, PlacementPending, Placed, Paused, DrainRequested, Saving, LogoutPending, Recovering, Backoff, Failed };
enum class BotFleetControllerState : uint8_t { Disabled, Starting, Reconciling, Stable, Paused, Draining, Overloaded, Stopping, Failed };
enum class BotFleetLifecycleRequestType : uint8_t { Login, Logout };
enum class BotFleetFailure : uint8_t { None, InvalidPolicy, DuplicateMember, BudgetReached, StartupDelay, Paused, Overloaded, UnsafeBoundary, RetryExhausted, DrainTimeout };
enum class BotFleetDistributionDimension : uint8_t { Vocation, LevelRange, PlannerPolicy, Role, CoordinationGroup, Region, PartyProfile };

struct BotFleetMemberProfile {
	BotFleetMemberId id = 0;
	std::string name;
	uint16_t vocationCategory = 0;
	uint16_t levelRangeCategory = 0;
	uint32_t plannerPolicyId = 0;
	uint32_t coordinationGroupId = 0;
	uint32_t roleId = 0;
	uint32_t partyProfileId = 0;
	std::vector<uint32_t> allowedRegionIds;
	uint16_t priority = 0;
	uint32_t maximumSessionTicks = 0;
	bool enabled = true;
	bool alwaysOffline = false;
	bool operator==(const BotFleetMemberProfile &) const = default;
};

struct BotFleetDistributionLimit {
	BotFleetDistributionDimension dimension = BotFleetDistributionDimension::Vocation;
	uint32_t category = 0;
	uint16_t minimum = 0;
	uint16_t desired = 0;
	uint16_t maximum = 0;
	bool operator==(const BotFleetDistributionLimit &) const = default;
};

struct BotFleetObservation {
	BotFleetMemberId id = 0;
	BotFleetMemberState state = BotFleetMemberState::Offline;
	uint64_t observationRevision = 0;
	uint64_t nextEligibleTick = 0;
	uint16_t loginFailures = 0;
	uint16_t logoutFailures = 0;
	uint64_t placedAtTick = 0;
	uint32_t regionIntent = 0;
	bool safeLogoutBoundary = true;
	bool ordinaryHuman = false;
	bool duplicateSession = false;
};

struct BotFleetPopulationPolicy {
	uint32_t desiredOnline = 0;
	uint32_t minimumOnline = 0;
	uint32_t maximumOnline = 0;
	uint32_t absoluteHardMaximum = 0;
	uint16_t maximumLoginsPerInterval = 1;
	uint16_t maximumLogoutsPerInterval = 1;
	uint16_t maximumPendingLogins = 1;
	uint16_t maximumPendingLogouts = 1;
	uint16_t maximumRetries = 3;
	uint32_t startupDelayTicks = 0;
	uint32_t maximumRetryBackoffTicks = 64;
	uint32_t drainTimeoutTicks = 1024;
	uint32_t drainTarget = 0;
	uint32_t maximumSessionTicks = 0;
	uint16_t overloadRecoveryIntervals = 2;
	uint64_t revision = 1;
	bool enabled = true;
	bool overloadPause = true;
	bool operator==(const BotFleetPopulationPolicy &) const = default;
};

struct BotFleetDistributionPolicy {
	uint16_t maximumPerVocation = 64;
	uint16_t maximumPerLevelRange = 64;
	uint16_t maximumPerPlannerPolicy = 64;
	uint16_t maximumPerRegion = 64;
	std::vector<BotFleetDistributionLimit> limits;
	uint64_t revision = 1;
	bool operator==(const BotFleetDistributionPolicy &) const = default;
};

struct BotFleetBudget {
	uint16_t loginsRemaining = 0;
	uint16_t logoutsRemaining = 0;
	uint16_t pendingLogins = 0;
	uint16_t pendingLogouts = 0;
};

struct BotFleetLifecycleRequest {
	uint64_t requestId = 0;
	BotFleetMemberId memberId = 0;
	BotFleetLifecycleRequestType type = BotFleetLifecycleRequestType::Login;
	uint64_t policyRevision = 0;
	uint32_t regionIntent = 0;
	bool operator==(const BotFleetLifecycleRequest &) const = default;
};

struct BotFleetLifecycleResult {
	uint64_t requestId = 0;
	BotFleetMemberId memberId = 0;
	BotFleetMemberState state = BotFleetMemberState::Offline;
	BotFleetFailure failure = BotFleetFailure::None;
	uint64_t nextEligibleTick = 0;
};

struct BotFleetReconciliation {
	BotFleetControllerState state = BotFleetControllerState::Disabled;
	BotFleetFailure failure = BotFleetFailure::None;
	uint32_t placed = 0;
	uint32_t pendingLogins = 0;
	uint32_t pendingLogouts = 0;
	uint64_t observationRevision = 0;
	std::vector<BotFleetLifecycleRequest> requests;
};

struct BotFleetControllerStateValue {
	BotFleetControllerState state = BotFleetControllerState::Disabled;
	uint64_t lastObservationRevision = 0;
	uint64_t requestSequence = 0;
	uint64_t startedAtTick = 0;
	uint64_t drainStartedAtTick = 0;
	uint32_t drainTarget = 0;
	uint16_t healthyIntervals = 0;
	uint64_t generation = 1;
	bool wakeupPending = false;
	bool freshObservationRequired = true;
	bool stopping = false;
	bool paused = false;
	bool draining = false;
};

class BotFleet final {
public:
	static constexpr uint32_t AbsoluteMaximumMembers = 1024;
	static constexpr uint16_t AbsoluteMaximumRate = 64;
	static constexpr uint32_t AbsoluteMaximumBackoffTicks = 86400;
	static constexpr uint16_t AbsoluteMaximumRetries = 32;
	static constexpr uint16_t AbsoluteMaximumRegions = 32;
	static constexpr uint32_t AbsoluteMaximumReconciliationOperations = 65536;

	[[nodiscard]] static BotFleetFailure validate(const BotFleetPopulationPolicy &policy);
	[[nodiscard]] static BotFleetFailure validate(const BotFleetDistributionPolicy &policy, const std::vector<BotFleetMemberProfile> &members);
	[[nodiscard]] static uint64_t retryAt(uint64_t now, uint16_t failures, uint32_t cap);
	[[nodiscard]] static BotFleetReconciliation reconcile(BotFleetControllerStateValue &controller, const BotFleetPopulationPolicy &population, const BotFleetDistributionPolicy &distribution, const std::vector<BotFleetMemberProfile> &members, const std::vector<BotFleetObservation> &observations, uint64_t now, bool overloaded = false);
	static void pause(BotFleetControllerStateValue &controller);
	static void resume(BotFleetControllerStateValue &controller);
	static void drain(BotFleetControllerStateValue &controller, uint32_t target = 0);
	static void clear(BotFleetControllerStateValue &controller);
};
