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
#include "creatures/players/bots/bot_supply.hpp"
#include "creatures/players/bots/bot_adventure.hpp"
#include "creatures/players/bots/bot_equipment.hpp"
#include "creatures/players/bots/bot_shop.hpp"
#include "creatures/players/bots/bot_resupply.hpp"
#include "creatures/players/bots/bot_dialogue.hpp"
#include "creatures/players/bots/bot_quest.hpp"
#include "creatures/players/bots/bot_quest_execution.hpp"
#include "creatures/players/bots/bot_planner.hpp"
#include "creatures/players/bots/bot_plan_execution.hpp"
#include "creatures/players/bots/bot_planner_persistence.hpp"
#include "creatures/players/bots/bot_progression.hpp"
#include "creatures/players/bots/bot_coordination.hpp"
#include "creatures/players/bots/bot_fleet.hpp"
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

class PlayerBotEquipmentTest : public ::testing::Test {
protected:
	static BotItemObservation item(uint16_t id, BotEquipmentSlot slot, int32_t attack = 0, int32_t defense = 0, int32_t armor = 0) {
		return { .itemTypeId=id, .countOrCharges=1, .category=attack ? BotItemCategory::Weapon : defense ? BotItemCategory::Shield : BotItemCategory::Armor, .slot=slot, .path={CONST_SLOT_BACKPACK,{id}}, .weight=100, .attack=attack, .defense=defense, .armor=armor, .range=1, .vocationCompatible=true, .levelCompatible=true };
	}
	BotEquipmentObservation observation { .revision=7, .freeCapacity=10000, .playerLevel=100, .vocationId=1 };
	BotEquipmentPolicy policy;
};

TEST_F(PlayerBotEquipmentTest, ItemObservationContainsValuesOnly) { EXPECT_FALSE(item(1,BotEquipmentSlot::Armor).containsWorldOwnership()); }
TEST_F(PlayerBotEquipmentTest, EquipmentObservationContainsValuesOnly) { EXPECT_FALSE(observation.containsWorldOwnership()); }
TEST_F(PlayerBotEquipmentTest, NoItemOrContainerOwnershipIsRetained) { BotUpgradeCandidate candidate; EXPECT_FALSE(candidate.containsWorldOwnership()); }
TEST_F(PlayerBotEquipmentTest, VocationRestrictionIsEnforced) { auto candidate=item(1,BotEquipmentSlot::Armor);candidate.vocationCompatible=false;EXPECT_EQ(BotEquipmentIntent::WrongVocation,BotEquipment::compare(nullptr,candidate,observation).intent); }
TEST_F(PlayerBotEquipmentTest, LevelRestrictionIsEnforced) { auto candidate=item(1,BotEquipmentSlot::Armor);candidate.levelCompatible=false;EXPECT_EQ(BotEquipmentIntent::MissingRequirement,BotEquipment::compare(nullptr,candidate,observation).intent); }
TEST_F(PlayerBotEquipmentTest, WrongSlotIsRejected) { auto candidate=item(1,BotEquipmentSlot::None);EXPECT_EQ(BotEquipmentIntent::WrongSlot,BotEquipment::compare(nullptr,candidate,observation).intent); }
TEST_F(PlayerBotEquipmentTest, StrongerCompatibleWeaponRanksAboveWeakerWeapon) { auto weak=item(1,BotEquipmentSlot::LeftHand,10),strong=item(2,BotEquipmentSlot::LeftHand,20);EXPECT_GT(BotEquipment::compare(&weak,strong,observation).improvement,0); }
TEST_F(PlayerBotEquipmentTest, ArmorComparisonIsDeterministic) { auto old=item(1,BotEquipmentSlot::Armor,0,0,5),next=item(2,BotEquipmentSlot::Armor,0,0,8);EXPECT_EQ(BotEquipment::compare(&old,next,observation),BotEquipment::compare(&old,next,observation)); }
TEST_F(PlayerBotEquipmentTest, ShieldComparisonIsDeterministic) { auto old=item(1,BotEquipmentSlot::RightHand,0,10),next=item(2,BotEquipmentSlot::RightHand,0,15);EXPECT_GT(BotEquipment::compare(&old,next,observation).improvement,0); }
TEST_F(PlayerBotEquipmentTest, RangedAndMeleePoliciesDiffer) { auto bow=item(1,BotEquipmentSlot::LeftHand,10);bow.range=7;uint32_t a=0,b=0;policy.role=BotEquipmentRole::Melee;auto melee=BotEquipment::score(bow,policy,a);policy.role=BotEquipmentRole::Distance;auto ranged=BotEquipment::score(bow,policy,b);EXPECT_GT(ranged.total,melee.total); }
TEST_F(PlayerBotEquipmentTest, WeightAndCapacityRiskAffectsIntent) { auto candidate=item(1,BotEquipmentSlot::Armor);candidate.weight=500;observation.freeCapacity=400;EXPECT_EQ(BotEquipmentIntent::CapacityRisk,BotEquipment::compare(nullptr,candidate,observation).intent); }
TEST_F(PlayerBotEquipmentTest, ChargedOrTemporaryStateIsRepresented) { auto candidate=item(1,BotEquipmentSlot::Ring);candidate.countOrCharges=5;candidate.temporary=true;candidate.durationMilliseconds=1000;EXPECT_TRUE(candidate.temporary);EXPECT_EQ(5U,candidate.countOrCharges); }
TEST_F(PlayerBotEquipmentTest, UnknownPriceRemainsExplicit) { auto candidate=item(1,BotEquipmentSlot::Armor);EXPECT_FALSE(candidate.knownNpcBuyPrice);EXPECT_EQ(BotItemValueSource::Unknown,candidate.priceSource); }
TEST_F(PlayerBotEquipmentTest, NpcPriceIsNotMarketPrice) { auto candidate=item(1,BotEquipmentSlot::Armor);candidate.knownNpcSellPrice=50;candidate.priceSource=BotItemValueSource::NpcObservation;EXPECT_EQ(BotItemValueSource::NpcObservation,candidate.priceSource); }
TEST_F(PlayerBotEquipmentTest, SupplyItemIsNotMarkedForSale) { auto candidate=item(1,BotEquipmentSlot::Armor);candidate.category=BotItemCategory::Supply;candidate.supply=true;EXPECT_EQ(BotEquipmentIntent::KeepForSupply,BotEquipment::compare(nullptr,candidate,observation).intent); }
TEST_F(PlayerBotEquipmentTest, CurrentEquipmentReceivesRetentionHysteresis) { auto current=item(1,BotEquipmentSlot::Armor);current.equipped=true;uint32_t operations=0;EXPECT_GT(BotEquipment::score(current,policy,operations).retention,0); }
TEST_F(PlayerBotEquipmentTest, MarginalUpgradeBelowThresholdIsRejected) { auto old=item(1,BotEquipmentSlot::Armor,0,0,5),next=item(2,BotEquipmentSlot::Armor,0,0,6);policy.upgradeThreshold=100;EXPECT_EQ(BotEquipmentIntent::DowngradeRejected,BotEquipment::compare(&old,next,observation,policy).intent); }
TEST_F(PlayerBotEquipmentTest, MaterialUpgradeExceedsThreshold) { auto old=item(1,BotEquipmentSlot::Armor,0,0,1),next=item(2,BotEquipmentSlot::Armor,0,0,10);EXPECT_EQ(BotEquipmentIntent::UpgradeAvailable,BotEquipment::compare(&old,next,observation).intent); }
TEST_F(PlayerBotEquipmentTest, TieBreakingIsDeterministic) { auto a=item(9,BotEquipmentSlot::Armor,0,0,10),b=item(3,BotEquipmentSlot::Armor,0,0,10);observation.items={a,b};auto result=BotEquipment::upgrades(observation);ASSERT_EQ(2U,result.size());EXPECT_EQ(3U,result.front().itemTypeId); }
TEST_F(PlayerBotEquipmentTest, ContainerTraversalBudgetIsRepresented) { observation.containerBudgetExceeded=true;EXPECT_TRUE(observation.containerBudgetExceeded); }
TEST_F(PlayerBotEquipmentTest, ItemBudgetIsRepresented) { observation.itemBudgetExceeded=true;EXPECT_TRUE(observation.itemBudgetExceeded); }
TEST_F(PlayerBotEquipmentTest, ArithmeticCannotOverflow) { auto candidate=item(1,BotEquipmentSlot::Armor,INT32_MAX,INT32_MAX,INT32_MAX);candidate.skillModifiers={INT32_MAX};uint32_t operations=0;EXPECT_LE(BotEquipment::score(candidate,policy,operations).total,1000000); }
TEST_F(PlayerBotEquipmentTest, IdenticalObservationsYieldIdenticalResults) { auto candidate=item(1,BotEquipmentSlot::Armor,0,0,5);EXPECT_EQ(BotEquipment::compare(nullptr,candidate,observation),BotEquipment::compare(nullptr,candidate,observation)); }
TEST_F(PlayerBotEquipmentTest, EvaluationDoesNotMutateObservation) { auto candidate=item(1,BotEquipmentSlot::Armor,0,0,5),before=candidate;(void)BotEquipment::compare(nullptr,candidate,observation);EXPECT_EQ(before,candidate); }
TEST_F(PlayerBotEquipmentTest, InvalidLifecycleIsEmptyObservation) { EXPECT_EQ(0U,BotEquipment::observe(nullptr).revision); }
TEST_F(PlayerBotEquipmentTest, ScoreOperationBudgetIsEnforced) { auto candidate=item(1,BotEquipmentSlot::Armor,1,1,1);policy.maximumScoreOperations=1;EXPECT_EQ(BotValuationFailure::ScoreBudgetExceeded,BotEquipment::compare(nullptr,candidate,observation,policy).failure); }

namespace {
BotShopObservation shopObservation() {
	BotShopOffer offer { .itemTypeId=7618,.subType=0,.buyPrice=50,.sellPrice=20,.unitWeight=100 };
	offer.signature=BotShop::offerSignature(offer);
	return { .npcId=44,.revision=offer.signature,.offers={offer},.money={.carried=1000,.total=1000},.freeCapacity=1000,.open=true,.npcVisible=true };
}
BotEquipmentObservation saleInventory(bool equipped=false) {
	return { .revision=1,.items={{.itemTypeId=7618,.countOrCharges=20,.equipped=equipped}} };
}
}

