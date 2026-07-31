/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include <gtest/gtest.h>

#include "creatures/players/bots/bot_manager.hpp"
#include "creatures/players/bots/bot_navigation.hpp"
#include "creatures/combat/combat.hpp"
#include "creatures/players/player.hpp"
#include "creatures/monsters/monster.hpp"
#include "creatures/monsters/monsters.hpp"
#include "creatures/npcs/npc.hpp"
#include "creatures/npcs/npcs.hpp"
#include "database/database.hpp"
#include "game/game.hpp"
#include "items/item.hpp"
#include "io/iologindata.hpp"
#include "io/functions/iologindata_load_player.hpp"
#include "lib/logging/in_memory_logger.hpp"
#include "lua/creature/actions.hpp"
#include "lua/scripts/scripts.hpp"
#include "test_database.hpp"

namespace {
	class ProductionTransitionActionFixture final {
	public:
		ProductionTransitionActionFixture() {
			static const bool coreLoaded = g_scripts().loadEventSchedulerScripts("data/core.lua");
			g_actions().clear();
			loaded = coreLoaded
				&& g_scripts().loadEventSchedulerScripts("data/scripts/actions/doors/custom_door.lua")
				&& g_scripts().loadEventSchedulerScripts("data/scripts/actions/doors/level_door.lua")
				&& g_scripts().loadEventSchedulerScripts("data/scripts/actions/items/ladder_up.lua");
		}

		~ProductionTransitionActionFixture() {
			g_actions().clear();
		}

		[[nodiscard]] bool isLoaded() const { return loaded; }

	private:
		bool loaded = false;
	};

	class RemovalCountingCreature final : public Creature {
	public:
		const std::string &getName() const override { return name; }
		const std::string &getTypeName() const override { return name; }
		const std::string &getNameDescription() const override { return name; }
		std::string getDescription(int32_t) override { return name; }
		CreatureType_t getType() const override { return CREATURETYPE_MONSTER; }
		void setID() override { id = 0x40000001; }
		void removeList() override { }
		void addList() override { }

		void onRemoveCreature(const std::shared_ptr<Creature> &creature, bool isLogout) override {
			Creature::onRemoveCreature(creature, isLogout);
			if (creature == observedCreature.lock()) {
				++observedRemovalCount;
			}
		}

		void onCreatureMove(const std::shared_ptr<Creature> &creature, const std::shared_ptr<Tile> &newTile, const Position &newPos, const std::shared_ptr<Tile> &oldTile, const Position &oldPos, bool teleport) override {
			Creature::onCreatureMove(creature, newTile, newPos, oldTile, oldPos, teleport);
			if (creature == observedCreature.lock()) {
				++observedMovementCount;
			}
		}

		std::weak_ptr<Creature> observedCreature;
		size_t observedRemovalCount = 0;
		size_t observedMovementCount = 0;

	private:
		std::string name = "PlayerBotRemovalObserver";
	};

	class PlayerBotDatabaseFixture final {
	public:
		explicit PlayerBotDatabaseFixture(Database &database, std::optional<Position> requestedStart = std::nullopt) :
			database(database) {
			TestDatabase::init();
			TestDatabase::requireDisposableDatabase(database);

			static std::atomic<uint32_t> sequence { 0 };
			const auto suffix = static_cast<uint32_t>(
				std::chrono::steady_clock::now().time_since_epoch().count() & 0x0FFFFFFF
			) + sequence.fetch_add(1);
			name = fmt::format("PlayerBot{}", suffix);
			start = requestedStart.value_or(Position(
				static_cast<uint16_t>(30000U + suffix % 1000U),
				static_cast<uint16_t>(30000U + (suffix / 1000U) % 1000U),
				7
			));

			const auto escapedName = database.escapeString(name);
			if (!database.executeQuery(fmt::format(
					"INSERT INTO `accounts` (`name`, `email`, `password`) VALUES ({}, {}, '')",
					escapedName,
					database.escapeString(name + "@test.invalid")
				))) {
				throw std::runtime_error("Failed to create PlayerBots test account.");
			}
			accountId = lastInsertId();

			if (!database.executeQuery(fmt::format(
					"INSERT INTO `players` "
					"(`name`, `account_id`, `group_id`, `vocation`, `town_id`, `conditions`, `posx`, `posy`, `posz`, `deletion`) "
					"VALUES ({}, {}, 1, 1, 1, X'', {}, {}, {}, 0)",
					escapedName,
					accountId,
					start.x,
					start.y,
					start.z
				))) {
				cleanup();
				throw std::runtime_error("Failed to create PlayerBots test player.");
			}
			playerId = lastInsertId();
		}

		~PlayerBotDatabaseFixture() noexcept {
			if (!cleanup()) {
				std::fprintf(stderr, "[PlayerBotDatabaseFixture] cleanup failed for account %u, player %u\n", accountId, playerId);
			}
		}

		PlayerBotDatabaseFixture(const PlayerBotDatabaseFixture &) = delete;
		PlayerBotDatabaseFixture &operator=(const PlayerBotDatabaseFixture &) = delete;

		bool cleanup() noexcept {
			if (accountId == 0) {
				return true;
			}
			// players.account_id and all rows produced by IOLoginData::savePlayer are
			// covered by the schema's ON DELETE CASCADE chain from accounts.
			const bool removed = database.executeQuery(fmt::format("DELETE FROM `accounts` WHERE `id` = {}", accountId));
			if (removed) {
				accountId = 0;
				playerId = 0;
			}
			return removed;
		}

		bool hasCommittedRows() const {
			return database.storeQuery(fmt::format(
				"SELECT `id` FROM `accounts` WHERE `name` = {}",
				database.escapeString(name)
			)) != nullptr
				|| database.storeQuery(fmt::format(
					   "SELECT `id` FROM `players` WHERE `name` = {}",
					   database.escapeString(name)
				   )) != nullptr;
		}

		Position persistedPosition() const {
			const auto result = database.storeQuery(fmt::format(
				"SELECT `posx`, `posy`, `posz` FROM `players` WHERE `id` = {}",
				playerId
			));
			if (!result) {
				throw std::runtime_error("Failed to read PlayerBots persisted position.");
			}
			return Position(
				result->getNumber<uint16_t>("posx"),
				result->getNumber<uint16_t>("posy"),
				result->getNumber<uint8_t>("posz")
			);
		}

		Database &database;
		uint32_t accountId = 0;
		uint32_t playerId = 0;
		std::string name;
		Position start;

	private:
		uint32_t lastInsertId() const {
			const auto result = database.storeQuery("SELECT LAST_INSERT_ID() AS `id`");
			if (!result) {
				throw std::runtime_error("Failed to read PlayerBots fixture ID.");
			}
			return result->getNumber<uint32_t>("id");
		}
	};

	void createWalkableTile(const Position &position) {
		auto town = g_game().map.towns.getOrCreateTown(1);
		town->setName("PlayerBotTestTown");
		town->setTemplePos(position);

		auto tile = g_game().map.getOrCreateTile(position, true);
		ASSERT_NE(nullptr, tile);
		if (!tile->getGround()) {
			auto ground = Item::CreateItem(4526);
			ASSERT_NE(nullptr, ground);
			tile->internalAddThing(ground);
		}
	}

	std::shared_ptr<const BotSession> loginBotOrReport(BotManager &manager, const std::string &name) {
		auto session = manager.login(name);
		if (session) {
			return session;
		}

		const auto *logger = dynamic_cast<const InMemoryLogger *>(&g_logger());
		if (!logger) {
			std::fprintf(stderr, "[PlayerBotIntegrationTest] login failed; test logger is unavailable\n");
			return nullptr;
		}
		std::fprintf(stderr, "[PlayerBotIntegrationTest] login failed; captured logger output:\n");
		for (size_t index = 0; index < logger->logCount(); ++index) {
			const auto &[level, message] = logger->getLogEntry(index);
			std::fprintf(stderr, "  [%s] %s\n", level.c_str(), message.c_str());
		}
		std::fflush(stderr);
		return nullptr;
	}
}

