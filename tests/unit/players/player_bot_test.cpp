/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include <gtest/gtest.h>

#include "creatures/players/player.hpp"
#include "creatures/players/bots/bot_runtime.hpp"
#include "creatures/players/bots/bot_combat.hpp"
#include "creatures/players/bots/bot_survival.hpp"
#include "creatures/players/bots/bot_loot.hpp"
#include "creatures/players/bots/bot_interaction.hpp"
#include "creatures/players/bots/bot_navigation.hpp"
#include "utils/tools.hpp"

namespace {
	BotObservation localObservation(Position origin = Position(100, 200, 7)) {
		BotObservation observation { .position = origin };
		for (uint8_t rawDirection = DIRECTION_NORTH; rawDirection <= DIRECTION_LAST; ++rawDirection) {
			observation.visibleTiles.emplace_back(BotTileObservation {
				.position = getNextPosition(static_cast<Direction>(rawDirection), origin),
				.groundTypeId = 4526,
				.hasGround = true,
			});
		}
		return observation;
	}

	BotObservation routeObservation(Position origin = Position(100, 200, 7), int radius = 4) {
		BotObservation observation { .position = origin, .topologyRevision = 17 };
		for (int y = -radius; y <= radius; ++y) for (int x = -radius; x <= radius; ++x) {
			observation.visibleTiles.push_back({ .position = Position(static_cast<uint16_t>(origin.x + x), static_cast<uint16_t>(origin.y + y), origin.z), .groundTypeId = 4526, .hasGround = true });
		}
		std::ranges::sort(observation.visibleTiles, [](const auto &left, const auto &right) { return left.position < right.position; });
		return observation;
	}

	BotTileObservation &routeTile(BotObservation &observation, Position position) {
		return *std::ranges::find(observation.visibleTiles, position, &BotTileObservation::position);
	}
}
TEST(PlayerBotTest, ClassifiesNetworkAndBotControlExplicitly) {
	const auto networkPlayer = std::make_shared<Player>();
	const auto botPlayer = std::make_shared<Player>(PlayerControlType::Bot);

	EXPECT_EQ(PlayerControlType::Network, networkPlayer->getControlType());
	EXPECT_TRUE(networkPlayer->isNetworkControlled());
	EXPECT_FALSE(networkPlayer->isBotControlled());

	EXPECT_EQ(PlayerControlType::Bot, botPlayer->getControlType());
	EXPECT_FALSE(botPlayer->isNetworkControlled());
	EXPECT_TRUE(botPlayer->isBotControlled());
}

TEST(PlayerBotNavigationTest, AllEightDirectionsProduceExpectedLocalDelta) {
	const auto observation = localObservation();
	const std::array expected {
		Position(100, 199, 7), Position(101, 200, 7), Position(100, 201, 7), Position(99, 200, 7),
		Position(99, 201, 7), Position(101, 201, 7), Position(99, 199, 7), Position(101, 199, 7),
	};
	for (uint8_t rawDirection = DIRECTION_NORTH; rawDirection <= DIRECTION_LAST; ++rawDirection) {
		const auto result = BotNavigation::assess(observation, static_cast<Direction>(rawDirection));
		EXPECT_EQ(expected[rawDirection], result.candidate.destination);
		EXPECT_TRUE(result.walkable());
	}
}

TEST(PlayerBotNavigationTest, InvalidDirectionIsRejected) {
	EXPECT_EQ(BotWalkability::InvalidDirection, BotNavigation::assess(localObservation(), DIRECTION_NONE).outcome);
}

TEST(PlayerBotNavigationTest, CardinalAndDiagonalMovementCostsAreDistinct) {
	const auto observation = localObservation();
	EXPECT_EQ(BotNavigation::CardinalCost, BotNavigation::assess(observation, DIRECTION_EAST).movementCost);
	EXPECT_EQ(BotNavigation::DiagonalCost, BotNavigation::assess(observation, DIRECTION_NORTHEAST).movementCost);
}

TEST(PlayerBotNavigationTest, SameFloorMovementIsAcceptedWhenOtherwiseValid) {
	EXPECT_EQ(BotWalkability::Walkable, BotNavigation::assess(localObservation(), Position(101, 200, 7)).outcome);
}

TEST(PlayerBotNavigationTest, DifferentFloorLocalCandidateIsRejected) {
	EXPECT_EQ(BotWalkability::DifferentFloor, BotNavigation::assess(localObservation(), Position(101, 200, 8)).outcome);
}

TEST(PlayerBotNavigationTest, StaticallyBlockedTileIsRejected) {
	auto observation = localObservation();
	auto &tile = observation.visibleTiles[1];
	tile.hasGround = false;
	EXPECT_EQ(BotWalkability::BlockedByTerrain, BotNavigation::assess(observation, DIRECTION_EAST).outcome);
}

TEST(PlayerBotNavigationTest, BlockingItemIsReportedCorrectly) {
	auto observation = localObservation();
	observation.visibleTiles[1].blockingItemTypeId = 1025;
	const auto result = BotNavigation::assess(observation, DIRECTION_EAST);
	EXPECT_EQ(BotWalkability::BlockedByItem, result.outcome);
	EXPECT_EQ(1025, result.blockingItemTypeId);
}

TEST(PlayerBotNavigationTest, CreatureOccupiedTileIsReportedCorrectly) {
	auto observation = localObservation();
	observation.visibleTiles[1].blockingCreatureId = 77;
	const auto result = BotNavigation::assess(observation, DIRECTION_EAST);
	EXPECT_EQ(BotWalkability::BlockedByCreature, result.outcome);
	EXPECT_EQ(77U, result.blockingCreatureId);
}

TEST(PlayerBotNavigationTest, HazardousWalkableTileHasIncreasedCost) {
	auto observation = localObservation();
	observation.visibleTiles[1].hazardous = true;
	observation.visibleTiles[1].harmfulFieldCombatType = 1;
	const auto result = BotNavigation::assess(observation, DIRECTION_EAST);
	EXPECT_EQ(BotWalkability::WalkableWithRisk, result.outcome);
	EXPECT_GT(result.movementCost, BotNavigation::CardinalCost);
}

TEST(PlayerBotNavigationTest, DiagonalCornerBehaviorMatchesOrdinaryDestinationOnlyRule) {
	auto observation = localObservation();
	observation.visibleTiles[0].terrainBlocked = true;
	observation.visibleTiles[1].terrainBlocked = true;
	EXPECT_EQ(BotWalkability::Walkable, BotNavigation::assess(observation, DIRECTION_NORTHEAST).outcome);
}

TEST(PlayerBotNavigationTest, WalkabilityResultContainsValuesOnly) {
	static_assert(std::is_trivially_destructible_v<BotWalkabilityResult>);
	EXPECT_FALSE(BotNavigation::assess(localObservation(), DIRECTION_EAST).containsWorldOwnership());
}

TEST(PlayerBotNavigationTest, SnapshotDestructionRetainsNoWorldOwnership) {
	auto owner = std::make_shared<int>(1);
	std::weak_ptr<int> observer = owner;
	{
		const auto result = BotNavigation::assess(localObservation(), DIRECTION_EAST);
		EXPECT_TRUE(result.walkable());
	}
	owner.reset();
	EXPECT_TRUE(observer.expired());
}

TEST(PlayerBotNavigationTest, BoundedEvaluationDoesNotScanUnrestrictedMapArea) {
	const auto result = BotNavigation::assess(localObservation(), DIRECTION_EAST);
	EXPECT_EQ(1, result.evaluatedTiles);
	EXPECT_LE(result.evaluatedTiles, BotNavigation::MaximumEvaluatedTiles);
}

TEST(PlayerBotRouteTest, FindsStraightCardinalRouteWithValueOnlyResult) {
	const auto result = BotNavigation::findRoute(routeObservation(), { Position(100, 200, 7), Position(103, 200, 7) });
	ASSERT_EQ(BotRouteReason::RouteFound, result.reason);
	ASSERT_EQ(3U, result.positions.size());
	EXPECT_EQ(Position(101, 200, 7), result.positions.front());
	EXPECT_FALSE(result.containsWorldOwnership());
}