TEST(PlayerBotShopTest, ObservationContainsValuesOnly) { EXPECT_FALSE(shopObservation().containsWorldOwnership); }
TEST(PlayerBotShopTest, ContractsRetainNoNpcOrItemOwnership) { EXPECT_FALSE(BotShopProgress{}.containsWorldOwnership);EXPECT_TRUE(std::is_trivially_destructible_v<BotShopOffer>); }
TEST(PlayerBotShopTest, HiddenOffersCannotBeUsed) { EXPECT_EQ(BotShopFailure::HiddenOffer,BotShop::planBuy(shopObservation(),999,0,1,50,0).failure); }
TEST(PlayerBotShopTest, StalePriceIsRejected) { EXPECT_EQ(BotShopOutcome::PriceChanged,BotShop::planBuy(shopObservation(),7618,0,1,51,0).outcome); }
TEST(PlayerBotShopTest, StaleAmountIsRejected) { EXPECT_EQ(BotShopFailure::AmountChanged,BotShop::planBuy(shopObservation(),7618,0,0,50,0).failure); }
TEST(PlayerBotShopTest, InsufficientMoneyIsExplicit) { auto o=shopObservation();o.money.total=49;EXPECT_EQ(BotShopOutcome::InsufficientMoney,BotShop::planBuy(o,7618,0,1,50,0).outcome); }
TEST(PlayerBotShopTest, InsufficientCapacityIsExplicit) { auto o=shopObservation();o.freeCapacity=0;EXPECT_EQ(BotShopOutcome::CapacityInsufficient,BotShop::planBuy(o,7618,0,1,50,0).outcome); }
TEST(PlayerBotShopTest, PurchaseAmountIsBounded) { BotShopPolicy p;p.maximumAmount=7;EXPECT_EQ(7,BotShop::planBuy(shopObservation(),7618,0,99,50,0,p).request.amount); }
TEST(PlayerBotShopTest, SupplyMaximumIsEnforced) { BotShopPolicy p;p.maximumSupplyCount=25;EXPECT_EQ(5,BotShop::planBuy(shopObservation(),7618,0,20,50,20,p).request.amount); }
TEST(PlayerBotShopTest, ReservedItemIsNotSold) { BotShopPolicy p;p.reservedItemTypeIds={7618};EXPECT_EQ(BotShopOutcome::ReservedItem,BotShop::planSell(shopObservation(),saleInventory(),7618,0,1,20,p).outcome); }
TEST(PlayerBotShopTest, EquippedItemIsNotSold) { EXPECT_EQ(BotShopFailure::EquippedItem,BotShop::planSell(shopObservation(),saleInventory(true),7618,0,1,20).failure); }
TEST(PlayerBotShopTest, BuyPlanningIsDeterministic) { EXPECT_EQ(BotShop::planBuy(shopObservation(),7618,0,2,50,0),BotShop::planBuy(shopObservation(),7618,0,2,50,0)); }
TEST(PlayerBotShopTest, SalePlanningIsDeterministic) { EXPECT_EQ(BotShop::planSell(shopObservation(),saleInventory(),7618,0,2,20),BotShop::planSell(shopObservation(),saleInventory(),7618,0,2,20)); }
TEST(PlayerBotShopTest, PendingTransactionSuppressesReplacement) { BotShopProgress p{.state=BotShopState::TransactionPending,.request=BotShopTransactionRequest{.itemTypeId=7618}};EXPECT_TRUE(p.request); }
TEST(PlayerBotShopTest, AcceptanceRequiresObservedResult) { EXPECT_NE(BotShopOutcome::Pending,BotShopOutcome::Succeeded); }
TEST(PlayerBotShopTest, NoEffectIsDistinct) { EXPECT_NE(BotShopOutcome::NoEffect,BotShopOutcome::Succeeded); }
TEST(PlayerBotShopTest, PartialResultReconcilesObservedAmount) { BotShopTransactionResult r{.outcome=BotShopOutcome::Partial,.itemsBefore=5,.itemsAfter=7,.reconciledAmount=2};EXPECT_EQ(r.itemsAfter-r.itemsBefore,r.reconciledAmount); }
TEST(PlayerBotShopTest, RetriesAndBackoffAreFinite) { BotShopPolicy p;EXPECT_EQ(3,p.maximumRetries);EXPECT_EQ(p.maximumBackoff,BotShop::backoff(p,20)); }
TEST(PlayerBotShopTest, ShopCloseCanCancelObsoleteTransaction) { EXPECT_NE(BotShopOutcome::ShopClosed,BotShopOutcome::Pending); }
TEST(PlayerBotShopTest, InvalidLifecycleIsExplicit) { EXPECT_EQ(BotShopFailure::InvalidLifecycle,BotShopFailure::InvalidLifecycle); }
TEST(PlayerBotShopTest, CancellationIsTerminal) { EXPECT_EQ(BotShopState::Cancelled,BotShopState::Cancelled); }
TEST(PlayerBotShopTest, MoneyChangesOnlyAppearInObservedResult) { BotShopTransactionResult r{.moneyBefore={.total=100},.moneyAfter={.total=50}};EXPECT_EQ(50,r.moneyAfter.total); }
TEST(PlayerBotShopTest, ItemChangesOnlyAppearInObservedResult) { BotShopTransactionResult r{.itemsBefore=1,.itemsAfter=2};EXPECT_EQ(1,r.itemsAfter-r.itemsBefore); }
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
TEST(PlayerBotLootTransferTest, CriticalHealingBlocksNewTransfer) { BotLootExecutionProgress p; BotLootTransfer::requestSurvivalInterrupt(p,BotSurvivalDecision::Heal); EXPECT_FALSE(BotLootTransfer::mayDispatch(p)); EXPECT_EQ(BotLootPriorityState::HealingPriority,p.priority); }
TEST(PlayerBotLootTransferTest, CriticalSurvivalInterruptsActiveTransfer) { BotLootExecutionProgress p{.state=BotLootExecutionState::SelectingItem}; BotLootTransfer::requestSurvivalInterrupt(p,BotSurvivalDecision::Heal); EXPECT_EQ(BotLootExecutionState::Cancelled,p.state); }
TEST(PlayerBotLootTransferTest, PendingTransferWaitsForAuthority) { BotLootExecutionProgress p{.state=BotLootExecutionState::TransferPending,.request=BotLootTransferRequest{.itemTypeId=3031,.count=5}}; BotLootTransfer::requestSurvivalInterrupt(p,BotSurvivalDecision::Heal); EXPECT_EQ(BotLootExecutionState::TransferPending,p.state); EXPECT_TRUE(p.request); EXPECT_EQ(BotLootPriorityState::WaitingForAuthoritativeBoundary,p.priority); }
TEST(PlayerBotLootTransferTest, CompletedBoundaryCanPrecedeHealing) { BotLootExecutionProgress p{.state=BotLootExecutionState::VerifyingTransfer}; BotLootTransfer::requestSurvivalInterrupt(p,BotSurvivalDecision::Heal); EXPECT_EQ(BotLootPriorityState::WaitingForAuthoritativeBoundary,p.priority); }
TEST(PlayerBotLootTransferTest, FailedBoundaryCanPrecedeHealing) { BotLootExecutionProgress p{.state=BotLootExecutionState::TransferPending}; BotLootTransfer::requestSurvivalInterrupt(p,BotSurvivalDecision::Heal); EXPECT_TRUE(p.freshCorpseRequired); EXPECT_TRUE(p.freshInventoryRequired); }
TEST(PlayerBotLootTransferTest, FleeHasPriorityWithoutHeal) { BotLootExecutionProgress p; BotLootTransfer::requestSurvivalInterrupt(p,BotSurvivalDecision::Flee); EXPECT_EQ(BotLootPriorityState::FleePriority,p.priority); }
TEST(PlayerBotLootTransferTest, DeathOverridesPendingTransfer) { BotLootExecutionProgress p{.state=BotLootExecutionState::TransferPending,.request=BotLootTransferRequest{.itemTypeId=3031}}; BotLootTransfer::requestSurvivalInterrupt(p,BotSurvivalDecision::Dead,true); EXPECT_EQ(BotLootPriorityState::DeathOverride,p.priority); EXPECT_FALSE(p.request); }
TEST(PlayerBotLootTransferTest, InterruptClearsCorpseFreshness) { BotLootExecutionProgress p{.corpseObservationRevision=7}; BotLootTransfer::requestSurvivalInterrupt(p,BotSurvivalDecision::Heal); EXPECT_TRUE(p.freshCorpseRequired); }
TEST(PlayerBotLootTransferTest, InterruptClearsInventoryFreshness) { BotLootExecutionProgress p{.inventoryObservationRevision=8}; BotLootTransfer::requestSurvivalInterrupt(p,BotSurvivalDecision::Heal); EXPECT_TRUE(p.freshInventoryRequired); }
TEST(PlayerBotLootTransferTest, RecoveryRequiresFreshObservation) { BotLootExecutionProgress p{.priority=BotLootPriorityState::HealingPriority,.corpseObservationRevision=7,.inventoryObservationRevision=8,.freshCorpseRequired=true,.freshInventoryRequired=true}; EXPECT_FALSE(BotLootTransfer::observeFresh(p,7,8,true)); EXPECT_EQ(BotLootPriorityState::FreshObservationRequired,p.priority); }
TEST(PlayerBotLootTransferTest, StableRecoveryAllowsEligibleLoot) { BotLootExecutionProgress p{.priority=BotLootPriorityState::HealingPriority,.corpseObservationRevision=7,.inventoryObservationRevision=8,.freshCorpseRequired=true,.freshInventoryRequired=true}; EXPECT_TRUE(BotLootTransfer::observeFresh(p,9,10,true)); EXPECT_TRUE(BotLootTransfer::mayDispatch(p)); }
TEST(PlayerBotLootTransferTest, DisappearedCorpsePreventsResume) { BotLootExecutionProgress p{.freshCorpseRequired=true,.freshInventoryRequired=true}; EXPECT_FALSE(BotLootTransfer::observeFresh(p,9,10,false)); EXPECT_EQ(BotLootPriorityState::LootAbandoned,p.priority); }
TEST(PlayerBotLootTransferTest, HealingAndTransferCannotBeSimultaneous) { BotLootExecutionProgress p; BotLootTransfer::requestSurvivalInterrupt(p,BotSurvivalDecision::Heal); EXPECT_FALSE(BotLootTransfer::mayDispatch(p)); }
TEST(PlayerBotLootTransferTest, FleeAndTransferCannotBeSimultaneous) { BotLootExecutionProgress p; BotLootTransfer::requestSurvivalInterrupt(p,BotSurvivalDecision::Flee); EXPECT_FALSE(BotLootTransfer::mayDispatch(p)); }
TEST(PlayerBotLootTransferTest, SessionCloseStateClearsSuspendedRequest) { BotLootExecutionProgress p{.state=BotLootExecutionState::TransferPending,.request=BotLootTransferRequest{.itemTypeId=3031}}; p={.state=BotLootExecutionState::Cancelled}; EXPECT_FALSE(p.request); EXPECT_FALSE(p.containsWorldOwnership); }
TEST(PlayerBotLootTransferTest, SurvivalPriorityStateContainsValuesOnly) { BotLootExecutionProgress p{.priority=BotLootPriorityState::FreshObservationRequired,.corpseObservationRevision=4,.inventoryObservationRevision=5}; EXPECT_FALSE(p.containsWorldOwnership); }
TEST(PlayerBotLootTransferTest, InterruptionRetainsNoWorldOwnership) { BotLootExecutionProgress p{.state=BotLootExecutionState::TransferPending}; BotLootTransfer::requestSurvivalInterrupt(p,BotSurvivalDecision::Heal); EXPECT_FALSE(p.containsWorldOwnership); }
TEST(PlayerBotLootTransferTest, ExistingCompatibleNestedStackIsPreferred) { BotInventoryObservation o{.containers={{.rootSlot=3,.childIndices={0},.capacity=20,.size=2,.depth=1,.items={{.itemTypeId=3031,.count=40,.stackPosition=1,.stackable=true}}}}};const auto r=BotLootTransfer::selectDestination(o,3031,30);EXPECT_EQ(BotDestinationOutcome::SelectedMerge,r.outcome);EXPECT_EQ(std::vector<uint16_t>({0}),r.childIndices);EXPECT_EQ(30U,r.mergeCount); }
TEST(PlayerBotLootTransferTest, NestedTraversalOrderingIsDeterministic) { BotInventoryObservation o{.containers={{.rootSlot=3,.capacity=0,.size=0},{.rootSlot=3,.childIndices={1},.capacity=20,.size=0,.depth=1}}};EXPECT_EQ(std::vector<uint16_t>({1}),BotLootTransfer::selectDestination(o,3031,1).childIndices); }
TEST(PlayerBotLootTransferTest, NestingDepthBoundIsRepresented) { BotLootTransferPolicy p;EXPECT_EQ(2,p.maxInventoryDepth); }
TEST(PlayerBotLootTransferTest, ContainerCountBoundIsExplicit) { BotLootTransferPolicy p;EXPECT_EQ(16,p.maxInventoryContainers); }
TEST(PlayerBotLootTransferTest, FullFirstContainerChoosesAlternate) { BotInventoryObservation o{.containers={{.rootSlot=3,.capacity=1,.size=1},{.rootSlot=4,.capacity=2,.size=0}}};EXPECT_EQ(4,BotLootTransfer::selectDestination(o,3031,1).rootSlot); }
TEST(PlayerBotLootTransferTest, AllContainersFullIsExplicit) { BotInventoryObservation o{.containers={{.rootSlot=3,.capacity=1,.size=1}}};EXPECT_EQ(BotDestinationOutcome::AllDestinationsFull,BotLootTransfer::selectDestination(o,3031,1).outcome); }
TEST(PlayerBotLootTransferTest, DestinationBudgetExceededIsExplicit) { BotInventoryObservation o{.containerBudgetExceeded=true};EXPECT_EQ(BotDestinationOutcome::DestinationBudgetExceeded,BotLootTransfer::selectDestination(o,3031,1).outcome); }
TEST(PlayerBotLootTransferTest, CapacityInsufficientRemainsDistinct) { EXPECT_NE(BotDestinationOutcome::CapacityInsufficient,BotDestinationOutcome::AllDestinationsFull); }
TEST(PlayerBotLootTransferTest, PartialRequestedCountIsValueOnly) { BotLootTransferRequest r{.itemTypeId=3031,.count=7,.destinationRootSlot=3,.destinationChildIndices={0}};EXPECT_EQ(7U,r.count); }
TEST(PlayerBotLootTransferTest, PartialResultReconcilesBothSides) { BotLootTransferResult r{.outcome=BotLootTransferOutcome::Partial,.sourceBefore=20,.sourceAfter=13,.destinationBefore=4,.destinationAfter=11,.movedCount=7};EXPECT_EQ(r.sourceBefore-r.sourceAfter,r.movedCount);EXPECT_EQ(r.destinationAfter-r.destinationBefore,r.movedCount); }
TEST(PlayerBotLootTransferTest, MergedStackReconciliationIsExplicit) { BotLootTransferResult r{.mergedStack=true};EXPECT_TRUE(r.mergedStack);EXPECT_FALSE(r.createdStack); }
TEST(PlayerBotLootTransferTest, NewStackReconciliationIsExplicit) { BotLootTransferResult r{.createdStack=true};EXPECT_TRUE(r.createdStack);EXPECT_FALSE(r.mergedStack); }
TEST(PlayerBotLootTransferTest, NoEffectResultIsDistinct) { EXPECT_NE(BotLootTransferOutcome::NoEffect,BotLootTransferOutcome::Partial); }
TEST(PlayerBotLootTransferTest, StaleDestinationIsDistinct) { EXPECT_NE(BotLootTransferOutcome::StaleDestination,BotLootTransferOutcome::DestinationFull); }
TEST(PlayerBotLootTransferTest, CorpseExpirationIsDistinct) { EXPECT_NE(BotLootTransferOutcome::CorpseExpired,BotLootTransferOutcome::StaleItem); }
TEST(PlayerBotLootTransferTest, RetryRemainsFinite) { BotLootTransferPolicy p;EXPECT_GT(p.maxAttempts,0);EXPECT_LT(p.maxAttempts,4); }
TEST(PlayerBotLootTransferTest, DestinationPathsRetainNoContainerOwnership) { BotDestinationSelection r{.rootSlot=3,.childIndices={0,1}};EXPECT_EQ(2U,r.childIndices.size()); }

namespace {
BotSupplyPolicy supplyPolicy() {
	return {
		.rules = { { 7618, BotSupplyCategory::HealthHealing }, { 3447, BotSupplyCategory::Ammunition }, { 268, BotSupplyCategory::Food } },
		.thresholds = { { BotSupplyCategory::HealthHealing, 5, 0, true }, { BotSupplyCategory::Ammunition, 20, 0, false } },
	};
}
BotSupplyObservation supplyObservation() {
	return { .revision=1,.inventorySignature=77,.freeCapacity=5000,.entries={
		{.itemTypeId=7618,.category=BotSupplyCategory::HealthHealing,.count=3,.depth=0,.signature=1},
		{.itemTypeId=3447,.category=BotSupplyCategory::Ammunition,.count=10,.depth=1,.signature=2},
		{.itemTypeId=268,.category=BotSupplyCategory::Food,.count=2,.charges=7,.depth=2,.signature=3},
	}};
}
}

TEST(PlayerBotSupplyTest, ObservationAndAssessmentContainValuesOnly) { EXPECT_FALSE(supplyObservation().containsWorldOwnership); EXPECT_TRUE(std::is_trivially_destructible_v<BotSupplyEntry>); }
TEST(PlayerBotSupplyTest, CategoryCountingIsDeterministic) { const auto r=BotSupply::assess(supplyObservation(),supplyPolicy());ASSERT_EQ(3U,r.totals.size());EXPECT_EQ(std::pair(BotSupplyCategory::HealthHealing,3U),r.totals[0]); }
TEST(PlayerBotSupplyTest, NestedContainerDepthIsBoundedValueData) { const auto o=supplyObservation();EXPECT_EQ(2,o.entries.back().depth); }
TEST(PlayerBotSupplyTest, ChargesTakePrecedenceOverStackCount) { const auto r=BotSupply::assess(supplyObservation(),supplyPolicy());EXPECT_EQ(std::pair(BotSupplyCategory::Food,7U),r.totals[2]); }
TEST(PlayerBotSupplyTest, HealingDepletionRequiresReturn) { auto o=supplyObservation();o.entries[0].count=0;const auto r=BotSupply::assess(o,supplyPolicy());EXPECT_EQ(BotSupplyIntent::NoHealingSupplies,r.intent); }
TEST(PlayerBotSupplyTest, RequiredAmmunitionDepletionStopsHunt) { auto o=supplyObservation();o.entries.erase(o.entries.begin()+1);o.ammunitionRequired=true;EXPECT_EQ(BotSupplyIntent::NoAmmunition,BotSupply::assess(o,supplyPolicy()).intent); }
TEST(PlayerBotSupplyTest, CapacityThresholdIsAuthoritative) { auto o=supplyObservation();o.freeCapacity=50;EXPECT_EQ(BotSupplyIntent::CapacityFull,BotSupply::assess(o,supplyPolicy()).intent); }
TEST(PlayerBotSupplyTest, ConservationThresholdIsDeterministic) { EXPECT_EQ(BotSupplyIntent::Conserve,BotSupply::assess(supplyObservation(),supplyPolicy()).intent); }
TEST(PlayerBotSupplyTest, StopThresholdIsConfigurable) { auto p=supplyPolicy();p.thresholds[0].stopBelow=3;EXPECT_EQ(BotSupplyIntent::NoHealingSupplies,BotSupply::assess(supplyObservation(),p).intent); }
TEST(PlayerBotSupplyTest, StaleInventoryIsRejected) { EXPECT_EQ(BotSupplyFailure::ObservationStale,BotSupply::assess(supplyObservation(),supplyPolicy(),78).failure); }
TEST(PlayerBotSupplyTest, IdenticalInputsProduceIdenticalDecisionValues) { const auto a=BotSupply::assess(supplyObservation(),supplyPolicy());const auto b=BotSupply::assess(supplyObservation(),supplyPolicy());EXPECT_EQ(a.intent,b.intent);EXPECT_EQ(a.reasonScore,b.reasonScore);EXPECT_EQ(a.totals,b.totals); }
TEST(PlayerBotSupplyTest, TotalsSaturateWithoutOverflow) { auto o=supplyObservation();o.entries={{.itemTypeId=1,.category=BotSupplyCategory::HealthHealing,.count=UINT32_MAX},{.itemTypeId=2,.category=BotSupplyCategory::HealthHealing,.count=UINT32_MAX}};const auto r=BotSupply::assess(o,{});ASSERT_EQ(1U,r.totals.size());EXPECT_EQ(UINT32_MAX,r.totals[0].second); }
TEST(PlayerBotSupplyTest, AssessmentDoesNotMutateObservation) { const auto o=supplyObservation();const auto before=o;(void)BotSupply::assess(o,supplyPolicy());EXPECT_EQ(before,o); }

namespace {
BotAdventurePolicy adventurePolicy() { return {.startRegion={{90,90,7},1},.huntRegion={{100,100,7},1},.returnRegion={{90,90,7},1},.maximumDuration=std::chrono::seconds(60),.maximumCombatCount=2,.maximumRepeatedFailures=2,.minimumHealthPercent=40,.targetLevel=20}; }
BotAdventureObservation adventureObservation(uint64_t revision=1) { return {.revision=revision,.position={100,100,7},.level=10,.experience=100,.healthPercent=100}; }
}

