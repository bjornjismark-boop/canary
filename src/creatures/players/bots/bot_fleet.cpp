/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_fleet.hpp"

namespace {
bool active(BotFleetMemberState state) {
	return state == BotFleetMemberState::Placed || state == BotFleetMemberState::DrainRequested || state == BotFleetMemberState::Saving || state == BotFleetMemberState::LogoutPending;
}
bool pendingLogin(BotFleetMemberState state) {
	return state == BotFleetMemberState::LoginQueued || state == BotFleetMemberState::Loading || state == BotFleetMemberState::PlacementPending || state == BotFleetMemberState::Recovering;
}
bool pendingLogout(BotFleetMemberState state) {
	return state == BotFleetMemberState::DrainRequested || state == BotFleetMemberState::Saving || state == BotFleetMemberState::LogoutPending;
}

uint32_t category(const BotFleetMemberProfile &member, BotFleetDistributionDimension dimension, uint32_t regionIntent) {
	switch (dimension) {
		case BotFleetDistributionDimension::Vocation: return member.vocationCategory;
		case BotFleetDistributionDimension::LevelRange: return member.levelRangeCategory;
		case BotFleetDistributionDimension::PlannerPolicy: return member.plannerPolicyId;
		case BotFleetDistributionDimension::Role: return member.roleId;
		case BotFleetDistributionDimension::CoordinationGroup: return member.coordinationGroupId;
		case BotFleetDistributionDimension::Region: return regionIntent;
		case BotFleetDistributionDimension::PartyProfile: return member.partyProfileId;
	}
	return 0;
}

const BotFleetMemberProfile *profile(const std::vector<BotFleetMemberProfile> &members, BotFleetMemberId id) {
	const auto found = std::ranges::find(members, id, &BotFleetMemberProfile::id);
	return found == members.end() ? nullptr : &*found;
}
}

BotFleetFailure BotFleet::validate(const BotFleetPopulationPolicy &p) {
	if (p.minimumOnline > p.desiredOnline || p.desiredOnline > p.maximumOnline || p.maximumOnline > p.absoluteHardMaximum || p.drainTarget > p.absoluteHardMaximum || p.absoluteHardMaximum > AbsoluteMaximumMembers || p.maximumLoginsPerInterval > AbsoluteMaximumRate || p.maximumLogoutsPerInterval > AbsoluteMaximumRate || p.maximumPendingLogins > AbsoluteMaximumRate || p.maximumPendingLogouts > AbsoluteMaximumRate || p.maximumRetries > AbsoluteMaximumRetries || p.maximumRetryBackoffTicks > AbsoluteMaximumBackoffTicks || p.revision == 0) return BotFleetFailure::InvalidPolicy;
	return BotFleetFailure::None;
}

BotFleetFailure BotFleet::validate(const BotFleetDistributionPolicy &p, const std::vector<BotFleetMemberProfile> &members) {
	if (members.size() > AbsoluteMaximumMembers || p.limits.size() > AbsoluteMaximumMembers || p.maximumPerVocation > AbsoluteMaximumMembers || p.maximumPerLevelRange > AbsoluteMaximumMembers || p.maximumPerPlannerPolicy > AbsoluteMaximumMembers || p.maximumPerRegion > AbsoluteMaximumMembers || p.revision == 0) return BotFleetFailure::InvalidPolicy;
	for (const auto &limit : p.limits) {
		if (limit.minimum > limit.desired || limit.desired > limit.maximum || limit.maximum > AbsoluteMaximumMembers) return BotFleetFailure::InvalidPolicy;
		const auto eligible = std::ranges::count_if(members, [&](const auto &member) {
			if (!member.enabled || member.alwaysOffline) return false;
			if (limit.dimension == BotFleetDistributionDimension::Region) return std::ranges::contains(member.allowedRegionIds, limit.category);
			return category(member, limit.dimension, 0) == limit.category;
		});
		if (eligible < limit.minimum) return BotFleetFailure::InvalidPolicy;
	}
	std::vector<BotFleetMemberId> ids;
	for (const auto &member : members) {
		if (member.id == 0 || member.name.empty() || member.allowedRegionIds.size() > AbsoluteMaximumRegions || std::ranges::find(ids, member.id) != ids.end()) return member.id && !member.name.empty() ? BotFleetFailure::DuplicateMember : BotFleetFailure::InvalidPolicy;
		ids.push_back(member.id);
	}
	return BotFleetFailure::None;
}