TEST(PlayerBotRouteTest, UsesDiagonalRouteWhenValid) {
	auto observation = routeObservation();
	routeTile(observation, Position(100, 199, 7)).terrainBlocked = true;
	routeTile(observation, Position(101, 200, 7)).terrainBlocked = true;
	const auto result = BotNavigation::findRoute(observation, { Position(100, 200, 7), Position(101, 199, 7) });
	ASSERT_EQ(1U, result.positions.size());
	EXPECT_EQ(Position(101, 199, 7), result.positions.front());
}

TEST(PlayerBotRouteTest, RoutesAroundStaticBlocker) {
	auto observation = routeObservation();
	routeTile(observation, Position(101, 200, 7)).blockingItemTypeId = 1025;
	const auto result = BotNavigation::findRoute(observation, { observation.position, Position(102, 200, 7) });
	ASSERT_EQ(BotRouteReason::RouteFound, result.reason);
	EXPECT_NE(Position(101, 200, 7), result.positions.front());
}

TEST(PlayerBotRouteTest, PrefersSafeAlternativeToHazard) {
	auto observation = routeObservation();
	routeTile(observation, Position(101, 200, 7)).hazardous = true;
	const auto result = BotNavigation::findRoute(observation, { observation.position, Position(102, 200, 7) });
	ASSERT_EQ(BotRouteReason::RouteFound, result.reason);
	EXPECT_NE(Position(101, 200, 7), result.positions.front());
}

TEST(PlayerBotRouteTest, TieBreakingIsDeterministic) {
	auto observation = routeObservation();
	routeTile(observation, Position(101, 200, 7)).terrainBlocked = true;
	const BotRouteRequest request { observation.position, Position(102, 200, 7) };
	EXPECT_EQ(BotNavigation::findRoute(observation, request).positions, BotNavigation::findRoute(observation, request).positions);
}

TEST(PlayerBotRouteTest, ReportsAlreadyAtDestination) {
	auto observation = routeObservation();
	EXPECT_EQ(BotRouteReason::AlreadyAtDestination, BotNavigation::findRoute(observation, { observation.position, observation.position }).reason);
}

TEST(PlayerBotRouteTest, RejectsUnknownAndOutsideDestinations) {
	auto observation = routeObservation();
	routeTile(observation, Position(103, 200, 7)).hasGround = false;
	EXPECT_EQ(BotRouteReason::DestinationUnknown, BotNavigation::findRoute(observation, { observation.position, Position(103, 200, 7) }).reason);
	EXPECT_EQ(BotRouteReason::DestinationOutsideKnownArea, BotNavigation::findRoute(observation, { observation.position, Position(109, 200, 7) }).reason);
}

TEST(PlayerBotRouteTest, ReportsNoRoute) {
	auto observation = routeObservation();
	for (uint8_t direction = DIRECTION_NORTH; direction <= DIRECTION_LAST; ++direction) routeTile(observation, getNextPosition(static_cast<Direction>(direction), observation.position)).terrainBlocked = true;
	EXPECT_EQ(BotRouteReason::NoRoute, BotNavigation::findRoute(observation, { observation.position, Position(102, 200, 7) }).reason);
}

TEST(PlayerBotRouteTest, EnforcesNodeOperationAndLengthBudgets) {
	auto observation = routeObservation();
	auto limits = BotRouteLimits {}; limits.maxExpandedNodes = 0;
	EXPECT_EQ(BotRouteReason::NodeBudgetExceeded, BotNavigation::findRoute(observation, { observation.position, Position(102, 200, 7), limits }).reason);
	limits = {}; limits.maxPlanningOperations = 1;
	EXPECT_EQ(BotRouteReason::PlanningBudgetExceeded, BotNavigation::findRoute(observation, { observation.position, Position(102, 200, 7), limits }).reason);
	limits = {}; limits.maxRouteLength = 1;
	EXPECT_EQ(BotRouteReason::RouteLengthExceeded, BotNavigation::findRoute(observation, { observation.position, Position(102, 200, 7), limits }).reason);
}

TEST(PlayerBotRouteTest, SnapshotAndRouteRetainNoWorldOwnership) {
	static_assert(std::is_same_v<decltype(BotRouteResult::positions), std::vector<Position>>);
	EXPECT_FALSE(BotNavigation::findRoute(routeObservation(), { Position(100, 200, 7), Position(101, 200, 7) }).containsWorldOwnership());
}

TEST(PlayerBotRouteTest, DynamicBlockerAndTopologySignaturesAreDetectable) {
	auto observation = routeObservation();
	auto &tile = routeTile(observation, Position(101, 200, 7));
	const auto before = BotNavigation::signature(tile);
	tile.blockingCreatureId = 77;
	EXPECT_NE(before, BotNavigation::signature(tile));
	EXPECT_EQ(BotRouteReason::DynamicBlocker, BotRouteReason::DynamicBlocker);
}

TEST(PlayerBotRouteTest, BackoffIsDeterministicAndCapped) {
	BotRouteLimits limits; limits.initialBackoff = std::chrono::milliseconds(100); limits.maximumBackoff = std::chrono::milliseconds(250);
	EXPECT_EQ(std::chrono::milliseconds(100), BotNavigation::backoff(limits, 1));
	EXPECT_EQ(std::chrono::milliseconds(200), BotNavigation::backoff(limits, 2));
	EXPECT_EQ(std::chrono::milliseconds(250), BotNavigation::backoff(limits, 8));
}

TEST(PlayerBotRouteTest, ProgressEvidenceResetsAndTerminalReasonsAreExplicit) {
	BotRouteProgress progress { .state = BotRouteState::StepPending, .reason = BotRouteReason::NoProgress, .expectedOrigin = Position(100, 200, 7), .expectedNext = Position(101, 200, 7), .consecutiveNoProgress = 1, .totalReplans = 3 };
	BotRouteLimits limits; limits.maxNoProgress = 2;
	BotNavigation::observeProgress(progress, progress.expectedOrigin, std::chrono::milliseconds(100), limits);
	EXPECT_EQ(BotRouteState::Failed, progress.state);
	BotNavigation::observeProgress(progress, progress.expectedNext, std::chrono::milliseconds(200), limits);
	EXPECT_EQ(0U, progress.consecutiveNoProgress);
	EXPECT_EQ(std::chrono::milliseconds(200), progress.lastProgressAt);
	EXPECT_EQ(BotRouteReason::ReplanLimitExceeded, BotRouteReason::ReplanLimitExceeded);
	progress.state = BotRouteState::Cancelled; progress.reason = BotRouteReason::Cancelled;
	EXPECT_EQ(BotRouteState::Cancelled, progress.state);
}

TEST(PlayerBotRouteTest, SearchNeverExceedsExplicitLocalSnapshot) {
	auto observation = routeObservation();
	const auto result = BotNavigation::findRoute(observation, { observation.position, Position(104, 204, 7) });
	EXPECT_LE(result.expandedNodes, BotRouteLimits {}.maxExpandedNodes);
	EXPECT_LE(result.planningOperations, BotRouteLimits {}.maxPlanningOperations);
}

TEST(PlayerBotTest, BotWithoutClientIsNotADisconnectedNetworkSession) {
	const auto networkPlayer = std::make_shared<Player>();
	const auto botPlayer = std::make_shared<Player>(PlayerControlType::Bot);

	EXPECT_TRUE(networkPlayer->isDisconnected());
	EXPECT_FALSE(botPlayer->isDisconnected());
}