TEST(PlayerBotAdventureTest, PolicyObservationAndProgressContainValuesOnly) { const auto o=adventureObservation();const BotAdventureProgress p;EXPECT_FALSE(o.containsWorldOwnership);EXPECT_FALSE(p.containsWorldOwnership);EXPECT_EQ(Position(100,100,7),adventurePolicy().huntRegion.center); }
TEST(PlayerBotAdventureTest, LegalTransitionsEncodeCombatLootSupplyOrder) { EXPECT_TRUE(BotAdventure::legalTransition(BotAdventureState::Engaging,BotAdventureState::Fighting));EXPECT_TRUE(BotAdventure::legalTransition(BotAdventureState::Fighting,BotAdventureState::Looting));EXPECT_TRUE(BotAdventure::legalTransition(BotAdventureState::Looting,BotAdventureState::EvaluatingSupplies));EXPECT_FALSE(BotAdventure::legalTransition(BotAdventureState::Engaging,BotAdventureState::Looting)); }
TEST(PlayerBotAdventureTest, SurvivalOverridesActiveCombat) { BotAdventureProgress p{.state=BotAdventureState::Fighting};auto o=adventureObservation();o.survivalUrgency=BotSurvivalUrgency::Critical;const auto r=BotAdventure::advance(p,o,std::chrono::milliseconds(1),adventurePolicy());EXPECT_EQ(BotAdventureState::Recovering,r.state); }
TEST(PlayerBotAdventureTest, DeathOverridesActiveLoop) { BotAdventureProgress p{.state=BotAdventureState::Looting};auto o=adventureObservation();o.dead=true;EXPECT_EQ(BotAdventureState::Dead,BotAdventure::advance(p,o,std::chrono::milliseconds(1),adventurePolicy()).state); }
TEST(PlayerBotAdventureTest, SupplyStopOverridesNewEngagement) { BotAdventureProgress p{.state=BotAdventureState::Searching};auto o=adventureObservation();o.visibleTargetId=9;o.supplyIntent=BotSupplyIntent::NoHealingSupplies;EXPECT_EQ(BotAdventureState::Returning,BotAdventure::advance(p,o,std::chrono::milliseconds(1),adventurePolicy()).state); }
TEST(PlayerBotAdventureTest, ConfiguredReserveStopsNewEngagement) { BotAdventureProgress p{.state=BotAdventureState::Searching};auto o=adventureObservation();o.healingReserve=0;EXPECT_EQ(BotAdventureState::Returning,BotAdventure::advance(p,o,std::chrono::milliseconds(1),adventurePolicy()).state); }
TEST(PlayerBotAdventureTest, StaleObservationRequestsFreshValues) { BotAdventureProgress p{.state=BotAdventureState::Searching,.lastObservationRevision=5};const auto r=BotAdventure::advance(p,adventureObservation(5),std::chrono::milliseconds(1),adventurePolicy());EXPECT_EQ(BotAdventureIntent::Reobserve,r.intent);EXPECT_EQ(BotAdventureFailure::StaleObservation,r.failure); }
TEST(PlayerBotAdventureTest, RepeatedFailuresAreFinite) { BotAdventureProgress p{.state=BotAdventureState::Searching,.repeatedFailures=2};auto o=adventureObservation();o.actionFailed=true;EXPECT_EQ(BotAdventureState::Failed,BotAdventure::advance(p,o,std::chrono::milliseconds(1),adventurePolicy()).state); }
TEST(PlayerBotAdventureTest, AdventureDurationIsFinite) { BotAdventureProgress p{.state=BotAdventureState::Searching,.startedAt=std::chrono::milliseconds(1)};const auto r=BotAdventure::advance(p,adventureObservation(),std::chrono::seconds(62),adventurePolicy());EXPECT_EQ(BotAdventureFailure::DurationLimit,r.failure); }
TEST(PlayerBotAdventureTest, CombatDeathPrecedesLoot) { BotAdventureProgress p{.state=BotAdventureState::Fighting};auto o=adventureObservation();o.targetDefeated=true;o.corpseAvailable=true;const auto r=BotAdventure::advance(p,o,std::chrono::milliseconds(1),adventurePolicy());EXPECT_EQ(BotAdventureState::Looting,r.state);EXPECT_EQ(1,r.combatCount); }
TEST(PlayerBotAdventureTest, LootCompletionPrecedesSupplyAssessment) { BotAdventureProgress p{.state=BotAdventureState::Looting};auto o=adventureObservation();o.lootComplete=true;EXPECT_EQ(BotAdventureState::EvaluatingSupplies,BotAdventure::advance(p,o,std::chrono::milliseconds(1),adventurePolicy()).state); }
TEST(PlayerBotAdventureTest, DepletionProducesReturnIntent) { BotAdventureProgress p{.state=BotAdventureState::EvaluatingSupplies};auto o=adventureObservation();o.supplyIntent=BotSupplyIntent::NoAmmunition;const auto r=BotAdventure::advance(p,o,std::chrono::milliseconds(1),adventurePolicy());EXPECT_EQ(BotAdventureIntent::Return,r.intent); }
TEST(PlayerBotAdventureTest, TargetLevelCompletesAdventure) { BotAdventureProgress p{.state=BotAdventureState::Searching};auto o=adventureObservation();o.level=20;EXPECT_EQ(BotAdventureState::Completed,BotAdventure::advance(p,o,std::chrono::milliseconds(1),adventurePolicy()).state); }
TEST(PlayerBotAdventureTest, IdenticalInputsProduceIdenticalProgress) { BotAdventureProgress p{.state=BotAdventureState::Searching};const auto a=BotAdventure::advance(p,adventureObservation(),std::chrono::milliseconds(1),adventurePolicy());const auto b=BotAdventure::advance(p,adventureObservation(),std::chrono::milliseconds(1),adventurePolicy());EXPECT_EQ(a,b); }
TEST(PlayerBotAdventureTest, CancellationIsTerminal) { auto p=BotAdventure::cancel({.state=BotAdventureState::Fighting});EXPECT_EQ(BotAdventureState::Cancelled,p.state);EXPECT_EQ(p,BotAdventure::advance(p,adventureObservation(),std::chrono::milliseconds(1),adventurePolicy())); }
TEST(PlayerBotAdventureTest, DynamicBlockerRetriesAreBounded) { BotAdventureProgress p{.state=BotAdventureState::TravelingToArea,.repeatedFailures=2};auto o=adventureObservation();o.position={50,50,7};o.dynamicBlocker=true;EXPECT_EQ(BotAdventureFailure::RepeatedFailure,BotAdventure::advance(p,o,std::chrono::milliseconds(1),adventurePolicy()).failure); }
namespace { BotCampaignPolicy campaignPolicy(){return {.allowedRegions={{"near",{{100,100,7},2},16},{"far",{{110,100,7},2},24}},.targetKillCount=10,.maximumKillAttempts=14,.maximumCombatFailures=2,.maximumRecoveryAttempts=2};} }
TEST(PlayerBotCampaignTest, KillCountNeedsUniqueAuthoritativeDeathEvidence) { auto p=BotCampaign::begin(campaignPolicy());p=BotCampaign::recordAuthoritativeKill(p,7,campaignPolicy());p=BotCampaign::recordAuthoritativeKill(p,7,campaignPolicy());EXPECT_EQ(1,p.authoritativeKills); }
TEST(PlayerBotCampaignTest, TargetKillCountIsTen) { EXPECT_EQ(10,campaignPolicy().targetKillCount); }
TEST(PlayerBotCampaignTest, MaximumAttemptsAreFinite) { auto p=BotCampaign::begin(campaignPolicy());for(int i=0;i<15;++i)p=BotCampaign::recordAttempt(p,campaignPolicy());EXPECT_EQ(BotCampaignState::Failed,p.state); }
TEST(PlayerBotCampaignTest, MultiCycleOrderingIsExplicit) { auto p=BotCampaign::begin(campaignPolicy());p=BotCampaign::recordAttempt(p,campaignPolicy());EXPECT_EQ(BotCampaignState::Combat,p.state);p=BotCampaign::recordAuthoritativeKill(p,1,campaignPolicy());EXPECT_EQ(BotCampaignState::Loot,p.state); }
TEST(PlayerBotCampaignTest, DeathInterruptsCampaign) { auto p=BotCampaign::recordDeath(BotCampaign::begin(campaignPolicy()),9,campaignPolicy());EXPECT_EQ(BotCampaignState::Recovering,p.state); }
TEST(PlayerBotCampaignTest, RecoveryResetsTransientObservation) { auto p=BotCampaign::recordDeath(BotCampaign::begin(campaignPolicy()),9,campaignPolicy());EXPECT_TRUE(p.freshObservationRequired); }
TEST(PlayerBotCampaignTest, RecoveryRetainsConfiguredRegionIdOnly) { auto p=BotCampaign::begin(campaignPolicy());p=BotCampaign::recordDeath(p,9,campaignPolicy());EXPECT_EQ("near",p.selectedRegionId);EXPECT_FALSE(p.containsWorldOwnership); }
TEST(PlayerBotCampaignTest, SaveBoundaryRejectsPendingSubsystems) { EXPECT_FALSE(BotCampaign::safeSaveBoundary(true,false,false,false));EXPECT_TRUE(BotCampaign::safeSaveBoundary(false,false,false,false)); }
TEST(PlayerBotCampaignTest, LogoutCancellationClearsRuntimeIntent) { auto p=BotCampaign::cancel(BotCampaign::begin(campaignPolicy()));EXPECT_EQ(BotCampaignState::Cancelled,p.state);EXPECT_TRUE(p.freshObservationRequired); }
TEST(PlayerBotCampaignTest, LoadedSessionRequiresFreshObservation) { EXPECT_TRUE(BotCampaignProgress{}.freshObservationRequired); }
TEST(PlayerBotCampaignTest, PersistedProgressUsesAuthoritativeValues) { BotCampaignProgress p{.authoritativeKills=10};EXPECT_EQ(10,p.authoritativeKills); }
TEST(PlayerBotCampaignTest, ConfiguredRegionOrderingIsDeterministic) { const auto r=BotCampaign::selectRegion(campaignPolicy(),{"far","near"});ASSERT_TRUE(r);EXPECT_EQ("near",r->id); }
TEST(PlayerBotCampaignTest, UnknownRegionIsRejected) { EXPECT_FALSE(BotCampaign::selectRegion(campaignPolicy(),{"unknown"})); }
TEST(PlayerBotCampaignTest, UnreachableRegionsFailFinitely) { EXPECT_FALSE(BotCampaign::selectRegion(campaignPolicy(),{})); }
TEST(PlayerBotCampaignTest, NoHiddenAreaDiscoveryExists) { EXPECT_EQ(2U,campaignPolicy().allowedRegions.size()); }
TEST(PlayerBotCampaignTest, CampaignDurationIsBounded) { auto p=BotCampaign::begin(campaignPolicy(),std::chrono::milliseconds(1));p=BotCampaign::recordAttempt(p,campaignPolicy(),std::chrono::milliseconds(300001));EXPECT_EQ(BotCampaignFailure::DurationLimit,p.failure);EXPECT_EQ(BotCampaignState::Failed,p.state); }
TEST(PlayerBotCampaignTest, CancellationIsTerminal) { const auto p=BotCampaign::cancel(BotCampaign::begin(campaignPolicy()));EXPECT_EQ(p,BotCampaign::cancel(p)); }
TEST(PlayerBotCampaignTest, CampaignRetainsNoWorldOwnership) { EXPECT_FALSE(BotCampaignProgress{}.containsWorldOwnership); }

namespace {
BotDepotObservation depotObservation(){return {.depotId=7,.revision=19,.containers={{{1,{}},20,2,0}},.items={{7618,40,{1,{0}},11},{3447,100,{1,{1}},12}},.open=true};}
BotEquipmentObservation resupplyInventory(){return {.revision=17,.freeCapacity=5000,.items={{.itemTypeId=7618,.countOrCharges=2,.path={CONST_SLOT_BACKPACK,{0}}},{.itemTypeId=3031,.countOrCharges=8,.path={CONST_SLOT_BACKPACK,{1}}}}};}
BotResupplyPolicy resupplyPolicy(){return {.targets={{7618,5,10,20}},.depositItemTypeIds={3031},.maximumTransferCount=25,.maximumDepotContainers=4,.maximumNestingDepth=2,.maximumItemsObserved=32,.maximumRetries=3};}
}
TEST(PlayerBotResupplyTest, DepotObservationContainsValuesOnly){EXPECT_FALSE(depotObservation().containsWorldOwnership);}
TEST(PlayerBotResupplyTest, ProgressRetainsNoContainerOrItemOwnership){EXPECT_FALSE(BotResupplyProgress{}.containsWorldOwnership);}
TEST(PlayerBotResupplyTest, DepotTraversalBoundsAreExplicit){const auto p=resupplyPolicy();EXPECT_EQ(4,p.maximumDepotContainers);EXPECT_EQ(2,p.maximumNestingDepth);EXPECT_EQ(32,p.maximumItemsObserved);}
TEST(PlayerBotResupplyTest, OwnershipRejectionIsDistinct){EXPECT_NE(BotResupplyOutcome::DepotOwnershipRejected,BotResupplyOutcome::DepotUnavailable);}
TEST(PlayerBotResupplyTest, SourceStaleIsDistinct){EXPECT_NE(BotResupplyOutcome::SourceStale,BotResupplyOutcome::DestinationStale);}
TEST(PlayerBotResupplyTest, DestinationStaleIsDistinct){EXPECT_NE(BotResupplyOutcome::DestinationStale,BotResupplyOutcome::ContainerFull);}
TEST(PlayerBotResupplyTest, SupplyTargetCalculationIsDeterministic){const auto plan=BotResupply::plan(resupplyInventory(),depotObservation(),resupplyPolicy());ASSERT_EQ(2,plan.transfers.size());EXPECT_EQ(8,plan.transfers[1].count);}
TEST(PlayerBotResupplyTest, ReserveStockIsPreserved){auto p=resupplyPolicy();p.targets[0].reserve=38;const auto plan=BotResupply::plan(resupplyInventory(),depotObservation(),p);ASSERT_FALSE(plan.transfers.empty());EXPECT_EQ(2,plan.transfers.back().count);}
TEST(PlayerBotResupplyTest, PartialTransferReconcilesBothSides){BotResupplyResult r{.outcome=BotResupplyOutcome::Partial,.sourceBefore=20,.sourceAfter=13,.destinationBefore=2,.destinationAfter=9,.movedCount=7};EXPECT_EQ(r.sourceBefore-r.sourceAfter,r.movedCount);EXPECT_EQ(r.destinationAfter-r.destinationBefore,r.movedCount);}
TEST(PlayerBotResupplyTest, CapacityFailureIsExplicit){EXPECT_NE(BotResupplyOutcome::CapacityInsufficient,BotResupplyOutcome::ContainerFull);}
TEST(PlayerBotResupplyTest, FullDestinationIsExplicit){EXPECT_EQ(BotResupplyOutcome::ContainerFull,BotResupplyOutcome::ContainerFull);}
TEST(PlayerBotResupplyTest, EquipmentRequirementsAreRevalidated){BotEquipmentExecutionResult r{.outcome=BotResupplyOutcome::RequirementNotMet,.failure=BotEquipmentFailure::RequirementNotMet};EXPECT_EQ(BotEquipmentFailure::RequirementNotMet,r.failure);}
TEST(PlayerBotResupplyTest, SlotConflictIsExplicit){EXPECT_NE(BotEquipmentFailure::SlotConflict,BotEquipmentFailure::TwoHandedConflict);}
TEST(PlayerBotResupplyTest, TwoHandedConflictIsExplicit){EXPECT_EQ(BotResupplyOutcome::TwoHandedConflict,BotResupplyOutcome::TwoHandedConflict);}
TEST(PlayerBotResupplyTest, ReplacementDestinationUsesStablePath){BotEquipmentExecutionRequest r{.replacementDestination={CONST_SLOT_BACKPACK,{2,1}}};EXPECT_EQ(std::vector<uint16_t>({2,1}),r.replacementDestination.childIndices);}
TEST(PlayerBotResupplyTest, AcceptedEquipRequiresObservedResult){EXPECT_NE(BotResupplyOutcome::Pending,BotResupplyOutcome::Succeeded);}
TEST(PlayerBotResupplyTest, NoEffectIsExplicit){EXPECT_NE(BotResupplyOutcome::NoEffect,BotResupplyOutcome::Succeeded);}
TEST(PlayerBotResupplyTest, SameEquipmentIsNotRequestedByUpgradeContract){BotUpgradeCandidate c;EXPECT_EQ(BotEquipmentIntent::UnknownValue,c.comparison.intent);}
TEST(PlayerBotResupplyTest, TransferCountIsFinite){auto p=resupplyPolicy();p.maximumTransferCount=3;const auto plan=BotResupply::plan(resupplyInventory(),depotObservation(),p);for(const auto&r:plan.transfers)EXPECT_LE(r.count,3);}
TEST(PlayerBotResupplyTest, RetryBackoffIsFinite){const auto p=resupplyPolicy();EXPECT_EQ(std::chrono::milliseconds(800),BotResupply::backoff(p,2));EXPECT_EQ(p.maximumBackoff,BotResupply::backoff(p,99));}
TEST(PlayerBotResupplyTest, CloseClearsPendingState){BotResupplyProgress p{.state=BotResupplyState::TransferPending,.transfer=BotResupplyRequest{.itemTypeId=7618}};p={.state=BotResupplyState::Cancelled};EXPECT_FALSE(p.transfer);}
TEST(PlayerBotResupplyTest, ContractsProvideNoDirectMutationHandle){EXPECT_FALSE(BotResupplyRequest{}.containsWorldOwnership());EXPECT_FALSE(BotResupplyProgress{}.containsWorldOwnership);}

namespace{BotNpcDialoguePolicy dialoguePolicy(){return{.configuredNpcNames={"guide"},.phrases={{BotDialogueIntent::Greeting,"hi","welcome"},{BotDialogueIntent::Farewell,"bye","farewell"},{BotDialogueIntent::Yes,"yes","confirmed"},{BotDialogueIntent::No,"no","cancelled"},{BotDialogueIntent::Trade,"trade","offer"},{BotDialogueIntent::ConfiguredTopic,"job","guide"},{BotDialogueIntent::ConfiguredFollowUp,"help","help"}},.maximumPhrases=4,.maximumRetries=2,.maximumTopics=3,.maximumResponseLength=32};}BotDialogueObservation dialogueObservation(){return{.revision=3,.npcs={{7,"guide",{100,100,7},1,true,true,false,3},{9,"guide",{101,100,7},2,true,true,false,4}}};}}
TEST(PlayerBotDialogueTest,NpcObservationContainsValuesOnly){EXPECT_FALSE(dialogueObservation().npcs.front().containsWorldOwnership());}
TEST(PlayerBotDialogueTest,ProgressRetainsNoNpcOrLuaOwnership){EXPECT_FALSE(BotDialogueProgress{}.containsWorldOwnership);}
TEST(PlayerBotDialogueTest,ConfiguredSelectionIsDeterministic){EXPECT_EQ(7,BotDialogue::select(dialogueObservation(),dialoguePolicy())->npcId);}
TEST(PlayerBotDialogueTest,HiddenNpcIsRejected){auto o=dialogueObservation();o.npcs[0].visible=false;o.npcs[1].visible=false;EXPECT_FALSE(BotDialogue::select(o,dialoguePolicy()));}
TEST(PlayerBotDialogueTest,DifferentFloorNpcIsRejected){auto o=dialogueObservation();for(auto&n:o.npcs)n.sameFloor=false;EXPECT_FALSE(BotDialogue::select(o,dialoguePolicy()));}
TEST(PlayerBotDialogueTest,GreetingStateIsRepresented){EXPECT_EQ(BotDialogueState::GreetingPending,BotDialogueState::GreetingPending);}
TEST(PlayerBotDialogueTest,GreetingRequiresObservedResponse){EXPECT_NE(BotDialogueResponse::NoResponse,BotDialogueResponse::GreetingAccepted);}
TEST(PlayerBotDialogueTest,NoResponseCanReachTimeout){EXPECT_NE(BotDialogueResponse::NoResponse,BotDialogueResponse::TimedOut);}
TEST(PlayerBotDialogueTest,UnexpectedResponseIsExplicit){EXPECT_EQ(BotDialogueResponse::UnexpectedResponse,BotDialogueResponse::UnexpectedResponse);}
TEST(PlayerBotDialogueTest,PhraseCountIsBounded){EXPECT_EQ(4,dialoguePolicy().maximumPhrases);}
TEST(PlayerBotDialogueTest,TopicCountIsBounded){auto p=dialoguePolicy();p.maximumTopics=0;EXPECT_EQ(nullptr,BotDialogue::phrase(p,BotDialogueIntent::ConfiguredTopic));}
TEST(PlayerBotDialogueTest,RetryBackoffIsFinite){const auto p=dialoguePolicy();EXPECT_EQ(std::chrono::milliseconds(500),BotDialogue::backoff(p,1));EXPECT_EQ(p.maximumBackoff,BotDialogue::backoff(p,99));}
TEST(PlayerBotDialogueTest,SpeechSpamHasSingleAwaitingState){BotDialogueProgress p{.state=BotDialogueState::AwaitingResponse,.npcId=7,.intent=BotDialogueIntent::Greeting};EXPECT_EQ(BotDialogueState::AwaitingResponse,p.state);}
TEST(PlayerBotDialogueTest,FocusLossIsExplicit){EXPECT_EQ(BotDialogueResponse::FocusLost,BotDialogueResponse::FocusLost);}
TEST(PlayerBotDialogueTest,NpcDisappearanceIsExplicit){EXPECT_EQ(BotDialogueFailure::NpcUnavailable,BotDialogueFailure::NpcUnavailable);}
TEST(PlayerBotDialogueTest,MovementAwayIsExplicit){EXPECT_EQ(BotDialogueResponse::OutOfRange,BotDialogueResponse::OutOfRange);}
TEST(PlayerBotDialogueTest,FarewellPhraseIsConfigured){EXPECT_EQ("bye",BotDialogue::phrase(dialoguePolicy(),BotDialogueIntent::Farewell)->text);}
TEST(PlayerBotDialogueTest,CancellationIsTerminal){BotConversationResult r{.state=BotDialogueState::Cancelled,.response=BotDialogueResponse::Cancelled};EXPECT_EQ(BotDialogueState::Cancelled,r.state);}
TEST(PlayerBotDialogueTest,SessionCloseValueStateClears){BotDialogueProgress p{.state=BotDialogueState::AwaitingResponse,.npcId=7};p={.state=BotDialogueState::Cancelled};EXPECT_EQ(0,p.npcId);}
TEST(PlayerBotDialogueTest,ResponseBufferLengthIsBounded){EXPECT_EQ(32,dialoguePolicy().maximumResponseLength);}
TEST(PlayerBotDialogueTest,IdenticalObservationsProduceDeterministicIntent){EXPECT_EQ(BotDialogue::select(dialogueObservation(),dialoguePolicy()),BotDialogue::select(dialogueObservation(),dialoguePolicy()));}
TEST(PlayerBotDialogueTest,ContractsContainNoStorageOrRewardMutation){EXPECT_FALSE(BotDialogueObservation{}.containsWorldOwnership());EXPECT_EQ(nullptr,BotDialogue::phrase(dialoguePolicy(),BotDialogueIntent::Cancel));}

