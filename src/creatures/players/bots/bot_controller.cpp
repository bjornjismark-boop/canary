/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_controller.hpp"

#include "creatures/players/bots/bot_navigation.hpp"
#include "creatures/players/player.hpp"
#include "game/game.hpp"
#include "lib/logging/log_with_spd_log.hpp"

BotController::BotController(Game &game, const std::shared_ptr<Player> &player, BotRuntimeLimits limits, DecisionLog decisionLog) :
	game(game), player(player), limits(limits), decisionLog(std::move(decisionLog)) {
}

BotActionResult BotController::tick(std::chrono::milliseconds now) {
	lastTickWork = 0;
	if (now < nextTickAt) {
		return { BotActionStatus::Rejected, BotActionFailure::RateLimited };
	}
	nextTickAt = now + limits.tickInterval;
	const auto controlledPlayer = player.lock();
	const auto observation = BotPerception::observe(controlledPlayer);
	if (!observation) {
		return { BotActionStatus::Rejected, BotActionFailure::InvalidLifecycle };
	}
	lastTickWork = std::min(observation->visibleCreatures.size(), limits.maxCreaturesPerTick);
	if (observation->visibleCreatures.size() > limits.maxCreaturesPerTick) {
		return { BotActionStatus::Rejected, BotActionFailure::TickBudgetExceeded };
	}
	const auto action = selectAction(*observation);
	blackboard.state = action.type == BotActionType::InspectCreature ? BotRootState::Observe : BotRootState::Idle;
	auto result = execute(action, now);
	if (now >= nextLogAt) {
		if (decisionLog) {
			decisionLog(*observation, blackboard.state, action, result);
		} else {
			g_logger().debug(
				"[BotDecision] guid={} creature={} state={} action={} reason={} status={} failure={} attempts={} work={}",
				observation->playerGuid,
				observation->playerCreatureId,
				static_cast<uint8_t>(blackboard.state),
				static_cast<uint8_t>(action.type),
				static_cast<uint8_t>(action.reason),
				static_cast<uint8_t>(result.status),
				static_cast<uint8_t>(result.failure),
				result.attempts,
				lastTickWork
			);
		}
		nextLogAt = now + limits.tickInterval;
	}
	return result;
}

BotActionResult BotController::execute(const BotAction &action, std::chrono::milliseconds now) {
	if (now < blackboard.nextActionAt) {
		return { BotActionStatus::Rejected, BotActionFailure::RateLimited, blackboard.attempts, blackboard.nextActionAt - now };
	}
	if (blackboard.pendingAction) {
		if (now - blackboard.actionStartedAt < limits.actionTimeout) {
			return { BotActionStatus::Pending, BotActionFailure::None, blackboard.attempts };
		}
		if (now - blackboard.actionStartedAt >= limits.actionTimeout) {
			blackboard.lastFailure = BotActionFailure::TimedOut;
			if (blackboard.attempts >= limits.maxAttempts) {
				blackboard.pendingAction.reset();
				blackboard.state = BotRootState::Recover;
				return { BotActionStatus::Rejected, BotActionFailure::TimedOut, blackboard.attempts };
			}
			const auto exponent = std::min<uint32_t>(blackboard.attempts - 1U, 10U);
			const auto backoff = limits.initialBackoff * (1U << exponent);
			blackboard.nextActionAt = now + backoff;
			blackboard.pendingAction.reset();
			return { BotActionStatus::RetryScheduled, BotActionFailure::TimedOut, blackboard.attempts, backoff };
		}
	}

	auto result = perform(action);
	result.attempts = ++blackboard.attempts;
	blackboard.nextActionAt = now + limits.actionInterval;
	blackboard.lastFailure = result.failure;
	if (result.status == BotActionStatus::Pending) {
		blackboard.pendingAction = action;
		blackboard.actionStartedAt = now;
	} else {
		blackboard.pendingAction.reset();
		blackboard.attempts = 0;
	}
	return result;
}

BotAction BotController::selectAction(const BotObservation &observation) const {
	if (!observation.visibleCreatures.empty()) {
		return { BotActionType::InspectCreature, BotActionReason::ObserveTarget, observation.visibleCreatures.front().id };
	}
	return {};
}