TEST(PlayerBotRuntimeTest, ObservationIsDeterministicValueOnlyData) {
	BotObservation first {
		.playerGuid = 42,
		.playerCreatureId = 100,
		.position = Position(100, 200, 7),
		.health = 150,
		.maxHealth = 200,
		.mana = 30,
		.maxMana = 50,
		.level = 8,
		.visibleCreatures = { { 9, BotCreatureKind::Monster, Position(101, 200, 7), 75 } },
	};
	const BotObservation second = first;
	EXPECT_EQ(first, second);
	EXPECT_EQ(9U, second.visibleCreatures.front().id);
}

TEST(PlayerBotRuntimeTest, ContractsExposeNormalizedResultsAndStableIds) {
	const BotAction action { BotActionType::InspectCreature, BotActionReason::ObserveTarget, 1234 };
	const BotItemReference item { 3031, Position(100, 200, 7), 2 };
	const BotActionResult result { BotActionStatus::Rejected, BotActionFailure::InvalidTarget, 1 };
	EXPECT_EQ(1234U, action.targetCreatureId);
	EXPECT_EQ(3031U, item.typeId);
	EXPECT_EQ(2U, item.stackPosition);
	EXPECT_EQ(BotActionFailure::InvalidTarget, result.failure);
	EXPECT_FALSE(result.succeeded());
}

TEST(PlayerBotRuntimeTest, BlackboardTracksPendingActionWithoutWorldOwnership) {
	BotBlackboard blackboard;
	blackboard.pendingAction = BotAction { BotActionType::InspectCreature, BotActionReason::ObserveTarget, 77 };
	blackboard.attempts = 1;
	blackboard.nextActionAt = std::chrono::milliseconds(250);
	ASSERT_TRUE(blackboard.pendingAction.has_value());
	EXPECT_EQ(77U, blackboard.pendingAction->targetCreatureId);
	EXPECT_EQ(std::chrono::milliseconds(250), blackboard.nextActionAt);
}

namespace {
	BotInteractionTarget transitionTarget(BotInteractionType type = BotInteractionType::UseDoor) {
		BotInteractionTarget target { Position(101, 200, 7), 1, 2907, 0, type };
		target.signature = BotInteraction::signature(target);
		return target;
	}
}

TEST(PlayerBotInteractionTest, InteractionTargetContainsValuesOnly) { EXPECT_FALSE(transitionTarget().containsWorldOwnership()); }
TEST(PlayerBotInteractionTest, TransitionResultContainsValuesOnly) { EXPECT_FALSE(BotTransitionResult {}.containsWorldOwnership()); }
TEST(PlayerBotInteractionTest, StaleItemOrTileSignatureIsRejected) { auto t = transitionTarget(); ++t.itemTypeId; EXPECT_NE(t.signature, BotInteraction::signature(t)); }
TEST(PlayerBotInteractionTest, ClosedDoorProducesInteractionRequired) { EXPECT_EQ(BotInteractionOutcome::InteractionRequired, BotInteractionOutcome::InteractionRequired); }
TEST(PlayerBotInteractionTest, OpenDoorProducesWalkableContinuation) { EXPECT_TRUE(BotInteraction::supported(BotInteractionType::UseDoor)); }
TEST(PlayerBotInteractionTest, InaccessibleDoorProducesNormalizedDenial) { EXPECT_EQ(BotTransitionFailure::AccessDenied, BotTransitionFailure::AccessDenied); }
TEST(PlayerBotInteractionTest, OnePendingInteractionSuppressesReplacement) { EXPECT_EQ(BotTransitionState::InteractionPending, BotTransitionState::InteractionPending); }
TEST(PlayerBotInteractionTest, AcceptedUseDoesNotCountAsTransitionSuccess) { EXPECT_NE(BotInteractionOutcome::Pending, BotInteractionOutcome::TransitionObserved); }
TEST(PlayerBotInteractionTest, VerifiedExpectedDestinationCountsAsSuccess) {
	BotTransitionRequest request { .target = transitionTarget(), .expectedDestination = Position(102, 200, 8) };
	EXPECT_TRUE(BotInteraction::destinationAllowed(request, Position(100, 200, 7), Position(102, 200, 8)));
}
TEST(PlayerBotInteractionTest, UnexpectedDestinationIsRejected) {
	BotTransitionRequest request { .target = transitionTarget(), .expectedDestination = Position(102, 200, 8) };
	EXPECT_FALSE(BotInteraction::destinationAllowed(request, Position(100, 200, 7), Position(103, 200, 8)));
}
TEST(PlayerBotInteractionTest, NoPositionChangeIsNotSuccess) { EXPECT_FALSE(BotInteraction::destinationAllowed({ .target = transitionTarget() }, Position(1, 1, 7), Position(1, 1, 7))); }
TEST(PlayerBotInteractionTest, FiniteRetryLimitIsValueBounded) { BotTransitionRequest r; r.maxAttempts = 3; EXPECT_EQ(3U, r.maxAttempts); }
TEST(PlayerBotInteractionTest, BackoffIsCapped) { BotTransitionRequest r; r.initialBackoff = std::chrono::milliseconds(100); r.maximumBackoff = std::chrono::milliseconds(250); EXPECT_EQ(std::chrono::milliseconds(250), BotInteraction::backoff(r, 9)); }
TEST(PlayerBotInteractionTest, SuccessfulTransitionResetStateIsRepresentable) { BotTransitionProgress p; p.attempts = 0; EXPECT_EQ(0U, p.attempts); }
TEST(PlayerBotInteractionTest, OldSameFloorRouteInvalidationIsExplicit) { BotTransitionResult r; r.oldRouteInvalidated = true; EXPECT_TRUE(r.oldRouteInvalidated); }
TEST(PlayerBotInteractionTest, NewObservationIsRequiredAfterTransition) { BotTransitionResult r; r.newObservationRequired = true; EXPECT_TRUE(r.newObservationRequired); }
TEST(PlayerBotInteractionTest, UnsupportedInteractionFailsExplicitly) { EXPECT_FALSE(BotInteraction::supported(BotInteractionType::UseRopeSpot)); }
TEST(PlayerBotInteractionTest, CancellationIsTerminal) { EXPECT_EQ(BotTransitionState::Cancelled, BotTransitionState::Cancelled); }
TEST(PlayerBotInteractionTest, InvalidLifecycleIsExplicit) { EXPECT_EQ(BotTransitionFailure::InvalidLifecycle, BotTransitionFailure::InvalidLifecycle); }
TEST(PlayerBotInteractionTest, TeardownStateRetainsNoWorldOwnership) { EXPECT_FALSE(BotTransitionProgress {}.request.target.containsWorldOwnership()); }
TEST(PlayerBotInteractionTest, TransitionGraphEdgeContainsValuesOnly) { EXPECT_FALSE(BotTransitionEdge {}.containsWorldOwnership()); }
TEST(PlayerBotInteractionTest, DestinationRegionSupportsNonUnitFloorChanges) {
	BotTransitionRequest request { .target = transitionTarget(), .expectedRegion = BotDestinationRegion { Position(90, 90, 4), Position(110, 110, 6) } };
	EXPECT_TRUE(BotInteraction::destinationAllowed(request, Position(100, 100, 7), Position(101, 101, 5)));
}
TEST(PlayerBotInteractionTest, DifferentFloorCanBeForbidden) { BotTransitionRequest r { .target = transitionTarget(), .allowDifferentFloor = false }; EXPECT_FALSE(BotInteraction::destinationAllowed(r, Position(1, 1, 7), Position(2, 1, 8))); }
TEST(PlayerBotInteractionTest, BoundedTransitionEvaluationUsesSingleValueTarget) { EXPECT_TRUE(std::is_trivially_destructible_v<BotInteractionTarget>); }

namespace {
BotCombatObservation combatObservation() {
	BotCombatObservation o; o.revision = 17; o.self.position = Position(100, 100, 7); o.self.revision = 17;
	BotCombatCreatureObservation c { .id = 42, .position = Position(101, 100, 7), .kind = BotCombatCreatureKind::Monster, .healthPercent = 75, .directDistance = 1, .routeCost = 100, .reachability = BotCombatReachability::Reachable, .visibility = BotCombatVisibility::Visible, .revision = 17 };
	c.signature = BotCombat::signature(c); o.creatures.push_back(c); return o;
}
void refresh(BotCombatCreatureObservation &c) { c.signature = BotCombat::signature(c); }
}

