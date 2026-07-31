/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include <gtest/gtest.h>

#include "creatures/players/bots/bot_manager.hpp"
#include "creatures/players/bots/bot_navigation.hpp"
#include "creatures/players/player.hpp"
#include "database/database.hpp"
#include "game/game.hpp"
#include "items/item.hpp"
#include "io/iologindata.hpp"
#include "io/functions/iologindata_load_player.hpp"
#include "lib/logging/in_memory_logger.hpp"
#include "test_database.hpp"

namespace {
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

		std::weak_ptr<Creature> observedCreature;
		size_t observedRemovalCount = 0;

	private:
		std::string name = "PlayerBotRemovalObserver";
	};

	class PlayerBotDatabaseFixture final {
	public:
		explicit PlayerBotDatabaseFixture(Database &database) :
			database(database) {
			TestDatabase::init();
			TestDatabase::requireDisposableDatabase(database);

			static std::atomic<uint32_t> sequence { 0 };
			const auto suffix = static_cast<uint32_t>(
				std::chrono::steady_clock::now().time_since_epoch().count() & 0x0FFFFFFF
			) + sequence.fetch_add(1);
			name = fmt::format("PlayerBot{}", suffix);
			start = Position(
				static_cast<uint16_t>(30000U + suffix % 1000U),
				static_cast<uint16_t>(30000U + (suffix / 1000U) % 1000U),
				7
			);

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