BotActionResult BotController::perform(const BotAction &action) const {
	const auto controlledPlayer = player.lock();
	if (!controlledPlayer || !controlledPlayer->isBotControlled() || controlledPlayer->isRemoved() || !controlledPlayer->getTile()) {
		return { BotActionStatus::Rejected, BotActionFailure::InvalidLifecycle };
	}
	switch (action.type) {
		case BotActionType::Wait:
			return { BotActionStatus::Succeeded, BotActionFailure::None };
		case BotActionType::Move:
		{
			auto movement = revalidateAndMove(action);
			BotActionFailure failure = BotActionFailure::None;
			switch (movement.outcome) {
				case BotWalkability::InvalidDirection: failure = BotActionFailure::InvalidDirection; break;
				case BotWalkability::DifferentFloor: failure = BotActionFailure::DifferentFloor; break;
				case BotWalkability::OutsideKnownOrVisibleArea: failure = BotActionFailure::OutsideKnownOrVisibleArea; break;
				case BotWalkability::StaleObservation: failure = BotActionFailure::StaleObservation; break;
				case BotWalkability::WorldRejected: failure = BotActionFailure::WorldRejected; break;
				case BotWalkability::BlockedByTerrain:
				case BotWalkability::BlockedByItem:
				case BotWalkability::BlockedByCreature: failure = BotActionFailure::WorldRejected; break;
				case BotWalkability::Walkable:
				case BotWalkability::WalkableWithRisk: break;
			}
			return { movement.walkable() ? BotActionStatus::Succeeded : BotActionStatus::Rejected, failure, 0, {}, movement };
		}
		case BotActionType::InspectCreature: {
			const auto target = game.getCreatureByID(action.targetCreatureId);
			if (!target || target->isRemoved()
				|| (target != controlledPlayer
					&& (!Position::areInRange<8, 6, 0>(controlledPlayer->getPosition(), target->getPosition())
						|| !controlledPlayer->canSeeCreature(target)))) {
				return { BotActionStatus::Rejected, BotActionFailure::InvalidTarget };
			}
			return { BotActionStatus::Pending, BotActionFailure::None };
		}
	}
	return { BotActionStatus::Rejected, BotActionFailure::WorldRejected };
}

BotWalkabilityResult BotController::assess(Direction direction) const {
	const auto controlledPlayer = player.lock();
	const auto observation = BotPerception::observe(controlledPlayer);
	if (!observation) {
		BotMovementCandidate candidate { .direction = direction };
		return { .candidate = candidate, .outcome = BotWalkability::WorldRejected, .movementCost = BotNavigation::BlockedCost };
	}
	return BotNavigation::assess(*observation, direction);
}

BotActionResult BotController::executeMovement(const BotWalkabilityResult &assessment, std::chrono::milliseconds now) {
	const auto &candidate = assessment.candidate;
	return execute(
		BotAction {
			.type = BotActionType::Move,
			.reason = BotActionReason::Retry,
			.targetCreatureId = 0,
			.direction = candidate.direction,
			.observedOrigin = candidate.origin,
			.observedDestination = candidate.destination,
			.observationSignature = candidate.observationSignature,
		},
		now
	);
}

BotWalkabilityResult BotController::revalidateAndMove(const BotAction &action) const {
	const auto controlledPlayer = player.lock();
	if (!controlledPlayer || controlledPlayer->getPosition() != action.observedOrigin) {
		return { .candidate = { action.observedOrigin, action.observedDestination, action.direction, action.observationSignature }, .outcome = BotWalkability::StaleObservation, .movementCost = BotNavigation::BlockedCost };
	}
	const auto observation = BotPerception::observe(controlledPlayer);
	if (!observation) {
		return { .candidate = { action.observedOrigin, action.observedDestination, action.direction, action.observationSignature }, .outcome = BotWalkability::WorldRejected, .movementCost = BotNavigation::BlockedCost };
	}
	auto current = BotNavigation::assess(*observation, action.observedDestination);
	if (current.candidate.direction != action.direction || current.candidate.observationSignature != action.observationSignature) {
		current.outcome = BotWalkability::StaleObservation;
		current.movementCost = BotNavigation::BlockedCost;
		return current;
	}
	if (!current.walkable()) {
		return current;
	}
	const auto destinationTile = game.map.getTile(action.observedDestination);
	const ReturnValue worldResult = destinationTile
		? game.internalMoveCreature(controlledPlayer, destinationTile)
		: RETURNVALUE_NOTPOSSIBLE;
	current.worldReturnValue = static_cast<uint16_t>(worldResult);
	if (worldResult != RETURNVALUE_NOERROR) {
		current.outcome = BotWalkability::WorldRejected;
		current.movementCost = BotNavigation::BlockedCost;
	}
	return current;
}

ReturnValue BotController::move(Direction direction) const {
	const auto controlledPlayer = player.lock();
	if (!controlledPlayer || !controlledPlayer->isBotControlled() || controlledPlayer->isRemoved() || !controlledPlayer->getTile()) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	return game.internalMoveCreature(controlledPlayer, direction);
}

BotRouteProgress BotController::startRoute(const Position &destination, std::chrono::milliseconds now, BotRouteLimits requestedLimits) {
	route = {};
	routeLimits = requestedLimits;
	routeProgress = { .state = BotRouteState::Planning, .destination = destination, .lastProgressAt = now };
	const auto controlledPlayer = player.lock();
	const auto observation = BotPerception::observe(controlledPlayer);
	if (!observation) {
		routeProgress.state = BotRouteState::Failed; routeProgress.reason = BotRouteReason::InvalidLifecycle; return routeProgress;
	}
	route = BotNavigation::findRoute(*observation, { observation->position, destination, routeLimits });
	routeProgress.actualPosition = observation->position;
	routeProgress.reason = route.reason;
	if (route.reason == BotRouteReason::AlreadyAtDestination) routeProgress.state = BotRouteState::Arrived;
	else if (route.reason == BotRouteReason::RouteFound) routeProgress.state = BotRouteState::Ready;
	else routeProgress.state = BotRouteState::Failed;
	return routeProgress;
}

