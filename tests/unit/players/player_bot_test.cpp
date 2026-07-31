/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include <gtest/gtest.h>

#include "creatures/players/player.hpp"
#include "creatures/players/bots/bot_runtime.hpp"
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