TEST(PlayerBotIntegrationTest, LocalWalkabilityUsesRealWorldStateAndRevalidatesBeforeMovement) {
	PlayerBotDatabaseFixture fixture(g_database());
	for (uint8_t rawDirection = DIRECTION_NORTH; rawDirection <= DIRECTION_LAST; ++rawDirection) {
		createWalkableTile(getNextPosition(static_cast<Direction>(rawDirection), fixture.start));
	}
	createWalkableTile(fixture.start);
	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	const auto player = std::const_pointer_cast<Player>(session->getPlayer());
	ASSERT_NE(nullptr, player);

	for (uint8_t rawDirection = DIRECTION_NORTH; rawDirection <= DIRECTION_LAST; ++rawDirection) {
		const auto result = manager.assess(fixture.name, static_cast<Direction>(rawDirection));
		EXPECT_TRUE(result.walkable()) << static_cast<unsigned>(rawDirection);
		EXPECT_EQ(getNextPosition(static_cast<Direction>(rawDirection), fixture.start), result.candidate.destination);
	}

	const Position east = getNextPosition(DIRECTION_EAST, fixture.start);
	const auto eastTile = g_game().map.getTile(east);
	ASSERT_NE(nullptr, eastTile);
	const auto blockingItem = Item::CreateItem(1025);
	ASSERT_NE(nullptr, blockingItem);
	ASSERT_TRUE(blockingItem->isBlocking());
	eastTile->internalAddThing(blockingItem);
	const auto itemBlocked = manager.assess(fixture.name, DIRECTION_EAST);
	EXPECT_EQ(BotWalkability::BlockedByItem, itemBlocked.outcome);
	EXPECT_EQ(blockingItem->getID(), itemBlocked.blockingItemTypeId);
	eastTile->removeThing(blockingItem, 1);

	auto blocker = std::make_shared<RemovalCountingCreature>();
	blocker->setID();
	ASSERT_TRUE(g_game().placeCreature(blocker, east, false, true));
	const auto creatureBlocked = manager.assess(fixture.name, DIRECTION_EAST);
	EXPECT_EQ(BotWalkability::BlockedByCreature, creatureBlocked.outcome);
	EXPECT_EQ(blocker->getID(), creatureBlocked.blockingCreatureId);
	ASSERT_TRUE(g_game().removeCreature(blocker, true));
	blocker.reset();
	EXPECT_EQ(BotWalkability::Walkable, manager.assess(fixture.name, DIRECTION_EAST).outcome);

	const auto field = Item::CreateItem(ITEM_FIREFIELD_PVP_FULL);
	ASSERT_NE(nullptr, field);
	eastTile->internalAddThing(field);
	const auto hazardous = manager.assess(fixture.name, DIRECTION_EAST);
	EXPECT_EQ(BotWalkability::WalkableWithRisk, hazardous.outcome);
	EXPECT_GT(hazardous.movementCost, BotNavigation::CardinalCost);
	eastTile->removeThing(field, 1);

	const auto previouslyWalkable = manager.assess(fixture.name, DIRECTION_EAST);
	ASSERT_TRUE(previouslyWalkable.walkable());
	const auto lateBlocker = Item::CreateItem(1025);
	ASSERT_NE(nullptr, lateBlocker);
	eastTile->internalAddThing(lateBlocker);
	const auto stale = manager.executeMovement(fixture.name, previouslyWalkable, std::chrono::milliseconds(1000));
	ASSERT_TRUE(stale.walkability.has_value());
	EXPECT_EQ(BotActionStatus::Rejected, stale.status);
	EXPECT_EQ(BotActionFailure::StaleObservation, stale.failure);
	EXPECT_EQ(BotWalkability::StaleObservation, stale.walkability->outcome);
	EXPECT_EQ(fixture.start, player->getPosition());
	eastTile->removeThing(lateBlocker, 1);
	const auto refreshed = manager.assess(fixture.name, DIRECTION_EAST);
	ASSERT_TRUE(refreshed.walkable());
	const auto moved = manager.executeMovement(fixture.name, refreshed, std::chrono::milliseconds(2000));
	ASSERT_TRUE(moved.walkability.has_value());
	EXPECT_EQ(BotActionStatus::Succeeded, moved.status);
	EXPECT_EQ(BotWalkability::Walkable, moved.walkability->outcome);
	EXPECT_EQ(east, player->getPosition());
	EXPECT_EQ(fixture.start.z, player->getPosition().z);

	const auto observation = BotPerception::observe(player);
	ASSERT_TRUE(observation.has_value());
	EXPECT_EQ(BotWalkability::OutsideKnownOrVisibleArea, BotNavigation::assess(*observation, Position(fixture.start.x + 9, fixture.start.y, fixture.start.z)).outcome);

	EXPECT_TRUE(manager.logout(fixture.name, false));
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, BoundedRouteExecutesRepathsAndCancelsWithRealWorldState) {
	PlayerBotDatabaseFixture fixture(g_database());
	for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 4; ++x) {
		createWalkableTile(Position(static_cast<uint16_t>(fixture.start.x + x), static_cast<uint16_t>(fixture.start.y + y), fixture.start.z));
	}
	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	const auto player = std::const_pointer_cast<Player>(session->getPlayer());
	const Position destination(fixture.start.x + 3, fixture.start.y, fixture.start.z);

	auto started = manager.startRoute(fixture.name, destination, std::chrono::milliseconds(1000));
	ASSERT_EQ(BotRouteState::Ready, started.state);
	auto blocker = std::make_shared<RemovalCountingCreature>();
	blocker->setID();
	ASSERT_TRUE(g_game().placeCreature(blocker, Position(fixture.start.x + 1, fixture.start.y, fixture.start.z), false, true));
	auto blocked = manager.advanceRoute(fixture.name, std::chrono::milliseconds(1100));
	EXPECT_EQ(BotRouteState::Backoff, blocked.state);
	EXPECT_EQ(BotRouteReason::DynamicBlocker, blocked.reason);
	EXPECT_EQ(fixture.start, player->getPosition());
	ASSERT_TRUE(g_game().removeCreature(blocker, true));
	blocker.reset();

	auto progress = manager.advanceRoute(fixture.name, blocked.backoffDeadline);
	EXPECT_EQ(BotRouteState::ReplanRequired, progress.state);
	EXPECT_NE(fixture.start, player->getPosition());
	for (uint32_t step = 1; step < 8 && progress.state != BotRouteState::Arrived; ++step) {
		progress = manager.advanceRoute(fixture.name, std::chrono::milliseconds(2000 + step * 1000));
	}
	EXPECT_EQ(BotRouteState::Arrived, progress.state);
	EXPECT_EQ(destination, player->getPosition());
	EXPECT_EQ(fixture.start.z, player->getPosition().z);
	EXPECT_EQ(0U, progress.consecutiveNoProgress);

	EXPECT_EQ(BotRouteState::Ready, manager.startRoute(fixture.name, fixture.start, std::chrono::milliseconds(12000)).state);
	EXPECT_EQ(BotRouteState::Cancelled, manager.cancelRoute(fixture.name).state);
	EXPECT_TRUE(manager.logout(fixture.name, false));
	EXPECT_EQ(nullptr, session->getRouteProgress());
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, ProductionLadderTransitionIsObservedAndInvalidatesOldRoute) {
	ProductionTransitionActionFixture actionFixture;
	ASSERT_TRUE(actionFixture.isLoaded());
	PlayerBotDatabaseFixture fixture(g_database());
	PlayerBotDatabaseFixture spectatorFixture(g_database(), Position(fixture.start.x, fixture.start.y + 1, fixture.start.z));
	const Position ladderPosition(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	const Position destination(ladderPosition.x, ladderPosition.y + 1, ladderPosition.z - 1);
	createWalkableTile(fixture.start);
	createWalkableTile(spectatorFixture.start);
	createWalkableTile(ladderPosition);
	createWalkableTile(destination);
	const auto ladderTile = g_game().map.getTile(ladderPosition);
	const auto ladder = Item::CreateItem(1948);
	ASSERT_NE(nullptr, ladder);
	ladderTile->internalAddThing(ladder);

	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	const auto spectatorSession = loginBotOrReport(manager, spectatorFixture.name);
	ASSERT_NE(nullptr, spectatorSession);
	const auto player = std::const_pointer_cast<Player>(session->getPlayer());
	auto callbackObserver = std::make_shared<RemovalCountingCreature>();
	callbackObserver->setID();
	callbackObserver->observedCreature = player;
	const Position callbackObserverPosition(fixture.start.x - 1, fixture.start.y, fixture.start.z);
	createWalkableTile(callbackObserverPosition);
	ASSERT_TRUE(g_game().placeCreature(callbackObserver, callbackObserverPosition, false, true));
	ASSERT_EQ(BotRouteState::Ready, manager.startRoute(fixture.name, ladderPosition, std::chrono::milliseconds(100)).state);
	BotInteractionTarget target { ladderPosition, static_cast<uint8_t>(ladderTile->getThingIndex(ladder)), 1948, 0, BotInteractionType::UseLadder };
	target.signature = BotInteraction::signature(target);
	BotTransitionRequest request { .target = target, .expectedDestination = destination, .maxAttempts = 2, .timeout = std::chrono::milliseconds(50) };
	const auto accepted = manager.startTransition(fixture.name, request, std::chrono::milliseconds(200));
	EXPECT_EQ(BotTransitionState::AwaitingTransition, accepted.state);
	EXPECT_EQ(destination, player->getPosition());
	EXPECT_GT(callbackObserver->observedMovementCount, 0U);
	EXPECT_EQ(spectatorFixture.start, std::const_pointer_cast<Player>(spectatorSession->getPlayer())->getPosition());
	const auto verified = manager.advanceTransition(fixture.name, std::chrono::milliseconds(201));
	EXPECT_EQ(BotTransitionState::Completed, verified.state);
	EXPECT_EQ(BotInteractionOutcome::TransitionObserved, verified.outcome);
	EXPECT_TRUE(verified.oldRouteInvalidated);
	EXPECT_TRUE(verified.newObservationRequired);
	ASSERT_NE(nullptr, session->getRouteProgress());
	EXPECT_EQ(BotRouteState::ReplanRequired, session->getRouteProgress()->state);
	const auto fresh = BotPerception::observe(player);
	ASSERT_TRUE(fresh.has_value());
	EXPECT_EQ(destination, fresh->position);
	EXPECT_EQ(BotTransitionState::Cancelled, manager.cancelTransition(fixture.name).state);
	ASSERT_TRUE(g_game().removeCreature(callbackObserver, true));
	callbackObserver.reset();
	ladderTile->removeThing(ladder, 1);
	EXPECT_TRUE(manager.logout(fixture.name, false));
	EXPECT_TRUE(manager.logout(spectatorFixture.name, false));
	EXPECT_EQ(nullptr, session->getTransitionProgress());
	ASSERT_TRUE(fixture.cleanup());
	ASSERT_TRUE(spectatorFixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
	EXPECT_FALSE(spectatorFixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, ClosedDoorUsesOrdinaryActionAndRequiresObservedTransformation) {
	ProductionTransitionActionFixture actionFixture;
	ASSERT_TRUE(actionFixture.isLoaded());
	PlayerBotDatabaseFixture fixture(g_database());
	const Position doorPosition(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	const Position destination(fixture.start.x + 2, fixture.start.y, fixture.start.z);
	createWalkableTile(fixture.start);
	createWalkableTile(doorPosition);
	createWalkableTile(destination);
	const auto doorTile = g_game().map.getTile(doorPosition);
	const auto door = Item::CreateItem(1638);
	ASSERT_NE(nullptr, door);
	ASSERT_NE(nullptr, door->getDoor());
	doorTile->internalAddThing(door);
	const auto stack = static_cast<uint8_t>(doorTile->getThingIndex(door));
	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	BotInteractionTarget target { doorPosition, stack, 1638, 0, BotInteractionType::UseDoor };
	target.signature = BotInteraction::signature(target);
	BotTransitionRequest request { .target = target, .maxAttempts = 2, .timeout = std::chrono::milliseconds(50) };
	const auto accepted = manager.startTransition(fixture.name, request, std::chrono::milliseconds(100));
	EXPECT_EQ(BotTransitionState::AwaitingTransition, accepted.state);
	const auto opened = manager.advanceTransition(fixture.name, std::chrono::milliseconds(101));
	EXPECT_EQ(BotTransitionState::ReplanRequired, opened.state);
	EXPECT_EQ(BotInteractionOutcome::Succeeded, opened.outcome);
	std::shared_ptr<Item> transformed;
	for (const auto &candidate : *doorTile->getItemList()) if (candidate && candidate->getDoor()) { transformed = candidate; break; }
	ASSERT_NE(nullptr, transformed);
	EXPECT_EQ(1639, transformed->getID());
	EXPECT_EQ(fixture.start, std::const_pointer_cast<Player>(session->getPlayer())->getPosition());

	ASSERT_EQ(BotRouteState::Ready, manager.startRoute(fixture.name, destination, std::chrono::milliseconds(200)).state);
	BotRouteProgress continued;
	for (uint32_t step = 1; step <= 8 && continued.state != BotRouteState::Arrived; ++step) {
		continued = manager.advanceRoute(fixture.name, std::chrono::milliseconds(200 + step * 1000));
	}
	EXPECT_EQ(BotRouteState::Arrived, continued.state);
	EXPECT_EQ(destination, std::const_pointer_cast<Player>(session->getPlayer())->getPosition());

	EXPECT_EQ(BotTransitionState::Cancelled, manager.cancelTransition(fixture.name).state);
	const auto stale = manager.startTransition(fixture.name, request, std::chrono::milliseconds(300));
	EXPECT_EQ(BotTransitionState::Failed, stale.state);
	EXPECT_EQ(BotInteractionOutcome::StaleObservation, stale.outcome);
	doorTile->removeThing(transformed, 1);
	EXPECT_TRUE(manager.logout(fixture.name, false));
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, DoorDenialAndDynamicDoorwayBlockerRemainAuthoritative) {
	ProductionTransitionActionFixture actionFixture;
	ASSERT_TRUE(actionFixture.isLoaded());
	PlayerBotDatabaseFixture fixture(g_database());
	const Position customDoorPosition(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	const Position levelDoorPosition(fixture.start.x, fixture.start.y + 1, fixture.start.z);
	createWalkableTile(fixture.start);
	createWalkableTile(customDoorPosition);
	createWalkableTile(levelDoorPosition);
	const auto customDoorTile = g_game().map.getTile(customDoorPosition);
	const auto levelDoorTile = g_game().map.getTile(levelDoorPosition);
	const auto customDoor = Item::CreateItem(1638);
	const auto levelDoor = Item::CreateItem(1646);
	ASSERT_NE(nullptr, customDoor);
	ASSERT_NE(nullptr, levelDoor);
	levelDoor->setAttribute(ItemAttribute_t::ACTIONID, uint16_t { 1100 });
	customDoorTile->internalAddThing(customDoor);
	levelDoorTile->internalAddThing(levelDoor);

	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	BotInteractionTarget levelTarget { levelDoorPosition, static_cast<uint8_t>(levelDoorTile->getThingIndex(levelDoor)), 1646, 0, BotInteractionType::UseDoor };
	levelTarget.signature = BotInteraction::signature(levelTarget);
	BotTransitionRequest levelRequest { .target = levelTarget, .maxAttempts = 1, .timeout = std::chrono::milliseconds(1) };
	const auto levelAccepted = manager.startTransition(fixture.name, levelRequest, std::chrono::milliseconds(100));
	EXPECT_EQ(BotTransitionState::AwaitingTransition, levelAccepted.state);
	EXPECT_EQ(BotInteractionOutcome::Pending, levelAccepted.outcome);
	EXPECT_EQ(1646, levelDoor->getID());
	const auto denied = manager.advanceTransition(fixture.name, std::chrono::milliseconds(101));
	EXPECT_EQ(BotTransitionState::Failed, denied.state);
	EXPECT_EQ(BotInteractionOutcome::AccessDenied, denied.outcome);
	EXPECT_EQ(1646, levelDoor->getID());

	EXPECT_EQ(BotTransitionState::Cancelled, manager.cancelTransition(fixture.name).state);
	auto blocker = std::make_shared<RemovalCountingCreature>();
	blocker->setID();
	ASSERT_TRUE(g_game().placeCreature(blocker, customDoorPosition, false, true));
	BotInteractionTarget customTarget { customDoorPosition, static_cast<uint8_t>(customDoorTile->getThingIndex(customDoor)), 1638, 0, BotInteractionType::UseDoor };
	customTarget.signature = BotInteraction::signature(customTarget);
	BotTransitionRequest customRequest { .target = customTarget, .maxAttempts = 1, .timeout = std::chrono::milliseconds(1) };
	const auto blockerAccepted = manager.startTransition(fixture.name, customRequest, std::chrono::milliseconds(200));
	EXPECT_EQ(BotTransitionState::AwaitingTransition, blockerAccepted.state);
	EXPECT_EQ(BotInteractionOutcome::Pending, blockerAccepted.outcome);
	EXPECT_EQ(1638, customDoor->getID());
	const auto blocked = manager.advanceTransition(fixture.name, std::chrono::milliseconds(201));
	EXPECT_EQ(BotTransitionState::Failed, blocked.state);
	EXPECT_NE(BotInteractionOutcome::Succeeded, blocked.outcome);
	EXPECT_EQ(1638, customDoor->getID());

	ASSERT_TRUE(g_game().removeCreature(blocker, true));
	blocker.reset();
	customDoorTile->removeThing(customDoor, 1);
	levelDoorTile->removeThing(levelDoor, 1);
	EXPECT_TRUE(manager.logout(fixture.name, false));
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, OrdinaryPlayerDoorUseStillDispatchesThroughProductionAction) {
	ProductionTransitionActionFixture actionFixture;
	ASSERT_TRUE(actionFixture.isLoaded());
	PlayerBotDatabaseFixture fixture(g_database());
	const Position doorPosition(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	createWalkableTile(fixture.start);
	createWalkableTile(doorPosition);
	const auto doorTile = g_game().map.getTile(doorPosition);
	const auto door = Item::CreateItem(1638);
	ASSERT_NE(nullptr, door);
	doorTile->internalAddThing(door);
	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	const auto player = std::const_pointer_cast<Player>(session->getPlayer());
	g_game().playerUseItem(player->getID(), doorPosition, static_cast<uint8_t>(doorTile->getThingIndex(door)), 0, 1638);
	EXPECT_EQ(1639, door->getID());
	doorTile->removeThing(door, 1);
	EXPECT_TRUE(manager.logout(fixture.name, false));
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, OrdinaryNetworkPlayerMovementRemainsUnchanged) {
	PlayerBotDatabaseFixture fixture(g_database());
	const Position destination(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	createWalkableTile(fixture.start);
	createWalkableTile(destination);
	const auto player = std::make_shared<Player>();
	player->setName(fixture.name);
	ASSERT_TRUE(IOLoginDataLoad::preLoadPlayer(player, fixture.name));
	ASSERT_TRUE(IOLoginData::loadPlayerById(player, fixture.playerId, false));
	player->setID();
	player->setOnline(true);
	ASSERT_TRUE(player->isNetworkControlled());
	ASSERT_TRUE(g_game().placeCreature(player, fixture.start, false, true));
	EXPECT_EQ(RETURNVALUE_NOERROR, g_game().internalMoveCreature(player, DIRECTION_EAST));
	EXPECT_EQ(destination, player->getPosition());
	player->setOnline(false);
	const std::function<bool(const std::shared_ptr<Player> &)> noSave;
	EXPECT_EQ(ManagedPlayerRemovalResult::Complete, g_game().removeManagedPlayer(player, true, noSave));
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, NavigationValuesOutliveClosedSessionWithoutWorldOwnership) {
	PlayerBotDatabaseFixture fixture(g_database());
	createWalkableTile(fixture.start);
	createWalkableTile(getNextPosition(DIRECTION_EAST, fixture.start));
	BotWalkabilityResult assessment;
	std::weak_ptr<const BotSession> sessionLifetime;
	{
		BotManager manager(g_game());
		auto session = loginBotOrReport(manager, fixture.name);
		ASSERT_NE(nullptr, session);
		sessionLifetime = session;
		assessment = manager.assess(fixture.name, DIRECTION_EAST);
		EXPECT_TRUE(assessment.walkable());
		session.reset();
		EXPECT_TRUE(manager.logout(fixture.name, false));
	}
	EXPECT_TRUE(sessionLifetime.expired());
	EXPECT_FALSE(assessment.containsWorldOwnership());
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, RunsDatabaseToMovementSaveAndRemovalLifecycle) {
	std::fprintf(stderr, "[PlayerBotIntegrationTest] executing lifecycle body\n");
	PlayerBotDatabaseFixture fixture(g_database());
	const Position destination(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	createWalkableTile(fixture.start);
	createWalkableTile(destination);

	size_t removalCallCount = 0;
	BotSessionOperations operations;
		operations.remove = [&removalCallCount](const std::shared_ptr<Player> &player, bool savePlayer, const auto &saveOperation) {
		++removalCallCount;
		const std::function<bool(const std::shared_ptr<Player> &)> noSave;
		return g_game().removeManagedPlayer(player, true, savePlayer ? saveOperation : noSave);
	};
	BotManager manager(g_game(), std::move(operations));
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	const auto player = std::const_pointer_cast<Player>(session->getPlayer());
	ASSERT_NE(nullptr, player);
	EXPECT_TRUE(player->isBotControlled());
	EXPECT_FALSE(player->isDisconnected());
	EXPECT_EQ(fixture.start, player->getPosition());
	EXPECT_EQ(player, g_game().getPlayerByName(fixture.name));
	EXPECT_EQ(nullptr, manager.login(fixture.name));
	EXPECT_EQ(RETURNVALUE_NOERROR, manager.move(fixture.name, DIRECTION_EAST));
	EXPECT_EQ(destination, player->getPosition());
	EXPECT_TRUE(manager.save(fixture.name));

	const auto result = fixture.database.storeQuery(fmt::format(
		"SELECT `posx`, `posy`, `posz` FROM `players` WHERE `id` = {}",
		fixture.playerId
	));
	ASSERT_NE(nullptr, result);
	EXPECT_EQ(destination.x, result->getNumber<uint16_t>("posx"));
	EXPECT_EQ(destination.y, result->getNumber<uint16_t>("posy"));
	EXPECT_EQ(destination.z, result->getNumber<uint8_t>("posz"));

	const auto previousTile = player->getTile();
	const auto guid = player->getGUID();
	ASSERT_NE(nullptr, previousTile);
	const bool logoutSucceeded = manager.logout(fixture.name, false);
	if (!logoutSucceeded) {
		const auto tile = player->getTile();
		std::fprintf(
			stderr,
			"[PlayerBotIntegrationTest] lifecycle removal failed: state=%u removed=%d parent=%d tile=%d position=%s registeredByName=%d registeredByGuid=%d callback=real calls=%zu\n",
			static_cast<unsigned>(session->getState()),
			player->isRemoved(),
			player->getParent() != nullptr,
			tile != nullptr,
			tile ? tile->getPosition().toString().c_str() : "<none>",
			g_game().getPlayerByName(fixture.name) == player,
			g_game().getPlayerByGUID(fixture.playerId) == player,
			removalCallCount
		);
	}
	EXPECT_TRUE(logoutSucceeded);
	EXPECT_EQ(1U, removalCallCount);
	EXPECT_FALSE(manager.logout(fixture.name, false));
	EXPECT_EQ(BotSessionState::Closed, session->getState());
	EXPECT_EQ(0U, manager.size());
	EXPECT_TRUE(player->isRemoved());
	EXPECT_FALSE(player->isOnline());
	EXPECT_EQ(nullptr, g_game().getPlayerByName(fixture.name));
	EXPECT_EQ(nullptr, g_game().getPlayerByGUID(guid));
	EXPECT_EQ(-1, previousTile->getThingIndex(player));
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, ObservesVisibleCreatureByValueAndRejectsStaleId) {
	PlayerBotDatabaseFixture fixture(g_database());
	createWalkableTile(fixture.start);
	const Position observerPosition(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	createWalkableTile(observerPosition);
	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	const auto player = std::const_pointer_cast<Player>(session->getPlayer());
	auto observer = std::make_shared<RemovalCountingCreature>();
	observer->setID();
	ASSERT_TRUE(g_game().placeCreature(observer, observerPosition, false, true));
	std::weak_ptr<Creature> observerLifetime = observer;

	const auto observation = BotPerception::observe(player);
	ASSERT_TRUE(observation.has_value());
	const auto found = std::ranges::find(observation->visibleCreatures, observer->getID(), &BotCreatureObservation::id);
	ASSERT_NE(observation->visibleCreatures.end(), found);
	EXPECT_EQ(observerPosition, found->position);
	const BotAction pendingAction { BotActionType::InspectCreature, BotActionReason::ObserveTarget, player->getID() };
	const auto pending = manager.execute(fixture.name, pendingAction, std::chrono::milliseconds(1000));
	EXPECT_EQ(BotActionStatus::Pending, pending.status);
	const auto noSpam = manager.execute(fixture.name, pendingAction, std::chrono::milliseconds(1250));
	EXPECT_EQ(BotActionStatus::Pending, noSpam.status);
	EXPECT_EQ(1U, noSpam.attempts);
	const auto retry = manager.execute(fixture.name, pendingAction, std::chrono::milliseconds(2000));
	EXPECT_EQ(BotActionStatus::RetryScheduled, retry.status);
	EXPECT_EQ(BotActionFailure::TimedOut, retry.failure);
	EXPECT_EQ(std::chrono::milliseconds(200), retry.retryAfter);
	const auto staleId = observer->getID();
	ASSERT_TRUE(g_game().removeCreature(observer, true));
	observer.reset();
	EXPECT_TRUE(observerLifetime.expired());

	const auto staleResult = manager.execute(
		fixture.name,
		BotAction { BotActionType::InspectCreature, BotActionReason::ObserveTarget, staleId },
		std::chrono::milliseconds(3000)
	);
	EXPECT_EQ(BotActionStatus::Rejected, staleResult.status);
	EXPECT_EQ(BotActionFailure::InvalidTarget, staleResult.failure);
	EXPECT_TRUE(manager.logout(fixture.name, false));
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, TickAndActionRatesAreBoundedAndDoNotSpamPendingAction) {
	PlayerBotDatabaseFixture fixture(g_database());
	createWalkableTile(fixture.start);
	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);

	const auto first = manager.tick(fixture.name, std::chrono::milliseconds(1000));
	EXPECT_EQ(BotActionStatus::Succeeded, first.status);
	const auto rateLimited = manager.tick(fixture.name, std::chrono::milliseconds(1050));
	EXPECT_EQ(BotActionFailure::RateLimited, rateLimited.failure);
	ASSERT_NE(nullptr, session->getBlackboard());
	EXPECT_FALSE(session->getBlackboard()->pendingAction.has_value());
	EXPECT_TRUE(manager.logout(fixture.name, false));
	const auto closed = manager.tick(fixture.name, std::chrono::milliseconds(2000));
	EXPECT_EQ(BotActionFailure::InvalidLifecycle, closed.failure);
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, SaveEnabledLogoutUsesOnlyInjectedSaveOperation) {
	PlayerBotDatabaseFixture fixture(g_database());
	const Position destination(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	createWalkableTile(fixture.start);
	createWalkableTile(destination);

	size_t saveCallCount = 0;
	BotSessionOperations operations;
	operations.save = [&saveCallCount, destination](const std::shared_ptr<Player> &player) {
		++saveCallCount;
		EXPECT_EQ(destination, player->getLoginPosition());
		EXPECT_TRUE(player->isRemoved());
		const auto tile = player->getTile();
		if (tile) {
			EXPECT_EQ(-1, tile->getThingIndex(player));
		}
		EXPECT_EQ(nullptr, g_game().getPlayerByName(player->getName()));
		return true;
	};
	BotManager manager(g_game(), std::move(operations));
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	ASSERT_EQ(RETURNVALUE_NOERROR, manager.move(fixture.name, DIRECTION_EAST));

	EXPECT_TRUE(manager.logout(fixture.name, true));
	EXPECT_EQ(1U, saveCallCount);
	EXPECT_EQ(BotSessionState::Closed, session->getState());
	EXPECT_EQ(0U, manager.size());
	EXPECT_EQ(fixture.start, fixture.persistedPosition());

	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, NoSaveLogoutPerformsNoPersistence) {
	PlayerBotDatabaseFixture fixture(g_database());
	const Position destination(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	createWalkableTile(fixture.start);
	createWalkableTile(destination);

	size_t saveCallCount = 0;
	BotSessionOperations operations;
	operations.save = [&saveCallCount](const std::shared_ptr<Player> &) {
		++saveCallCount;
		return true;
	};
	BotManager manager(g_game(), std::move(operations));
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	ASSERT_EQ(RETURNVALUE_NOERROR, manager.move(fixture.name, DIRECTION_EAST));

	EXPECT_TRUE(manager.logout(fixture.name, false));
	EXPECT_EQ(0U, saveCallCount);
	EXPECT_EQ(BotSessionState::Closed, session->getState());
	EXPECT_EQ(0U, manager.size());
	EXPECT_EQ(fixture.start, fixture.persistedPosition());

	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, SaveExceptionLeavesRemovedSessionPendingAndRetryOnlySaves) {
	PlayerBotDatabaseFixture fixture(g_database());
	createWalkableTile(fixture.start);
	const Position observerPosition(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	createWalkableTile(observerPosition);

	size_t removalCallCount = 0;
	size_t saveCallCount = 0;
	bool throwOnSave = true;
	BotSessionOperations operations;
	operations.save = [&saveCallCount, &throwOnSave](const std::shared_ptr<Player> &) {
		++saveCallCount;
		if (throwOnSave) {
			throw std::runtime_error("injected save failure");
		}
		return true;
	};
	operations.remove = [&removalCallCount](const std::shared_ptr<Player> &player, bool savePlayer, const auto &saveOperation) {
		++removalCallCount;
		const std::function<bool(const std::shared_ptr<Player> &)> noSave;
		return g_game().removeManagedPlayer(player, true, savePlayer ? saveOperation : noSave);
	};
	BotManager manager(g_game(), std::move(operations));
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	const auto player = std::const_pointer_cast<Player>(session->getPlayer());
	ASSERT_NE(nullptr, player);
	const auto previousTile = player->getTile();
	ASSERT_NE(nullptr, previousTile);
	const auto observer = std::make_shared<RemovalCountingCreature>();
	observer->setID();
	observer->observedCreature = player;
	ASSERT_TRUE(g_game().placeCreature(observer, observerPosition, false, true));

	EXPECT_FALSE(manager.logout(fixture.name, true));
	EXPECT_EQ(BotSessionState::PendingSave, session->getState());
	const auto blockedTick = manager.tick(fixture.name, std::chrono::milliseconds(1000));
	EXPECT_EQ(BotActionFailure::InvalidLifecycle, blockedTick.failure);
	EXPECT_EQ(1U, removalCallCount);
	EXPECT_EQ(1U, saveCallCount);
	EXPECT_FALSE(player->isOnline());
	EXPECT_TRUE(player->isRemoved());
	EXPECT_EQ(-1, previousTile->getThingIndex(player));
	EXPECT_EQ(nullptr, g_game().getPlayerByName(fixture.name));
	EXPECT_EQ(1U, observer->observedRemovalCount);

	throwOnSave = false;
	EXPECT_TRUE(manager.save(fixture.name));
	EXPECT_EQ(BotSessionState::Closed, session->getState());
	EXPECT_EQ(1U, removalCallCount);
	EXPECT_EQ(2U, saveCallCount);
	EXPECT_EQ(0U, manager.size());
	EXPECT_EQ(1U, observer->observedRemovalCount);
	EXPECT_TRUE(g_game().removeCreature(observer, true));

	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, OrdinaryGameRemovalStillPersistsPlayer) {
	PlayerBotDatabaseFixture fixture(g_database());
	const Position destination(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	createWalkableTile(fixture.start);
	createWalkableTile(destination);

	BotSessionOperations operations;
	operations.remove = [](const std::shared_ptr<Player> &player, bool, const auto &) {
		player->setOnline(false);
		return g_game().removeCreature(player, true) ? ManagedPlayerRemovalResult::Complete : ManagedPlayerRemovalResult::RemovalFailed;
	};
	BotManager manager(g_game(), std::move(operations));
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	ASSERT_EQ(RETURNVALUE_NOERROR, manager.move(fixture.name, DIRECTION_EAST));

	EXPECT_TRUE(manager.logout(fixture.name, false));
	EXPECT_EQ(BotSessionState::Closed, session->getState());
	EXPECT_EQ(0U, manager.size());
	EXPECT_EQ(destination, fixture.persistedPosition());

	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, ManagerOwnsSessionAndDestructorCleansWorldPlacement) {
	std::fprintf(stderr, "[PlayerBotIntegrationTest] executing manager-destruction body\n");
	PlayerBotDatabaseFixture fixture(g_database());
	createWalkableTile(fixture.start);

	std::weak_ptr<const BotSession> sessionObserver;
	std::weak_ptr<const Player> playerObserver;
	{
		BotManager manager(g_game());
		auto session = loginBotOrReport(manager, fixture.name);
		ASSERT_NE(nullptr, session);
		sessionObserver = session;
		playerObserver = session->getPlayer();

		session.reset();
		EXPECT_FALSE(sessionObserver.expired());
		EXPECT_FALSE(playerObserver.expired());
		EXPECT_NE(nullptr, manager.getSession(fixture.name));
	}

	EXPECT_TRUE(sessionObserver.expired());
	EXPECT_TRUE(playerObserver.expired());
	EXPECT_EQ(nullptr, g_game().getPlayerByName(fixture.name));
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, DestructorDoesNotRetryPendingSaveOrReplayRemoval) {
	PlayerBotDatabaseFixture fixture(g_database());
	createWalkableTile(fixture.start);

	size_t removalCallCount = 0;
	size_t saveCallCount = 0;
	std::shared_ptr<Player> player;
	{
		BotSessionOperations operations;
		operations.save = [&saveCallCount](const std::shared_ptr<Player> &) {
			++saveCallCount;
			return false;
		};
		operations.remove = [&removalCallCount](const std::shared_ptr<Player> &removedPlayer, bool savePlayer, const auto &saveOperation) {
			++removalCallCount;
			const std::function<bool(const std::shared_ptr<Player> &)> noSave;
			return g_game().removeManagedPlayer(removedPlayer, true, savePlayer ? saveOperation : noSave);
		};
		BotManager manager(g_game(), std::move(operations));
		const auto session = loginBotOrReport(manager, fixture.name);
		ASSERT_NE(nullptr, session);
		player = std::const_pointer_cast<Player>(session->getPlayer());
		ASSERT_NE(nullptr, player);
		EXPECT_FALSE(manager.logout(fixture.name, true));
		EXPECT_EQ(BotSessionState::PendingSave, session->getState());
	}

	EXPECT_EQ(1U, removalCallCount);
	EXPECT_EQ(1U, saveCallCount);
	ASSERT_NE(nullptr, player);
	EXPECT_TRUE(player->isRemoved());
	EXPECT_FALSE(player->isOnline());
	EXPECT_EQ(nullptr, g_game().getPlayerByName(fixture.name));

	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, DestructorAttemptsPreTeardownRemovalFailureOnlyOnce) {
	PlayerBotDatabaseFixture fixture(g_database());
	createWalkableTile(fixture.start);

	size_t removalCallCount = 0;
	std::shared_ptr<Player> player;
	{
		BotSessionOperations operations;
		operations.remove = [&removalCallCount](const std::shared_ptr<Player> &, bool, const auto &) {
			++removalCallCount;
			return ManagedPlayerRemovalResult::RemovalFailed;
		};
		BotManager manager(g_game(), std::move(operations));
		const auto session = loginBotOrReport(manager, fixture.name);
		ASSERT_NE(nullptr, session);
		player = std::const_pointer_cast<Player>(session->getPlayer());
		ASSERT_NE(nullptr, player);
	}

	EXPECT_EQ(1U, removalCallCount);
	ASSERT_NE(nullptr, player);
	EXPECT_FALSE(player->isRemoved());
	EXPECT_TRUE(player->isOnline());
	EXPECT_EQ(player, g_game().getPlayerByName(fixture.name));
	const std::function<bool(const std::shared_ptr<Player> &)> noSave;
	EXPECT_EQ(ManagedPlayerRemovalResult::Complete, g_game().removeManagedPlayer(player, true, noSave));

	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, SaveAndRemovalFailuresRetainManagedSessionForRetry) {
	std::fprintf(stderr, "[PlayerBotIntegrationTest] executing failure-retention body\n");
	PlayerBotDatabaseFixture fixture(g_database());
	createWalkableTile(fixture.start);

	bool allowSave = false;
	size_t saveCallCount = 0;
	size_t removalCallCount = 0;
	BotSessionOperations operations;
	operations.save = [&allowSave, &saveCallCount](const std::shared_ptr<Player> &player) {
		++saveCallCount;
		return allowSave && IOLoginData::savePlayer(player);
	};
	operations.remove = [&removalCallCount](const std::shared_ptr<Player> &player, bool savePlayer, const auto &saveOperation) {
		++removalCallCount;
		if (removalCallCount == 1) {
			return ManagedPlayerRemovalResult::RemovalFailed;
		}
		const std::function<bool(const std::shared_ptr<Player> &)> noSave;
		return g_game().removeManagedPlayer(player, true, savePlayer ? saveOperation : noSave);
	};
	BotManager manager(g_game(), std::move(operations));
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	const auto player = std::const_pointer_cast<Player>(session->getPlayer());
	ASSERT_NE(nullptr, player);
	const auto previousTile = player->getTile();
	const auto guid = player->getGUID();
	ASSERT_NE(nullptr, previousTile);
	EXPECT_EQ(BotSessionState::Placed, session->getState());
	EXPECT_EQ(1U, manager.size());
	EXPECT_TRUE(player->isOnline());
	EXPECT_FALSE(player->isRemoved());
	EXPECT_NE(nullptr, player->getParent());
	EXPECT_NE(nullptr, player->getTile());
	EXPECT_NE(-1, previousTile->getThingIndex(player));
	EXPECT_EQ(player, g_game().getPlayerByName(fixture.name));
	EXPECT_EQ(player, g_game().getPlayerByGUID(fixture.playerId));

	EXPECT_FALSE(manager.logout(fixture.name));
	EXPECT_EQ(BotSessionState::Placed, session->getState());
	EXPECT_EQ(1U, manager.size());
	EXPECT_EQ(1U, removalCallCount);
	EXPECT_EQ(0U, saveCallCount);
	EXPECT_EQ(player, session->getPlayer());
	EXPECT_EQ(fixture.start, player->getLoginPosition());
	EXPECT_TRUE(player->isOnline());
	EXPECT_FALSE(player->isRemoved());
	EXPECT_NE(nullptr, player->getParent());
	EXPECT_NE(nullptr, player->getTile());
	EXPECT_NE(-1, previousTile->getThingIndex(player));
	EXPECT_EQ(player, g_game().getPlayerByName(fixture.name));
	EXPECT_EQ(player, g_game().getPlayerByGUID(fixture.playerId));

	EXPECT_FALSE(manager.logout(fixture.name));
	EXPECT_EQ(BotSessionState::PendingSave, session->getState());
	EXPECT_EQ(1U, manager.size());
	EXPECT_EQ(2U, removalCallCount);
	EXPECT_EQ(1U, saveCallCount);
	EXPECT_EQ(player, session->getPlayer());
	EXPECT_FALSE(player->isOnline());
	EXPECT_TRUE(player->isRemoved());
	EXPECT_EQ(-1, previousTile->getThingIndex(player));
	EXPECT_EQ(nullptr, g_game().getPlayerByName(fixture.name));
	EXPECT_EQ(nullptr, g_game().getPlayerByGUID(fixture.playerId));

	allowSave = true;
	EXPECT_TRUE(manager.logout(fixture.name, true));
	EXPECT_EQ(BotSessionState::Closed, session->getState());
	EXPECT_EQ(0U, manager.size());
	EXPECT_EQ(2U, removalCallCount);
	EXPECT_EQ(2U, saveCallCount);
	EXPECT_EQ(nullptr, session->getPlayer());
	EXPECT_FALSE(player->isOnline());
	EXPECT_TRUE(player->isRemoved());
	EXPECT_EQ(nullptr, g_game().getPlayerByName(fixture.name));
	EXPECT_EQ(nullptr, g_game().getPlayerByGUID(guid));
	EXPECT_EQ(-1, previousTile->getThingIndex(player));
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, CombatPerceptionSelectsVisibleRealMonsterWithoutMutatingPlayerState) {
	PlayerBotDatabaseFixture fixture(g_database());
	const Position monsterPosition(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	createWalkableTile(fixture.start); createWalkableTile(monsterPosition);
	BotManager manager(g_game()); const auto session = loginBotOrReport(manager, fixture.name); ASSERT_NE(nullptr, session);
	const auto player = std::const_pointer_cast<Player>(session->getPlayer()); ASSERT_NE(nullptr, player);
	const auto monster = std::make_shared<Monster>(std::make_shared<MonsterType>("CombatRat")); ASSERT_TRUE(g_game().placeCreature(monster, monsterPosition, false, true));
	const Position before = player->getPosition(); const auto health = player->getHealth(); const auto mana = player->getMana();
	const auto first = manager.evaluateCombat(fixture.name); const auto second = manager.evaluateCombat(fixture.name);
	EXPECT_EQ(monster->getID(), first.selectedCreatureId); EXPECT_EQ(first.selectedCreatureId, second.selectedCreatureId);
	EXPECT_EQ(BotCombatIntent::AcquireTarget, first.intent); EXPECT_EQ(BotCombatIntent::HoldTarget, second.intent);
	EXPECT_EQ(nullptr, player->getAttackedCreature()); EXPECT_EQ(nullptr, player->getFollowCreature()); EXPECT_EQ(before, player->getPosition());
	EXPECT_EQ(health, player->getHealth()); EXPECT_EQ(mana, player->getMana());
	ASSERT_TRUE(g_game().removeCreature(monster, true)); const auto removed = manager.evaluateCombat(fixture.name);
	EXPECT_EQ(BotCombatIntent::ReleaseTarget, removed.intent); EXPECT_EQ(0U, session->getCombatLock()->creatureId);
	EXPECT_TRUE(manager.logout(fixture.name, false)); ASSERT_TRUE(fixture.cleanup()); EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, CombatPerceptionRejectsRealPlayerNpcAndPlayerOwnedSummon) {
	PlayerBotDatabaseFixture fixture(g_database()); PlayerBotDatabaseFixture otherFixture(g_database(), Position(fixture.start.x + 1, fixture.start.y, fixture.start.z));
	const Position npcPosition(fixture.start.x, fixture.start.y + 1, fixture.start.z); const Position summonPosition(fixture.start.x + 1, fixture.start.y + 1, fixture.start.z);
	createWalkableTile(fixture.start); createWalkableTile(otherFixture.start); createWalkableTile(npcPosition); createWalkableTile(summonPosition);
	BotManager manager(g_game()); const auto session = loginBotOrReport(manager, fixture.name); const auto other = loginBotOrReport(manager, otherFixture.name); ASSERT_NE(nullptr, session); ASSERT_NE(nullptr, other);
	auto npcType = std::make_shared<NpcType>("CombatFixtureNpc"); npcType->name="CombatFixtureNpc"; npcType->nameDescription="CombatFixtureNpc"; const auto npc=std::make_shared<Npc>(npcType); ASSERT_TRUE(g_game().placeCreature(npc,npcPosition,false,true));
	const auto summon=std::make_shared<Monster>(std::make_shared<MonsterType>("CombatSummon")); ASSERT_TRUE(summon->setMaster(std::const_pointer_cast<Player>(other->getPlayer()))); ASSERT_TRUE(g_game().placeCreature(summon,summonPosition,false,true));
	const auto result=manager.evaluateCombat(fixture.name); EXPECT_EQ(0U,result.selectedCreatureId); EXPECT_EQ(BotCombatFailure::NoTarget,result.failure);
	EXPECT_TRUE(g_game().removeCreature(npc,true)); EXPECT_TRUE(g_game().removeCreature(summon,true)); EXPECT_TRUE(manager.logout(otherFixture.name,false)); EXPECT_TRUE(manager.logout(fixture.name,false));
	ASSERT_TRUE(otherFixture.cleanup()); ASSERT_TRUE(fixture.cleanup()); EXPECT_FALSE(otherFixture.hasCommittedRows()); EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, AttackingMonsterOutranksPassiveAndLifecycleCloseClearsValueLock) {
	PlayerBotDatabaseFixture fixture(g_database()); const Position a(fixture.start.x+1,fixture.start.y,fixture.start.z), b(fixture.start.x,fixture.start.y+1,fixture.start.z);
	createWalkableTile(fixture.start); createWalkableTile(a); createWalkableTile(b); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); const auto player=std::const_pointer_cast<Player>(session->getPlayer());
	const auto passive=std::make_shared<Monster>(std::make_shared<MonsterType>("CombatPassive")), attacker=std::make_shared<Monster>(std::make_shared<MonsterType>("CombatAttacker")); ASSERT_TRUE(g_game().placeCreature(passive,a,false,true)); ASSERT_TRUE(g_game().placeCreature(attacker,b,false,true)); ASSERT_TRUE(attacker->setAttackedCreature(player));
	EXPECT_EQ(attacker->getID(),manager.evaluateCombat(fixture.name).selectedCreatureId); EXPECT_EQ(nullptr,player->getAttackedCreature());
	attacker->setAttackedCreature(nullptr); player->addDamagePoints(passive, 10); BotCombatPolicy immediateSwitch; immediateSwitch.switchThreshold = 0;
	EXPECT_EQ(passive->getID(), manager.evaluateCombat(fixture.name, immediateSwitch).selectedCreatureId);
	EXPECT_TRUE(g_game().removeCreature(passive,true)); EXPECT_TRUE(g_game().removeCreature(attacker,true)); EXPECT_TRUE(manager.logout(fixture.name,false)); EXPECT_EQ(nullptr,session->getCombatLock());
	const auto closed=manager.evaluateCombat(fixture.name); EXPECT_EQ(BotCombatFailure::InvalidLifecycle,closed.failure); ASSERT_TRUE(fixture.cleanup()); EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, CombatExecutionUsesOrdinaryAttackSelectionAndSuppressesSpam) {
	PlayerBotDatabaseFixture fixture(g_database()); const Position targetPosition(fixture.start.x+1,fixture.start.y,fixture.start.z);
	createWalkableTile(fixture.start); createWalkableTile(targetPosition); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session);
	const auto player=std::const_pointer_cast<Player>(session->getPlayer()); const auto monsterType=std::make_shared<MonsterType>("ExecutionMonster"); monsterType->info.health=100000; monsterType->info.healthMax=100000; const auto monster=std::make_shared<Monster>(monsterType); ASSERT_TRUE(g_game().placeCreature(monster,targetPosition,false,true));
	const auto selection=manager.evaluateCombat(fixture.name); ASSERT_EQ(monster->getID(),selection.selectedCreatureId);
	ASSERT_EQ(player,g_game().getPlayerByID(player->getID())); ASSERT_EQ(monster,g_game().getCreatureByID(monster->getID())); ASSERT_TRUE(player->canSeeCreature(monster)); ASSERT_EQ(RETURNVALUE_NOERROR,Combat::canTargetCreature(player,monster));
	ASSERT_TRUE(player->setAttackedCreature(monster)); ASSERT_EQ(monster,player->getAttackedCreature()); player->setAttackedCreature(nullptr);
	g_game().playerSetAttackedCreature(player->getID(),monster->getID()); ASSERT_EQ(monster,player->getAttackedCreature()); g_game().playerSetAttackedCreature(player->getID(),0); ASSERT_EQ(nullptr,player->getAttackedCreature());
	const auto base=BotPerception::observe(player); ASSERT_TRUE(base); const auto combat=BotCombat::observe(player,*base); ASSERT_TRUE(combat); const auto observed=std::ranges::find(combat->creatures,monster->getID(),&BotCombatCreatureObservation::id); ASSERT_NE(combat->creatures.end(),observed);
	const BotCombatExecutionRequest request{monster->getID(),combat->revision,observed->signature,player->getPosition()}; const auto health=player->getHealth(); const auto mana=player->getMana();
	const auto acquired=manager.executeCombat(fixture.name,request,std::chrono::milliseconds(1000)); EXPECT_EQ(BotCombatExecutionOutcome::TargetAcquired,acquired.outcome); ASSERT_NE(nullptr,player->getAttackedCreature()); EXPECT_EQ(monster->getID(),player->getAttackedCreature()->getID());
	const auto cooldown=manager.executeCombat(fixture.name,request,std::chrono::milliseconds(1001)); EXPECT_EQ(BotCombatExecutionOutcome::CooldownActive,cooldown.outcome); EXPECT_EQ(1U,session->getAttackExecutionState()->assignmentAttempts); EXPECT_EQ(health,player->getHealth()); EXPECT_EQ(mana,player->getMana());
	g_game().playerSetAttackedCreature(player->getID(),0); EXPECT_EQ(nullptr,player->getAttackedCreature()); g_game().playerSetAttackedCreature(player->getID(),monster->getID()); ASSERT_NE(nullptr,player->getAttackedCreature());
	EXPECT_TRUE(g_game().removeCreature(monster,true)); EXPECT_EQ(nullptr,player->getAttackedCreature()); EXPECT_TRUE(manager.logout(fixture.name,false)); EXPECT_EQ(nullptr,session->getAttackExecutionState()); ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, CombatExecutionRevalidatesRealNonPvpTargets) {
	PlayerBotDatabaseFixture fixture(g_database()); PlayerBotDatabaseFixture otherFixture(g_database(),Position(fixture.start.x+1,fixture.start.y,fixture.start.z)); const Position npcPosition(fixture.start.x,fixture.start.y+1,fixture.start.z), summonPosition(fixture.start.x+1,fixture.start.y+1,fixture.start.z);
	createWalkableTile(fixture.start);createWalkableTile(otherFixture.start);createWalkableTile(npcPosition);createWalkableTile(summonPosition);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);const auto other=loginBotOrReport(manager,otherFixture.name);ASSERT_NE(nullptr,session);ASSERT_NE(nullptr,other);
	auto npcType=std::make_shared<NpcType>("ExecutionNpc");npcType->name="ExecutionNpc";npcType->nameDescription="ExecutionNpc";const auto npc=std::make_shared<Npc>(npcType);ASSERT_TRUE(g_game().placeCreature(npc,npcPosition,false,true));const auto summon=std::make_shared<Monster>(std::make_shared<MonsterType>("ExecutionSummon"));ASSERT_TRUE(summon->setMaster(std::const_pointer_cast<Player>(other->getPlayer())));ASSERT_TRUE(g_game().placeCreature(summon,summonPosition,false,true));
	EXPECT_EQ(0U,manager.evaluateCombat(fixture.name).selectedCreatureId);EXPECT_EQ(nullptr,std::const_pointer_cast<Player>(session->getPlayer())->getAttackedCreature());
	EXPECT_TRUE(g_game().removeCreature(npc,true));EXPECT_TRUE(g_game().removeCreature(summon,true));EXPECT_TRUE(manager.logout(otherFixture.name,false));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(otherFixture.cleanup());ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, CombatPositioningIsBoundedAndValueOnly) {
	PlayerBotDatabaseFixture fixture(g_database()); const Position targetPosition(fixture.start.x+3,fixture.start.y,fixture.start.z); for(int x=0;x<=3;++x)for(int y=-2;y<=2;++y)createWalkableTile(Position(fixture.start.x+x,fixture.start.y+y,fixture.start.z));
	BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);const auto player=std::const_pointer_cast<Player>(session->getPlayer());const auto monsterType=std::make_shared<MonsterType>("PositionMonster");monsterType->info.health=100000;monsterType->info.healthMax=100000;const auto monster=std::make_shared<Monster>(monsterType);ASSERT_TRUE(g_game().placeCreature(monster,targetPosition,false,true));
	ASSERT_EQ(monster->getID(),manager.evaluateCombat(fixture.name).selectedCreatureId);const auto base=BotPerception::observe(player);ASSERT_TRUE(base);const auto combat=BotCombat::observe(player,*base);ASSERT_TRUE(combat);const auto observed=std::ranges::find(combat->creatures,monster->getID(),&BotCombatCreatureObservation::id);ASSERT_NE(combat->creatures.end(),observed);const BotCombatExecutionRequest request{monster->getID(),combat->revision,observed->signature,player->getPosition()};const Position before=player->getPosition();
	const auto result=manager.executeCombat(fixture.name,request,std::chrono::milliseconds(2000));EXPECT_EQ(BotCombatExecutionOutcome::RepositionStarted,result.outcome);EXPECT_TRUE(result.positioning.has_value());EXPECT_FALSE(result.positioning->containsWorldOwnership());EXPECT_NE(before,player->getPosition());EXPECT_LE(session->getAttackExecutionState()->repositionAttempts,3U);
	EXPECT_TRUE(g_game().removeCreature(monster,true));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, OrdinaryHeadlessNetworkVisibilityRemainsClientBound) {
	const auto ordinary = std::make_shared<Player>();
	const auto bot = std::make_shared<Player>(PlayerControlType::Bot);
	const Position nearby(1, 1, 0);
	EXPECT_FALSE(ordinary->canSee(nearby));
	EXPECT_TRUE(bot->canSee(nearby));
}