namespace {
BotMissionDefinition questMission(){return{.id=2,.visibleName="Trial",.visibleDescription="Visible mission",.stateStorage={900100,-1,10},.availableValue=-1,.startedValue=1,.objectiveCompleteValue=2,.completedValue=3,.turnInRequired=true,.prerequisites={{.type=BotQuestPrerequisiteType::MinimumLevel,.value=8},{.type=BotQuestPrerequisiteType::Vocation,.value=1},{.type=BotQuestPrerequisiteType::ItemPresent,.value=2,.itemTypeId=3031},{.type=BotQuestPrerequisiteType::MoneyAvailable,.value=100},{.type=BotQuestPrerequisiteType::StoragePredicate,.storage={900101,4,8}}},.objectives={{.type=BotQuestObjectiveType::KillCreatureType,.requiredCount=2,.creatureTypeId=77},{.type=BotQuestObjectiveType::VisitLocation,.requiredCount=1,.position={100,100,7},.radius=1}},.rewards={{BotQuestRewardType::ItemReceived,2160,1},{BotQuestRewardType::ExperienceIncreased,0,50},{BotQuestRewardType::MissionStateChanged,0,1}}};}
BotQuestDefinition questDefinition(){return{.id=1,.visibleName="Configured Trial",.revision=7,.missions={questMission()}};}
BotQuestObservation questObservation(){return{.questId=1,.visibleName="Configured Trial",.level=10,.vocationId=1,.money=200,.experience=1000,.position={100,100,7},.items={{3031,2},{2160,0}},.configuredStorages={{900100,1},{900101,5}},.missions={{.missionId=2,.state=BotMissionState::InProgress,.configuredStorageValue=1,.visibleName="Trial",.visibleDescription="Visible mission",.progress={{BotQuestObjectiveType::KillCreatureType,2,2,true},{BotQuestObjectiveType::VisitLocation,1,1,true}},.revision=8}},.revision=8,.definitionRevision=7};}
}
TEST(PlayerBotQuestTest,QuestObservationsContainValuesOnly){EXPECT_FALSE(questObservation().containsWorldOwnership());EXPECT_FALSE(questObservation().missions.front().containsWorldOwnership());}
TEST(PlayerBotQuestTest,ContractsRetainNoNpcItemOrLuaOwnership){EXPECT_FALSE(BotQuestRewardObservation{}.containsWorldOwnership());EXPECT_FALSE(BotQuestVerification{}.containsWorldOwnership());}
TEST(PlayerBotQuestTest,MissionStateNormalizationIsExplicit){const auto m=questMission();EXPECT_EQ(BotMissionState::Unavailable,BotQuest::normalize(m,-2,false));EXPECT_EQ(BotMissionState::Available,BotQuest::normalize(m,0,false));EXPECT_EQ(BotMissionState::Started,BotQuest::normalize(m,1,false));EXPECT_EQ(BotMissionState::TurnInRequired,BotQuest::normalize(m,2,true));EXPECT_EQ(BotMissionState::Completed,BotQuest::normalize(m,3,true));}
TEST(PlayerBotQuestTest,MinimumLevelPrerequisiteIsAuthoritative){auto o=questObservation();o.level=7;EXPECT_EQ(BotQuestFailure::LevelTooLow,BotQuest::evaluate(o,questMission()).result);}
TEST(PlayerBotQuestTest,VocationPrerequisiteIsAuthoritative){auto o=questObservation();o.vocationId=2;EXPECT_EQ(BotQuestFailure::WrongVocation,BotQuest::evaluate(o,questMission()).result);}
TEST(PlayerBotQuestTest,PreviousMissionPrerequisiteIsAuthoritative){auto m=questMission();m.prerequisites={{.type=BotQuestPrerequisiteType::PreviousMissionComplete,.missionId=9}};EXPECT_EQ(BotQuestFailure::PreviousMissionIncomplete,BotQuest::evaluate(questObservation(),m).result);}
TEST(PlayerBotQuestTest,ItemPrerequisiteUsesObservedCount){auto o=questObservation();o.items[0].second=1;EXPECT_EQ(BotQuestFailure::MissingItem,BotQuest::evaluate(o,questMission()).result);}
TEST(PlayerBotQuestTest,MoneyPrerequisiteUsesObservedValue){auto o=questObservation();o.money=99;EXPECT_EQ(BotQuestFailure::InsufficientMoney,BotQuest::evaluate(o,questMission()).result);}
TEST(PlayerBotQuestTest,ConfiguredStoragePredicateIsBounded){auto o=questObservation();o.configuredStorages[1].second=9;EXPECT_EQ(BotQuestFailure::StorageConditionNotMet,BotQuest::evaluate(o,questMission()).result);}
TEST(PlayerBotQuestTest,MissingNpcPrerequisiteIsExplicit){auto m=questMission();m.prerequisites={{.type=BotQuestPrerequisiteType::NpcAvailable,.configuredNpcName="guide"}};EXPECT_EQ(BotQuestFailure::NpcUnavailable,BotQuest::evaluate(questObservation(),m).result);}
TEST(PlayerBotQuestTest,ObjectiveIncompleteIsExplicit){auto m=questMission();m.prerequisites={{.type=BotQuestPrerequisiteType::ObjectiveCount}};auto o=questObservation();o.missions[0].progress[0].complete=false;EXPECT_EQ(BotQuestFailure::ObjectiveIncomplete,BotQuest::evaluate(o,m).result);}
TEST(PlayerBotQuestTest,CompletedObjectivesAreEligible){auto m=questMission();m.prerequisites={{.type=BotQuestPrerequisiteType::ObjectiveCount}};EXPECT_TRUE(BotQuest::evaluate(questObservation(),m).eligible());}
TEST(PlayerBotQuestTest,StaleObservationIsRejected){auto o=questObservation();o.missions.front().state=BotMissionState::Stale;EXPECT_EQ(BotQuestFailure::ObservationStale,BotQuest::evaluate(o,questMission()).result);}
TEST(PlayerBotQuestTest,PrerequisiteOrderingIsDeterministic){auto o=questObservation();o.level=1;o.vocationId=9;const auto r=BotQuest::evaluate(o,questMission());EXPECT_EQ(BotQuestFailure::LevelTooLow,r.result);EXPECT_EQ(0,r.prerequisiteIndex);}
TEST(PlayerBotQuestTest,PrerequisiteCountIsBounded){auto m=questMission();m.prerequisites.resize(4);BotQuestBounds b;b.maximumPrerequisites=2;EXPECT_EQ(BotQuestFailure::EvaluationBudgetExceeded,BotQuest::evaluate(questObservation(),m,{},b).result);}
TEST(PlayerBotQuestTest,ObjectiveCountIsBounded){BotQuestBounds b;b.maximumObjectives=1;EXPECT_EQ(1,b.maximumObjectives);}
TEST(PlayerBotQuestTest,KillProgressIsValueBased){BotQuestEvidence e{.type=BotQuestObjectiveType::KillCreatureType,.subjectId=77,.count=2};EXPECT_EQ(2,e.count);EXPECT_FALSE(questObservation().containsWorldOwnership());}
TEST(PlayerBotQuestTest,ItemProgressIsValueBased){BotQuestProgress p{BotQuestObjectiveType::CollectItem,3,3,true};EXPECT_TRUE(p.complete);}
TEST(PlayerBotQuestTest,LocationProgressIsValueBased){BotQuestEvidence e{.type=BotQuestObjectiveType::VisitLocation,.count=1,.position={100,100,7}};EXPECT_EQ(Position(100,100,7),e.position);}
TEST(PlayerBotQuestTest,DialogueProgressRequiresObservedResponse){BotQuestEvidence e{.type=BotQuestObjectiveType::TalkToNpc,.normalizedSubjectName="guide",.count=0,.tokenHash=BotQuest::dialogueTokenHash("welcome")};EXPECT_EQ(0,e.count);EXPECT_NE(0,e.tokenHash);}
TEST(PlayerBotQuestTest,RewardVerificationRequiresActualStateChange){auto before=BotQuest::rewardObservation(questObservation(),2);auto after=before;++after.questRevision;EXPECT_EQ(BotQuestVerificationResult::ExpectedItemMissing,BotQuest::verifyRewards(before,after,{{BotQuestRewardType::ItemReceived,2160,1}}).result);}
TEST(PlayerBotQuestTest,PartialRewardIsExplicit){auto before=BotQuest::rewardObservation(questObservation(),2),after=before;++after.questRevision;after.items[1].second=1;EXPECT_EQ(BotQuestVerificationResult::Partial,BotQuest::verifyRewards(before,after,{{BotQuestRewardType::ItemReceived,2160,1},{BotQuestRewardType::ExperienceIncreased,0,1}}).result);}
TEST(PlayerBotQuestTest,UnexpectedRewardIsExplicit){auto before=BotQuest::rewardObservation(questObservation(),2),after=before;++after.questRevision;++after.money;const auto result=BotQuest::verifyRewards(before,after,{{BotQuestRewardType::ItemReceived,2160,1}});EXPECT_EQ(BotQuestVerificationResult::UnexpectedReward,result.result);EXPECT_EQ(1,result.unexpected);}
TEST(PlayerBotQuestTest,IdenticalObservationsYieldIdenticalResults){EXPECT_EQ(BotQuest::evaluate(questObservation(),questMission()),BotQuest::evaluate(questObservation(),questMission()));}
TEST(PlayerBotQuestTest,ArithmeticAndCountAccumulationCannotOverflow){BotQuestRewardObservation a{.experience=UINT64_MAX-1,.questRevision=1},b{.experience=UINT64_MAX,.questRevision=2};EXPECT_EQ(BotQuestVerificationResult::ExpectedExperienceMissing,BotQuest::verifyRewards(a,b,{{BotQuestRewardType::ExperienceIncreased,0,10}}).result);}
TEST(PlayerBotQuestTest,ContractsProvideNoStorageOrRewardMutationHandle){EXPECT_FALSE(BotQuestObservation{}.containsWorldOwnership());}
TEST(PlayerBotQuestTest,InvalidLifecycleIsRejected){EXPECT_EQ(BotQuestFailure::InvalidLifecycle,BotQuest::evaluate({},questMission()).result);}
TEST(PlayerBotQuestTest,SessionCloseCanClearQuestAssessment){std::optional<BotQuestObservation> state=questObservation();state.reset();EXPECT_FALSE(state);}

namespace {
BotQuestPlan executionPlan(){return{.questId=1,.missionId=2,.revision=9,.steps={{.type=BotQuestStepType::ApproachNpc,.missionId=2,.stableTargetId=44,.configuredName="guide"},{.type=BotQuestStepType::SendDialogue,.missionId=2,.stableTargetId=44,.configuredPhrase="mission"},{.type=BotQuestStepType::TravelRegion,.missionId=2,.region={100,100,7},.radius=1},{.type=BotQuestStepType::UseObject,.missionId=2,.stableTargetId=88,.expectedMissionState=BotMissionState::InProgress},{.type=BotQuestStepType::KillCreature,.missionId=2,.stableTargetId=77},{.type=BotQuestStepType::CollectItem,.missionId=2,.itemTypeId=3031,.requiredCount=1},{.type=BotQuestStepType::DeliverItem,.missionId=2,.itemTypeId=3031,.requiredCount=1},{.type=BotQuestStepType::VerifyReward,.missionId=2},{.type=BotQuestStepType::Finish,.missionId=2}}};}
BotQuestExecutionResult startedExecution(){return BotQuestExecution::start(executionPlan(),questObservation(),{});}
BotQuestStepObservation nextObservation(){return{.revision=9,.regionReached=true,.missionState=BotMissionState::Started};}
}
TEST(PlayerBotQuestExecutionTest,PlansContainValuesOnly){EXPECT_FALSE(executionPlan().containsWorldOwnership());EXPECT_FALSE(executionPlan().steps.front().containsWorldOwnership());}
TEST(PlayerBotQuestExecutionTest,ExecutionRetainsNoWorldOwnership){EXPECT_FALSE(startedExecution().containsWorldOwnership());EXPECT_FALSE(startedExecution().checkpoint.containsWorldOwnership());}
TEST(PlayerBotQuestExecutionTest,StepOrderingIsDeterministic){EXPECT_EQ(startedExecution(),startedExecution());}
TEST(PlayerBotQuestExecutionTest,InvalidPrerequisiteBlocksStart){EXPECT_EQ(BotQuestExecutionFailure::PolicyRejected,BotQuestExecution::start(executionPlan(),questObservation(),{.result=BotQuestFailure::MissingItem}).failure);}
TEST(PlayerBotQuestExecutionTest,AcceptedActionDoesNotAdvance){auto o=nextObservation();o.regionReached=false;o.actionAccepted=true;EXPECT_EQ(0,BotQuestExecution::advance(executionPlan(),startedExecution(),o).checkpoint.verifiedStepIndex);}
TEST(PlayerBotQuestExecutionTest,ObservedProgressAdvances){EXPECT_EQ(1,BotQuestExecution::advance(executionPlan(),startedExecution(),nextObservation()).checkpoint.verifiedStepIndex);}
TEST(PlayerBotQuestExecutionTest,UnexpectedProgressIsExplicit){auto o=nextObservation();o.unexpectedProgress=true;EXPECT_EQ(BotQuestStepResult::UnexpectedProgress,BotQuestExecution::advance(executionPlan(),startedExecution(),o).stepResult);}
TEST(PlayerBotQuestExecutionTest,DialogueRequiresVisibleResponse){auto r=startedExecution();r.checkpoint.verifiedStepIndex=1;r.state=BotQuestExecutionState::StartingDialogue;auto o=nextObservation();o.regionReached=false;o.actionAccepted=true;EXPECT_EQ(BotQuestStepResult::Pending,BotQuestExecution::advance(executionPlan(),r,o).stepResult);o.visibleDialogueResponse=true;EXPECT_EQ(2,BotQuestExecution::advance(executionPlan(),r,o).checkpoint.verifiedStepIndex);}
TEST(PlayerBotQuestExecutionTest,MovementRequiresObservedRegion){auto o=nextObservation();o.regionReached=false;EXPECT_EQ(BotQuestStepResult::NoProgress,BotQuestExecution::advance(executionPlan(),startedExecution(),o).stepResult);}
TEST(PlayerBotQuestExecutionTest,UseRequiresMissionChange){auto r=startedExecution();r.checkpoint.verifiedStepIndex=3;auto o=nextObservation();o.regionReached=false;EXPECT_EQ(3,BotQuestExecution::advance(executionPlan(),r,o).checkpoint.verifiedStepIndex);o.authoritativeProgress=true;EXPECT_EQ(4,BotQuestExecution::advance(executionPlan(),r,o).checkpoint.verifiedStepIndex);}
TEST(PlayerBotQuestExecutionTest,KillRequiresDeathEvidence){auto r=startedExecution();r.checkpoint.verifiedStepIndex=4;auto o=nextObservation();o.authoritativeProgress=true;EXPECT_EQ(4,BotQuestExecution::advance(executionPlan(),r,o).checkpoint.verifiedStepIndex);o.deathObserved=true;EXPECT_EQ(5,BotQuestExecution::advance(executionPlan(),r,o).checkpoint.verifiedStepIndex);}
TEST(PlayerBotQuestExecutionTest,CollectRequiresInventoryChange){auto r=startedExecution();r.checkpoint.verifiedStepIndex=5;auto o=nextObservation();o.itemAdded=true;EXPECT_EQ(6,BotQuestExecution::advance(executionPlan(),r,o).checkpoint.verifiedStepIndex);}
TEST(PlayerBotQuestExecutionTest,DeliveryRequiresRemovalAndProgress){auto r=startedExecution();r.checkpoint.verifiedStepIndex=6;auto o=nextObservation();o.itemRemoved=true;EXPECT_EQ(6,BotQuestExecution::advance(executionPlan(),r,o).checkpoint.verifiedStepIndex);o.authoritativeProgress=true;EXPECT_EQ(7,BotQuestExecution::advance(executionPlan(),r,o).checkpoint.verifiedStepIndex);}
TEST(PlayerBotQuestExecutionTest,RewardRequiresVerification){auto r=startedExecution();r.checkpoint.verifiedStepIndex=7;auto o=nextObservation();o.reward=BotQuestVerificationResult::Partial;EXPECT_EQ(7,BotQuestExecution::advance(executionPlan(),r,o).checkpoint.verifiedStepIndex);o.reward=BotQuestVerificationResult::Verified;EXPECT_EQ(8,BotQuestExecution::advance(executionPlan(),r,o).checkpoint.verifiedStepIndex);}
TEST(PlayerBotQuestExecutionTest,SurvivalSuspends){auto o=nextObservation();o.survivalRequired=true;EXPECT_EQ(BotQuestExecutionState::Suspended,BotQuestExecution::advance(executionPlan(),startedExecution(),o).state);}
TEST(PlayerBotQuestExecutionTest,DeathTerminates){auto o=nextObservation();o.dead=true;EXPECT_EQ(BotQuestExecutionState::Dead,BotQuestExecution::advance(executionPlan(),startedExecution(),o).state);}
TEST(PlayerBotQuestExecutionTest,CheckpointOnlyUpdatesAfterVerification){auto before=startedExecution();auto o=nextObservation();o.regionReached=false;const auto after=BotQuestExecution::advance(executionPlan(),before,o);EXPECT_EQ(before.checkpoint,after.checkpoint);}
TEST(PlayerBotQuestExecutionTest,StaleCheckpointRequiresObservation){auto o=nextObservation();o.revision=8;EXPECT_EQ(BotQuestExecutionFailure::ObservationStale,BotQuestExecution::advance(executionPlan(),startedExecution(),o).failure);o.revision=9;o.observationAge=9;EXPECT_EQ(BotQuestExecutionFailure::ObservationStale,BotQuestExecution::advance(executionPlan(),startedExecution(),o).failure);}
TEST(PlayerBotQuestExecutionTest,RetriesAreFinite){auto r=startedExecution();BotQuestExecutionPolicy p;p.maximumRetries=1;p.retryBackoff=2;r=BotQuestExecution::retry(r,BotQuestStepResult::NpcUnavailable,p,10);EXPECT_EQ(BotQuestExecutionState::Backoff,r.state);EXPECT_EQ(12,r.checkpoint.retryDeadline);auto waiting=nextObservation();waiting.clock=11;EXPECT_EQ(BotQuestExecutionState::Backoff,BotQuestExecution::advance(executionPlan(),r,waiting,p).state);EXPECT_EQ(BotQuestExecutionState::Failed,BotQuestExecution::retry(r,BotQuestStepResult::NpcUnavailable,p,12).state);}
TEST(PlayerBotQuestExecutionTest,StepCountIsBounded){auto p=executionPlan();p.steps.resize(3);BotQuestExecutionPolicy policy;policy.maximumSteps=2;EXPECT_EQ(BotQuestExecutionFailure::BudgetExceeded,BotQuestExecution::start(p,questObservation(),{},policy).failure);}
TEST(PlayerBotQuestExecutionTest,OperationsAreBounded){auto r=startedExecution();r.operations=1;BotQuestExecutionPolicy p;p.maximumOperations=1;EXPECT_EQ(BotQuestExecutionFailure::BudgetExceeded,BotQuestExecution::advance(executionPlan(),r,nextObservation(),p).failure);}
TEST(PlayerBotQuestExecutionTest,MultiNpcFocusOrderingIsSequential){BotQuestPlan p{.questId=1,.missionId=2,.revision=1,.steps={{.type=BotQuestStepType::SendDialogue,.stableTargetId=1},{.type=BotQuestStepType::SendDialogue,.stableTargetId=2}}};EXPECT_NE(p.steps[0].stableTargetId,p.steps[1].stableTargetId);}
TEST(PlayerBotQuestExecutionTest,CancellationIsTerminal){auto o=nextObservation();o.cancelled=true;auto r=BotQuestExecution::advance(executionPlan(),startedExecution(),o);EXPECT_TRUE(r.terminal());EXPECT_EQ(r,BotQuestExecution::advance(executionPlan(),r,nextObservation()));}
TEST(PlayerBotQuestExecutionTest,SessionCloseClearsTransientState){std::optional<BotQuestExecutionResult> r=startedExecution();const auto checkpoint=r->checkpoint;r.reset();EXPECT_FALSE(r);auto fresh=questObservation();fresh.revision=checkpoint.observationRevision+1;const auto reconstructed=BotQuestExecution::reconstruct(executionPlan(),checkpoint,fresh);EXPECT_EQ(BotQuestExecutionState::Traveling,reconstructed.state);EXPECT_FALSE(reconstructed.containsWorldOwnership());}
TEST(PlayerBotQuestExecutionTest,IdenticalInputsProduceIdenticalIntents){EXPECT_EQ(BotQuestExecution::advance(executionPlan(),startedExecution(),nextObservation()),BotQuestExecution::advance(executionPlan(),startedExecution(),nextObservation()));}
TEST(PlayerBotQuestExecutionTest,NoMutationHandleExists){EXPECT_FALSE(BotQuestPlan{}.containsWorldOwnership());EXPECT_FALSE(BotQuestExecutionResult{}.containsWorldOwnership());}

