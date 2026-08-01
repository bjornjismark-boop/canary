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
