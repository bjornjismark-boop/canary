#include "creatures/players/bots/bot_coordination.hpp"

namespace {
bool eligible(const BotCoordinationMemberObservation &member, const BotCoordinationPolicy &policy) {
	return member.configured && member.playerBot && member.alive && member.placed
		&& (!policy.requireParty || member.partyMember)
		&& std::ranges::contains(policy.configuredMembers, member.id);
}

BotCoordinationRole defaultRole(const BotCoordinationMemberObservation &member, BotCoordinationMemberId leader) {
	if (member.id == leader) {
		return BotCoordinationRole::Leader;
	}
	switch (member.vocationId) {
		case 2: return BotCoordinationRole::Healer;
		case 3: return BotCoordinationRole::Scout;
		case 4: return BotCoordinationRole::Frontline;
		default: return BotCoordinationRole::Damage;
	}
}

bool exclusive(BotCoordinationRole role) {
	return role == BotCoordinationRole::Leader || role == BotCoordinationRole::Healer || role == BotCoordinationRole::Carrier;
}
}

BotCoordinationFailure BotCoordination::validate(const BotCoordinationPolicy &policy) {
	if (policy.id == 0 || policy.revision == 0 || policy.configuredMembers.empty()) {
		return BotCoordinationFailure::InvalidGroup;
	}
	if (policy.maximumGroups == 0 || policy.maximumGroups > AbsoluteMaximumGroups
		|| policy.maximumMembers == 0 || policy.maximumMembers > AbsoluteMaximumMembers
		|| policy.maximumReservations == 0 || policy.maximumReservations > AbsoluteMaximumReservations
		|| policy.maximumMemberObservations == 0 || policy.maximumMemberObservations > AbsoluteMaximumMembers
		|| policy.maximumIntentsPerMember == 0 || policy.maximumIntentsPerMember > AbsoluteMaximumIntentsPerMember
		|| policy.maximumEventsPerTick == 0 || policy.maximumEventsPerTick > AbsoluteMaximumEventsPerTick
		|| policy.maximumFormationOffset > AbsoluteMaximumFormationOffset
		|| policy.maximumReservationDuration == 0 || policy.maximumReservationDuration > AbsoluteMaximumReservationDuration
		|| policy.criticalHealthPercent > 100) {
		return BotCoordinationFailure::BudgetReached;
	}
	auto members = policy.configuredMembers;
	std::ranges::sort(members);
	if (members.size() > policy.maximumMembers || std::ranges::adjacent_find(members) != members.end() || members.front() == 0) {
		return BotCoordinationFailure::MemberLimit;
	}
	auto roles = policy.configuredRoles;
	std::ranges::sort(roles);
	for (size_t index = 0; index < roles.size(); ++index) {
		const auto &[memberId, role] = roles[index];
		if (!std::ranges::contains(members, memberId) || role == BotCoordinationRole::Leader || role == BotCoordinationRole::Unassigned) {
			return BotCoordinationFailure::RoleIncompatible;
		}
		if (index != 0 && roles[index - 1].first == memberId) {
			return BotCoordinationFailure::RoleIncompatible;
		}
	}
	return BotCoordinationFailure::None;
}

bool BotCoordination::roleCompatible(BotCoordinationRole role, uint16_t vocationId) {
	switch (role) {
		case BotCoordinationRole::Frontline: return vocationId == 4;
		case BotCoordinationRole::Healer: return vocationId == 2;
		case BotCoordinationRole::Scout: return vocationId == 3;
		case BotCoordinationRole::Support: return vocationId == 1 || vocationId == 2;
		case BotCoordinationRole::Damage: return vocationId >= 1 && vocationId <= 4;
		case BotCoordinationRole::Leader:
		case BotCoordinationRole::Carrier:
		case BotCoordinationRole::Unassigned: return true;
	}
	return false;
}