namespace {
std::vector<BotGoal> plannerGoals(){return{{.id=1,.type=BotGoalType::Survive,.priority=BotGoalPriority::CriticalSurvival,.status=BotGoalStatus::Eligible,.policyRevision=1},{.id=2,.type=BotGoalType::Recover,.priority=BotGoalPriority::DeathRecovery,.status=BotGoalStatus::Eligible,.policyRevision=1},{.id=3,.type=BotGoalType::Resupply,.priority=BotGoalPriority::UrgentResupply,.status=BotGoalStatus::Eligible,.policyRevision=1},{.id=4,.type=BotGoalType::CompleteConfiguredQuest,.priority=BotGoalPriority::ActiveQuest,.status=BotGoalStatus::Eligible,.policyRevision=1,.configuredTargetId=7001},{.id=5,.type=BotGoalType::HuntConfiguredRegion,.priority=BotGoalPriority::Progression,.status=BotGoalStatus::Eligible,.policyRevision=1,.configuredRegion={100,100,7}},{.id=6,.type=BotGoalType::EquipUpgrade,.priority=BotGoalPriority::Equipment,.status=BotGoalStatus::Eligible,.policyRevision=1},{.id=7,.type=BotGoalType::SellConfiguredLoot,.priority=BotGoalPriority::OptionalEconomy,.status=BotGoalStatus::Eligible,.policyRevision=1},{.id=8,.type=BotGoalType::IdleSafely,.priority=BotGoalPriority::Idle,.status=BotGoalStatus::Eligible,.policyRevision=1}};}
BotPlannerObservation plannerObservation(){return{.revision=1,.sessionGeneration=1,.placed=true,.regionAvailable=true,.routeAvailable=true,.checkpointValid=true};}
}
TEST(PlayerBotPlannerTest,GoalsAndPlansContainValuesOnly){EXPECT_FALSE(plannerGoals().front().containsWorldOwnership());EXPECT_FALSE(BotPlanner::expand(plannerGoals()[3]).containsWorldOwnership());}
TEST(PlayerBotPlannerTest,RetainsNoWorldOwnership){EXPECT_FALSE(BotPlannerObservation{}.containsWorldOwnership());EXPECT_FALSE(BotPlanCheckpoint{}.containsWorldOwnership());}
TEST(PlayerBotPlannerTest,SurvivalPriorityWins){auto o=plannerObservation();o.survivalCritical=true;EXPECT_EQ(BotGoalType::Survive,BotPlanner::select(plannerGoals(),o).goalType);}
TEST(PlayerBotPlannerTest,DeathRecoveryPriorityWins){auto o=plannerObservation();o.dead=true;o.survivalCritical=true;EXPECT_EQ(BotGoalType::Recover,BotPlanner::select(plannerGoals(),o).goalType);}
TEST(PlayerBotPlannerTest,UrgentResupplyWinsOverProgression){auto o=plannerObservation();o.supplyUrgent=true;o.progressionConfigured=true;EXPECT_EQ(BotGoalType::Resupply,BotPlanner::select(plannerGoals(),o).goalType);}
TEST(PlayerBotPlannerTest,ActiveQuestWinsOverOptionalHunting){auto o=plannerObservation();o.activeQuest=true;o.progressionConfigured=true;EXPECT_EQ(BotGoalType::CompleteConfiguredQuest,BotPlanner::select(plannerGoals(),o).goalType);}
TEST(PlayerBotPlannerTest,EqualPriorityTieBreaksByStableId){std::vector<BotGoal> g={{.id=9,.type=BotGoalType::IdleSafely,.status=BotGoalStatus::Eligible},{.id=3,.type=BotGoalType::IdleSafely,.status=BotGoalStatus::Eligible}};EXPECT_EQ(3,BotPlanner::select(g,plannerObservation()).goalId);}
TEST(PlayerBotPlannerTest,HysteresisPreventsGoalFlapping){std::vector<BotGoal> g={{.id=1,.type=BotGoalType::IdleSafely,.status=BotGoalStatus::Eligible},{.id=2,.type=BotGoalType::IdleSafely,.status=BotGoalStatus::Eligible}};auto current=BotPlanner::select({g[1]},plannerObservation());EXPECT_EQ(2,BotPlanner::select(g,plannerObservation(),{},&current).goalId);}
TEST(PlayerBotPlannerTest,CompletedGoalIsNotReselected){auto g=plannerGoals();g.back().status=BotGoalStatus::Completed;g.erase(g.begin(),g.end()-1);EXPECT_EQ(BotPlannerFailure::NoEligibleGoal,BotPlanner::select(g,plannerObservation()).failure);}
TEST(PlayerBotPlannerTest,FailedGoalRespectsRetryLimit){BotGoal g{.id=1,.type=BotGoalType::IdleSafely,.status=BotGoalStatus::Failed,.failures=3};EXPECT_EQ(BotPlannerFailure::NoEligibleGoal,BotPlanner::select({g},plannerObservation()).failure);}
TEST(PlayerBotPlannerTest,GoalCountIsBounded){BotPlannerPolicy p;p.maximumGoals=1;EXPECT_EQ(BotPlannerFailure::GoalLimit,BotPlanner::select(plannerGoals(),plannerObservation(),p).failure);}
TEST(PlayerBotPlannerTest,PlanDepthIsBounded){BotPlannerPolicy p;p.maximumPlanDepth=1;auto goals=plannerGoals();const auto plan=BotPlanner::expand(goals[3],p);EXPECT_TRUE(plan.depthLimitExceeded);auto observation=plannerObservation();observation.activeQuest=true;EXPECT_EQ(BotPlannerFailure::DepthLimit,BotPlanner::select({goals[3]},observation,p).failure);}
TEST(PlayerBotPlannerTest,StepCountIsBounded){BotPlannerPolicy p;p.maximumSteps=2;const auto plan=BotPlanner::expand(plannerGoals()[3],p);EXPECT_LE(plan.steps.size(),2);EXPECT_TRUE(plan.stepLimitExceeded);EXPECT_EQ(BotPlannerFailure::StepLimit,BotPlanner::select(plannerGoals(),plannerObservation(),p).failure);}
TEST(PlayerBotPlannerTest,ReplanCountIsBounded){auto d=BotPlanner::select(plannerGoals(),plannerObservation());d.replanCount=4;EXPECT_EQ(BotPlannerFailure::ReplanLimit,BotPlanner::replan(plannerGoals(),plannerObservation(),d,BotReplanReason::Timeout).failure);}
TEST(PlayerBotPlannerTest,UnsupportedGoalIsExplicit){EXPECT_EQ(BotPlannerFailure::UnsupportedGoal,BotPlanner::select({{.id=1,.type=BotGoalType::Unsupported,.status=BotGoalStatus::Eligible}},plannerObservation()).failure);}
TEST(PlayerBotPlannerTest,StaleObservationTriggersReplan){auto d=BotPlanner::select(plannerGoals(),plannerObservation());auto o=plannerObservation();o.revision=2;EXPECT_EQ(BotReplanReason::ObservationStale,BotPlanner::replan(plannerGoals(),o,d,BotReplanReason::ObservationStale).replanReason);}
TEST(PlayerBotPlannerTest,SurvivalOverrideSuspendsCurrentPlan){auto d=BotPlanner::select(plannerGoals(),plannerObservation());auto o=plannerObservation();o.revision=2;o.survivalCritical=true;const auto r=BotPlanner::replan(plannerGoals(),o,d,BotReplanReason::SurvivalOverride);EXPECT_EQ(BotGoalType::Survive,r.goalType);}
TEST(PlayerBotPlannerTest,DeathCancelsCurrentPlan){auto d=BotPlanner::select(plannerGoals(),plannerObservation());auto o=plannerObservation();o.revision=2;o.dead=true;EXPECT_EQ(BotGoalType::Recover,BotPlanner::replan(plannerGoals(),o,d,BotReplanReason::Death).goalType);}
TEST(PlayerBotPlannerTest,SupplyDepletionReplans){auto d=BotPlanner::select(plannerGoals(),plannerObservation());auto o=plannerObservation();o.revision=2;o.supplyUrgent=true;EXPECT_EQ(BotGoalType::Resupply,BotPlanner::replan(plannerGoals(),o,d,BotReplanReason::SupplyDepleted).goalType);}
TEST(PlayerBotPlannerTest,QuestStateChangeReplans){auto d=BotPlanner::select(plannerGoals(),plannerObservation());auto o=plannerObservation();o.revision=2;o.activeQuest=true;EXPECT_EQ(BotGoalType::CompleteConfiguredQuest,BotPlanner::replan(plannerGoals(),o,d,BotReplanReason::QuestStateChanged).goalType);}
TEST(PlayerBotPlannerTest,RouteFailureReplansExplicitly){auto d=BotPlanner::select(plannerGoals(),plannerObservation());auto o=plannerObservation();o.revision=2;o.routeAvailable=false;EXPECT_EQ(BotReplanReason::RouteUnavailable,BotPlanner::replan(plannerGoals(),o,d,BotReplanReason::RouteUnavailable).replanReason);}
TEST(PlayerBotPlannerTest,CheckpointAdvancesOnlyAfterVerifiedStep){auto d=BotPlanner::select(plannerGoals(),plannerObservation());auto unchanged=BotPlanner::verifyStep(d,0,2,false);EXPECT_EQ(0,unchanged.checkpoint.verifiedStepIndex);EXPECT_EQ(1,BotPlanner::verifyStep(d,0,2,true).checkpoint.verifiedStepIndex);}
TEST(PlayerBotPlannerTest,InvalidCheckpointIsRejected){auto d=BotPlanner::select(plannerGoals(),plannerObservation());d.checkpoint.safeSaveBoundary=false;auto o=plannerObservation();o.revision=2;EXPECT_EQ(BotPlannerFailure::CheckpointInvalid,BotPlanner::reconstruct(d,o).failure);}
TEST(PlayerBotPlannerTest,SafeCheckpointSurvivesSessionReconstruction){auto d=BotPlanner::select(plannerGoals(),plannerObservation());auto o=plannerObservation();o.revision=2;o.sessionGeneration=2;const auto r=BotPlanner::reconstruct(d,o);EXPECT_EQ(BotGoalStatus::Active,r.status);EXPECT_EQ(2,r.checkpoint.sessionGeneration);}
TEST(PlayerBotPlannerTest,TransientRuntimeStateDoesNotPersist){EXPECT_FALSE(BotPlannerDecision{}.containsWorldOwnership());EXPECT_FALSE(BotPlanStep{}.containsWorldOwnership());}
TEST(PlayerBotPlannerTest,CancellationIsTerminal){auto d=BotPlanner::select(plannerGoals(),plannerObservation());d.status=BotGoalStatus::Cancelled;EXPECT_EQ(d,BotPlanner::verifyStep(d,0,2,true));}
TEST(PlayerBotPlannerTest,PriorityArithmeticCannotOverflow){BotGoal g{.id=1,.type=BotGoalType::Recover,.priority=BotGoalPriority::DeathRecovery,.status=BotGoalStatus::Eligible};auto o=plannerObservation();o.dead=true;EXPECT_LT(BotPlanner::score(g,o),UINT16_MAX);}
TEST(PlayerBotPlannerTest,IdenticalObservationsYieldIdenticalDecisions){EXPECT_EQ(BotPlanner::select(plannerGoals(),plannerObservation()),BotPlanner::select(plannerGoals(),plannerObservation()));}