uint64_t BotFleet::retryAt(uint64_t now, uint16_t failures, uint32_t cap) {
	const auto shift = std::min<uint16_t>(failures, 31);
	const auto delay = std::min<uint64_t>(uint64_t { 1 } << shift, std::min<uint32_t>(cap, AbsoluteMaximumBackoffTicks));
	return now > std::numeric_limits<uint64_t>::max() - delay ? std::numeric_limits<uint64_t>::max() : now + delay;
}

BotFleetReconciliation BotFleet::reconcile(BotFleetControllerStateValue &c, const BotFleetPopulationPolicy &p, const BotFleetDistributionPolicy &d, const std::vector<BotFleetMemberProfile> &members, const std::vector<BotFleetObservation> &observations, uint64_t now, bool overloaded) {
	BotFleetReconciliation result;
	if (const auto failure = validate(p); failure != BotFleetFailure::None) { result.state = BotFleetControllerState::Failed; result.failure = failure; return result; }
	if (const auto failure = validate(d, members); failure != BotFleetFailure::None) { result.state = BotFleetControllerState::Failed; result.failure = failure; return result; }
	result.observationRevision = observations.empty() ? (c.lastObservationRevision == std::numeric_limits<uint64_t>::max() ? c.lastObservationRevision : c.lastObservationRevision + 1) : std::ranges::max(observations, {}, &BotFleetObservation::observationRevision).observationRevision;
	if (!p.enabled) { c.state = result.state = BotFleetControllerState::Disabled; return result; }
	if (c.stopping) { c.state = result.state = BotFleetControllerState::Stopping; return result; }
	std::vector<BotFleetMemberId> observedIds;
	for (const auto &o : observations) {
		if (o.id == 0 || std::ranges::find(observedIds, o.id) != observedIds.end()) { result.state = BotFleetControllerState::Failed; result.failure = BotFleetFailure::DuplicateMember; return result; }
		observedIds.push_back(o.id);
		result.placed += active(o.state); result.pendingLogins += pendingLogin(o.state); result.pendingLogouts += pendingLogout(o.state);
	}
	if (result.placed <= p.absoluteHardMaximum && result.pendingLogins > p.absoluteHardMaximum - result.placed) { result.state = BotFleetControllerState::Failed; result.failure = BotFleetFailure::BudgetReached; return result; }
	if (c.paused) { c.state = result.state = BotFleetControllerState::Paused; c.lastObservationRevision = result.observationRevision; return result; }
	if (now < c.startedAtTick || now - c.startedAtTick < p.startupDelayTicks) { c.state = result.state = BotFleetControllerState::Starting; result.failure = BotFleetFailure::StartupDelay; return result; }
	const uint32_t target = c.draining ? std::min(c.drainTarget, p.absoluteHardMaximum) : p.desiredOnline;
	if (c.draining && c.drainStartedAtTick == 0) c.drainStartedAtTick = now == 0 ? 1 : now;
	if (c.draining && result.placed > target && now >= c.drainStartedAtTick && now - c.drainStartedAtTick > p.drainTimeoutTicks) { c.state = result.state = BotFleetControllerState::Failed; result.failure = BotFleetFailure::DrainTimeout; return result; }
	if (overloaded) c.healthyIntervals = 0; else if (c.healthyIntervals < std::numeric_limits<uint16_t>::max()) ++c.healthyIntervals;
	if ((overloaded || c.state == BotFleetControllerState::Overloaded) && !c.draining && (overloaded || c.healthyIntervals < p.overloadRecoveryIntervals)) { c.state = result.state = BotFleetControllerState::Overloaded; result.failure = BotFleetFailure::Overloaded; c.lastObservationRevision = result.observationRevision; return result; }
	std::vector<BotFleetMemberProfile> ordered = members;
	uint32_t operations = 0;
	auto countFor = [&](const BotFleetDistributionLimit &limit, uint32_t wanted, const std::vector<BotFleetLifecycleRequest> &requests) {
		uint32_t count = 0;
		for (const auto &o : observations) { if (++operations > AbsoluteMaximumReconciliationOperations) return std::numeric_limits<uint32_t>::max(); if (active(o.state) || pendingLogin(o.state)) if (const auto *m = profile(members, o.id); m && category(*m, limit.dimension, o.regionIntent) == wanted) ++count; }
		for (const auto &request : requests) { if (++operations > AbsoluteMaximumReconciliationOperations) return std::numeric_limits<uint32_t>::max(); if (request.type == BotFleetLifecycleRequestType::Login) if (const auto *m = profile(members, request.memberId); m && category(*m, limit.dimension, request.regionIntent) == wanted) ++count; }
		return count;
	};
	auto regionFor = [&](const BotFleetMemberProfile &member) {
		for (const auto region : member.allowedRegionIds) {
			bool permitted = true;
			for (const auto &limit : d.limits) if (limit.dimension == BotFleetDistributionDimension::Region && limit.category == region && countFor(limit, region, result.requests) >= limit.maximum) permitted = false;
			if (permitted) return region;
		}
		return uint32_t { 0 };
	};
	auto deficit = [&](const BotFleetMemberProfile &member) {
		uint32_t score = 0;
		for (const auto &limit : d.limits) if (category(member, limit.dimension, member.allowedRegionIds.empty() ? 0 : member.allowedRegionIds.front()) == limit.category) score += countFor(limit, limit.category, result.requests) < limit.desired;
		return score;
	};
	std::ranges::sort(ordered, [&](const auto &a, const auto &b) { return std::tuple(-static_cast<int32_t>(deficit(a)), a.priority, a.id) < std::tuple(-static_cast<int32_t>(deficit(b)), b.priority, b.id); });
	if (result.placed + result.pendingLogins < target) {
		uint32_t allowance = std::min<uint32_t>(p.maximumLoginsPerInterval, p.maximumPendingLogins > result.pendingLogins ? p.maximumPendingLogins - result.pendingLogins : 0);
		for (const auto &member : ordered) {
			if (!allowance || !member.enabled || member.alwaysOffline) continue;
			const auto found = std::ranges::find(observations, member.id, &BotFleetObservation::id);
			if (found != observations.end() && ((found->state != BotFleetMemberState::Offline && found->state != BotFleetMemberState::Backoff && found->state != BotFleetMemberState::Failed) || found->duplicateSession || found->ordinaryHuman || found->nextEligibleTick > now || found->loginFailures >= p.maximumRetries)) continue;
			const auto region = regionFor(member);
			if (!member.allowedRegionIds.empty() && region == 0) continue;
			bool permitted = true;
			for (const auto &limit : d.limits) {
				const auto value = category(member, limit.dimension, region);
				if (value == limit.category && countFor(limit, value, result.requests) >= limit.maximum) permitted = false;
			}
			for (const auto dimension : { BotFleetDistributionDimension::Vocation, BotFleetDistributionDimension::LevelRange, BotFleetDistributionDimension::PlannerPolicy, BotFleetDistributionDimension::Region }) {
				const auto value = category(member, dimension, region);
				const auto maximum = dimension == BotFleetDistributionDimension::Vocation ? d.maximumPerVocation : dimension == BotFleetDistributionDimension::LevelRange ? d.maximumPerLevelRange : dimension == BotFleetDistributionDimension::PlannerPolicy ? d.maximumPerPlannerPolicy : d.maximumPerRegion;
				BotFleetDistributionLimit legacy { dimension, value, 0, 0, maximum };
				if (countFor(legacy, value, result.requests) >= maximum) permitted = false;
			}
			if (!permitted) continue;
			result.requests.push_back({ ++c.requestSequence, member.id, BotFleetLifecycleRequestType::Login, p.revision, region });
			--allowance;
			if (result.placed + result.pendingLogins + result.requests.size() >= target) break;
		}
	} else {
		const auto sessionExpired = [&](const BotFleetMemberProfile &member, const BotFleetObservation &observation) {
			const auto maximum = member.maximumSessionTicks == 0 ? p.maximumSessionTicks : member.maximumSessionTicks;
			return maximum != 0 && observation.placedAtTick != 0 && now >= observation.placedAtTick && now - observation.placedAtTick >= maximum;
		};
		const bool hasExpiredSession = std::ranges::any_of(ordered, [&](const auto &member) {
			const auto found = std::ranges::find(observations, member.id, &BotFleetObservation::id);
			return found != observations.end() && found->state == BotFleetMemberState::Placed && sessionExpired(member, *found);
		});
		if (result.placed <= target && !hasExpiredSession) {
			c.lastObservationRevision = result.observationRevision;
			c.freshObservationRequired = false;
			c.state = result.state = c.draining ? BotFleetControllerState::Draining : BotFleetControllerState::Stable;
			return result;
		}
		uint32_t allowance = std::min<uint32_t>(p.maximumLogoutsPerInterval, p.maximumPendingLogouts > result.pendingLogouts ? p.maximumPendingLogouts - result.pendingLogouts : 0);
		std::ranges::reverse(ordered);
		for (const auto &member : ordered) {
			if (!allowance) break;
			const auto found = std::ranges::find(observations, member.id, &BotFleetObservation::id);
			if (found == observations.end() || found->state != BotFleetMemberState::Placed || !found->safeLogoutBoundary || found->ordinaryHuman || found->logoutFailures >= p.maximumRetries) continue;
			if (result.placed - result.requests.size() <= target && !sessionExpired(member, *found)) continue;
			bool preservesMinimums = true;
			for (const auto &limit : d.limits) {
				const auto value = category(member, limit.dimension, found->regionIntent);
				if (value == limit.category && countFor(limit, value, {}) <= limit.minimum) preservesMinimums = false;
			}
			if (!c.draining && !preservesMinimums) continue;
			result.requests.push_back({ ++c.requestSequence, member.id, BotFleetLifecycleRequestType::Logout, p.revision, 0 });
			--allowance;
		}
	}
	c.lastObservationRevision = result.observationRevision;
	c.freshObservationRequired = false;
	if (operations > AbsoluteMaximumReconciliationOperations && result.requests.empty()) result.failure = BotFleetFailure::BudgetReached;
	c.state = result.state = c.draining ? BotFleetControllerState::Draining : result.requests.empty() ? BotFleetControllerState::Stable : BotFleetControllerState::Reconciling;
	return result;
}

void BotFleet::pause(BotFleetControllerStateValue &c) { c.paused = true; c.state = BotFleetControllerState::Paused; }
void BotFleet::resume(BotFleetControllerStateValue &c) { c.paused = false; c.state = BotFleetControllerState::Reconciling; c.lastObservationRevision = 0; c.freshObservationRequired = true; }
void BotFleet::drain(BotFleetControllerStateValue &c, uint32_t target) { c.paused = false; c.draining = true; c.drainTarget = target; c.drainStartedAtTick = 0; c.state = BotFleetControllerState::Draining; }
void BotFleet::clear(BotFleetControllerStateValue &c) { const auto generation = c.generation == std::numeric_limits<uint64_t>::max() ? c.generation : c.generation + 1; c = {}; c.generation = generation; }