BotCoordinationDecision BotCoordination::evaluate(const BotCoordinationPolicy &policy, const BotCoordinationGroupObservation &observation, BotCoordinationMemberId previousLeader, uint8_t leaderChanges, uint8_t regroupAttempts) {
	BotCoordinationDecision decision;
	if (const auto failure = validate(policy); failure != BotCoordinationFailure::None) {
		decision.state = BotCoordinationState::Failed;
		decision.failure = failure;
		return decision;
	}
	if (observation.id != policy.id || observation.revision == 0) {
		decision.state = BotCoordinationState::Failed;
		decision.failure = BotCoordinationFailure::StaleObservation;
		return decision;
	}
	if (observation.members.size() > policy.maximumMemberObservations) {
		decision.state = BotCoordinationState::Failed;
		decision.failure = BotCoordinationFailure::MemberLimit;
		return decision;
	}

	std::vector<BotCoordinationMemberObservation> members;
	for (const auto &member : observation.members) {
		if (!member.configured || !member.playerBot || !std::ranges::contains(policy.configuredMembers, member.id)) {
			continue;
		}
		if (member.revision != observation.revision || member.sessionGeneration == 0) {
			decision.state = BotCoordinationState::Failed;
			decision.failure = BotCoordinationFailure::StaleObservation;
			return decision;
		}
		if (eligible(member, policy)) {
			members.push_back(member);
		}
	}
	std::ranges::sort(members, {}, &BotCoordinationMemberObservation::id);
	if (members.size() > policy.maximumMembers || members.size() > policy.maximumEventsPerTick) {
		decision.state = BotCoordinationState::Failed;
		decision.failure = BotCoordinationFailure::BudgetReached;
		return decision;
	}

	const auto explicitLeader = std::ranges::find(members, policy.configuredLeader, &BotCoordinationMemberObservation::id);
	if (explicitLeader != members.end()) {
		decision.leaderId = explicitLeader->id;
	} else if (policy.fallbackElection && !members.empty()) {
		decision.leaderId = members.front().id;
	}
	if (decision.leaderId == 0) {
		decision.state = BotCoordinationState::LeaderUnavailable;
		decision.failure = BotCoordinationFailure::LeaderUnavailable;
		return decision;
	}
	if (previousLeader != 0 && previousLeader != decision.leaderId) {
		decision.previousLeaderIntentsInvalidated = true;
		if (leaderChanges >= policy.maximumLeaderChanges) {
			decision.state = BotCoordinationState::Degraded;
			decision.failure = BotCoordinationFailure::BudgetReached;
			return decision;
		}
	}
	if (regroupAttempts >= policy.maximumRegroupAttempts && previousLeader != 0 && previousLeader != decision.leaderId) {
		decision.state = BotCoordinationState::Failed;
		decision.failure = BotCoordinationFailure::BudgetReached;
		return decision;
	}

	decision.state = std::ranges::any_of(members, [&](const auto &member) { return member.healthPercent <= policy.criticalHealthPercent; })
		? BotCoordinationState::Retreating : BotCoordinationState::Active;
	std::vector<BotCoordinationRole> usedExclusive;
	for (size_t index = 0; index < members.size(); ++index) {
		const auto &member = members[index];
		auto role = defaultRole(member, decision.leaderId);
		if (const auto configured = std::ranges::find(policy.configuredRoles, member.id, &std::pair<BotCoordinationMemberId, BotCoordinationRole>::first); configured != policy.configuredRoles.end()) {
			role = configured->second;
		}
		if (!roleCompatible(role, member.vocationId) || (!policy.allowDuplicateExclusiveRoles && exclusive(role) && std::ranges::contains(usedExclusive, role))) {
			decision.state = BotCoordinationState::Failed;
			decision.failure = BotCoordinationFailure::RoleIncompatible;
			decision.roles.clear();
			decision.intents.clear();
			return decision;
		}
		if (exclusive(role)) {
			usedExclusive.push_back(role);
		}
		decision.roles.emplace_back(member.id, role);
		BotCoordinationIntent intent {
			.groupId = policy.id,
			.memberId = member.id,
			.type = decision.state == BotCoordinationState::Retreating ? BotCoordinationIntentType::Retreat : (member.id == decision.leaderId ? BotCoordinationIntentType::SharedObjective : BotCoordinationIntentType::Formation),
			.region = decision.state == BotCoordinationState::Retreating ? policy.retreatRegion : policy.objectiveRegion,
			.offsetX = member.id == decision.leaderId ? int8_t { 0 } : static_cast<int8_t>(std::min<size_t>(index, policy.maximumFormationOffset)),
			.offsetY = member.id == decision.leaderId ? int8_t { 0 } : static_cast<int8_t>(-std::min<size_t>(index, policy.maximumFormationOffset)),
			.observationRevision = observation.revision,
			.policyRevision = policy.revision,
		};
		decision.intents.push_back(intent);
	}
	decision.eventsConsumed = static_cast<uint8_t>(decision.intents.size());
	return decision;
}

BotCoordinationFailure BotCoordination::reserve(std::vector<BotCoordinationReservation> &reservations, BotCoordinationReservation value, const BotCoordinationPolicy &policy, uint64_t now) {
	expire(reservations, now);
	if (validate(policy) != BotCoordinationFailure::None || value.groupId != policy.id || value.memberId == 0 || value.targetSignature == 0 || value.policyRevision != policy.revision || !std::ranges::contains(policy.configuredMembers, value.memberId)) {
		return BotCoordinationFailure::InvalidGroup;
	}
	if (value.observationRevision == 0) {
		return BotCoordinationFailure::StaleObservation;
	}
	if (value.sessionGeneration == 0) {
		return BotCoordinationFailure::GenerationMismatch;
	}
	if (value.expiresAt <= now || value.expiresAt - now > policy.maximumReservationDuration) {
		return BotCoordinationFailure::Expired;
	}
	const auto conflict = std::ranges::find_if(reservations, [&](const auto &existing) { return existing.type == value.type && existing.targetSignature == value.targetSignature; });
	if (conflict != reservations.end()) {
		if (conflict->memberId == value.memberId) {
			return BotCoordinationFailure::None;
		}
		if (!(policy.focusFire && value.type == BotCoordinationReservationType::CombatTarget)) {
			if (value.memberId < conflict->memberId) {
				*conflict = value;
				std::ranges::sort(reservations);
				return BotCoordinationFailure::None;
			}
			return BotCoordinationFailure::Conflict;
		}
	}
	if (reservations.size() >= policy.maximumReservations) {
		return BotCoordinationFailure::ReservationLimit;
	}
	reservations.push_back(value);
	std::ranges::sort(reservations);
	return BotCoordinationFailure::None;
}

void BotCoordination::invalidate(std::vector<BotCoordinationReservation> &reservations, BotCoordinationMemberId memberId, uint64_t sessionGeneration) {
	std::erase_if(reservations, [=](const auto &reservation) { return reservation.memberId == memberId && (sessionGeneration == 0 || reservation.sessionGeneration != sessionGeneration); });
}

void BotCoordination::invalidateTarget(std::vector<BotCoordinationReservation> &reservations, BotCoordinationReservationType type, uint64_t targetSignature) {
	std::erase_if(reservations, [=](const auto &reservation) { return reservation.type == type && reservation.targetSignature == targetSignature; });
}

void BotCoordination::expire(std::vector<BotCoordinationReservation> &reservations, uint64_t now) {
	std::erase_if(reservations, [=](const auto &reservation) { return reservation.expiresAt <= now; });
}