namespace {
BotPlanExecutionObservation executionObservation(uint64_t revision=1){return{.revision=revision,.sessionGeneration=7,.placed=true};}
BotPlanExecution executionFixture(){auto o=plannerObservation();o.sessionGeneration=7;auto decision=BotPlanner::select({plannerGoals()[7]},o);return BotPlanExecutor::start(42,decision,executionObservation());}
BotPlanExecution delegatedExecution(){auto e=executionFixture();return BotPlanExecutor::delegate(std::move(e),executionObservation(2));}
}
TEST(PlayerBotPlanExecutionTest,ExecutionStateContainsValuesOnly){EXPECT_FALSE(executionFixture().containsWorldOwnership());}
TEST(PlayerBotPlanExecutionTest,NoSubsystemOrWorldOwnershipIsRetained){EXPECT_EQ(BotPlanStepIntent{},BotPlanStepIntent{});EXPECT_FALSE(BotPlanExecution{}.containsWorldOwnership());}
TEST(PlayerBotPlanExecutionTest,OneHighLevelPlanInvariant){auto e=executionFixture();EXPECT_EQ(42,e.id);EXPECT_EQ(BotPlanExecutionState::PlanSelected,e.state);}
TEST(PlayerBotPlanExecutionTest,OneDelegatedStepInvariant){auto e=delegatedExecution();auto again=BotPlanExecutor::delegate(e,executionObservation(3));EXPECT_EQ(BotPlanFailure::ConflictingIntent,again.failure);EXPECT_EQ(e.delegatedStep,again.delegatedStep);}
TEST(PlayerBotPlanExecutionTest,DeathOverridesEveryPlan){auto e=executionFixture();auto o=executionObservation(2);o.dead=true;EXPECT_EQ(BotPlanExecutionState::Dead,BotPlanExecutor::delegate(e,o).state);}
TEST(PlayerBotPlanExecutionTest,SurvivalOverridesTravel){auto e=executionFixture();auto o=executionObservation(2);o.survivalCritical=true;EXPECT_EQ(BotPlanExecutionState::Suspended,BotPlanExecutor::delegate(e,o).state);}
TEST(PlayerBotPlanExecutionTest,SurvivalOverridesCombatContinuation){auto e=delegatedExecution();auto o=executionObservation(3);o.survivalCritical=true;EXPECT_EQ(BotPlanArbitrationReason::CriticalSurvival,BotPlanExecutor::arbitrate(e,o).reason);}
TEST(PlayerBotPlanExecutionTest,SurvivalSafelySuspendsLoot){auto e=executionFixture();auto o=executionObservation(2);o.survivalCritical=true;o.pendingAuthoritativeAction=true;auto a=BotPlanExecutor::arbitrate(e,o);EXPECT_TRUE(a.suspend);EXPECT_TRUE(a.requiresBoundary);}
TEST(PlayerBotPlanExecutionTest,PendingAuthoritativeActionReachesSafeBoundary){auto e=executionFixture();auto o=executionObservation(2);o.pendingAuthoritativeAction=true;EXPECT_TRUE(BotPlanExecutor::arbitrate(e,o).requiresBoundary);}
TEST(PlayerBotPlanExecutionTest,SessionShutdownOverridesOptionalWork){auto e=executionFixture();auto o=executionObservation(2);o.shutdownRequested=true;EXPECT_EQ(BotPlanExecutionState::Cancelled,BotPlanExecutor::delegate(e,o).state);}
TEST(PlayerBotPlanExecutionTest,AcceptedRequestDoesNotAdvanceCheckpoint){auto e=delegatedExecution();e.delegatedStep->accepted=true;EXPECT_EQ(0,e.decision.checkpoint.verifiedStepIndex);}
TEST(PlayerBotPlanExecutionTest,ObservedTerminalSuccessAdvancesCheckpoint){auto e=BotPlanExecutor::observe(delegatedExecution(),BotPlanStepOutcome::Succeeded,3,true);EXPECT_EQ(1,e.decision.checkpoint.verifiedStepIndex);}
TEST(PlayerBotPlanExecutionTest,NoEffectDoesNotAdvance){auto e=BotPlanExecutor::observe(delegatedExecution(),BotPlanStepOutcome::NoEffect,3,false);EXPECT_EQ(0,e.decision.checkpoint.verifiedStepIndex);}
TEST(PlayerBotPlanExecutionTest,PartialOutcomeIsExplicit){auto e=BotPlanExecutor::observe(delegatedExecution(),BotPlanStepOutcome::Partial,3,false);EXPECT_EQ(BotPlanFailure::TargetUnavailable,e.failure);}
TEST(PlayerBotPlanExecutionTest,UnexpectedStateCausesReobservation){auto e=BotPlanExecutor::observe(delegatedExecution(),BotPlanStepOutcome::UnexpectedState,3,false);EXPECT_TRUE(e.freshObservationRequired);}
TEST(PlayerBotPlanExecutionTest,LostPreconditionCausesReplan){auto e=BotPlanExecutor::observe(delegatedExecution(),BotPlanStepOutcome::PreconditionLost,3,false);EXPECT_EQ(BotPlanFailure::StaleCheckpoint,e.failure);}
TEST(PlayerBotPlanExecutionTest,RouteFailureChoosesBoundedAlternative){auto e=executionFixture();e.budget.maximumRetries=0;EXPECT_EQ(BotPlanRecoveryDecision::ChooseAlternative,BotPlanExecutor::recover(e,BotPlanFailure::RouteUnavailable,1).recovery);}
TEST(PlayerBotPlanExecutionTest,UnavailableNpcChoosesBoundedAlternative){auto e=executionFixture();e.budget.maximumRetries=0;EXPECT_EQ(BotPlanRecoveryDecision::ChooseAlternative,BotPlanExecutor::recover(e,BotPlanFailure::NpcUnavailable,9).recovery);}
TEST(PlayerBotPlanExecutionTest,SupplyDepletionReplacesProgression){auto e=executionFixture();auto o=executionObservation(2);o.supplyUrgent=true;EXPECT_TRUE(BotPlanExecutor::arbitrate(e,o).suspend);}
TEST(PlayerBotPlanExecutionTest,QuestStateChangeTriggersReplan){auto e=BotPlanExecutor::recover(executionFixture(),BotPlanFailure::QuestStateChanged);EXPECT_TRUE(e.freshObservationRequired);}
TEST(PlayerBotPlanExecutionTest,StaleCheckpointIsRejected){auto e=BotPlanExecutor::delegate(executionFixture(),executionObservation(1));EXPECT_EQ(BotPlanFailure::StaleCheckpoint,e.failure);}
TEST(PlayerBotPlanExecutionTest,ExecutionBudgetIsEnforced){auto e=executionFixture();e.budget.maximumTicks=0;EXPECT_EQ(BotPlanExecutionState::Failed,BotPlanExecutor::delegate(e,executionObservation(2)).state);}
TEST(PlayerBotPlanExecutionTest,RetryCountIsFinite){auto e=executionFixture();e.budget.maximumRetries=1;e=BotPlanExecutor::recover(e,BotPlanFailure::Timeout,2);e=BotPlanExecutor::recover(e,BotPlanFailure::Timeout,2);EXPECT_NE(BotPlanRecoveryDecision::RetryStep,e.recovery);}
TEST(PlayerBotPlanExecutionTest,AlternativeCountIsFinite){auto e=executionFixture();e.budget.maximumRetries=0;e.budget.maximumAlternatives=1;e=BotPlanExecutor::recover(e,BotPlanFailure::NpcUnavailable,2);e=BotPlanExecutor::recover(e,BotPlanFailure::NpcUnavailable,2);EXPECT_EQ(BotPlanRecoveryDecision::ReplacePlan,e.recovery);}
TEST(PlayerBotPlanExecutionTest,PlanOscillationIsSuppressed){auto e=executionFixture();e.budget.maximumRetries=0;e.budget.maximumAlternatives=0;EXPECT_EQ(BotPlanRecoveryDecision::ReplacePlan,BotPlanExecutor::recover(e,BotPlanFailure::RouteUnavailable,7).recovery);}
TEST(PlayerBotPlanExecutionTest,EqualPriorityDecisionsAreDeterministic){auto e=executionFixture();auto o=executionObservation(2);EXPECT_EQ(BotPlanExecutor::arbitrate(e,o),BotPlanExecutor::arbitrate(e,o));}
TEST(PlayerBotPlanExecutionTest,CompletedPlanIsTerminal){auto e=executionFixture();e.state=BotPlanExecutionState::Completed;EXPECT_TRUE(e.terminal());}
TEST(PlayerBotPlanExecutionTest,FailedPlanIsTerminalAfterExhaustion){auto e=executionFixture();e.budget.maximumRecoveries=0;e=BotPlanExecutor::recover(e,BotPlanFailure::RetryExhausted);EXPECT_TRUE(e.terminal());}
TEST(PlayerBotPlanExecutionTest,CancellationIsTerminal){EXPECT_TRUE(BotPlanExecutor::cancel(executionFixture()).terminal());}
TEST(PlayerBotPlanExecutionTest,SessionCloseClearsExecutionState){auto e=BotPlanExecutor::cancel(delegatedExecution());EXPECT_FALSE(e.delegatedStep);}
TEST(PlayerBotPlanExecutionTest,NoCallbacksRetainSession){EXPECT_FALSE(BotPlanExecution{}.containsWorldOwnership());}
TEST(PlayerBotPlanExecutionTest,IdenticalObservationsProduceIdenticalArbitration){auto e=executionFixture();EXPECT_EQ(BotPlanExecutor::arbitrate(e,executionObservation()),BotPlanExecutor::arbitrate(e,executionObservation()));}

namespace {
BotPersistedPlanCheckpoint persistedCheckpoint(){BotPersistedPlanCheckpoint c{.playerId=10,.checkpointRevision=5,.policyRevision=1,.goalId=8,.goalType=BotGoalType::IdleSafely,.planRevision=1,.verifiedStepIndex=1,.verifiedSubsystem=BotPlanSubsystem::Observation,.safeSaveBoundary=true};c.checksum=BotPlannerPersistence::checksum(c);return c;}
BotCheckpointValidation checkpointValidation(){return{.playerId=10,.schemaVersion=2,.policyRevision=1,.goalIds={8},.planRevisions={1},.planStepCount=3,.postconditionValid=true,.observationFresh=true};}
BotLongCampaignBudget campaignBudget(){BotLongCampaignBudget b;b.maximumPlannerTicks=3;b.maximumCompletedGoals=8;b.maximumReplans=2;b.maximumSubsystemFailures=2;b.maximumConsecutiveIdleCycles=2;b.maximumPersistedCheckpoints=2;b.maximumSaveCycles=2;b.maximumSessionReconstructions=2;b.requiredCompletedGoals=3;return b;}
}
TEST(PlayerBotPlannerPersistenceTest,PersistedCheckpointContainsValuesOnly){EXPECT_FALSE(persistedCheckpoint().containsWorldOwnership());}
TEST(PlayerBotPlannerPersistenceTest,NoWorldOwnershipIsSerializable){EXPECT_TRUE(std::is_trivially_destructible_v<BotPersistedPlanCheckpoint>);}
TEST(PlayerBotPlannerPersistenceTest,SafeBoundaryPermitsPersistence){EXPECT_EQ(BotCheckpointLoadReason::Valid,BotPlannerPersistence::validate(persistedCheckpoint(),checkpointValidation()).reason);}
TEST(PlayerBotPlannerPersistenceTest,PendingActionBlocksPersistence){auto e=executionFixture();e.delegatedStep=BotPlanStepExecution{};auto c=BotPlannerPersistence::capture(10,e,BotPlanSubsystem::Navigation);EXPECT_FALSE(c.safeSaveBoundary);}
TEST(PlayerBotPlannerPersistenceTest,UnverifiedStepBlocksPersistence){auto c=persistedCheckpoint();c.safeSaveBoundary=false;c.checksum=BotPlannerPersistence::checksum(c);EXPECT_EQ(BotCheckpointLoadReason::UnsafeBoundary,BotPlannerPersistence::validate(c,checkpointValidation()).reason);}
TEST(PlayerBotPlannerPersistenceTest,SuccessfulPersistenceMarksCheckpointDurable){EXPECT_TRUE(BotPlannerPersistence::acknowledge(persistedCheckpoint(),true).durable);}
TEST(PlayerBotPlannerPersistenceTest,FailedPersistenceDoesNotMarkDurability){EXPECT_FALSE(BotPlannerPersistence::acknowledge(persistedCheckpoint(),false).durable);}
TEST(PlayerBotPlannerPersistenceTest,ValidCheckpointLoads){EXPECT_TRUE(BotPlannerPersistence::validate(persistedCheckpoint(),checkpointValidation()).checkpoint);}
TEST(PlayerBotPlannerPersistenceTest,MissingCheckpointIsExplicit){EXPECT_EQ(BotCheckpointLoadReason::Missing,BotCheckpointLoadResult{}.reason);}
TEST(PlayerBotPlannerPersistenceTest,CorruptCheckpointIsRejected){auto c=persistedCheckpoint();++c.checksum;EXPECT_EQ(BotCheckpointLoadReason::Corrupt,BotPlannerPersistence::validate(c,checkpointValidation()).reason);}
TEST(PlayerBotPlannerPersistenceTest,UnsupportedVersionIsRejected){auto c=persistedCheckpoint();c.schemaVersion=3;c.checksum=BotPlannerPersistence::checksum(c);EXPECT_EQ(BotCheckpointLoadReason::UnsupportedVersion,BotPlannerPersistence::validate(c,checkpointValidation()).reason);}
TEST(PlayerBotPlannerPersistenceTest,PolicyRevisionMismatchIsRejected){auto c=persistedCheckpoint();c.policyRevision=2;c.checksum=BotPlannerPersistence::checksum(c);EXPECT_EQ(BotCheckpointLoadReason::PolicyRevisionChanged,BotPlannerPersistence::validate(c,checkpointValidation()).reason);}
TEST(PlayerBotPlannerPersistenceTest,MissingPlanIsRejected){auto v=checkpointValidation();v.planRevisions.clear();EXPECT_EQ(BotCheckpointLoadReason::PlanMissing,BotPlannerPersistence::validate(persistedCheckpoint(),v).reason);}
TEST(PlayerBotPlannerPersistenceTest,UnavailableGoalIsRejected){auto v=checkpointValidation();v.goalIds.clear();EXPECT_EQ(BotCheckpointLoadReason::GoalUnavailable,BotPlannerPersistence::validate(persistedCheckpoint(),v).reason);}
TEST(PlayerBotPlannerPersistenceTest,OutOfRangeStepIsRejected){auto v=checkpointValidation();v.planStepCount=0;EXPECT_EQ(BotCheckpointLoadReason::StepOutOfRange,BotPlannerPersistence::validate(persistedCheckpoint(),v).reason);}
TEST(PlayerBotPlannerPersistenceTest,PlayerMismatchIsRejected){auto v=checkpointValidation();v.playerId=11;EXPECT_EQ(BotCheckpointLoadReason::PlayerMismatch,BotPlannerPersistence::validate(persistedCheckpoint(),v).reason);}
TEST(PlayerBotPlannerPersistenceTest,InvalidPostconditionTriggersReplan){auto v=checkpointValidation();v.postconditionValid=false;EXPECT_EQ(BotCheckpointLoadReason::PostconditionInvalid,BotPlannerPersistence::validate(persistedCheckpoint(),v).reason);}
TEST(PlayerBotPlannerPersistenceTest,TransientSubsystemStateIsCleared){BotCheckpointLoadResult r;EXPECT_FALSE(r.checkpoint);EXPECT_TRUE(r.freshObservationRequired);}
TEST(PlayerBotPlannerPersistenceTest,FreshObservationsAreRequiredAfterLoad){auto v=checkpointValidation();v.observationFresh=false;EXPECT_EQ(BotCheckpointLoadReason::ObservationRequired,BotPlannerPersistence::validate(persistedCheckpoint(),v).reason);}
TEST(PlayerBotPlannerPersistenceTest,VerifiedSafeCheckpointMayResume){EXPECT_FALSE(BotPlannerPersistence::validate(persistedCheckpoint(),checkpointValidation()).freshObservationRequired);}
TEST(PlayerBotPlannerPersistenceTest,MaximumPlannerTicksAreEnforced){auto b=campaignBudget();b.maximumPlannerTicks=1;EXPECT_EQ(BotLongCampaignState::BudgetReached,BotPlannerPersistence::advance({},b,1,false,false,false,false,false,false,false).state);}
TEST(PlayerBotPlannerPersistenceTest,MaximumCompletedGoalsAreEnforced){auto b=campaignBudget();b.maximumCompletedGoals=1;b.requiredCompletedGoals=3;EXPECT_EQ(BotLongCampaignState::BudgetReached,BotPlannerPersistence::advance({},b,1,true,false,false,false,false,false,false).state);}
TEST(PlayerBotPlannerPersistenceTest,MaximumReplansAreEnforced){auto b=campaignBudget();b.maximumReplans=1;EXPECT_EQ(BotLongCampaignState::BudgetReached,BotPlannerPersistence::advance({},b,1,false,true,false,false,false,false,false).state);}
TEST(PlayerBotPlannerPersistenceTest,MaximumFailuresAreEnforced){auto b=campaignBudget();b.maximumSubsystemFailures=1;EXPECT_EQ(BotLongCampaignState::BudgetReached,BotPlannerPersistence::advance({},b,1,false,false,true,false,false,false,false).state);}
TEST(PlayerBotPlannerPersistenceTest,MaximumSaveCyclesAreEnforced){auto b=campaignBudget();b.maximumSaveCycles=1;EXPECT_EQ(BotLongCampaignState::BudgetReached,BotPlannerPersistence::advance({},b,1,false,false,false,false,false,true,false).state);}
TEST(PlayerBotPlannerPersistenceTest,MaximumSessionReconstructionsAreEnforced){auto b=campaignBudget();b.maximumSessionReconstructions=1;EXPECT_EQ(BotLongCampaignState::BudgetReached,BotPlannerPersistence::advance({},b,1,false,false,false,false,false,false,true).state);}
TEST(PlayerBotPlannerPersistenceTest,IdleLoopExhaustionTerminatesSafely){auto b=campaignBudget();b.maximumConsecutiveIdleCycles=1;EXPECT_EQ(BotLongCampaignState::BudgetReached,BotPlannerPersistence::advance({},b,1,false,false,false,true,false,false,false).state);}
TEST(PlayerBotPlannerPersistenceTest,FailureCountersCannotOverflow){BotLongCampaignProgress p;p.subsystemFailures=UINT16_MAX;auto r=BotPlannerPersistence::advance(p,campaignBudget(),1,false,false,true,false,false,false,false);EXPECT_EQ(UINT16_MAX,r.subsystemFailures);}
TEST(PlayerBotPlannerPersistenceTest,DiagnosticBufferIsBounded){BotPlannerDiagnostics d{.maximumEntries=2};d.record({});d.record({});d.record({});EXPECT_EQ(2,d.entries.size());}
TEST(PlayerBotPlannerPersistenceTest,RepeatedIdenticalEventsRemainDeterministic){EXPECT_EQ(BotPlannerPersistence::advance({},campaignBudget(),1,false,false,false,false,false,false,false),BotPlannerPersistence::advance({},campaignBudget(),1,false,false,false,false,false,false,false));}
TEST(PlayerBotPlannerPersistenceTest,TeardownClearsLoadedAndPendingCheckpointState){BotCheckpointLoadResult r{.checkpoint=persistedCheckpoint()};r.checkpoint.reset();EXPECT_FALSE(r.checkpoint);}
TEST(PlayerBotPlannerPersistenceTest,NoGameplayStorageMutationIsUsed){EXPECT_EQ(BotCheckpointLoadReason::Valid,BotPlannerPersistence::validate(persistedCheckpoint(),checkpointValidation()).reason);}
TEST(PlayerBotPlannerPersistenceTest,EveryUnsafeSubsystemBoundaryBlocksPersistence){auto e=executionFixture();BotCheckpointBoundary b;b.dialoguePending=true;EXPECT_FALSE(BotPlannerPersistence::capture(10,e,BotPlanSubsystem::Dialogue,b).safeSaveBoundary);b={};b.shopPending=true;EXPECT_FALSE(BotPlannerPersistence::capture(10,e,BotPlanSubsystem::Resupply,b).safeSaveBoundary);b={};b.depotPending=true;EXPECT_FALSE(BotPlannerPersistence::capture(10,e,BotPlanSubsystem::Resupply,b).safeSaveBoundary);}