BotRouteProgress BotController::advanceRoute(std::chrono::milliseconds now) {
	if (routeProgress.state == BotRouteState::Cancelled || routeProgress.state == BotRouteState::Arrived || routeProgress.state == BotRouteState::Failed) return routeProgress;
	if (routeProgress.state == BotRouteState::Backoff && now < routeProgress.backoffDeadline) return routeProgress;
	const auto controlledPlayer = player.lock();
	const auto observation = BotPerception::observe(controlledPlayer);
	if (!observation) { routeProgress.state = BotRouteState::Failed; routeProgress.reason = BotRouteReason::InvalidLifecycle; return routeProgress; }
	routeProgress.actualPosition = observation->position;
	if (observation->position == routeProgress.destination) { routeProgress.state = BotRouteState::Arrived; routeProgress.reason = BotRouteReason::AlreadyAtDestination; return routeProgress; }
	if (routeProgress.state == BotRouteState::Ready && !route.positions.empty()) {
		routeProgress.expectedOrigin = observation->position;
		routeProgress.expectedNext = route.positions.front();
		const auto nextTile = std::ranges::find(observation->visibleTiles, routeProgress.expectedNext, &BotTileObservation::position);
		if (observation->topologyRevision != route.topologyRevision || nextTile == observation->visibleTiles.end() || BotNavigation::signature(*nextTile) != route.signatures.front()) {
			routeProgress.reason = nextTile != observation->visibleTiles.end() && nextTile->blockingCreatureId != 0 ? BotRouteReason::DynamicBlocker : BotRouteReason::StaleTopology;
			if (++routeProgress.totalReplans > routeLimits.maxReplans) { routeProgress.state = BotRouteState::Failed; routeProgress.reason = BotRouteReason::ReplanLimitExceeded; return routeProgress; }
			routeProgress.state = BotRouteState::Backoff;
			routeProgress.backoffDeadline = now + BotNavigation::backoff(routeLimits, routeProgress.totalReplans);
			return routeProgress;
		}
	}

	routeProgress.state = BotRouteState::Planning;
	route = BotNavigation::findRoute(*observation, { observation->position, routeProgress.destination, routeLimits });
	if (!route.found() || route.positions.empty()) { routeProgress.state = BotRouteState::Failed; routeProgress.reason = route.reason; return routeProgress; }
	routeProgress.state = BotRouteState::Ready;
	routeProgress.expectedOrigin = observation->position;
	routeProgress.expectedNext = route.positions.front();
	const auto currentTile = std::ranges::find(observation->visibleTiles, routeProgress.expectedNext, &BotTileObservation::position);
	if (currentTile == observation->visibleTiles.end() || BotNavigation::signature(*currentTile) != route.signatures.front()) {
		routeProgress.reason = BotRouteReason::StaleTopology;
	} else if (currentTile->blockingCreatureId != 0) {
		routeProgress.reason = BotRouteReason::DynamicBlocker;
	} else {
		routeProgress.state = BotRouteState::StepPending;
		const auto assessment = BotNavigation::assess(*observation, routeProgress.expectedNext);
		const auto actionResult = executeMovement(assessment, now);
		if (!actionResult.succeeded()) routeProgress.reason = actionResult.failure == BotActionFailure::StaleObservation ? BotRouteReason::StaleTopology : BotRouteReason::MovementRejected;
		else {
			const auto actual = controlledPlayer->getPosition();
			if (actual == routeProgress.expectedNext) {
				BotNavigation::observeProgress(routeProgress, actual, now, routeLimits); routeProgress.routeIndex++;
				if (actual == routeProgress.destination) { routeProgress.state = BotRouteState::Arrived; routeProgress.reason = BotRouteReason::RouteFound; }
				else { routeProgress.state = BotRouteState::ReplanRequired; routeProgress.reason = BotRouteReason::RouteFound; route = {}; }
				return routeProgress;
			}
			BotNavigation::observeProgress(routeProgress, actual, now, routeLimits);
		}
	}

	if (routeProgress.reason == BotRouteReason::NoProgress && routeProgress.consecutiveNoProgress >= routeLimits.maxNoProgress) {
		routeProgress.state = BotRouteState::Failed; return routeProgress;
	}
	if (++routeProgress.totalReplans > routeLimits.maxReplans) {
		routeProgress.state = BotRouteState::Failed; routeProgress.reason = BotRouteReason::ReplanLimitExceeded; return routeProgress;
	}
	routeProgress.state = BotRouteState::Backoff;
	routeProgress.backoffDeadline = now + BotNavigation::backoff(routeLimits, routeProgress.totalReplans);
	return routeProgress;
}

BotRouteProgress BotController::cancelRoute() {
	route = {};
	routeProgress.state = BotRouteState::Cancelled;
	routeProgress.reason = BotRouteReason::Cancelled;
	return routeProgress;
}