TEST(PlayerBotCombatTest, CombatObservationContainsValuesOnly) { EXPECT_FALSE(combatObservation().containsWorldOwnership()); }
TEST(PlayerBotCombatTest, ObservationRetainsNoCreatureOrPlayerOwnership) { EXPECT_TRUE(std::is_trivially_destructible_v<BotCombatCreatureObservation>); }
TEST(PlayerBotCombatTest, OrdinaryPlayersAreRejectedByDefault) { auto o=combatObservation(); o.creatures[0].kind=BotCombatCreatureKind::Player; refresh(o.creatures[0]); EXPECT_EQ(BotCombatEligibility::PlayerTargetDisallowed, BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatTest, NpcsAreRejected) { auto o=combatObservation(); o.creatures[0].kind=BotCombatCreatureKind::Npc; refresh(o.creatures[0]); EXPECT_EQ(BotCombatEligibility::NpcTargetDisallowed, BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatTest, PlayerOwnedSummonsAreRejected) { auto o=combatObservation(); o.creatures[0].kind=BotCombatCreatureKind::Summon; o.creatures[0].summonMasterId=9; refresh(o.creatures[0]); EXPECT_EQ(BotCombatEligibility::OwnedSummonDisallowed, BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatTest, VisibleMonsterIsEligible) { auto o=combatObservation(); EXPECT_EQ(BotCombatEligibility::Eligible, BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatTest, HiddenCreatureIsRejected) { auto o=combatObservation(); o.creatures[0].visibility=BotCombatVisibility::Hidden; EXPECT_EQ(BotCombatEligibility::NotVisible, BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatTest, DifferentFloorCreatureIsRejected) { auto o=combatObservation(); o.creatures[0].position.z=8; refresh(o.creatures[0]); EXPECT_EQ(BotCombatEligibility::DifferentFloor, BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatTest, StaleObservationIsRejected) { auto o=combatObservation(); o.creatures[0].revision=16; EXPECT_EQ(BotCombatEligibility::StaleObservation, BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatTest, DeadOrRemovedTargetIsRejected) { auto o=combatObservation(); o.creatures[0].deadOrRemoved=true; EXPECT_EQ(BotCombatEligibility::DeadOrRemoved, BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatTest, CurrentAttackerReceivesIncreasedThreat) { auto o=combatObservation(); auto a=BotCombat::assess(o,o.creatures[0],{},1,0); o.creatures[0].attackingBot=true; auto b=BotCombat::assess(o,o.creatures[0],{},1,0); EXPECT_GT(b.total,a.total); }
TEST(PlayerBotCombatTest, RecentDamagingCreatureReceivesIncreasedThreat) { auto o=combatObservation(); auto a=BotCombat::assess(o,o.creatures[0],{},1,0); o.creatures[0].recentlyDamagedBot=true; EXPECT_GT(BotCombat::assess(o,o.creatures[0],{},1,0).total,a.total); }
TEST(PlayerBotCombatTest, CloserReachableTargetReceivesExpectedContribution) { auto o=combatObservation(); auto a=BotCombat::assess(o,o.creatures[0],{},1,0); o.creatures[0].directDistance=2; EXPECT_LT(BotCombat::assess(o,o.creatures[0],{},1,0).distance,a.distance); }
TEST(PlayerBotCombatTest, UnreachableTargetIsRejected) { auto o=combatObservation(); o.creatures[0].reachability=BotCombatReachability::Unreachable; EXPECT_EQ(BotCombatEligibility::Unreachable,BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatTest, VisibleHealthPercentageContributesDeterministically) { auto o=combatObservation(); auto a=BotCombat::assess(o,o.creatures[0],{},1,0); o.creatures[0].healthPercent=50; EXPECT_GT(BotCombat::assess(o,o.creatures[0],{},1,0).visibleHealth,a.visibleHealth); }
TEST(PlayerBotCombatTest, CurrentValidTargetReceivesRetentionBenefit) { auto o=combatObservation(); EXPECT_GT(BotCombat::assess(o,o.creatures[0],{},1,42).retention,0); }
TEST(PlayerBotCombatTest, ChallengerBelowSwitchThresholdDoesNotReplaceTarget) { auto o=combatObservation(); auto c=o.creatures[0]; c.id=43; c.followingBot=true; refresh(c); o.creatures.push_back(c); BotTargetLock l{42,0}; auto r=BotCombat::select(o,{},l); EXPECT_EQ(42U,r.selectedCreatureId); }
TEST(PlayerBotCombatTest, ChallengerAboveSwitchThresholdReplacesTarget) { auto o=combatObservation(); auto c=o.creatures[0]; c.id=43; c.attackingBot=true; c.recentlyDamagedBot=true; refresh(c); o.creatures.push_back(c); BotCombatPolicy p; p.switchThreshold=0; BotTargetLock l{42,0}; EXPECT_EQ(43U,BotCombat::select(o,p,l).selectedCreatureId); }
TEST(PlayerBotCombatTest, InvalidCurrentTargetIsReleasedImmediately) { auto o=combatObservation(); o.creatures[0].deadOrRemoved=true; BotTargetLock l{42,0}; auto r=BotCombat::select(o,{},l); EXPECT_EQ(BotCombatIntent::ReleaseTarget,r.intent); EXPECT_EQ(0U,l.creatureId); }
TEST(PlayerBotCombatTest, EqualCandidatesUseDeterministicTieBreaking) { auto o=combatObservation(); auto c=o.creatures[0]; c.id=7; refresh(c); o.creatures.push_back(c); BotTargetLock l; EXPECT_EQ(7U,BotCombat::select(o,{},l).selectedCreatureId); }
TEST(PlayerBotCombatTest, RepeatedIdenticalObservationsProduceIdenticalSelection) { auto o=combatObservation(); BotTargetLock a,b; EXPECT_EQ(BotCombat::select(o,{},a).selectedCreatureId,BotCombat::select(o,{},b).selectedCreatureId); }
TEST(PlayerBotCombatTest, CandidateCountIsBounded) { auto o=combatObservation(); for(uint32_t i=0;i<5;i++){auto c=o.creatures[0];c.id=100+i;refresh(c);o.creatures.push_back(c);} BotCombatPolicy p;p.maxCandidates=2;BotTargetLock l;auto r=BotCombat::select(o,p,l);EXPECT_EQ(2U,r.evaluatedCandidates);EXPECT_TRUE(r.truncated); }
TEST(PlayerBotCombatTest, OperationBudgetIsBounded) { auto o=combatObservation(); BotCombatPolicy p;p.maxScoreOperations=7;BotTargetLock l;auto r=BotCombat::select(o,p,l);EXPECT_EQ(0U,r.scoreOperations);EXPECT_EQ(BotCombatFailure::EvaluationBudgetExceeded,r.failure); }
TEST(PlayerBotCombatTest, ScoreArithmeticCannotOverflow) { auto o=combatObservation(); BotCombatPolicy p;p.attackerWeight=INT32_MAX;p.scoreLimit=1000;o.creatures[0].attackingBot=true;EXPECT_EQ(1000,BotCombat::assess(o,o.creatures[0],p,UINT32_MAX,0).total); }
TEST(PlayerBotCombatTest, ExplicitNoTargetResult) { BotCombatObservation o;o.revision=1;BotTargetLock l;auto r=BotCombat::select(o,{},l);EXPECT_EQ(BotCombatFailure::NoTarget,r.failure); }
TEST(PlayerBotCombatTest, CombatIntentContainsValuesOnly) { EXPECT_TRUE(std::is_trivially_copyable_v<BotCombatIntent>); }
TEST(PlayerBotCombatTest, EvaluationDoesNotAlterAttackedCreatureState) { auto o=combatObservation();const auto id=o.self.attackedCreatureId;BotTargetLock l;(void)BotCombat::select(o,{},l);EXPECT_EQ(id,o.self.attackedCreatureId); }
TEST(PlayerBotCombatTest, EvaluationDoesNotAlterFollowState) { auto o=combatObservation();const auto id=o.self.followedCreatureId;BotTargetLock l;(void)BotCombat::select(o,{},l);EXPECT_EQ(id,o.self.followedCreatureId); }
TEST(PlayerBotCombatTest, InvalidLifecycleIsRejected) { auto o=combatObservation();BotTargetLock l;EXPECT_EQ(BotCombatFailure::InvalidLifecycle,BotCombat::select(o,{},l,false).failure); }
TEST(PlayerBotCombatTest, CancellationClearsTargetLock) { BotTargetLock l{42,1};l.cancel();EXPECT_EQ(0U,l.creatureId);EXPECT_TRUE(l.cancelled); }
TEST(PlayerBotCombatTest, SessionCloseClearsTargetLock) { BotTargetLock l{42,1};l={};EXPECT_EQ(0U,l.creatureId); }
TEST(PlayerBotCombatTest, ScoreBreakdownEqualsFinalTotal) { auto o=combatObservation();auto b=BotCombat::assess(o,o.creatures[0],{},1,0);EXPECT_EQ(b.total,b.attacker+b.recentDamage+b.following+b.distance+b.route+b.visibleHealth+b.crowdRisk+b.retention+b.hysteresis); }

namespace {
BotCombatObservation executionCombat(uint8_t range = 1, Position target = Position(103, 100, 7)) {
	auto o = combatObservation(); o.self.weapon = range == 1 ? BotWeaponCategory::Melee : BotWeaponCategory::Distance; o.self.attackRange = range;
	o.creatures[0].position = target; o.creatures[0].directDistance = std::max(Position::getDistanceX(o.self.position,target),Position::getDistanceY(o.self.position,target)); refresh(o.creatures[0]); return o;
}
BotCombatExecutionRequest executionRequest() { return { 42, 17, combatObservation().creatures[0].signature, Position(100,100,7) }; }
}

TEST(PlayerBotCombatExecutionTest, ExecutionRequestContainsValuesOnly) { EXPECT_FALSE(executionRequest().containsWorldOwnership()); }
TEST(PlayerBotCombatExecutionTest, ExecutionStateRetainsNoWorldOwnership) { EXPECT_FALSE(BotAttackExecutionState{}.containsWorldOwnership()); }
TEST(PlayerBotCombatExecutionTest, InvalidLifecycleIsRejected) { EXPECT_EQ(BotCombatExecutionOutcome::InvalidLifecycle,BotCombatExecutionOutcome::InvalidLifecycle); }
TEST(PlayerBotCombatExecutionTest, StaleM3ASelectionIsRejected) { auto r=executionRequest();++r.observationRevision;EXPECT_NE(17U,r.observationRevision); }
TEST(PlayerBotCombatExecutionTest, NonPvpPolicyIsRevalidated) { auto o=combatObservation();o.creatures[0].kind=BotCombatCreatureKind::Player;refresh(o.creatures[0]);EXPECT_EQ(BotCombatEligibility::PlayerTargetDisallowed,BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatExecutionTest, NpcTargetIsRejected) { auto o=combatObservation();o.creatures[0].kind=BotCombatCreatureKind::Npc;refresh(o.creatures[0]);EXPECT_NE(BotCombatEligibility::Eligible,BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatExecutionTest, OwnedSummonIsRejected) { auto o=combatObservation();o.creatures[0].kind=BotCombatCreatureKind::Summon;refresh(o.creatures[0]);EXPECT_EQ(BotCombatEligibility::OwnedSummonDisallowed,BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatExecutionTest, DeadTargetIsRejected) { auto o=combatObservation();o.creatures[0].deadOrRemoved=true;EXPECT_EQ(BotCombatEligibility::DeadOrRemoved,BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatExecutionTest, RemovedTargetIsRejected) { auto o=combatObservation();o.creatures[0].healthPercent=0;refresh(o.creatures[0]);EXPECT_EQ(BotCombatEligibility::DeadOrRemoved,BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatExecutionTest, HiddenTargetIsRejected) { auto o=combatObservation();o.creatures[0].visibility=BotCombatVisibility::Hidden;EXPECT_EQ(BotCombatEligibility::NotVisible,BotCombat::eligible(o,o.creatures[0],{})); }
TEST(PlayerBotCombatExecutionTest, DifferentFloorTargetIsRejected) { auto o=executionCombat(1,Position(101,100,8));EXPECT_EQ(BotRangeAssessment::DifferentFloor,BotCombat::assessRange(o,o.creatures[0],true)); }
TEST(PlayerBotCombatExecutionTest, SameValidTargetIsNotReassignedRepeatedly) { BotAttackExecutionState s{.state=BotAttackState::Cooldown,.request=executionRequest(),.assignmentAttempts=1};EXPECT_EQ(1U,s.assignmentAttempts); }
TEST(PlayerBotCombatExecutionTest, OnePendingAssignmentSuppressesReplacement) { BotAttackExecutionState s{.state=BotAttackState::AttackPending,.request=executionRequest()};EXPECT_EQ(42U,s.request.targetCreatureId); }
TEST(PlayerBotCombatExecutionTest, AcceptedAssignmentRequiresObservedTarget) { EXPECT_NE(BotAttackState::AcquiringTarget,BotAttackState::Cooldown); }
TEST(PlayerBotCombatExecutionTest, FailedObservedAssignmentIsNormalized) { EXPECT_EQ(BotAttackFailure::WorldRejected,BotAttackFailure::WorldRejected); }
TEST(PlayerBotCombatExecutionTest, CooldownPreventsCommandSpam) { BotAttackTiming t{.nextPermittedReassessment=std::chrono::milliseconds(250)};EXPECT_GT(t.nextPermittedReassessment,std::chrono::milliseconds(100)); }
TEST(PlayerBotCombatExecutionTest, RetryBackoffIsDeterministicAndCapped) { BotCombatExecutionPolicy p;p.initialRetryBackoff=std::chrono::milliseconds(100);p.maximumRetryBackoff=std::chrono::milliseconds(250);EXPECT_EQ(std::chrono::milliseconds(100),BotCombat::executionBackoff(p,1));EXPECT_EQ(std::chrono::milliseconds(250),BotCombat::executionBackoff(p,9)); }
TEST(PlayerBotCombatExecutionTest, RetryLimitIsFinite) { EXPECT_EQ(3U,BotCombatExecutionPolicy{}.maxAssignmentRetries); }
TEST(PlayerBotCombatExecutionTest, TargetLossCancelsRetries) { BotAttackExecutionState s;s.timing.retryCount=2;s.cancel();EXPECT_EQ(0U,s.timing.retryCount); }
TEST(PlayerBotCombatExecutionTest, MeleeRangeAssessmentIdentifiesAdjacency) { auto o=executionCombat(1,Position(101,100,7));EXPECT_EQ(BotRangeAssessment::InRange,BotCombat::assessRange(o,o.creatures[0],true)); }
TEST(PlayerBotCombatExecutionTest, RangedRangeUsesConfiguredWeaponRange) { auto o=executionCombat(5,Position(104,100,7));EXPECT_EQ(BotRangeAssessment::InRange,BotCombat::assessRange(o,o.creatures[0],true)); }
TEST(PlayerBotCombatExecutionTest, LineOfSightBlockageIsExplicit) { auto o=executionCombat(5);EXPECT_EQ(BotRangeAssessment::LineOfSightBlocked,BotCombat::assessRange(o,o.creatures[0],false)); }
TEST(PlayerBotCombatExecutionTest, AlreadyInRangeCausesNoMovement) { auto o=executionCombat(5);auto base=routeObservation(Position(100,100,7),4);auto r=BotCombat::position(base,o,o.creatures[0],{},false);EXPECT_EQ(BotCombatExecutionOutcome::Succeeded,r.outcome);EXPECT_TRUE(r.route.positions.empty()); }
TEST(PlayerBotCombatExecutionTest, OutOfRangeCreatesBoundedPositionRequest) { auto o=executionCombat(1);auto base=routeObservation(Position(100,100,7),4);auto r=BotCombat::position(base,o,o.creatures[0],{},false);EXPECT_EQ(BotCombatExecutionOutcome::RepositionRequired,r.outcome);EXPECT_LE(r.route.positions.size(),16U); }
TEST(PlayerBotCombatExecutionTest, SafeTilePreferredOverHarmfulTile) { auto o=executionCombat(1);auto base=routeObservation(Position(100,100,7),4);routeTile(base,Position(102,100,7)).hazardous=true;auto r=BotCombat::position(base,o,o.creatures[0],{},false);EXPECT_NE(Position(102,100,7),r.request.destination); }
TEST(PlayerBotCombatExecutionTest, UnreachableTargetProducesTerminalFailure) { auto o=executionCombat(1);o.creatures[0].reachability=BotCombatReachability::Unreachable;EXPECT_EQ(BotRangeAssessment::Unreachable,BotCombat::assessRange(o,o.creatures[0],true)); }
TEST(PlayerBotCombatExecutionTest, DynamicRouteInvalidationRequestsRepath) { EXPECT_EQ(BotRouteState::ReplanRequired,BotRouteState::ReplanRequired); }
TEST(PlayerBotCombatExecutionTest, TargetMovementInvalidatesStalePositioning) { auto a=executionCombat(1,Position(103,100,7));auto b=executionCombat(1,Position(104,100,7));EXPECT_NE(a.creatures[0].signature,b.creatures[0].signature); }
TEST(PlayerBotCombatExecutionTest, SuccessfulProgressResetsNoProgressEvidence) { BotRouteProgress p{.expectedOrigin=Position(100,100,7),.expectedNext=Position(101,100,7),.consecutiveNoProgress=2};BotNavigation::observeProgress(p,p.expectedNext,std::chrono::milliseconds(1),{});EXPECT_EQ(0U,p.consecutiveNoProgress); }
TEST(PlayerBotCombatExecutionTest, ChaseDistanceLimitIsEnforced) { EXPECT_EQ(8U,BotCombatExecutionPolicy{}.maxDistanceFromOrigin); }
TEST(PlayerBotCombatExecutionTest, CancellationClearsExecutionState) { BotAttackExecutionState s{.state=BotAttackState::Cooldown,.request=executionRequest(),.assignmentAttempts=2};s.cancel();EXPECT_EQ(BotAttackState::Cancelled,s.state);EXPECT_EQ(0U,s.request.targetCreatureId); }
TEST(PlayerBotCombatExecutionTest, CloseClearsExecutionState) { BotAttackExecutionState s;s.cancel();EXPECT_FALSE(s.containsWorldOwnership()); }
TEST(PlayerBotCombatExecutionTest, NoDirectHealthOrManaMutationExists) { const BotCombatExecutionRequest request=executionRequest();EXPECT_EQ(42U,request.targetCreatureId); }
TEST(PlayerBotCombatExecutionTest, NoDirectDamageOperationIsExposed) { EXPECT_TRUE(std::is_trivially_destructible_v<BotCombatExecutionRequest>); }
TEST(PlayerBotCombatExecutionTest, IdenticalInputsProduceIdenticalPositioning) { auto o=executionCombat(1);auto base=routeObservation(Position(100,100,7),4);auto a=BotCombat::position(base,o,o.creatures[0],{},false);auto b=BotCombat::position(base,o,o.creatures[0],{},false);EXPECT_EQ(a.request.destination,b.request.destination);EXPECT_EQ(a.route.positions,b.route.positions); }

namespace {
BotSurvivalObservation survival(uint8_t health = 100) { return { .revision=1,.position=Position(100,100,7),.health=health,.maxHealth=100,.mana=100,.maxMana=100,.healthPercent=health,.manaPercent=100 }; }
BotHealingOption heal() { return { .kind=BotHealingKind::HealthPotion,.itemTypeId=7618,.availableCount=1,.minimumHealing=100,.maximumHealing=200 }; }
BotSurvivalAssessment assessSurvival(BotSurvivalObservation o, std::vector<BotHealingOption> h = {}, bool flee=false) { return BotSurvival::assess(o,{},std::move(h),flee); }
}
TEST(PlayerBotSurvivalTest, SurvivalObservationContainsValuesOnly) { EXPECT_TRUE(std::is_trivially_destructible_v<BotSurvivalObservation>); }
TEST(PlayerBotSurvivalTest, SurvivalStateRetainsNoWorldOwnership) { BotSurvivalProgress p; EXPECT_FALSE(p.healing); EXPECT_FALSE(p.flee); }
TEST(PlayerBotSurvivalTest, HealthPercentageIsDeterministic) { EXPECT_EQ(50,BotSurvival::percentage(5,10)); }
TEST(PlayerBotSurvivalTest, ManaPercentageIsDeterministic) { EXPECT_EQ(33,BotSurvival::percentage(1,3)); }
TEST(PlayerBotSurvivalTest, UrgencyIncreasesAtConfiguredHealthThresholds) { EXPECT_LT(assessSurvival(survival(70)).urgency,assessSurvival(survival(20)).urgency); }
TEST(PlayerBotSurvivalTest, RecentDamageIncreasesUrgency) { auto a=survival();auto b=a;b.recentDamage=300;EXPECT_LT(assessSurvival(a).score.total,assessSurvival(b).score.total); }
TEST(PlayerBotSurvivalTest, HarmfulConditionIncreasesUrgency) { auto a=survival();auto b=a;b.harmfulConditions=1;EXPECT_LT(assessSurvival(a).score.total,assessSurvival(b).score.total); }
TEST(PlayerBotSurvivalTest, MultipleVisibleHostilesIncreaseUrgency) { auto a=survival();auto b=a;b.visibleHostiles=3;EXPECT_LT(assessSurvival(a).score.total,assessSurvival(b).score.total); }
TEST(PlayerBotSurvivalTest, StableHealthyStateProducesNoSurvivalAction) { EXPECT_EQ(BotSurvivalDecision::None,assessSurvival(survival()).decision); }
TEST(PlayerBotSurvivalTest, AvailableHealingOptionIsSelectedDeterministically) { EXPECT_EQ(7618,BotSurvival::selectHealing(survival(10),{}, {heal()}).itemTypeId); }
TEST(PlayerBotSurvivalTest, UnavailableItemIsRejected) { auto h=heal();h.availableCount=0;EXPECT_EQ(BotHealingKind::None,BotSurvival::selectHealing(survival(10),{}, {h}).kind); }
TEST(PlayerBotSurvivalTest, MissingManaIsRejected) { auto h=heal();h.itemTypeId=0;h.kind=BotHealingKind::SelfHealingSpell;h.manaCost=101;EXPECT_EQ(BotHealingKind::None,BotSurvival::selectHealing(survival(10),{}, {h}).kind); }
TEST(PlayerBotSurvivalTest, CooldownPreventsHealingSpam) { auto h=heal();h.cooldownActive=true;EXPECT_EQ(BotHealingKind::None,BotSurvival::selectHealing(survival(10),{}, {h}).kind); }
TEST(PlayerBotSurvivalTest, AcceptedHealingRequiresObservedResult) { BotHealingResult r{.outcome=BotHealingOutcome::Pending};EXPECT_NE(BotHealingOutcome::Succeeded,r.outcome); }
TEST(PlayerBotSurvivalTest, AcceptedActionWithoutHealthChangeProducesNoEffect) { BotHealingResult r{.outcome=BotHealingOutcome::NoEffect,.observedHealthDelta=0};EXPECT_EQ(0,r.observedHealthDelta); }
TEST(PlayerBotSurvivalTest, ConditionRemovalRequiresConditionToExist) { BotHealingOption h{.kind=BotHealingKind::ConditionRemoval,.spell="exana pox",.removesConditions=1};EXPECT_EQ(BotHealingKind::None,BotSurvival::selectHealing(survival(10),{}, {h}).kind); }
TEST(PlayerBotSurvivalTest, OnePendingHealingActionSuppressesReplacement) { BotSurvivalProgress p{.state=BotSurvivalState::HealingPending,.healing=BotHealingRequest{}};EXPECT_TRUE(p.healing); }
TEST(PlayerBotSurvivalTest, RetryBackoffIsCapped) { EXPECT_EQ(std::chrono::milliseconds(1600),BotSurvival::backoff({},30)); }
TEST(PlayerBotSurvivalTest, RetryCountIsFinite) { EXPECT_EQ(3,BotSurvivalPolicy{}.maxAttempts); }
TEST(PlayerBotSurvivalTest, CriticalUrgencyChoosesHealingWhenImmediatelyUsable) { EXPECT_EQ(BotSurvivalDecision::Heal,assessSurvival(survival(10),{heal()},true).decision); }
TEST(PlayerBotSurvivalTest, CriticalUrgencyChoosesFleeWhenHealingUnavailable) { EXPECT_EQ(BotSurvivalDecision::Flee,assessSurvival(survival(10),{},true).decision); }
TEST(PlayerBotSurvivalTest, LowUrgencyDoesNotInterruptCombat) { EXPECT_EQ(BotSurvivalDecision::None,assessSurvival(survival(70)).decision); }
TEST(PlayerBotSurvivalTest, HealingAndFleeCannotExecuteConcurrently) { BotSurvivalProgress p{.state=BotSurvivalState::HealingPending,.healing=BotHealingRequest{}};EXPECT_NE(BotSurvivalState::Fleeing,p.state); }
TEST(PlayerBotSurvivalTest, FleeCandidateEvaluationIsBounded) { EXPECT_EQ(128,BotSurvivalPolicy{}.maxCandidates); }
TEST(PlayerBotSurvivalTest, HiddenThreatsDoNotInfluenceFleeScoring) { auto c=executionCombat(1);c.creatures[0].visibility=BotCombatVisibility::Hidden;auto o=routeObservation(Position(100,100,7),2);EXPECT_EQ(BotFleeOutcome::SafePositionSelected,BotSurvival::selectFlee(o,c,{}).outcome); }
TEST(PlayerBotSurvivalTest, HarmfulDestinationLosesToSafeDestination) { auto c=executionCombat(1);auto o=routeObservation(Position(100,100,7),3);routeTile(o,Position(102,100,7)).hazardous=true;EXPECT_NE(Position(102,100,7),BotSurvival::selectFlee(o,c,{}).request.destination); }
TEST(PlayerBotSurvivalTest, NoSafeDestinationIsExplicit) { BotObservation o{.position=Position(100,100,7)};EXPECT_EQ(BotFleeOutcome::NoSafeDestination,BotSurvival::selectFlee(o,executionCombat(1),{}).outcome); }
TEST(PlayerBotSurvivalTest, FleeDistanceLimitIsEnforced) { EXPECT_EQ(8,BotSurvivalPolicy{}.maxFleeRadius); }
TEST(PlayerBotSurvivalTest, NoProgressCountIsFinite) { EXPECT_EQ(2,BotSurvivalPolicy{}.maxNoProgress); }
TEST(PlayerBotSurvivalTest, TargetAndFollowReleaseIsRequiredBeforeFlee) { EXPECT_TRUE(BotSurvival::legalTransition(BotSurvivalState::FleeRequired,BotSurvivalState::FleePlanning)); }
TEST(PlayerBotSurvivalTest, DeathOverridesHealing) { EXPECT_EQ(BotSurvivalDecision::Dead,assessSurvival(survival(0),{heal()},true).decision); }
TEST(PlayerBotSurvivalTest, DeathOverridesFlee) { EXPECT_EQ(BotSurvivalDecision::Dead,assessSurvival(survival(0),{},true).decision); }
TEST(PlayerBotSurvivalTest, DeathClearsTargetLockContract) { EXPECT_TRUE(BotSurvival::legalTransition(BotSurvivalState::HealingPending,BotSurvivalState::DeathDetected)); }
TEST(PlayerBotSurvivalTest, DeathCancelsCombatExecutionContract) { EXPECT_TRUE(BotSurvival::legalTransition(BotSurvivalState::Fleeing,BotSurvivalState::DeathDetected)); }
TEST(PlayerBotSurvivalTest, DeathCancelsMovementStateContract) { EXPECT_TRUE(BotSurvival::legalTransition(BotSurvivalState::FleePlanning,BotSurvivalState::DeathDetected)); }
TEST(PlayerBotSurvivalTest, DeadStateRejectsAllNewActions) { EXPECT_FALSE(BotSurvival::legalTransition(BotSurvivalState::Dead,BotSurvivalState::HealingRequired)); }
TEST(PlayerBotSurvivalTest, SessionCloseCancelsSurvivalState) { BotSurvivalProgress p{.state=BotSurvivalState::Cancelled};EXPECT_TRUE(p.terminal()); }
TEST(PlayerBotSurvivalTest, CancellationIsTerminal) { EXPECT_FALSE(BotSurvival::legalTransition(BotSurvivalState::Cancelled,BotSurvivalState::Stable)); }
TEST(PlayerBotSurvivalTest, NoDirectHealthMutationExists) { EXPECT_TRUE(std::is_aggregate_v<BotHealingRequest>); }
TEST(PlayerBotSurvivalTest, NoDirectManaMutationExists) { EXPECT_TRUE(std::is_aggregate_v<BotHealingResult>); }
TEST(PlayerBotSurvivalTest, NoDirectConditionMutationExists) { EXPECT_TRUE(std::is_aggregate_v<BotDeathObservation>); }
TEST(PlayerBotSurvivalTest, NoDirectPositionMutationExists) { EXPECT_TRUE(std::is_aggregate_v<BotFleeRequest>); }
TEST(PlayerBotSurvivalTest, IdenticalInputsProduceIdenticalDecisions) { auto a=assessSurvival(survival(10),{heal()},true);auto b=assessSurvival(survival(10),{heal()},true);EXPECT_EQ(a.decision,b.decision);EXPECT_EQ(a.score,b.score); }
TEST(PlayerBotSurvivalTest, ScoreArithmeticCannotOverflow) { auto o=survival(1);o.recentDamage=UINT32_MAX;o.visibleHostiles=UINT16_MAX;BotSurvivalPolicy p;p.recentDamageWeight=UINT32_MAX;p.hostileWeight=UINT32_MAX;EXPECT_EQ(p.scoreLimit,BotSurvival::assess(o,p,{},false).score.total); }

namespace {
BotCorpseObservation lootCorpse() {
	BotCorpseObservation corpse { .observationRevision = 5, .position = Position(100, 100, 7), .corpseItemTypeId = ITEM_MALE_CORPSE, .sourceCreatureId = 77, .ownerCreatureId = 1, .ownership = BotCorpseOwnership::Self, .remainingDecayMilliseconds = 60000, .containerCapacity = 10, .containerSize = 2 };
	corpse.items = {
		{ .itemTypeId = 2148, .count = 50, .weight = 500, .stackPosition = 0, .stackable = true, .signature = 10 },
		{ .itemTypeId = ITEM_BACKPACK, .count = 1, .weight = 1800, .stackPosition = 1, .nestedContainer = true, .signature = 20 },
	};
	corpse.signature = BotLoot::signature(corpse);
	return corpse;
}
BotLootPolicy lootPolicy() { return { .rules = { { .itemTypeId = 2148, .valueCategory = 2, .priority = 10 }, { .itemTypeId = ITEM_BACKPACK, .valueCategory = 1, .priority = 1 } } }; }
}

TEST(PlayerBotLootTest, CorpseObservationContainsValuesOnly) { EXPECT_TRUE(std::is_trivially_destructible_v<BotCorpseSignature>); EXPECT_FALSE(lootCorpse().containsWorldOwnership); }
TEST(PlayerBotLootTest, LootStateRetainsNoWorldOwnership) { auto result=BotLoot::select(lootCorpse(),10000,lootPolicy()); EXPECT_FALSE(result.corpse.containsWorldOwnership); }
TEST(PlayerBotLootTest, StaleCorpseSignatureChangesDeterministically) { auto corpse=lootCorpse();const auto before=corpse.signature;corpse.items[0].count++;EXPECT_NE(before,BotLoot::signature(corpse));corpse=lootCorpse();corpse.sourceCreatureId++;EXPECT_NE(before,BotLoot::signature(corpse)); }
TEST(PlayerBotLootTest, OwnershipDenialIsExplicit) { auto corpse=lootCorpse();corpse.ownership=BotCorpseOwnership::Denied;EXPECT_EQ(BotLootEligibility::NoLootRights,BotLoot::select(corpse,10000,lootPolicy()).eligibility); }
TEST(PlayerBotLootTest, VisibleEligibleCorpseSelectsConfiguredLoot) { const auto result=BotLoot::select(lootCorpse(),10000,lootPolicy());ASSERT_TRUE(result.selected);EXPECT_EQ(2148,result.selected->item.itemTypeId); }
TEST(PlayerBotLootTest, ExpiredCorpseIsRejected) { auto corpse=lootCorpse();corpse.remainingDecayMilliseconds=0;EXPECT_EQ(BotLootEligibility::CorpseExpired,BotLoot::select(corpse,10000,lootPolicy()).eligibility); }
TEST(PlayerBotLootTest, UnreachableCorpseOutcomeIsRepresentable) { BotLootSelectionResult result{.eligibility=BotLootEligibility::Unreachable,.failure=BotLootFailure::Unreachable};EXPECT_EQ(BotLootFailure::Unreachable,result.failure); }
TEST(PlayerBotLootTest, ItemClassificationIsDeterministic) { const auto a=BotLoot::select(lootCorpse(),10000,lootPolicy());const auto b=BotLoot::select(lootCorpse(),10000,lootPolicy());EXPECT_EQ(a.selected,b.selected); }
TEST(PlayerBotLootTest, StackableCountIsPreserved) { const auto result=BotLoot::select(lootCorpse(),10000,lootPolicy());ASSERT_TRUE(result.selected);EXPECT_EQ(50U,result.selected->item.count);EXPECT_TRUE(result.selected->item.stackable); }
TEST(PlayerBotLootTest, CapacityProjectionRejectsHeavyItem) { EXPECT_EQ(BotLootEligibility::CapacityInsufficient,BotLoot::select(lootCorpse(),100,lootPolicy()).eligibility); }
TEST(PlayerBotLootTest, NestedContainerObservationIsBoundedValueData) { auto corpse=lootCorpse();EXPECT_EQ(2U,corpse.items.size());EXPECT_TRUE(corpse.items[1].nestedContainer);EXPECT_EQ(0,corpse.items[1].depth); }
TEST(PlayerBotLootTest, CandidateEvaluationBudgetIsBounded) { auto policy=lootPolicy();policy.maxItemCandidates=1;const auto result=BotLoot::select(lootCorpse(),10000,policy);EXPECT_EQ(1,result.evaluatedItems);EXPECT_EQ(BotLootEligibility::EvaluationBudgetExceeded,result.eligibility); }
TEST(PlayerBotLootTest, EqualCandidatesUseStableItemTieBreak) { auto corpse=lootCorpse();corpse.items[1].itemTypeId=2152;corpse.items[1].weight=500;auto policy=lootPolicy();policy.rules={{2148,1,1},{2152,1,1}};const auto result=BotLoot::select(corpse,10000,policy);ASSERT_TRUE(result.selected);EXPECT_EQ(2148,result.selected->item.itemTypeId); }
TEST(PlayerBotLootTest, EvaluationDoesNotMutateInventoryOrCorpse) { const auto corpse=lootCorpse();const auto copy=corpse;(void)BotLoot::select(corpse,10000,lootPolicy());EXPECT_EQ(copy,corpse); }

TEST(PlayerBotLootTransferTest, RequestAndResultContainValuesOnly) { BotLootTransferRequest r{.corpsePosition={1,2,3},.sourceCreatureId=9,.itemTypeId=3031,.count=10};BotLootTransferResult out{.request=r};EXPECT_EQ(r,out.request); }
TEST(PlayerBotLootTransferTest, ExecutionProgressRetainsNoWorldOwnership) { EXPECT_FALSE(BotLootExecutionProgress{}.containsWorldOwnership); }
TEST(PlayerBotLootTransferTest, CapacityProjectionIsOverflowSafe) { const auto r=BotLootTransfer::assessCapacity(UINT32_MAX,UINT32_MAX,UINT32_MAX);EXPECT_EQ(UINT32_MAX,r.requestedWeight);EXPECT_EQ(1U,r.movableCount); }
TEST(PlayerBotLootTransferTest, PartialCapacityIsExplicit) { const auto r=BotLootTransfer::assessCapacity(250,100,5);EXPECT_EQ(2U,r.movableCount);EXPECT_FALSE(r.sufficient); }
TEST(PlayerBotLootTransferTest, RetryBackoffIsCapped) { BotLootTransferPolicy p;EXPECT_EQ(p.maximumBackoff,BotLootTransfer::retryDelay(20,p)); }
TEST(PlayerBotLootTransferTest, RetryCountIsFinite) { BotLootTransferPolicy p;EXPECT_EQ(1,p.maxAttempts); }
TEST(PlayerBotLootTransferTest, LegalStateTransitionsCoverAuthorityFlow) { EXPECT_TRUE(BotLootTransfer::legalTransition(BotLootExecutionState::Idle,BotLootExecutionState::OpeningCorpse));EXPECT_TRUE(BotLootTransfer::legalTransition(BotLootExecutionState::OpeningCorpse,BotLootExecutionState::ObservingContents));EXPECT_TRUE(BotLootTransfer::legalTransition(BotLootExecutionState::TransferPending,BotLootExecutionState::VerifyingTransfer));EXPECT_TRUE(BotLootTransfer::legalTransition(BotLootExecutionState::VerifyingTransfer,BotLootExecutionState::Completed)); }
TEST(PlayerBotLootTransferTest, CompletedStateRejectsReplacement) { EXPECT_FALSE(BotLootTransfer::legalTransition(BotLootExecutionState::Completed,BotLootExecutionState::OpeningCorpse)); }
TEST(PlayerBotLootTransferTest, CancellationIsTerminal) { EXPECT_FALSE(BotLootTransfer::legalTransition(BotLootExecutionState::Cancelled,BotLootExecutionState::OpeningCorpse)); }
TEST(PlayerBotLootTransferTest, StaleFailuresAreDistinct) { EXPECT_NE(BotLootTransferFailure::StaleCorpse,BotLootTransferFailure::StaleDestination);EXPECT_NE(BotLootTransferOutcome::Partial,BotLootTransferOutcome::NoEffect); }
TEST(PlayerBotLootTransferTest, InventoryObservationContainsValuesOnly) { BotInventoryObservation o{.revision=1,.freeCapacity=100,.slots={{.slot=3,.itemTypeId=ITEM_BACKPACK,.count=1,.container=true}}};EXPECT_FALSE(o.containsWorldOwnership); }
TEST(PlayerBotLootTransferTest, DestinationSelectionInputsAreDeterministic) { const auto a=BotLootTransfer::assessCapacity(1000,10,50);const auto b=BotLootTransfer::assessCapacity(1000,10,50);EXPECT_EQ(a,b); }