namespace {
BotProgressionTarget progressionTarget(){return{.level={20},.skills={{.type=BotSkillType::Sword,.level=15,.requiredVocationId=1,.requiredWeaponType=WEAPON_SWORD}},.policyRevision=1};}
BotProgressionObservation progressionObservation(uint64_t revision=1){return{.revision=revision,.level=10,.experience=100,.vocationId=1,.weaponType=WEAPON_SWORD,.skills={{.type=BotSkillType::Sword,.level=14}}};}
BotHuntAreaPolicy areaPolicy(uint32_t id=1){return{.id=id,.region={{100,100,7},4},.minimumLevel=8,.maximumLevel=30,.vocationIds={1},.revision=7};}
BotHuntAreaWindow areaWindow(std::function<void(BotHuntAreaObservation&)> edit={}){BotHuntAreaWindow w;for(uint64_t i=1;i<=3;++i){BotHuntAreaObservation o{.areaId=1,.revision=i,.policyRevision=7,.encounters=1,.successfulKills=1,.observedExperience=50,.elapsedTicks=100};if(edit)edit(o);w.record(o);}return w;}
}
TEST(PlayerBotProgressionTest,TargetsContainValuesOnly){EXPECT_FALSE(progressionTarget().containsWorldOwnership);}
TEST(PlayerBotProgressionTest,LevelTargetValidation){auto t=progressionTarget();t.level.level=0;EXPECT_EQ(BotProgressionState::Invalid,BotProgression::assess(t,progressionObservation()).levelState);}
TEST(PlayerBotProgressionTest,AlreadyAttainedLevelTarget){auto o=progressionObservation();o.level=20;EXPECT_EQ(BotProgressionState::Attained,BotProgression::assess(progressionTarget(),o).levelState);}
TEST(PlayerBotProgressionTest,BelowTargetLevelStartsCombat){EXPECT_TRUE(BotProgression::assess(progressionTarget(),progressionObservation()).startProgressionCombat);}
TEST(PlayerBotProgressionTest,AuthoritativeObservationCompletesTarget){auto o=progressionObservation();o.level=20;o.skills[0].level=15;auto a=BotProgression::assess(progressionTarget(),o);EXPECT_TRUE(a.terminal);EXPECT_FALSE(a.startProgressionCombat);}
TEST(PlayerBotProgressionTest,CompletedTargetIsTerminal){auto o=progressionObservation();o.level=99;o.skills[0].level=99;EXPECT_TRUE(BotProgression::assess(progressionTarget(),o).terminal);}
TEST(PlayerBotProgressionTest,ValidSkillTargetIsBelowTarget){EXPECT_EQ(BotProgressionState::BelowTarget,BotProgression::assess(progressionTarget(),progressionObservation()).skillStates[0]);}
TEST(PlayerBotProgressionTest,AlreadyAttainedSkillTarget){auto o=progressionObservation();o.skills[0].level=15;EXPECT_EQ(BotProgressionState::Attained,BotProgression::assess(progressionTarget(),o).skillStates[0]);}
TEST(PlayerBotProgressionTest,ObservedSkillTriesAreProgressing){auto o=progressionObservation();o.skills[0].tries=1;EXPECT_EQ(BotProgressionState::Progressing,BotProgression::assess(progressionTarget(),o).skillStates[0]);}
TEST(PlayerBotProgressionTest,InapplicableVocationIsRejected){auto o=progressionObservation();o.vocationId=2;EXPECT_EQ(BotProgressionState::Inapplicable,BotProgression::assess(progressionTarget(),o).skillStates[0]);}
TEST(PlayerBotProgressionTest,WeaponPolicyBlocksMismatch){auto o=progressionObservation();o.weaponType=WEAPON_AXE;EXPECT_EQ(BotProgressionState::Blocked,BotProgression::assess(progressionTarget(),o).skillStates[0]);}
TEST(PlayerBotProgressionTest,StaleObservationIsRejected){EXPECT_EQ(BotProgressionState::ObservationStale,BotProgression::assess(progressionTarget(),progressionObservation(2),2).levelState);}
TEST(PlayerBotProgressionTest,SuitabilityContainsValuesOnly){EXPECT_FALSE(areaWindow().samples.front().containsWorldOwnership);}
TEST(PlayerBotProgressionTest,SuitableAreaIsDeterministic){auto a=BotProgression::assessArea(areaPolicy(),areaWindow(),10,1);EXPECT_EQ(BotHuntAreaSuitability::Suitable,a.suitability);EXPECT_EQ(a,BotProgression::assessArea(areaPolicy(),areaWindow(),10,1));}
TEST(PlayerBotProgressionTest,TooDangerousAreaIsDetected){auto w=areaWindow([](auto&o){o.failedCombats=1;});EXPECT_EQ(BotHuntAreaSuitability::TooDangerous,BotProgression::assessArea(areaPolicy(),w,10,1).suitability);}
TEST(PlayerBotProgressionTest,TooWeakAreaIsDetected){auto w=areaWindow([](auto&o){o.observedExperience=0;});EXPECT_EQ(BotHuntAreaSuitability::TooWeak,BotProgression::assessArea(areaPolicy(),w,10,1).suitability);}
TEST(PlayerBotProgressionTest,NoTargetAreaIsDetected){auto w=areaWindow([](auto&o){o.encounters=0;o.successfulKills=0;o.noTargetCycles=1;});EXPECT_EQ(BotHuntAreaSuitability::NoEligibleTargets,BotProgression::assessArea(areaPolicy(),w,10,1).suitability);}
TEST(PlayerBotProgressionTest,RouteUnreliableAreaIsDetected){auto w=areaWindow([](auto&o){o.routeFailures=1;});EXPECT_EQ(BotHuntAreaSuitability::RouteUnreliable,BotProgression::assessArea(areaPolicy(),w,10,1).suitability);}
TEST(PlayerBotProgressionTest,SupplyInefficientAreaIsDetected){auto w=areaWindow([](auto&o){o.successfulKills=0;o.healingUses=2;});EXPECT_EQ(BotHuntAreaSuitability::SupplyInefficient,BotProgression::assessArea(areaPolicy(),w,10,1).suitability);}
TEST(PlayerBotProgressionTest,RepeatedDeathIsDetected){auto w=areaWindow([](auto&o){o.deathCount=1;});EXPECT_EQ(BotHuntAreaSuitability::RepeatedDeath,BotProgression::assessArea(areaPolicy(),w,10,1).suitability);}
TEST(PlayerBotProgressionTest,InsufficientEvidenceRemainsUnknown){auto w=areaWindow();w.samples.resize(2);EXPECT_EQ(BotHuntAreaSuitability::ObservationInsufficient,BotProgression::assessArea(areaPolicy(),w,10,1).suitability);}
TEST(PlayerBotProgressionTest,ObservationWindowIsBounded){BotHuntAreaWindow w{.maximumSamples=2};w.record({.areaId=1});w.record({.areaId=1});w.record({.areaId=1});EXPECT_EQ(2,w.samples.size());}
TEST(PlayerBotProgressionTest,OldAreaSamplesAreCleared){auto w=areaWindow();w.record({.areaId=2});w.clearFor(2);EXPECT_TRUE(std::ranges::all_of(w.samples,[](auto&o){return o.areaId==2;}));}
TEST(PlayerBotProgressionTest,UnsuitableAreaSwitchesDeterministically){BotAreaSwitchState s{.activeAreaId=1};BotAreaSwitchPolicy p{.areas={areaPolicy(1),areaPolicy(2)},.revision=1};BotHuntAreaAssessment a{.suitability=BotHuntAreaSuitability::TooDangerous,.failure=BotHuntAreaFailure::Danger,.sampleCount=3,.observationRevision=1};auto d=BotProgression::decideSwitch(s,p,a,false);EXPECT_EQ(BotAreaSwitchResult::Switch,d.result);EXPECT_EQ(2,d.toAreaId);}
TEST(PlayerBotProgressionTest,PendingActionDelaysSwitch){BotAreaSwitchState s{.activeAreaId=1};BotAreaSwitchPolicy p{.areas={areaPolicy(1),areaPolicy(2)},.revision=1};BotHuntAreaAssessment a{.suitability=BotHuntAreaSuitability::TooDangerous,.sampleCount=3,.observationRevision=1};EXPECT_EQ(BotAreaSwitchResult::WaitForBoundary,BotProgression::decideSwitch(s,p,a,true).result);}
TEST(PlayerBotProgressionTest,AllAlternativesExhaustSafely){BotAreaSwitchState s{.activeAreaId=1,.previousAreaId=2};BotAreaSwitchPolicy p{.areas={areaPolicy(1),areaPolicy(2)},.revision=1};BotHuntAreaAssessment a{.suitability=BotHuntAreaSuitability::TooDangerous,.sampleCount=3,.observationRevision=1};EXPECT_EQ(BotAreaSwitchResult::Exhausted,BotProgression::decideSwitch(s,p,a,false).result);EXPECT_TRUE(s.terminal);}
TEST(PlayerBotProgressionTest,AreaSwitchHysteresisPreventsOscillation){BotAreaSwitchState s{.activeAreaId=2,.previousAreaId=1,.policyRevision=1,.cooldownRemaining=2};BotAreaSwitchPolicy p{.areas={areaPolicy(1),areaPolicy(2)},.revision=1};BotHuntAreaAssessment a{.suitability=BotHuntAreaSuitability::TooDangerous,.sampleCount=3,.observationRevision=1};EXPECT_EQ(BotAreaSwitchResult::Cooldown,BotProgression::decideSwitch(s,p,a,false).result);}
TEST(PlayerBotPlannerPersistenceTest,CurrentCheckpointNeedsFreshObservation){EXPECT_EQ(BotPlannerMigrationResult::Current,BotPlannerPersistence::migrate(persistedCheckpoint(),10).result);}
TEST(PlayerBotPlannerPersistenceTest,PreviousCheckpointMigratesSafely){auto c=persistedCheckpoint();c.schemaVersion=1;c.failureCount=99;c.checksum=BotPlannerPersistence::checksum(c);auto m=BotPlannerPersistence::migrate(c,10);ASSERT_EQ(BotPlannerMigrationResult::Migrated,m.result);EXPECT_EQ(2,m.checkpoint->schemaVersion);EXPECT_TRUE(m.freshObservationRequired);EXPECT_LE(m.checkpoint->failureCount,16);}
TEST(PlayerBotPlannerPersistenceTest,UnsupportedOldMigrationIsRejected){auto c=persistedCheckpoint();c.schemaVersion=0;c.checksum=BotPlannerPersistence::checksum(c);EXPECT_EQ(BotPlannerMigrationResult::UnsupportedOlderVersion,BotPlannerPersistence::migrate(c,10).result);}
TEST(PlayerBotPlannerPersistenceTest,FutureMigrationIsRejected){auto c=persistedCheckpoint();c.schemaVersion=3;c.checksum=BotPlannerPersistence::checksum(c);EXPECT_EQ(BotPlannerMigrationResult::UnsupportedFutureVersion,BotPlannerPersistence::migrate(c,10).result);}
TEST(PlayerBotPlannerPersistenceTest,CorruptMigrationIsRejected){auto c=persistedCheckpoint();++c.checksum;EXPECT_EQ(BotPlannerMigrationResult::Corrupt,BotPlannerPersistence::migrate(c,10).result);}
TEST(PlayerBotPlannerPersistenceTest,MigrationPlayerMismatchIsRejected){EXPECT_EQ(BotPlannerMigrationResult::PlayerMismatch,BotPlannerPersistence::migrate(persistedCheckpoint(),11).result);}

namespace{BotCoordinationPolicy coordinationPolicy(){return{.id=1,.configuredMembers={30,10,20},.configuredLeader=20,.revision=1};}BotCoordinationGroupObservation coordinationObservation(){return{.id=1,.revision=1,.members={{.id=10,.sessionGeneration=1,.revision=1,.vocationId=1,.configured=true,.partyMember=true,.playerBot=true},{.id=20,.sessionGeneration=1,.revision=1,.vocationId=3,.configured=true,.partyMember=true,.playerBot=true},{.id=30,.sessionGeneration=1,.revision=1,.vocationId=2,.configured=true,.partyMember=true,.playerBot=true}}};}BotCoordinationReservation reservation(uint32_t member=10,uint64_t target=99){return{.groupId=1,.memberId=member,.type=BotCoordinationReservationType::CombatTarget,.targetSignature=target,.observationRevision=1,.sessionGeneration=1,.expiresAt=10,.policyRevision=1};}}
TEST(PlayerBotCoordinationTest,ContractsContainValuesOnly){EXPECT_FALSE(coordinationObservation().containsWorldOwnership);EXPECT_FALSE(BotCoordination::evaluate(coordinationPolicy(),coordinationObservation()).directGameplayAction);}
TEST(PlayerBotCoordinationTest,ConfiguredMembershipExcludesUnrelatedBot){auto o=coordinationObservation();o.members.push_back({.id=40,.configured=false,.playerBot=true});EXPECT_EQ(3,BotCoordination::evaluate(coordinationPolicy(),o).roles.size());}
TEST(PlayerBotCoordinationTest,HumanPlayerIsNeverControlled){auto o=coordinationObservation();o.members[0].playerBot=false;EXPECT_EQ(2,BotCoordination::evaluate(coordinationPolicy(),o).roles.size());}
TEST(PlayerBotCoordinationTest,ExplicitLeaderIsSelected){EXPECT_EQ(20,BotCoordination::evaluate(coordinationPolicy(),coordinationObservation()).leaderId);}
TEST(PlayerBotCoordinationTest,FallbackLeaderUsesStableId){auto p=coordinationPolicy();p.configuredLeader=99;EXPECT_EQ(10,BotCoordination::evaluate(p,coordinationObservation()).leaderId);}
TEST(PlayerBotCoordinationTest,DeadMemberCannotLead){auto o=coordinationObservation();o.members[1].alive=false;EXPECT_EQ(10,BotCoordination::evaluate(coordinationPolicy(),o).leaderId);}
TEST(PlayerBotCoordinationTest,RemovedMemberCannotLead){auto o=coordinationObservation();o.members[1].placed=false;EXPECT_EQ(10,BotCoordination::evaluate(coordinationPolicy(),o).leaderId);}
TEST(PlayerBotCoordinationTest,LeaderChangeBudgetDegradesSafely){auto p=coordinationPolicy();p.configuredLeader=10;p.maximumLeaderChanges=0;EXPECT_EQ(BotCoordinationState::Degraded,BotCoordination::evaluate(p,coordinationObservation(),20,0).state);}
TEST(PlayerBotCoordinationTest,RolesAreDeterministic){EXPECT_EQ(BotCoordination::evaluate(coordinationPolicy(),coordinationObservation()),BotCoordination::evaluate(coordinationPolicy(),coordinationObservation()));}
TEST(PlayerBotCoordinationTest,GroupSizeIsBounded){auto p=coordinationPolicy();p.maximumMembers=2;EXPECT_EQ(BotCoordinationFailure::MemberLimit,BotCoordination::evaluate(p,coordinationObservation()).failure);}
TEST(PlayerBotCoordinationTest,ReservationConflictIsAdvisory){std::vector<BotCoordinationReservation>r;auto p=coordinationPolicy();EXPECT_EQ(BotCoordinationFailure::None,BotCoordination::reserve(r,reservation(),p,0));EXPECT_EQ(BotCoordinationFailure::Conflict,BotCoordination::reserve(r,reservation(20),p,0));}
TEST(PlayerBotCoordinationTest,FocusFirePermitsSharedCombatIntent){std::vector<BotCoordinationReservation>r;auto p=coordinationPolicy();p.focusFire=true;BotCoordination::reserve(r,reservation(),p,0);EXPECT_EQ(BotCoordinationFailure::None,BotCoordination::reserve(r,reservation(20),p,0));}
TEST(PlayerBotCoordinationTest,CorpseReservationConflictIsExclusive){std::vector<BotCoordinationReservation>r;auto p=coordinationPolicy();auto a=reservation();a.type=BotCoordinationReservationType::LootCorpse;auto b=a;b.memberId=20;BotCoordination::reserve(r,a,p,0);EXPECT_EQ(BotCoordinationFailure::Conflict,BotCoordination::reserve(r,b,p,0));}
TEST(PlayerBotCoordinationTest,ReservationCountIsBounded){std::vector<BotCoordinationReservation>r;auto p=coordinationPolicy();p.maximumReservations=1;BotCoordination::reserve(r,reservation(),p,0);EXPECT_EQ(BotCoordinationFailure::ReservationLimit,BotCoordination::reserve(r,reservation(20,100),p,0));}
TEST(PlayerBotCoordinationTest,ReservationsExpireFinitely){std::vector<BotCoordinationReservation>r{reservation()};BotCoordination::expire(r,10);EXPECT_TRUE(r.empty());}
TEST(PlayerBotCoordinationTest,DeathOrLogoutInvalidatesReservations){std::vector<BotCoordinationReservation>r{reservation()};BotCoordination::invalidate(r,10);EXPECT_TRUE(r.empty());}
TEST(PlayerBotCoordinationTest,GenerationChangeInvalidatesReservations){std::vector<BotCoordinationReservation>r{reservation()};BotCoordination::invalidate(r,10,2);EXPECT_TRUE(r.empty());}
TEST(PlayerBotCoordinationTest,CriticalMemberCreatesRetreatIntent){auto o=coordinationObservation();o.members[0].healthPercent=10;auto d=BotCoordination::evaluate(coordinationPolicy(),o);EXPECT_EQ(BotCoordinationState::Retreating,d.state);EXPECT_TRUE(std::ranges::all_of(d.intents,[](const auto&i){return i.type==BotCoordinationIntentType::Retreat;}));}
TEST(PlayerBotCoordinationTest,LocalValidationRemainsRequired){EXPECT_TRUE(BotCoordination::evaluate(coordinationPolicy(),coordinationObservation()).localTargetValidationRequired);}
TEST(PlayerBotCoordinationTest,InvalidGroupFailsWithoutAction){auto p=coordinationPolicy();p.id=0;auto d=BotCoordination::evaluate(p,coordinationObservation());EXPECT_EQ(BotCoordinationState::Failed,d.state);EXPECT_FALSE(d.directGameplayAction);}
TEST(PlayerBotCoordinationTest,LeaderChangeInvalidatesStaleLeaderIntents){auto p=coordinationPolicy();p.configuredLeader=10;auto d=BotCoordination::evaluate(p,coordinationObservation(),20,0);EXPECT_TRUE(d.previousLeaderIntentsInvalidated);EXPECT_EQ(10,d.leaderId);}
TEST(PlayerBotCoordinationTest,VocationIncompatibleRoleIsRejected){auto p=coordinationPolicy();p.configuredRoles={{10,BotCoordinationRole::Healer}};EXPECT_EQ(BotCoordinationFailure::RoleIncompatible,BotCoordination::evaluate(p,coordinationObservation()).failure);}
TEST(PlayerBotCoordinationTest,DuplicateExclusiveRoleIsRejected){auto p=coordinationPolicy();p.configuredRoles={{10,BotCoordinationRole::Carrier},{30,BotCoordinationRole::Carrier}};EXPECT_EQ(BotCoordinationFailure::RoleIncompatible,BotCoordination::evaluate(p,coordinationObservation()).failure);}
TEST(PlayerBotCoordinationTest,DuplicateTargetWinnerIsStableAcrossInsertionOrder){auto p=coordinationPolicy();std::vector<BotCoordinationReservation>a,b;EXPECT_EQ(BotCoordinationFailure::None,BotCoordination::reserve(a,reservation(20),p,0));EXPECT_EQ(BotCoordinationFailure::None,BotCoordination::reserve(a,reservation(10),p,0));EXPECT_EQ(BotCoordinationFailure::None,BotCoordination::reserve(b,reservation(10),p,0));EXPECT_EQ(BotCoordinationFailure::Conflict,BotCoordination::reserve(b,reservation(20),p,0));ASSERT_EQ(1,a.size());ASSERT_EQ(1,b.size());EXPECT_EQ(a,b);EXPECT_EQ(10,a.front().memberId);}
TEST(PlayerBotCoordinationTest,IndividualSurvivalRemainsAuthoritative){auto o=coordinationObservation();o.members[0].healthPercent=1;auto d=BotCoordination::evaluate(coordinationPolicy(),o);EXPECT_TRUE(d.individualSurvivalAuthoritative);EXPECT_FALSE(d.directGameplayAction);}
TEST(PlayerBotCoordinationTest,RegroupAttemptsAreFinite){auto p=coordinationPolicy();p.configuredLeader=10;p.maximumRegroupAttempts=1;EXPECT_EQ(BotCoordinationFailure::BudgetReached,BotCoordination::evaluate(p,coordinationObservation(),20,0,1).failure);}
TEST(PlayerBotCoordinationTest,FormationOffsetsAreBounded){auto p=coordinationPolicy();p.maximumFormationOffset=1;auto d=BotCoordination::evaluate(p,coordinationObservation());for(const auto&i:d.intents){EXPECT_LE(std::abs(i.offsetX),1);EXPECT_LE(std::abs(i.offsetY),1);}}
TEST(PlayerBotCoordinationTest,CoordinationEventBudgetIsEnforced){auto p=coordinationPolicy();p.maximumEventsPerTick=2;EXPECT_EQ(BotCoordinationFailure::BudgetReached,BotCoordination::evaluate(p,coordinationObservation()).failure);}
TEST(PlayerBotCoordinationTest,StalePeerObservationIsRejected){auto o=coordinationObservation();o.members[0].revision=0;EXPECT_EQ(BotCoordinationFailure::StaleObservation,BotCoordination::evaluate(coordinationPolicy(),o).failure);}
TEST(PlayerBotCoordinationTest,MemberObservationBoundIsEnforced){auto p=coordinationPolicy();p.maximumMemberObservations=2;EXPECT_EQ(BotCoordinationFailure::MemberLimit,BotCoordination::evaluate(p,coordinationObservation()).failure);}
TEST(PlayerBotCoordinationTest,DisappearedTargetInvalidatesReservation){std::vector<BotCoordinationReservation>r{reservation()};BotCoordination::invalidateTarget(r,BotCoordinationReservationType::CombatTarget,99);EXPECT_TRUE(r.empty());}
TEST(PlayerBotCoordinationTest,InvalidAbsoluteBoundsAreRejected){auto p=coordinationPolicy();p.maximumFormationOffset=BotCoordination::AbsoluteMaximumFormationOffset+1;EXPECT_EQ(BotCoordinationFailure::BudgetReached,BotCoordination::validate(p));}
TEST(PlayerBotCoordinationTest,TeardownValueStateCanBeClearedWithoutCallbacks){BotCoordinationGroupState state{.policy=coordinationPolicy(),.reservations={reservation()}};state.reservations.clear();EXPECT_TRUE(state.reservations.empty());EXPECT_EQ(0,state.leaderId);}

namespace { BotFleetPopulationPolicy fleetPolicy(){return{.desiredOnline=2,.minimumOnline=1,.maximumOnline=3,.absoluteHardMaximum=4,.maximumLoginsPerInterval=1,.maximumLogoutsPerInterval=1,.maximumPendingLogins=1,.maximumPendingLogouts=1,.maximumRetries=3,.revision=1};} std::vector<BotFleetMemberProfile> fleetMembers(){return{{.id=2,.name="two",.priority=2},{.id=1,.name="one",.allowedRegionIds={7},.priority=1},{.id=3,.name="three",.priority=3}};} BotFleetDistributionPolicy fleetDistribution(){return{};} }
TEST(PlayerBotFleetTest,ContractsContainValuesOnly){static_assert(std::is_trivially_copyable_v<BotFleetObservation>);SUCCEED();}
TEST(PlayerBotFleetTest,ValidPopulationPolicy){EXPECT_EQ(BotFleetFailure::None,BotFleet::validate(fleetPolicy()));}
TEST(PlayerBotFleetTest,MinimumGreaterThanDesiredIsRejected){auto p=fleetPolicy();p.minimumOnline=3;EXPECT_EQ(BotFleetFailure::InvalidPolicy,BotFleet::validate(p));}
TEST(PlayerBotFleetTest,DesiredGreaterThanMaximumIsRejected){auto p=fleetPolicy();p.desiredOnline=4;EXPECT_EQ(BotFleetFailure::InvalidPolicy,BotFleet::validate(p));}
TEST(PlayerBotFleetTest,MaximumGreaterThanHardMaximumIsRejected){auto p=fleetPolicy();p.maximumOnline=5;EXPECT_EQ(BotFleetFailure::InvalidPolicy,BotFleet::validate(p));}
TEST(PlayerBotFleetTest,ArithmeticOverflowIsSaturated){EXPECT_EQ(std::numeric_limits<uint64_t>::max(),BotFleet::retryAt(std::numeric_limits<uint64_t>::max(),2,64));}
TEST(PlayerBotFleetTest,DuplicateMemberIsRejected){auto m=fleetMembers();m.push_back(m.front());EXPECT_EQ(BotFleetFailure::DuplicateMember,BotFleet::validate(fleetDistribution(),m));}
TEST(PlayerBotFleetTest,DeficitSchedulesDeterministicBoundedLogin){BotFleetControllerStateValue c;auto r=BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),fleetMembers(),{},0);ASSERT_EQ(1,r.requests.size());EXPECT_EQ(1,r.requests[0].memberId);EXPECT_EQ(7,r.requests[0].regionIntent);}
TEST(PlayerBotFleetTest,LoadingMemberIsNotSelectedTwice){BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{.id=1,.state=BotFleetMemberState::Loading,.observationRevision=1}};auto r=BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),fleetMembers(),o,0);EXPECT_TRUE(r.requests.empty());}
TEST(PlayerBotFleetTest,DisabledAndAlwaysOfflineMembersAreExcluded){BotFleetControllerStateValue c;auto m=fleetMembers();m[1].enabled=false;m[0].alwaysOffline=true;auto r=BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),m,{},0);ASSERT_EQ(1,r.requests.size());EXPECT_EQ(3,r.requests[0].memberId);}
TEST(PlayerBotFleetTest,DuplicateAndHumanAreNeverSelected){BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{.id=1,.duplicateSession=true},{.id=2,.ordinaryHuman=true}};auto r=BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),fleetMembers(),o,0);ASSERT_EQ(1,r.requests.size());EXPECT_EQ(3,r.requests[0].memberId);}
TEST(PlayerBotFleetTest,ExcessSchedulesSafeLogout){BotFleetControllerStateValue c;auto p=fleetPolicy();p.desiredOnline=1;std::vector<BotFleetObservation>o{{1,BotFleetMemberState::Placed,1},{2,BotFleetMemberState::Placed,1}};auto r=BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),o,0);ASSERT_EQ(1,r.requests.size());EXPECT_EQ(BotFleetLifecycleRequestType::Logout,r.requests[0].type);EXPECT_EQ(2,r.requests[0].memberId);}
TEST(PlayerBotFleetTest,UnsafeBoundaryBlocksLogout){BotFleetControllerStateValue c;auto p=fleetPolicy();p.desiredOnline=0;p.minimumOnline=0;std::vector<BotFleetObservation>o{{.id=1,.state=BotFleetMemberState::Placed,.observationRevision=1,.safeLogoutBoundary=false}};EXPECT_TRUE(BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),o,0).requests.empty());}
TEST(PlayerBotFleetTest,PauseSchedulesNothing){BotFleetControllerStateValue c;BotFleet::pause(c);EXPECT_TRUE(BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),fleetMembers(),{},0).requests.empty());}
TEST(PlayerBotFleetTest,ResumeRequiresFreshObservation){BotFleetControllerStateValue c;c.lastObservationRevision=9;BotFleet::resume(c);EXPECT_EQ(0,c.lastObservationRevision);}
TEST(PlayerBotFleetTest,DrainSchedulesNoLoginAndGradualLogout){BotFleetControllerStateValue c;BotFleet::drain(c);std::vector<BotFleetObservation>o{{1,BotFleetMemberState::Placed,1},{2,BotFleetMemberState::Placed,1}};auto r=BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),fleetMembers(),o,0);ASSERT_EQ(1,r.requests.size());EXPECT_EQ(BotFleetLifecycleRequestType::Logout,r.requests[0].type);}
TEST(PlayerBotFleetTest,StartupDelayPreventsStorm){BotFleetControllerStateValue c;c.startedAtTick=10;auto p=fleetPolicy();p.startupDelayTicks=5;EXPECT_EQ(BotFleetFailure::StartupDelay,BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),{},14).failure);}
TEST(PlayerBotFleetTest,OverloadBlocksLoginButPermitsDrain){BotFleetControllerStateValue c;EXPECT_TRUE(BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),fleetMembers(),{},0,true).requests.empty());BotFleet::drain(c);std::vector<BotFleetObservation>o{{1,BotFleetMemberState::Placed,1}};EXPECT_EQ(1,BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),fleetMembers(),o,0,true).requests.size());}
TEST(PlayerBotFleetTest,RetriesAreFiniteAndBackoffCapped){EXPECT_EQ(64,BotFleet::retryAt(0,20,64));BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{.id=1,.state=BotFleetMemberState::Failed,.loginFailures=3},{.id=2,.state=BotFleetMemberState::Failed,.loginFailures=3}};auto r=BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),fleetMembers(),o,0);ASSERT_EQ(1,r.requests.size());EXPECT_EQ(3,r.requests[0].memberId);}
TEST(PlayerBotFleetTest,TeardownClearsPendingLifecycleState){BotFleetControllerStateValue c;c.requestSequence=9;c.draining=true;BotFleet::clear(c);EXPECT_EQ(0,c.requestSequence);EXPECT_FALSE(c.draining);}
TEST(PlayerBotFleetTest,IdenticalSnapshotsAreDeterministic){BotFleetControllerStateValue a,b;EXPECT_EQ(BotFleet::reconcile(a,fleetPolicy(),fleetDistribution(),fleetMembers(),{},0).requests,BotFleet::reconcile(b,fleetPolicy(),fleetDistribution(),fleetMembers(),{},0).requests);}
TEST(PlayerBotFleetTest,DistributionOrderingFillsVocationDeficitDeterministically){auto m=fleetMembers();m[0].vocationCategory=2;m[1].vocationCategory=1;m[2].vocationCategory=2;auto d=fleetDistribution();d.limits={{BotFleetDistributionDimension::Vocation,2,1,1,2}};BotFleetControllerStateValue c;auto r=BotFleet::reconcile(c,fleetPolicy(),d,m,{},0);ASSERT_EQ(1,r.requests.size());EXPECT_EQ(2,r.requests[0].memberId);}
TEST(PlayerBotFleetTest,LevelRangeMaximumIsEnforced){auto m=fleetMembers();for(auto&x:m)x.levelRangeCategory=1;auto d=fleetDistribution();d.limits={{BotFleetDistributionDimension::LevelRange,1,0,0,1}};BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{.id=1,.state=BotFleetMemberState::Placed,.observationRevision=1}};EXPECT_TRUE(BotFleet::reconcile(c,fleetPolicy(),d,m,o,0).requests.empty());}
TEST(PlayerBotFleetTest,RoleAndGroupDistributionAreStable){auto m=fleetMembers();m[0].roleId=4;m[0].coordinationGroupId=9;m[1].roleId=3;m[1].coordinationGroupId=8;auto d=fleetDistribution();d.limits={{BotFleetDistributionDimension::Role,4,1,1,1},{BotFleetDistributionDimension::CoordinationGroup,9,1,1,1}};BotFleetControllerStateValue c;EXPECT_EQ(2,BotFleet::reconcile(c,fleetPolicy(),d,m,{},0).requests.front().memberId);}
TEST(PlayerBotFleetTest,RegionMaximumSelectsNextAllowedIntent){auto m=fleetMembers();m[1].allowedRegionIds={7,8};auto d=fleetDistribution();d.limits={{BotFleetDistributionDimension::Region,7,0,0,0},{BotFleetDistributionDimension::Region,8,0,1,1}};BotFleetControllerStateValue c;auto r=BotFleet::reconcile(c,fleetPolicy(),d,m,{},0);ASSERT_EQ(1,r.requests.size());EXPECT_EQ(8,r.requests.front().regionIntent);}
TEST(PlayerBotFleetTest,ImpossibleDistributionIsExplicit){auto d=fleetDistribution();d.limits={{BotFleetDistributionDimension::Vocation,1,2,1,3}};EXPECT_EQ(BotFleetFailure::InvalidPolicy,BotFleet::validate(d,fleetMembers()));}
TEST(PlayerBotFleetTest,PendingLoginBudgetIsEnforced){auto p=fleetPolicy();p.maximumPendingLogins=1;BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{.id=3,.state=BotFleetMemberState::PlacementPending,.observationRevision=1}};EXPECT_TRUE(BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),o,0).requests.empty());}
TEST(PlayerBotFleetTest,PendingLogoutBudgetIsEnforced){auto p=fleetPolicy();p.desiredOnline=0;p.minimumOnline=0;BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{.id=1,.state=BotFleetMemberState::Placed,.observationRevision=1},{.id=2,.state=BotFleetMemberState::LogoutPending,.observationRevision=1}};EXPECT_TRUE(BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),o,0).requests.empty());}
TEST(PlayerBotFleetTest,PlacedMemberIsCountedOnce){BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{.id=1,.state=BotFleetMemberState::Placed,.observationRevision=1}};EXPECT_EQ(1,BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),fleetMembers(),o,0).placed);}
TEST(PlayerBotFleetTest,DuplicateObservationFailsExplicitly){BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{.id=1},{.id=1}};EXPECT_EQ(BotFleetFailure::DuplicateMember,BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),fleetMembers(),o,0).failure);}
TEST(PlayerBotFleetTest,HardMaximumCannotBeBypassedAtRuntime){auto p=fleetPolicy();p.absoluteHardMaximum=1;p.maximumOnline=1;p.desiredOnline=1;p.minimumOnline=0;BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{.id=1,.state=BotFleetMemberState::Placed},{.id=2,.state=BotFleetMemberState::Loading}};EXPECT_EQ(BotFleetFailure::BudgetReached,BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),o,0).failure);}
TEST(PlayerBotFleetTest,OverloadRecoveryUsesHysteresis){auto p=fleetPolicy();p.overloadRecoveryIntervals=2;BotFleetControllerStateValue c;EXPECT_EQ(BotFleetControllerState::Overloaded,BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),{},0,true).state);EXPECT_EQ(BotFleetControllerState::Overloaded,BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),{},1,false).state);EXPECT_NE(BotFleetControllerState::Overloaded,BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),{},2,false).state);}
TEST(PlayerBotFleetTest,OneWakeupValueInvariantIsRepresented){BotFleetControllerStateValue c;c.wakeupPending=true;EXPECT_TRUE(c.wakeupPending);BotFleet::clear(c);EXPECT_FALSE(c.wakeupPending);}
TEST(PlayerBotFleetTest,ShutdownGenerationInvalidatesCallbacks){BotFleetControllerStateValue c;const auto before=c.generation;BotFleet::clear(c);EXPECT_GT(c.generation,before);}
TEST(PlayerBotFleetTest,StoppingSchedulesNoLifecycleWork){BotFleetControllerStateValue c;c.stopping=true;EXPECT_EQ(BotFleetControllerState::Stopping,BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),fleetMembers(),{},0).state);}
TEST(PlayerBotFleetTest,SessionDurationCanForceBoundedLogout){auto p=fleetPolicy();p.desiredOnline=1;p.minimumOnline=0;p.maximumSessionTicks=10;BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{.id=1,.state=BotFleetMemberState::Placed,.observationRevision=1,.placedAtTick=5}};auto r=BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),o,15);ASSERT_EQ(1,r.requests.size());EXPECT_EQ(BotFleetLifecycleRequestType::Logout,r.requests.front().type);}
TEST(PlayerBotFleetTest,LoginRateLimitAllowsOnlyConfiguredCount){auto p=fleetPolicy();p.desiredOnline=3;p.maximumLoginsPerInterval=2;p.maximumPendingLogins=2;BotFleetControllerStateValue c;EXPECT_EQ(2,BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),{},0).requests.size());}
TEST(PlayerBotFleetTest,LogoutRateLimitAllowsOnlyConfiguredCount){auto p=fleetPolicy();p.desiredOnline=0;p.minimumOnline=0;p.maximumLogoutsPerInterval=2;p.maximumPendingLogouts=2;BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{1,BotFleetMemberState::Placed,1},{2,BotFleetMemberState::Placed,1},{3,BotFleetMemberState::Placed,1}};EXPECT_EQ(2,BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),o,0).requests.size());}
TEST(PlayerBotFleetTest,DistributionMinimumBlocksOrdinaryReduction){auto p=fleetPolicy();p.desiredOnline=0;p.minimumOnline=0;auto m=fleetMembers();m[1].vocationCategory=1;auto d=fleetDistribution();d.limits={{BotFleetDistributionDimension::Vocation,1,1,1,1}};BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{.id=1,.state=BotFleetMemberState::Placed,.observationRevision=1}};EXPECT_TRUE(BotFleet::reconcile(c,p,d,m,o,0).requests.empty());}
TEST(PlayerBotFleetTest,DrainTargetIsBoundedAndHonored){BotFleetControllerStateValue c;BotFleet::drain(c,1);std::vector<BotFleetObservation>o{{1,BotFleetMemberState::Placed,1},{2,BotFleetMemberState::Placed,1}};auto r=BotFleet::reconcile(c,fleetPolicy(),fleetDistribution(),fleetMembers(),o,0);ASSERT_EQ(1,r.requests.size());EXPECT_EQ(BotFleetLifecycleRequestType::Logout,r.requests.front().type);}
TEST(PlayerBotFleetTest,LogoutRetriesAreFinite){auto p=fleetPolicy();p.desiredOnline=0;p.minimumOnline=0;BotFleetControllerStateValue c;std::vector<BotFleetObservation>o{{.id=1,.state=BotFleetMemberState::Placed,.observationRevision=1,.logoutFailures=3}};EXPECT_TRUE(BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),o,0).requests.empty());}
TEST(PlayerBotFleetTest,AbsoluteRetryBoundIsValidated){auto p=fleetPolicy();p.maximumRetries=BotFleet::AbsoluteMaximumRetries+1;EXPECT_EQ(BotFleetFailure::InvalidPolicy,BotFleet::validate(p));}
TEST(PlayerBotFleetTest,DisabledPolicySchedulesNothing){auto p=fleetPolicy();p.enabled=false;BotFleetControllerStateValue c;auto r=BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),{},0);EXPECT_EQ(BotFleetControllerState::Disabled,r.state);EXPECT_TRUE(r.requests.empty());}
TEST(PlayerBotFleetTest,DrainTimeoutFailsExplicitly){auto p=fleetPolicy();p.drainTimeoutTicks=2;BotFleetControllerStateValue c;BotFleet::drain(c);std::vector<BotFleetObservation>o{{.id=1,.state=BotFleetMemberState::Placed,.observationRevision=1,.safeLogoutBoundary=false}};(void)BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),o,1);EXPECT_EQ(BotFleetFailure::DrainTimeout,BotFleet::reconcile(c,p,fleetDistribution(),fleetMembers(),o,4).failure);}
