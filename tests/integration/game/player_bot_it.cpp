/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include <gtest/gtest.h>

#include "creatures/players/bots/bot_manager.hpp"
#include "creatures/players/bots/bot_navigation.hpp"
#include "creatures/players/bots/bot_resupply.hpp"
#include "creatures/players/bots/bot_quest_execution.hpp"
#include "creatures/players/bots/bot_planner.hpp"
#include "creatures/players/bots/bot_plan_execution.hpp"
#include "creatures/players/bots/bot_planner_persistence.hpp"
#include "creatures/players/bots/bot_progression.hpp"
#include "creatures/players/bots/bot_coordination.hpp"
#include "creatures/players/bots/bot_fleet.hpp"
#include "creatures/players/bots/bot_fleet_configuration.hpp"
#include "creatures/players/bots/bot_fleet_telemetry.hpp"
#include "creatures/players/bots/bot_fleet_hardening.hpp"
#include "creatures/players/grouping/party.hpp"
#include "creatures/combat/combat.hpp"
#include "creatures/combat/condition.hpp"
#include "creatures/players/player.hpp"
#include "creatures/players/vocations/vocation.hpp"
#include "creatures/monsters/monster.hpp"
#include "creatures/monsters/monsters.hpp"
#include "creatures/npcs/npc.hpp"
#include "creatures/npcs/npcs.hpp"
#include "database/database.hpp"
#include "game/game.hpp"
#include "items/item.hpp"
#include "items/containers/depot/depotlocker.hpp"
#include "items/containers/depot/depotchest.hpp"
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
					"(`name`, `account_id`, `group_id`, `vocation`, `town_id`, `health`, `healthmax`, `mana`, `manamax`, `cap`, `conditions`, `posx`, `posy`, `posz`, `deletion`) "
					"VALUES ({}, {}, 1, 1, 1, 150, 150, 100, 100, 100000, X'', {}, {}, {}, 0)",
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

TEST(PlayerBotIntegrationTest, ProgressionTargetAttainsThroughOrdinaryCombatAndPersists) {
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); createWalkableTile(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));
	const uint32_t startingLevel=8,targetLevel=9;const auto startingExperience=Player::getExpForLevel(targetLevel)-10;
	ASSERT_TRUE(g_database().executeQuery("UPDATE `players` SET `level`="+std::to_string(startingLevel)+",`experience`="+std::to_string(startingExperience)+" WHERE `id`="+std::to_string(fixture.playerId)));
	BotManager manager(g_game());auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());
	BotProgressionTarget target{.level={targetLevel},.policyRevision=1};auto before=BotProgression::observe(player,1);EXPECT_EQ(BotProgressionState::BelowTarget,BotProgression::assess(target,before).levelState);
	auto type=std::make_shared<MonsterType>("ProgressionTargetMonster");type->info.health=10;type->info.healthMax=10;type->info.experience=100;type->info.lookcorpse=3994;auto monster=std::make_shared<Monster>(type);ASSERT_TRUE(g_game().placeCreature(monster,Position(fixture.start.x+1,fixture.start.y,fixture.start.z),false,true));
	const auto selection=manager.evaluateCombat(fixture.name);ASSERT_EQ(monster->getID(),selection.selectedCreatureId);
	UPDATE_OTSYS_TIME();CombatDamage fatal;fatal.primary={COMBAT_PHYSICALDAMAGE,-1000};ASSERT_TRUE(g_game().combatChangeHealth(player,monster,fatal));monster->onDeath();ASSERT_GE(player->getLevel(),targetLevel);
	auto attained=BotProgression::assess(target,BotProgression::observe(player,2));EXPECT_TRUE(attained.terminal);EXPECT_FALSE(attained.startProgressionCombat);EXPECT_TRUE(manager.logout(fixture.name,true));
	BotManager reloaded(g_game());auto next=loginBotOrReport(reloaded,fixture.name);ASSERT_NE(nullptr,next);auto loaded=std::const_pointer_cast<Player>(next->getPlayer());EXPECT_TRUE(BotProgression::assess(target,BotProgression::observe(loaded,3)).terminal);EXPECT_TRUE(reloaded.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, ProgressionObservesAuthoritativeSkillAndMigratesPersistedCheckpoint) {
	PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());
	const auto skillBefore=player->getSkillLevel(SKILL_SWORD);player->addSkillAdvance(SKILL_SWORD,player->getVocation()->getReqSkillTries(SKILL_SWORD,skillBefore+1));const auto observed=BotProgression::observe(player,1,WEAPON_SWORD);EXPECT_GT(player->getSkillLevel(SKILL_SWORD),skillBefore);BotProgressionTarget target{.level={player->getLevel()},.skills={{.type=BotSkillType::Sword,.level=player->getSkillLevel(SKILL_SWORD),.requiredVocationId=player->getVocationId(),.requiredWeaponType=WEAPON_SWORD}},.policyRevision=1};EXPECT_TRUE(BotProgression::assess(target,observed).terminal);
	BotPersistedPlanCheckpoint old{.playerId=fixture.playerId,.schemaVersion=1,.checkpointRevision=1,.policyRevision=1,.goalId=1,.goalType=BotGoalType::GainConfiguredProgress,.planRevision=1,.safeSaveBoundary=true};old.checksum=BotPlannerPersistence::checksum(old);
	std::ostringstream q;q<<"INSERT INTO `player_bot_planner_state` (`player_id`,`schema_version`,`checkpoint_revision`,`policy_revision`,`goal_id`,`goal_type`,`plan_revision`,`verified_step_index`,`verified_subsystem`,`failure_count`,`retry_count`,`configured_target_id`,`region_x`,`region_y`,`region_z`,`safe_boundary`,`checksum`) VALUES ("<<old.playerId<<",1,1,1,1,"<<static_cast<uint16_t>(old.goalType)<<",1,0,0,0,0,0,0,0,0,1,"<<old.checksum<<")";ASSERT_TRUE(g_database().executeQuery(q.str()));auto loaded=BotPlannerPersistence::load(fixture.playerId);ASSERT_EQ(BotCheckpointLoadReason::Valid,loaded.reason);ASSERT_TRUE(loaded.checkpoint);EXPECT_EQ(2,loaded.checkpoint->schemaVersion);EXPECT_TRUE(loaded.freshObservationRequired);EXPECT_TRUE(manager.logout(fixture.name,false));EXPECT_TRUE(BotPlannerPersistence::erase(fixture.playerId));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, FleetReconciliationUsesOrdinaryManagedLifecycleGradually) {
	PlayerBotDatabaseFixture first(g_database());
	PlayerBotDatabaseFixture second(g_database());
	createWalkableTile(first.start);
	createWalkableTile(second.start);
	BotManager manager(g_game());
	BotFleetPopulationPolicy policy { .desiredOnline=2, .minimumOnline=0, .maximumOnline=2, .absoluteHardMaximum=2, .maximumLoginsPerInterval=1, .maximumLogoutsPerInterval=1, .maximumPendingLogins=1, .maximumPendingLogouts=1, .maximumRetries=3, .revision=1 };
	BotFleetDistributionPolicy distribution;
	std::vector<BotFleetMemberProfile> members { { .id=first.playerId, .name=first.name, .allowedRegionIds={17}, .priority=1 }, { .id=second.playerId, .name=second.name, .priority=2 } };
	BotFleetControllerStateValue controller;
	auto reconciliation = BotFleet::reconcile(controller, policy, distribution, members, {}, 0);
	ASSERT_EQ(1, reconciliation.requests.size());
	EXPECT_EQ(first.playerId, reconciliation.requests.front().memberId);
	auto firstSession = loginBotOrReport(manager, first.name);
	ASSERT_NE(nullptr, firstSession);
	ASSERT_EQ(BotSessionState::Placed, firstSession->getState());
	std::vector<BotFleetObservation> observations { { .id=first.playerId, .state=BotFleetMemberState::Placed, .observationRevision=1 } };
	reconciliation = BotFleet::reconcile(controller, policy, distribution, members, observations, 1);
	ASSERT_EQ(1, reconciliation.requests.size());
	EXPECT_EQ(second.playerId, reconciliation.requests.front().memberId);
	auto secondSession = loginBotOrReport(manager, second.name);
	ASSERT_NE(nullptr, secondSession);
	EXPECT_EQ(2, manager.size());
	EXPECT_EQ(nullptr, manager.login(first.name));
	BotFleet::pause(controller);
	EXPECT_TRUE(BotFleet::reconcile(controller, policy, distribution, members, observations, 2).requests.empty());
	BotFleet::drain(controller);
	observations.push_back({ .id=second.playerId, .state=BotFleetMemberState::Placed, .observationRevision=2 });
	reconciliation = BotFleet::reconcile(controller, policy, distribution, members, observations, 3);
	ASSERT_EQ(1, reconciliation.requests.size());
	EXPECT_EQ(BotFleetLifecycleRequestType::Logout, reconciliation.requests.front().type);
	EXPECT_TRUE(manager.logout(second.name, true));
	EXPECT_TRUE(manager.logout(first.name, true));
	EXPECT_EQ(0, manager.size());
	ASSERT_TRUE(second.cleanup());
	ASSERT_TRUE(first.cleanup());
}

TEST(PlayerBotIntegrationTest, FleetManagerOwnsAuthoritativeLoginDrainAndCoordinationCleanup) {
	PlayerBotDatabaseFixture first(g_database()); PlayerBotDatabaseFixture second(g_database()); createWalkableTile(first.start); createWalkableTile(second.start);
	BotManager manager(g_game()); BotFleetPopulationPolicy policy{.desiredOnline=2,.minimumOnline=0,.maximumOnline=2,.absoluteHardMaximum=2,.maximumLoginsPerInterval=1,.maximumLogoutsPerInterval=1,.maximumPendingLogins=1,.maximumPendingLogouts=1,.maximumRetries=3,.revision=1};
	std::vector<BotFleetMemberProfile> members{{.id=first.playerId,.name=first.name,.vocationCategory=1,.coordinationGroupId=44,.allowedRegionIds={7},.priority=1},{.id=second.playerId,.name=second.name,.vocationCategory=2,.coordinationGroupId=44,.allowedRegionIds={8},.priority=2}};
	BotFleetDistributionPolicy distribution;distribution.limits={{BotFleetDistributionDimension::Vocation,1,1,1,1},{BotFleetDistributionDimension::Vocation,2,1,1,1},{BotFleetDistributionDimension::Region,7,0,1,1},{BotFleetDistributionDimension::Region,8,0,1,1}};
	ASSERT_EQ(BotFleetFailure::None,manager.configureFleet(policy,distribution,members,10));auto firstTick=manager.reconcileFleet(1);ASSERT_EQ(1,firstTick.requests.size());EXPECT_EQ(first.playerId,firstTick.requests.front().memberId);EXPECT_EQ(1,manager.size());EXPECT_EQ(BotSessionState::Placed,manager.getSession(first.name)->getState());
	auto secondTick=manager.reconcileFleet(2);ASSERT_EQ(1,secondTick.requests.size());EXPECT_EQ(second.playerId,secondTick.requests.front().memberId);EXPECT_EQ(2,manager.size());EXPECT_EQ(nullptr,manager.login(first.name));
	BotCoordinationPolicy coordination{.id=44,.configuredMembers={first.playerId,second.playerId},.revision=1};ASSERT_EQ(BotCoordinationFailure::None,manager.configureCoordinationGroup(coordination));manager.pauseFleet();EXPECT_TRUE(manager.reconcileFleet(3).requests.empty());manager.resumeFleet();EXPECT_TRUE(manager.fleetState().freshObservationRequired);
	manager.drainFleet();auto drainOne=manager.reconcileFleet(4);ASSERT_EQ(1,drainOne.requests.size());EXPECT_EQ(BotFleetLifecycleRequestType::Logout,drainOne.requests.front().type);EXPECT_EQ(1,manager.size());auto drainTwo=manager.reconcileFleet(5);ASSERT_EQ(1,drainTwo.requests.size());EXPECT_EQ(0,manager.size());EXPECT_EQ(0,manager.coordinationGroupCount());
	manager.stopFleet(true);EXPECT_TRUE(manager.fleetState().stopping);EXPECT_FALSE(manager.fleetState().wakeupPending);ASSERT_TRUE(second.cleanup());ASSERT_TRUE(first.cleanup());
}

TEST(PlayerBotIntegrationTest, FleetFailedSaveDoesNotReportSafeLogoutCompletion) {
	PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);size_t saveAttempts=0;BotSessionOperations operations;operations.save=[&](const std::shared_ptr<Player>&){return ++saveAttempts>1;};BotManager manager(g_game(),std::move(operations));
	BotFleetPopulationPolicy policy{.desiredOnline=1,.minimumOnline=0,.maximumOnline=1,.absoluteHardMaximum=1,.maximumLoginsPerInterval=1,.maximumLogoutsPerInterval=1,.maximumPendingLogins=1,.maximumPendingLogouts=1,.maximumRetries=2,.revision=1};std::vector<BotFleetMemberProfile>members{{.id=fixture.playerId,.name=fixture.name}};ASSERT_EQ(BotFleetFailure::None,manager.configureFleet(policy,{},members,10));ASSERT_EQ(1,manager.reconcileFleet(1).requests.size());ASSERT_EQ(1,manager.size());manager.drainFleet();auto result=manager.reconcileFleet(2);ASSERT_EQ(1,result.requests.size());EXPECT_EQ(1U,saveAttempts);EXPECT_EQ(1,manager.size());EXPECT_EQ(BotSessionState::PendingSave,manager.getSession(fixture.name)->getState());manager.stopFleet(false);ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, FleetConfigurationCommandsAreAtomicAuditedAndAuthoritative) {
	PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);const auto json=fmt::format(R"({{"schemaVersion":1,"revision":1,"population":{{"enabled":true,"desiredOnline":0,"minimumOnline":0,"maximumOnline":1,"absoluteHardMaximum":1}},"members":[{{"id":{},"name":"{}"}}]}})",fixture.playerId,fixture.name);auto parsed=BotFleetConfiguration::parse(json);ASSERT_TRUE(parsed.success());
	BotManager manager(g_game());BotFleetAdministration admin([&]{return BotFleetConfiguration::parse(fmt::format(R"({{"schemaVersion":1,"revision":3,"population":{{"enabled":true,"desiredOnline":0,"minimumOnline":0,"maximumOnline":1,"absoluteHardMaximum":1}},"members":[{{"id":{},"name":"{}"}}]}})",fixture.playerId,fixture.name));});ASSERT_EQ(BotFleetConfigurationFailure::None,admin.install(manager,parsed.revision));EXPECT_EQ(1,admin.activeRevision()->revision);
	auto rejected=admin.submit({.type=BotFleetCommandType::Pause,.operatorId=7},false);EXPECT_EQ(BotFleetCommandFailure::Unauthorized,rejected.failure);auto accepted=admin.submit({.type=BotFleetCommandType::LoginMember,.operatorId=7,.expectedRevision=1,.memberName=fixture.name,.reason="operator request"},true);EXPECT_EQ(BotFleetCommandState::Accepted,accepted.state);EXPECT_EQ(0,manager.size());auto completed=admin.process(manager);ASSERT_EQ(1,completed.size());EXPECT_EQ(BotFleetCommandState::Completed,completed.front().state);EXPECT_EQ(1,manager.size());
	(void)admin.submit({.type=BotFleetCommandType::Pause,.operatorId=7,.expectedRevision=1},true);EXPECT_EQ(BotFleetCommandState::Completed,admin.process(manager).front().state);EXPECT_TRUE(manager.fleetState().paused);(void)admin.submit({.type=BotFleetCommandType::Resume,.operatorId=7,.expectedRevision=1},true);EXPECT_EQ(BotFleetCommandState::Completed,admin.process(manager).front().state);EXPECT_FALSE(manager.fleetState().paused);
	(void)admin.submit({.type=BotFleetCommandType::SetDesired,.operatorId=7,.expectedRevision=1,.value=1},true);EXPECT_EQ(BotFleetCommandState::Completed,admin.process(manager).front().state);ASSERT_EQ(2,admin.activeRevision()->revision);(void)admin.submit({.type=BotFleetCommandType::Reload,.operatorId=7,.expectedRevision=2},true);EXPECT_EQ(BotFleetCommandState::Completed,admin.process(manager).front().state);EXPECT_EQ(3,admin.activeRevision()->revision);
	(void)admin.submit({.type=BotFleetCommandType::LogoutMember,.operatorId=7,.expectedRevision=3,.memberName=fixture.name},true);EXPECT_EQ(BotFleetCommandState::Completed,admin.process(manager).front().state);EXPECT_EQ(0,manager.size());EXPECT_FALSE(admin.audit().empty());admin.stop();ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, FleetAdminTelemetryObservesOrdinarySessionAndQueuesSafeLogout) {
	PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());BotFleetPopulationPolicy population{.desiredOnline=0,.minimumOnline=0,.maximumOnline=1,.absoluteHardMaximum=1,.revision=1};ASSERT_EQ(BotFleetFailure::None,manager.configureFleet(population,{},{{.id=fixture.playerId,.name=fixture.name}}));const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);
	BotFleetAdministration admin;BotFleetTelemetry telemetry;BotFleetAdminServicePolicy policy{.transport=BotFleetAdminTransport::UnixSocket};BotFleetAdminService service(admin,telemetry,policy,[](uint64_t id,std::string_view credential){return id==42&&credential=="fixture";});
	auto status=service.handle(manager,{.operatorId=42,.encodedSize=16,.credential="fixture"});ASSERT_TRUE(status.success());ASSERT_TRUE(status.snapshot);EXPECT_EQ(1,status.snapshot->managedSessions);EXPECT_EQ(1,status.snapshot->plannerHealthy);
	auto command=service.handle(manager,{.operation=BotFleetAdminOperation::Command,.operatorId=42,.window=1,.encodedSize=32,.credential="fixture",.command={.type=BotFleetCommandType::LogoutMember,.memberName=fixture.name}});ASSERT_EQ(BotFleetCommandState::Accepted,command.command.state);EXPECT_EQ(1,admin.queued());const auto completed=admin.process(manager);ASSERT_EQ(1,completed.size());EXPECT_EQ(BotFleetCommandState::Completed,completed.front().state);EXPECT_EQ(nullptr,session->getPlayer());
	service.stop();telemetry.stop();admin.stop();ASSERT_TRUE(fixture.cleanup());EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, FleetHardeningReconstructsThreeCyclesWithoutDuplicateSessionOrReservation) {
	PlayerBotDatabaseFixture first(g_database(), Position(31500, 31500, 7));
	PlayerBotDatabaseFixture second(g_database(), Position(31501, 31500, 7));
	createWalkableTile(first.start);
	createWalkableTile(second.start);
	for (uint32_t cycle = 0; cycle < 3; ++cycle) {
		BotManager manager(g_game());
		BotFleetPopulationPolicy population { .desiredOnline=2, .minimumOnline=0, .maximumOnline=2, .absoluteHardMaximum=2, .maximumLoginsPerInterval=2, .maximumLogoutsPerInterval=2, .maximumPendingLogins=2, .maximumPendingLogouts=2, .maximumRetries=3, .revision=cycle + 1 };
		ASSERT_EQ(BotFleetFailure::None, manager.configureFleet(population, {}, {{.id=first.playerId,.name=first.name,.coordinationGroupId=91},{.id=second.playerId,.name=second.name,.coordinationGroupId=91}}, 10));
		auto reconciliation = manager.reconcileFleet(cycle * 10 + 1);
		ASSERT_EQ(2, reconciliation.requests.size());
		ASSERT_EQ(2, manager.size());
		EXPECT_EQ(nullptr, manager.login(first.name));
		BotCoordinationPolicy coordination { .id=91, .configuredMembers={first.playerId,second.playerId}, .configuredLeader=first.playerId, .revision=cycle + 1 };
		ASSERT_EQ(BotCoordinationFailure::None, manager.configureCoordinationGroup(coordination));
		EXPECT_EQ(0, manager.coordinationReservationCount());
		ASSERT_TRUE(manager.clear(false));
		manager.stopFleet(false);
		EXPECT_EQ(0, manager.size());
		EXPECT_EQ(0, manager.coordinationReservationCount());
	}
	ASSERT_TRUE(first.cleanup());
	ASSERT_TRUE(second.cleanup());
}

TEST(PlayerBotIntegrationTest, FleetHardeningResourcePressureBlocksLoginAndRecoversWithHysteresis) {
	PlayerBotDatabaseFixture fixture(g_database(), Position(31510, 31510, 7));
	createWalkableTile(fixture.start);
	BotManager manager(g_game());
	BotFleetPopulationPolicy population { .desiredOnline=1, .minimumOnline=0, .maximumOnline=1, .absoluteHardMaximum=1, .maximumLoginsPerInterval=1, .maximumLogoutsPerInterval=1, .maximumPendingLogins=1, .maximumPendingLogouts=1, .maximumRetries=3, .overloadRecoveryIntervals=1, .revision=1 };
	ASSERT_EQ(BotFleetFailure::None, manager.configureFleet(population, {}, {{.id=fixture.playerId,.name=fixture.name}}, 10));
	BotFleetResourcePolicy resources;
	resources.recoveryObservations = 2;
	ASSERT_TRUE(manager.configureFleetResources(resources));
	auto blocked = manager.reconcileFleet(1, BotFleetResourceObservation{.dispatcherPressure=BotFleetPressureState::Critical});
	EXPECT_TRUE(blocked.requests.empty());
	EXPECT_EQ(BotFleetPressureState::Critical, manager.lastFleetLoadShedding().pressure);
	BotFleetAdministration administration;
	BotFleetTelemetry telemetry;
	const auto snapshot = telemetry.collect(manager, administration);
	ASSERT_TRUE(snapshot);
	EXPECT_EQ(BotFleetPressureState::Critical, snapshot->pressureState);
	EXPECT_LT(snapshot->reconciliationWorkBudget, resources.maximumReconciliationWorkPerTick);
	EXPECT_TRUE(manager.reconcileFleet(2, BotFleetResourceObservation{}).requests.empty());
	EXPECT_EQ(BotFleetPressureState::Recovering, manager.lastFleetLoadShedding().pressure);
	auto recovered = manager.reconcileFleet(3, BotFleetResourceObservation{});
	ASSERT_EQ(1, recovered.requests.size());
	EXPECT_EQ(BotFleetPressureState::Normal, manager.lastFleetLoadShedding().pressure);
	ASSERT_TRUE(manager.clear(false));
	ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, FleetHardeningDatabaseAdmissionFailureBacksOffAndRecoversWithoutDuplicateSession) {
	PlayerBotDatabaseFixture fixture(g_database(), Position(31520, 31520, 7));
	createWalkableTile(fixture.start);
	size_t loadAttempts = 0;
	BotSessionOperations operations;
	operations.admitLoad = [&](std::string_view) { return ++loadAttempts > 1; };
	BotManager manager(g_game(), std::move(operations));
	BotFleetPopulationPolicy population { .desiredOnline=1, .minimumOnline=0, .maximumOnline=1, .absoluteHardMaximum=1, .maximumLoginsPerInterval=1, .maximumLogoutsPerInterval=1, .maximumPendingLogins=1, .maximumPendingLogouts=1, .maximumRetries=3, .maximumRetryBackoffTicks=4, .revision=1 };
	ASSERT_EQ(BotFleetFailure::None, manager.configureFleet(population, {}, {{.id=fixture.playerId,.name=fixture.name}}, 10));
	ASSERT_EQ(1, manager.reconcileFleet(1).requests.size());
	EXPECT_EQ(0, manager.size());
	EXPECT_EQ(nullptr, g_game().getPlayerByName(fixture.name));
	EXPECT_TRUE(manager.reconcileFleet(2).requests.empty());
	auto recovered = manager.reconcileFleet(3);
	ASSERT_EQ(1, recovered.requests.size());
	EXPECT_EQ(2, loadAttempts);
	EXPECT_EQ(1, manager.size());
	EXPECT_EQ(nullptr, manager.login(fixture.name));
	ASSERT_TRUE(manager.clear(false));
	ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, FleetHardeningFailedPlacementLeavesNoWorldSessionAndRecoversOnce) {
	PlayerBotDatabaseFixture fixture(g_database(), Position(31530, 31530, 7));
	createWalkableTile(fixture.start);
	size_t placementAttempts = 0;
	BotSessionOperations operations;
	operations.admitPlacement = [&](const std::shared_ptr<Player> &) { return ++placementAttempts > 1; };
	BotManager manager(g_game(), std::move(operations));
	BotFleetPopulationPolicy population { .desiredOnline=1, .minimumOnline=0, .maximumOnline=1, .absoluteHardMaximum=1, .maximumLoginsPerInterval=1, .maximumLogoutsPerInterval=1, .maximumPendingLogins=1, .maximumPendingLogouts=1, .maximumRetries=3, .maximumRetryBackoffTicks=4, .revision=1 };
	ASSERT_EQ(BotFleetFailure::None, manager.configureFleet(population, {}, {{.id=fixture.playerId,.name=fixture.name}}, 10));
	ASSERT_EQ(1, manager.reconcileFleet(1).requests.size());
	EXPECT_EQ(0, manager.size());
	EXPECT_EQ(nullptr, g_game().getPlayerByName(fixture.name));
	EXPECT_TRUE(manager.reconcileFleet(2).requests.empty());
	ASSERT_EQ(1, manager.reconcileFleet(3).requests.size());
	EXPECT_EQ(2, placementAttempts);
	EXPECT_EQ(1, manager.size());
	EXPECT_EQ(1, std::ranges::count_if(g_game().getPlayers(), [&](const auto &entry) { return entry.second->getName() == fixture.name; }));
	ASSERT_TRUE(manager.clear(false));
	ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, MultiBotCoordinationObservesPartyAndDelegatesFormationToM2) {
	const Position origin(31000,31000,7), memberStart(31001,31000,7), objective(31002,31000,7),formationDestination(31003,30999,7);
	PlayerBotDatabaseFixture first(g_database(),origin),second(g_database(),memberStart);for(int x=0;x<=3;++x)for(int y=-1;y<=1;++y)createWalkableTile(Position(origin.x+x,origin.y+y,origin.z));
	BotManager manager(g_game());auto a=loginBotOrReport(manager,first.name);auto b=loginBotOrReport(manager,second.name);ASSERT_NE(nullptr,a);ASSERT_NE(nullptr,b);auto pa=std::const_pointer_cast<Player>(a->getPlayer());auto pb=std::const_pointer_cast<Player>(b->getPlayer());
	auto party=Party::create(pa);ASSERT_NE(nullptr,party);ASSERT_TRUE(party->invitePlayer(pb));ASSERT_TRUE(party->joinParty(pb));ASSERT_EQ(party,pa->getParty());ASSERT_EQ(party,pb->getParty());
	BotCoordinationPolicy policy{.id=7,.configuredMembers={second.playerId,first.playerId},.configuredLeader=first.playerId,.objectiveRegion=objective,.requireParty=true,.revision=1};ASSERT_EQ(BotCoordinationFailure::None,manager.configureCoordinationGroup(policy));auto observed=manager.observeCoordinationGroup(7);ASSERT_EQ(2,observed.members.size());EXPECT_TRUE(std::ranges::all_of(observed.members,[](const auto&m){return m.partyMember&&m.playerBot;}));
	auto decision=manager.evaluateCoordinationGroup(7);ASSERT_EQ(first.playerId,decision.leaderId);ASSERT_EQ(2,decision.roles.size());auto intent=std::ranges::find(decision.intents,second.playerId,&BotCoordinationIntent::memberId);ASSERT_NE(decision.intents.end(),intent);EXPECT_EQ(BotCoordinationIntentType::Formation,intent->type);const auto before=pb->getPosition();auto route=manager.executeCoordinationMovement(second.name,*intent,std::chrono::milliseconds(1));ASSERT_EQ(BotRouteState::Ready,route.state);EXPECT_EQ(before,pb->getPosition());for(uint32_t tick=1;tick<=8&&route.state!=BotRouteState::Arrived;++tick)route=manager.advanceRoute(second.name,std::chrono::milliseconds(1+tick*1000));EXPECT_EQ(BotRouteState::Arrived,route.state);EXPECT_EQ(formationDestination,pb->getPosition());
	ASSERT_TRUE(party->leaveParty(pb,true));observed=manager.observeCoordinationGroup(7);ASSERT_EQ(2,observed.members.size());EXPECT_EQ(0,std::ranges::count(observed.members,true,&BotCoordinationMemberObservation::partyMember));EXPECT_EQ(BotCoordinationState::LeaderUnavailable,BotCoordination::evaluate(policy,observed).state);manager.clearCoordination();EXPECT_EQ(0,manager.coordinationGroupCount());if(pa->getParty())pa->getParty()->disband();EXPECT_TRUE(manager.logout(second.name,false));EXPECT_TRUE(manager.logout(first.name,false));ASSERT_TRUE(second.cleanup());ASSERT_TRUE(first.cleanup());
}

TEST(PlayerBotIntegrationTest, MultiBotCoordinationAdvisesM3FocusFireAndInvalidatesDisappearedTarget) {
	const Position firstStart(31100,31100,7),secondStart(31100,31102,7),unrelatedStart(31102,31102,7),targetPosition(31100,31101,7);PlayerBotDatabaseFixture first(g_database(),firstStart),second(g_database(),secondStart),unrelated(g_database(),unrelatedStart);for(int x=-2;x<=2;++x)for(int y=-2;y<=4;++y)createWalkableTile(Position(firstStart.x+x,firstStart.y+y,firstStart.z));
	BotManager manager(g_game());auto a=loginBotOrReport(manager,first.name);auto b=loginBotOrReport(manager,second.name);auto c=loginBotOrReport(manager,unrelated.name);ASSERT_NE(nullptr,a);ASSERT_NE(nullptr,b);ASSERT_NE(nullptr,c);auto pa=std::const_pointer_cast<Player>(a->getPlayer());auto pb=std::const_pointer_cast<Player>(b->getPlayer());BotCoordinationPolicy policy{.id=8,.configuredMembers={first.playerId,second.playerId},.configuredLeader=first.playerId,.revision=1};ASSERT_EQ(BotCoordinationFailure::None,manager.configureCoordinationGroup(policy));
	auto type=std::make_shared<MonsterType>("CoordinationCombatMonster");type->info.health=100000;type->info.healthMax=100000;auto monster=std::make_shared<Monster>(type);ASSERT_TRUE(g_game().placeCreature(monster,targetPosition,false,true));ASSERT_EQ(monster->getID(),manager.evaluateCombat(first.name).selectedCreatureId);ASSERT_EQ(monster->getID(),manager.evaluateCombat(second.name).selectedCreatureId);
	auto group=manager.observeCoordinationGroup(8);ASSERT_EQ(2,group.members.size());EXPECT_FALSE(std::ranges::contains(group.members,unrelated.playerId,&BotCoordinationMemberObservation::id));const auto firstGeneration=std::ranges::find(group.members,first.playerId,&BotCoordinationMemberObservation::id)->sessionGeneration;const auto secondGeneration=std::ranges::find(group.members,second.playerId,&BotCoordinationMemberObservation::id)->sessionGeneration;BotCoordinationReservation firstReservation{.groupId=8,.memberId=first.playerId,.type=BotCoordinationReservationType::CombatTarget,.targetSignature=monster->getID(),.observationRevision=group.revision,.sessionGeneration=firstGeneration,.expiresAt=10,.policyRevision=1};auto secondReservation=firstReservation;secondReservation.memberId=second.playerId;secondReservation.sessionGeneration=secondGeneration;EXPECT_EQ(BotCoordinationFailure::None,manager.reserveCoordinationTarget(firstReservation,0));EXPECT_EQ(BotCoordinationFailure::Conflict,manager.reserveCoordinationTarget(secondReservation,0));
	policy.focusFire=true;policy.revision=2;ASSERT_EQ(BotCoordinationFailure::None,manager.configureCoordinationGroup(policy));group=manager.observeCoordinationGroup(8);firstReservation.observationRevision=group.revision;firstReservation.policyRevision=2;secondReservation.observationRevision=group.revision;secondReservation.policyRevision=2;EXPECT_EQ(BotCoordinationFailure::None,manager.reserveCoordinationTarget(firstReservation,0));EXPECT_EQ(BotCoordinationFailure::None,manager.reserveCoordinationTarget(secondReservation,0));
	for(const auto&entry:std::array<std::pair<std::string,std::shared_ptr<Player>>,2>{{{first.name,pa},{second.name,pb}}}){auto perception=BotPerception::observe(entry.second);ASSERT_TRUE(perception);auto combat=BotCombat::observe(entry.second,*perception);ASSERT_TRUE(combat);auto target=std::ranges::find(combat->creatures,monster->getID(),&BotCombatCreatureObservation::id);ASSERT_NE(combat->creatures.end(),target);auto result=manager.executeCombat(entry.first,{monster->getID(),combat->revision,target->signature,entry.second->getPosition()},std::chrono::milliseconds(1));EXPECT_TRUE(result.outcome==BotCombatExecutionOutcome::TargetAcquired||result.outcome==BotCombatExecutionOutcome::RepositionStarted);}
	ASSERT_TRUE(g_game().removeCreature(monster,true));manager.invalidateCoordinationTarget(8,BotCoordinationReservationType::CombatTarget,monster->getID());EXPECT_EQ(0,manager.coordinationReservationCount(8));EXPECT_EQ(0,manager.evaluateCombat(first.name).selectedCreatureId);EXPECT_TRUE(manager.logout(unrelated.name,false));EXPECT_TRUE(manager.logout(second.name,false));EXPECT_TRUE(manager.logout(first.name,false));ASSERT_TRUE(unrelated.cleanup());ASSERT_TRUE(second.cleanup());ASSERT_TRUE(first.cleanup());
}

TEST(PlayerBotIntegrationTest, MultiBotCoordinationRetreatsReelectsAndRejectsStaleGeneration) {
	const Position firstStart(31200,31200,7),secondStart(31202,31200,7);PlayerBotDatabaseFixture first(g_database(),firstStart),second(g_database(),secondStart);for(int x=-4;x<=4;++x)for(int y=-4;y<=4;++y)createWalkableTile(Position(firstStart.x+x,firstStart.y+y,firstStart.z));
	BotManager manager(g_game());auto a=loginBotOrReport(manager,first.name);auto b=loginBotOrReport(manager,second.name);ASSERT_NE(nullptr,a);ASSERT_NE(nullptr,b);auto pa=std::const_pointer_cast<Player>(a->getPlayer());BotCoordinationPolicy policy{.id=9,.configuredMembers={first.playerId,second.playerId},.configuredLeader=first.playerId,.retreatRegion=Position(firstStart.x-2,firstStart.y,firstStart.z),.revision=1};ASSERT_EQ(BotCoordinationFailure::None,manager.configureCoordinationGroup(policy));auto group=manager.observeCoordinationGroup(9);auto firstObserved=std::ranges::find(group.members,first.playerId,&BotCoordinationMemberObservation::id);ASSERT_NE(group.members.end(),firstObserved);const auto oldGeneration=firstObserved->sessionGeneration;
	BotCoordinationReservation corpse{.groupId=9,.memberId=first.playerId,.type=BotCoordinationReservationType::LootCorpse,.targetSignature=777,.observationRevision=group.revision,.sessionGeneration=oldGeneration,.expiresAt=10,.policyRevision=1};ASSERT_EQ(BotCoordinationFailure::None,manager.reserveCoordinationTarget(corpse,0));CombatDamage damage;damage.primary={COMBAT_PHYSICALDAMAGE,-120};ASSERT_TRUE(g_game().combatChangeHealth(nullptr,pa,damage));ASSERT_LE(pa->getHealth()*100/pa->getMaxHealth(),25);auto retreat=manager.evaluateCoordinationGroup(9);EXPECT_EQ(BotCoordinationState::Retreating,retreat.state);EXPECT_TRUE(retreat.individualSurvivalAuthoritative);EXPECT_EQ(BotFleeOutcome::FleeStarted,manager.executeFlee(first.name,std::chrono::milliseconds(1)).outcome);damage.primary={COMBAT_PHYSICALDAMAGE,-1000};ASSERT_TRUE(g_game().combatChangeHealth(nullptr,pa,damage));EXPECT_EQ(0,pa->getHealth());auto deadFallback=manager.evaluateCoordinationGroup(9);EXPECT_EQ(second.playerId,deadFallback.leaderId);EXPECT_EQ(0,manager.coordinationReservationCount(9));
	EXPECT_TRUE(manager.logout(first.name,false));EXPECT_EQ(0,manager.coordinationReservationCount(9));auto fallback=manager.evaluateCoordinationGroup(9);EXPECT_EQ(second.playerId,fallback.leaderId);auto rejoined=loginBotOrReport(manager,first.name);ASSERT_NE(nullptr,rejoined);group=manager.observeCoordinationGroup(9);firstObserved=std::ranges::find(group.members,first.playerId,&BotCoordinationMemberObservation::id);ASSERT_NE(group.members.end(),firstObserved);EXPECT_GT(firstObserved->sessionGeneration,oldGeneration);corpse.observationRevision=group.revision;EXPECT_EQ(BotCoordinationFailure::GenerationMismatch,manager.reserveCoordinationTarget(corpse,0));auto restored=manager.evaluateCoordinationGroup(9);EXPECT_EQ(first.playerId,restored.leaderId);EXPECT_TRUE(restored.previousLeaderIntentsInvalidated);manager.clearCoordination();EXPECT_EQ(0,manager.coordinationGroupCount());EXPECT_TRUE(manager.logout(first.name,false));EXPECT_TRUE(manager.logout(second.name,false));ASSERT_TRUE(first.cleanup());ASSERT_TRUE(second.cleanup());
}

TEST(PlayerBotIntegrationTest, MultiBotCoordinationReservesCorpseAndDelegatesLootToM4) {
	const Position firstStart(31300,31300,7),secondStart(31300,31302,7),corpsePosition(31300,31301,7);PlayerBotDatabaseFixture first(g_database(),firstStart),second(g_database(),secondStart);createWalkableTile(firstStart);createWalkableTile(secondStart);createWalkableTile(corpsePosition);BotManager manager(g_game());auto a=loginBotOrReport(manager,first.name);auto b=loginBotOrReport(manager,second.name);ASSERT_NE(nullptr,a);ASSERT_NE(nullptr,b);auto pa=std::const_pointer_cast<Player>(a->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(pa,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));
	BotCoordinationPolicy policy{.id=10,.configuredMembers={first.playerId,second.playerId},.configuredLeader=first.playerId,.revision=1};ASSERT_EQ(BotCoordinationFailure::None,manager.configureCoordinationGroup(policy));auto group=manager.observeCoordinationGroup(10);auto firstMember=std::ranges::find(group.members,first.playerId,&BotCoordinationMemberObservation::id);auto secondMember=std::ranges::find(group.members,second.playerId,&BotCoordinationMemberObservation::id);ASSERT_NE(group.members.end(),firstMember);ASSERT_NE(group.members.end(),secondMember);
	auto corpse=Item::CreateItem(3994);corpse->setAttribute(ItemAttribute_t::CORPSEOWNER,pa->getID());ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(g_game().map.getTile(corpsePosition),corpse,INDEX_WHEREEVER,FLAG_NOLIMIT));corpse->startDecaying();corpse->getContainer()->internalAddThing(Item::CreateItem(3031,5));BotLootPolicy lootPolicy{.rules={{{.itemTypeId=3031,.valueCategory=1,.priority=1}}}};auto selected=manager.evaluateLoot(first.name,corpsePosition,88,{},lootPolicy);ASSERT_TRUE(selected.selected);const auto signature=selected.corpse.signature;
	BotCoordinationReservation primary{.groupId=10,.memberId=first.playerId,.type=BotCoordinationReservationType::LootCorpse,.targetSignature=signature.value,.observationRevision=group.revision,.sessionGeneration=firstMember->sessionGeneration,.expiresAt=10,.policyRevision=1};auto duplicate=primary;duplicate.memberId=second.playerId;duplicate.sessionGeneration=secondMember->sessionGeneration;EXPECT_EQ(BotCoordinationFailure::None,manager.reserveCoordinationTarget(primary,0));EXPECT_EQ(BotCoordinationFailure::Conflict,manager.reserveCoordinationTarget(duplicate,0));BotLootTransferRequest request{.corpsePosition=corpsePosition,.sourceCreatureId=88,.corpseSignature=signature,.itemTypeId=3031,.itemSignature=selected.selected->item.signature,.count=5};EXPECT_EQ(BotLootTransferOutcome::Pending,manager.executeLoot(first.name,request,std::chrono::milliseconds(1),lootPolicy).outcome);EXPECT_EQ(BotLootTransferOutcome::Succeeded,manager.executeLoot(first.name,request,std::chrono::milliseconds(2),lootPolicy).outcome);EXPECT_EQ(5U,std::static_pointer_cast<Cylinder>(pa)->getItemTypeCount(3031));manager.invalidateCoordinationTarget(10,BotCoordinationReservationType::LootCorpse,signature.value);EXPECT_EQ(0,manager.coordinationReservationCount(10));EXPECT_TRUE(manager.logout(second.name,false));EXPECT_TRUE(manager.logout(first.name,false));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalRemoveItem(corpse));ASSERT_TRUE(second.cleanup());ASSERT_TRUE(first.cleanup());
}

TEST(PlayerBotIntegrationTest, MultiBotCoordinationExcludesOrdinaryNetworkPartyMember) {
	const Position botStart(31400,31400,7),humanStart(31401,31400,7);PlayerBotDatabaseFixture botFixture(g_database(),botStart),humanFixture(g_database(),humanStart);createWalkableTile(botStart);createWalkableTile(humanStart);BotManager manager(g_game());auto session=loginBotOrReport(manager,botFixture.name);ASSERT_NE(nullptr,session);auto bot=std::const_pointer_cast<Player>(session->getPlayer());
	auto human=std::make_shared<Player>();human->setName(humanFixture.name);ASSERT_TRUE(IOLoginDataLoad::preLoadPlayer(human,humanFixture.name));ASSERT_TRUE(IOLoginData::loadPlayerById(human,humanFixture.playerId,false));human->setID();human->setOnline(true);ASSERT_TRUE(human->isNetworkControlled());ASSERT_TRUE(g_game().placeCreature(human,humanStart,false,true));auto party=Party::create(bot);ASSERT_TRUE(party->invitePlayer(human));ASSERT_TRUE(party->joinParty(human));
	BotCoordinationPolicy policy{.id=11,.configuredMembers={botFixture.playerId,humanFixture.playerId},.configuredLeader=botFixture.playerId,.requireParty=true,.revision=1};ASSERT_EQ(BotCoordinationFailure::None,manager.configureCoordinationGroup(policy));auto observed=manager.observeCoordinationGroup(11);ASSERT_EQ(1,observed.members.size());EXPECT_EQ(botFixture.playerId,observed.members.front().id);auto decision=manager.evaluateCoordinationGroup(11);ASSERT_EQ(1,decision.roles.size());EXPECT_EQ(botFixture.playerId,decision.roles.front().first);EXPECT_FALSE(decision.directGameplayAction);ASSERT_TRUE(party->leaveParty(human,true));human->setOnline(false);const std::function<bool(const std::shared_ptr<Player>&)> noSave;EXPECT_EQ(ManagedPlayerRemovalResult::Complete,g_game().removeManagedPlayer(human,true,noSave));EXPECT_TRUE(manager.logout(botFixture.name,false));ASSERT_TRUE(humanFixture.cleanup());ASSERT_TRUE(botFixture.cleanup());
}

TEST(PlayerBotIntegrationTest, MultiBotCoordinationManagerTeardownClearsCrossSessionState) {
	const Position firstStart(31500,31500,7),secondStart(31501,31500,7);PlayerBotDatabaseFixture first(g_database(),firstStart),second(g_database(),secondStart);createWalkableTile(firstStart);createWalkableTile(secondStart);std::shared_ptr<const BotSession> a,b;
	{BotManager manager(g_game());a=loginBotOrReport(manager,first.name);b=loginBotOrReport(manager,second.name);ASSERT_NE(nullptr,a);ASSERT_NE(nullptr,b);BotCoordinationPolicy policy{.id=12,.configuredMembers={first.playerId,second.playerId},.configuredLeader=first.playerId,.revision=1};ASSERT_EQ(BotCoordinationFailure::None,manager.configureCoordinationGroup(policy));auto observed=manager.observeCoordinationGroup(12);auto firstMember=std::ranges::find(observed.members,first.playerId,&BotCoordinationMemberObservation::id);ASSERT_NE(observed.members.end(),firstMember);ASSERT_EQ(BotCoordinationFailure::None,manager.reserveCoordinationTarget({.groupId=12,.memberId=first.playerId,.type=BotCoordinationReservationType::RegroupRegion,.targetSignature=99,.observationRevision=observed.revision,.sessionGeneration=firstMember->sessionGeneration,.expiresAt=10,.policyRevision=1},0));EXPECT_EQ(1,manager.coordinationReservationCount(12));}
	EXPECT_EQ(nullptr,a->getPlayer());EXPECT_EQ(nullptr,b->getPlayer());ASSERT_TRUE(first.cleanup());ASSERT_TRUE(second.cleanup());
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

TEST(PlayerBotIntegrationTest, SurvivalObservesActualHealthManaAndRealDamage) {
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto player=std::const_pointer_cast<Player>(session->getPlayer());
	const auto before=manager.evaluateSurvival(fixture.name); EXPECT_NE(BotSurvivalDecision::Dead,before.decision); const auto health=player->getHealth(); CombatDamage damage; damage.primary={COMBAT_PHYSICALDAMAGE,-10}; ASSERT_TRUE(g_game().combatChangeHealth(nullptr,player,damage)); EXPECT_EQ(health-10,player->getHealth()); const auto after=manager.evaluateSurvival(fixture.name); EXPECT_GE(after.score.total,before.score.total); EXPECT_TRUE(manager.logout(fixture.name,false)); ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, SurvivalObservesRealHarmfulCondition) {
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto player=std::const_pointer_cast<Player>(session->getPlayer()); auto condition=Condition::createCondition(CONDITIONID_COMBAT,CONDITION_PARALYZE,5000,0); ASSERT_TRUE(player->addCondition(condition)); EXPECT_GT(manager.evaluateSurvival(fixture.name).score.conditions,0); EXPECT_TRUE(manager.logout(fixture.name,false)); ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, SurvivalRejectsMissingHealingItemWithoutMutation) {
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto player=std::const_pointer_cast<Player>(session->getPlayer()); const auto health=player->getHealth(); const auto mana=player->getMana(); auto option=BotHealingOption{.kind=BotHealingKind::HealthPotion,.itemTypeId=65500,.availableCount=1}; EXPECT_EQ(BotHealingOutcome::MissingItem,manager.executeHealing(fixture.name,option,std::chrono::milliseconds(1)).outcome); EXPECT_EQ(health,player->getHealth()); EXPECT_EQ(mana,player->getMana()); EXPECT_TRUE(manager.logout(fixture.name,false)); ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, SurvivalRejectsInsufficientManaAndMissingCondition) {
	static const bool coreLoaded = g_scripts().loadEventSchedulerScripts("data/core.lua"); ASSERT_TRUE(coreLoaded); g_actions().clear(); ASSERT_TRUE(g_scripts().loadEventSchedulerScripts("tests/fixture/playerbots/survival_actions.lua")); PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto player=std::const_pointer_cast<Player>(session->getPlayer()); player->learnInstantSpell("PlayerBot Test Expensive Healing"); player->learnInstantSpell("PlayerBot Test Cleanse"); auto spell=BotHealingOption{.kind=BotHealingKind::SelfHealingSpell,.spell="playerbot test expensive heal",.manaCost=1000}; EXPECT_EQ(BotHealingOutcome::MissingMana,manager.executeHealing(fixture.name,spell,std::chrono::milliseconds(1)).outcome); auto removal=BotHealingOption{.kind=BotHealingKind::ConditionRemoval,.spell="playerbot test cleanse",.removesConditions=1ULL<<3}; EXPECT_EQ(BotHealingOutcome::ConditionNotPresent,manager.executeHealing(fixture.name,removal,std::chrono::milliseconds(2)).outcome); EXPECT_TRUE(manager.logout(fixture.name,false)); ASSERT_TRUE(fixture.cleanup()); g_actions().clear();
}

TEST(PlayerBotIntegrationTest, CriticalSurvivalStartsBoundedM2FleeAndReleasesCombatState) {
	PlayerBotDatabaseFixture fixture(g_database()); const Position monsterPosition(fixture.start.x+1,fixture.start.y,fixture.start.z); for(int x=-3;x<=3;++x)for(int y=-3;y<=3;++y)createWalkableTile(Position(fixture.start.x+x,fixture.start.y+y,fixture.start.z)); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto player=std::const_pointer_cast<Player>(session->getPlayer()); auto monster=std::make_shared<Monster>(std::make_shared<MonsterType>("SurvivalThreat")); ASSERT_TRUE(g_game().placeCreature(monster,monsterPosition,false,true)); ASSERT_TRUE(player->setAttackedCreature(monster)); ASSERT_TRUE(player->setFollowCreature(monster)); CombatDamage damage; damage.primary={COMBAT_PHYSICALDAMAGE,-static_cast<int32_t>(player->getHealth()*8/10)}; ASSERT_TRUE(g_game().combatChangeHealth(monster,player,damage)); EXPECT_EQ(BotSurvivalDecision::Flee,manager.evaluateSurvival(fixture.name,{},{}).decision); const auto flee=manager.executeFlee(fixture.name,std::chrono::milliseconds(100)); EXPECT_EQ(BotFleeOutcome::FleeStarted,flee.outcome); EXPECT_EQ(nullptr,player->getAttackedCreature()); EXPECT_EQ(nullptr,player->getFollowCreature()); EXPECT_LE(flee.route.positions.size(),BotSurvivalPolicy{}.maxRouteLength); EXPECT_TRUE(g_game().removeCreature(monster,true)); EXPECT_TRUE(manager.logout(fixture.name,false)); ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, SurvivalFleeAvoidsRealHarmfulTileWhenSafeAlternativeExists) {
	PlayerBotDatabaseFixture fixture(g_database()); for(int x=-2;x<=2;++x)for(int y=-2;y<=2;++y)createWalkableTile(Position(fixture.start.x+x,fixture.start.y+y,fixture.start.z)); const Position hazard(fixture.start.x+1,fixture.start.y,fixture.start.z); auto field=Item::CreateItem(ITEM_FIREFIELD_PVP_FULL); ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(g_game().map.getTile(hazard),field,INDEX_WHEREEVER,FLAG_NOLIMIT)); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); const auto flee=manager.executeFlee(fixture.name,std::chrono::milliseconds(1)); EXPECT_NE(hazard,flee.request.destination); EXPECT_TRUE(manager.logout(fixture.name,false)); ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, SurvivalFleeUsesBoundedAlternativeAndDynamicRepath) {
	PlayerBotDatabaseFixture fixture(g_database()); for(int x=-4;x<=4;++x)for(int y=-4;y<=4;++y)createWalkableTile(Position(fixture.start.x+x,fixture.start.y+y,fixture.start.z)); const Position staticBlock(fixture.start.x-1,fixture.start.y,fixture.start.z); auto wall=Item::CreateItem(1025); ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(g_game().map.getTile(staticBlock),wall,INDEX_WHEREEVER,FLAG_NOLIMIT)); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto started=manager.executeFlee(fixture.name,std::chrono::milliseconds(1)); ASSERT_EQ(BotFleeOutcome::FleeStarted,started.outcome); EXPECT_NE(staticBlock,started.request.destination); ASSERT_FALSE(started.route.positions.empty()); auto blocker=std::make_shared<Monster>(std::make_shared<MonsterType>("FleeDynamicBlocker")); ASSERT_TRUE(g_game().placeCreature(blocker,started.route.positions.front(),false,true)); const auto repath=manager.executeFlee(fixture.name,std::chrono::milliseconds(2)); EXPECT_TRUE(repath.outcome==BotFleeOutcome::Progressing || repath.outcome==BotFleeOutcome::Blocked); EXPECT_LE(session->getRouteProgress()->totalReplans,BotSurvivalPolicy{}.maxAttempts); EXPECT_TRUE(g_game().removeCreature(blocker,true)); EXPECT_TRUE(manager.logout(fixture.name,false)); ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, SurvivalFleeNoSafeRouteAndTimeoutAreFinite) {
	{
		PlayerBotDatabaseFixture fixture(g_database());
		createWalkableTile(fixture.start);
		BotManager manager(g_game());
		const auto session = loginBotOrReport(manager, fixture.name);
		ASSERT_NE(nullptr, session);
		EXPECT_EQ(BotFleeOutcome::NoSafeDestination, manager.executeFlee(fixture.name, std::chrono::milliseconds(1)).outcome);
		EXPECT_NE(BotSurvivalState::Fleeing, session->getSurvivalProgress()->state);
		EXPECT_TRUE(manager.logout(fixture.name, false));
		ASSERT_TRUE(fixture.cleanup());
	}

	{
		PlayerBotDatabaseFixture fixture(g_database());
		for (int x = -2; x <= 2; ++x) {
			for (int y = -2; y <= 2; ++y) {
				createWalkableTile(Position(fixture.start.x + x, fixture.start.y + y, fixture.start.z));
			}
		}
		BotManager manager(g_game());
		const auto session = loginBotOrReport(manager, fixture.name);
		ASSERT_NE(nullptr, session);
		ASSERT_EQ(BotFleeOutcome::FleeStarted, manager.executeFlee(fixture.name, std::chrono::milliseconds(1)).outcome);
		EXPECT_EQ(BotFleeOutcome::TimedOut, manager.executeFlee(fixture.name, std::chrono::milliseconds(6000)).outcome);
		EXPECT_EQ(BotSurvivalState::Failed, session->getSurvivalProgress()->state);
		EXPECT_TRUE(manager.logout(fixture.name, false));
		ASSERT_TRUE(fixture.cleanup());
	}
}

TEST(PlayerBotIntegrationTest, VisibleHostileMovementRefreshesFleeWhileHiddenCreatureDoesNot) {
	PlayerBotDatabaseFixture fixture(g_database()); for(int x=-4;x<=4;++x)for(int y=-2;y<=2;++y)createWalkableTile(Position(fixture.start.x+x,fixture.start.y+y,fixture.start.z)); const Position east(fixture.start.x+2,fixture.start.y,fixture.start.z), west(fixture.start.x-2,fixture.start.y,fixture.start.z), hidden(fixture.start.x+20,fixture.start.y,fixture.start.z); createWalkableTile(hidden); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto threat=std::make_shared<Monster>(std::make_shared<MonsterType>("FleeVisibleThreat")); ASSERT_TRUE(g_game().placeCreature(threat,east,false,true)); const auto player=std::const_pointer_cast<Player>(session->getPlayer()); auto base=BotPerception::observe(player); ASSERT_TRUE(base); auto combat=BotCombat::observe(player,*base); ASSERT_TRUE(combat); const auto first=BotSurvival::selectFlee(*base,*combat,{}); ASSERT_EQ(BotFleeOutcome::SafePositionSelected,first.outcome); EXPECT_LT(first.request.destination.x,fixture.start.x); ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalMoveCreature(threat,g_game().map.getTile(west))); base=BotPerception::observe(player); combat=BotCombat::observe(player,*base); const auto second=BotSurvival::selectFlee(*base,*combat,{}); EXPECT_GT(second.request.destination.x,fixture.start.x); auto hiddenThreat=std::make_shared<Monster>(std::make_shared<MonsterType>("FleeHiddenThreat")); ASSERT_TRUE(g_game().placeCreature(hiddenThreat,hidden,false,true)); base=BotPerception::observe(player); combat=BotCombat::observe(player,*base); const auto third=BotSurvival::selectFlee(*base,*combat,{}); EXPECT_EQ(second.request.destination,third.request.destination); EXPECT_TRUE(g_game().removeCreature(hiddenThreat,true)); EXPECT_TRUE(g_game().removeCreature(threat,true)); EXPECT_TRUE(manager.logout(fixture.name,false)); ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, RealPlayerDeathBecomesTerminalAndCancelsBotState) {
	PlayerBotDatabaseFixture fixture(g_database());
	const Position monsterPosition(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	const Position routeDestination(fixture.start.x, fixture.start.y + 1, fixture.start.z);
	createWalkableTile(fixture.start);
	createWalkableTile(monsterPosition);
	createWalkableTile(routeDestination);
	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	auto player = std::const_pointer_cast<Player>(session->getPlayer());
	const auto deathTile = g_game().map.getTile(fixture.start);
	ASSERT_NE(nullptr, deathTile);
	const auto itemCount = deathTile->getItemCount();
	auto monsterType = std::make_shared<MonsterType>("DeathThreat");
	monsterType->info.health = 100000;
	monsterType->info.healthMax = 100000;
	auto monster = std::make_shared<Monster>(monsterType);
	ASSERT_TRUE(g_game().placeCreature(monster, monsterPosition, false, true));

	const auto selection = manager.evaluateCombat(fixture.name);
	ASSERT_EQ(monster->getID(), selection.selectedCreatureId);
	const auto base = BotPerception::observe(player);
	ASSERT_TRUE(base);
	const auto combat = BotCombat::observe(player, *base);
	ASSERT_TRUE(combat);
	const auto observed = std::ranges::find(combat->creatures, monster->getID(), &BotCombatCreatureObservation::id);
	ASSERT_NE(combat->creatures.end(), observed);
	const BotCombatExecutionRequest request { monster->getID(), combat->revision, observed->signature, player->getPosition() };
	ASSERT_EQ(BotCombatExecutionOutcome::TargetAcquired, manager.executeCombat(fixture.name, request, std::chrono::milliseconds(1)).outcome);
	ASSERT_EQ(BotRouteState::Ready, manager.startRoute(fixture.name, routeDestination, std::chrono::milliseconds(1)).state);
	ASSERT_NE(0U, session->getCombatLock()->creatureId);
	ASSERT_NE(BotAttackState::Idle, session->getAttackExecutionState()->state);
	ASSERT_EQ(BotRouteState::Ready, session->getRouteProgress()->state);

	CombatDamage damage;
	damage.primary = { COMBAT_PHYSICALDAMAGE, -player->getHealth() };
	ASSERT_TRUE(g_game().combatChangeHealth(monster, player, damage));
	const auto tick = manager.tick(fixture.name, std::chrono::milliseconds(2));
	EXPECT_EQ(BotActionFailure::InvalidLifecycle, tick.failure);
	ASSERT_NE(nullptr, session->getSurvivalProgress());
	EXPECT_EQ(BotSurvivalState::Dead, session->getSurvivalProgress()->state);
	EXPECT_EQ(0U, session->getCombatLock()->creatureId);
	EXPECT_EQ(BotAttackState::Cancelled, session->getAttackExecutionState()->state);
	EXPECT_EQ(BotRouteState::Cancelled, session->getRouteProgress()->state);
	EXPECT_GT(deathTile->getItemCount(), itemCount);
	EXPECT_EQ(BotHealingOutcome::Dead, manager.executeHealing(fixture.name, {}, std::chrono::milliseconds(3)).outcome);
	if (!monster->isRemoved()) {
		EXPECT_TRUE(g_game().removeCreature(monster, true));
	}
	EXPECT_TRUE(manager.logout(fixture.name, false));
	ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, SessionCloseCancelsPendingSurvivalWithoutWorldOwnership) {
	PlayerBotDatabaseFixture fixture(g_database()); for(int x=-2;x<=2;++x)for(int y=-2;y<=2;++y)createWalkableTile(Position(fixture.start.x+x,fixture.start.y+y,fixture.start.z)); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto pending=manager.executeFlee(fixture.name,std::chrono::milliseconds(1)); EXPECT_EQ(BotFleeOutcome::FleeStarted,pending.outcome); EXPECT_EQ(BotSurvivalState::Fleeing,session->getSurvivalProgress()->state); EXPECT_TRUE(manager.logout(fixture.name,false)); EXPECT_EQ(nullptr,session->getSurvivalProgress()); ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, RealHealingItemAndSpellUseOrdinaryActionsAndObservedResults) {
	static const bool coreLoaded = g_scripts().loadEventSchedulerScripts("data/core.lua"); ASSERT_TRUE(coreLoaded); g_actions().clear(); ASSERT_TRUE(g_scripts().loadEventSchedulerScripts("tests/fixture/playerbots/survival_actions.lua"));
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto player=std::const_pointer_cast<Player>(session->getPlayer()); CombatDamage damage; damage.primary={COMBAT_PHYSICALDAMAGE,-50}; ASSERT_TRUE(g_game().combatChangeHealth(nullptr,player,damage)); const auto beforeHealth=player->getHealth(); auto backpack=Item::CreateItem(ITEM_BACKPACK,1); ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT)); auto potion=Item::CreateItem(266,2); ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),potion,INDEX_WHEREEVER,FLAG_NOLIMIT)); auto option=BotHealingOption{.kind=BotHealingKind::HealthPotion,.itemTypeId=266,.availableCount=2,.minimumHealing=40,.maximumHealing=40}; const auto accepted=manager.executeHealing(fixture.name,option,std::chrono::milliseconds(1)); EXPECT_EQ(BotHealingOutcome::Pending,accepted.outcome); const auto observed=manager.executeHealing(fixture.name,option,std::chrono::milliseconds(2)); EXPECT_EQ(BotHealingOutcome::Succeeded,observed.outcome); EXPECT_EQ(beforeHealth+40,player->getHealth()); EXPECT_GT(observed.observedHealthDelta,0); EXPECT_GT(observed.observedItemDelta,0); EXPECT_EQ(BotHealingOutcome::Exhausted,manager.executeHealing(fixture.name,option,std::chrono::milliseconds(3)).outcome);
	(void)manager.evaluateSurvival(fixture.name); auto noEffectItem = Item::CreateItem(2854, 1); ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(backpack->getContainer(), noEffectItem, INDEX_WHEREEVER, FLAG_NOLIMIT)); BotHealingOption noEffectOption { .kind = BotHealingKind::HealingRune, .itemTypeId = 2854, .availableCount = 1 }; BotSurvivalPolicy noEffectPolicy; noEffectPolicy.timeout = std::chrono::milliseconds(10); EXPECT_EQ(BotHealingOutcome::Pending, manager.executeHealing(fixture.name, noEffectOption, std::chrono::milliseconds(10), noEffectPolicy).outcome); const auto noEffect = manager.executeHealing(fixture.name, noEffectOption, std::chrono::milliseconds(21), noEffectPolicy); EXPECT_EQ(BotHealingOutcome::NoEffect, noEffect.outcome); EXPECT_GT(noEffect.observedItemDelta, 0); EXPECT_EQ(0, noEffect.observedHealthDelta); (void)manager.evaluateSurvival(fixture.name);
	auto paralysis=Condition::createCondition(CONDITIONID_COMBAT,CONDITION_PARALYZE,5000,0); ASSERT_TRUE(player->addCondition(paralysis)); player->learnInstantSpell("PlayerBot Test Cleanse"); BotHealingOption cleanse{.kind=BotHealingKind::ConditionRemoval,.spell="playerbot test cleanse",.manaCost=5,.removesConditions=1ULL<<3}; EXPECT_EQ(BotHealingOutcome::Pending,manager.executeHealing(fixture.name,cleanse,std::chrono::milliseconds(4)).outcome); const auto cleansed=manager.executeHealing(fixture.name,cleanse,std::chrono::milliseconds(5)); EXPECT_EQ(BotHealingOutcome::Succeeded,cleansed.outcome); EXPECT_FALSE(player->hasCondition(CONDITION_PARALYZE)); EXPECT_NE(0U,cleansed.removedConditions); (void)manager.evaluateSurvival(fixture.name);
	CombatDamage secondDamage; secondDamage.primary={COMBAT_PHYSICALDAMAGE,-40}; ASSERT_TRUE(g_game().combatChangeHealth(nullptr,player,secondDamage)); player->learnInstantSpell("PlayerBot Test Healing"); const auto spellHealth=player->getHealth(); const auto spellMana=player->getMana(); BotHealingOption spell{.kind=BotHealingKind::SelfHealingSpell,.spell="playerbot test heal",.manaCost=10,.minimumHealing=30,.maximumHealing=30}; EXPECT_EQ(BotHealingOutcome::Pending,manager.executeHealing(fixture.name,spell,std::chrono::milliseconds(6)).outcome); const auto spellObserved=manager.executeHealing(fixture.name,spell,std::chrono::milliseconds(7)); EXPECT_EQ(BotHealingOutcome::Succeeded,spellObserved.outcome); EXPECT_EQ(spellHealth+30,player->getHealth()); EXPECT_EQ(spellMana-10,player->getMana()); EXPECT_LT(spellObserved.observedManaDelta,0); EXPECT_EQ(BotHealingOutcome::CooldownActive,manager.executeHealing(fixture.name,spell,std::chrono::milliseconds(8)).outcome); EXPECT_TRUE(manager.logout(fixture.name,false)); ASSERT_TRUE(fixture.cleanup()); g_actions().clear();
}

TEST(PlayerBotIntegrationTest, OrdinaryNetworkPlayerHealingRemainsAuthoritative) {
	static const bool coreLoaded = g_scripts().loadEventSchedulerScripts("data/core.lua");
	ASSERT_TRUE(coreLoaded);
	g_actions().clear();
	ASSERT_TRUE(g_scripts().loadEventSchedulerScripts("tests/fixture/playerbots/survival_actions.lua"));
	PlayerBotDatabaseFixture fixture(g_database());
	createWalkableTile(fixture.start);
	const auto player = std::make_shared<Player>();
	player->setName(fixture.name);
	ASSERT_TRUE(IOLoginDataLoad::preLoadPlayer(player, fixture.name));
	ASSERT_TRUE(IOLoginData::loadPlayerById(player, fixture.playerId, false));
	player->setID();
	player->setOnline(true);
	ASSERT_TRUE(player->isNetworkControlled());
	ASSERT_TRUE(g_game().placeCreature(player, fixture.start, false, true));
	CombatDamage damage;
	damage.primary = { COMBAT_PHYSICALDAMAGE, -50 };
	ASSERT_TRUE(g_game().combatChangeHealth(nullptr, player, damage));
	const auto beforeHealth = player->getHealth();
	auto backpack = Item::CreateItem(ITEM_BACKPACK, 1);
	ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(player, backpack, CONST_SLOT_BACKPACK, FLAG_NOLIMIT));
	auto potion = Item::CreateItem(266, 2);
	ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(backpack->getContainer(), potion, INDEX_WHEREEVER, FLAG_NOLIMIT));
	ASSERT_TRUE(g_actions().useItemEx(player, potion->getPosition(), player->getPosition(), 0, potion, false, player));
	EXPECT_EQ(beforeHealth + 40, player->getHealth());
	EXPECT_EQ(1, potion->getItemCount());
	player->setOnline(false);
	const std::function<bool(const std::shared_ptr<Player> &)> noSave;
	EXPECT_EQ(ManagedPlayerRemovalResult::Complete, g_game().removeManagedPlayer(player, true, noSave));
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
	g_actions().clear();
}

TEST(PlayerBotIntegrationTest, OrdinaryNetworkPlayerDeathStillUsesNormalCorpsePipeline) {
	PlayerBotDatabaseFixture fixture(g_database());
	createWalkableTile(fixture.start);
	const auto deathTile = g_game().map.getTile(fixture.start);
	ASSERT_NE(nullptr, deathTile);
	const auto itemCount = deathTile->getItemCount();
	const auto player = std::make_shared<Player>();
	player->setName(fixture.name);
	ASSERT_TRUE(IOLoginDataLoad::preLoadPlayer(player, fixture.name));
	ASSERT_TRUE(IOLoginData::loadPlayerById(player, fixture.playerId, false));
	player->setID();
	player->setOnline(true);
	ASSERT_TRUE(player->isNetworkControlled());
	ASSERT_TRUE(g_game().placeCreature(player, fixture.start, false, true));
	CombatDamage damage;
	damage.primary = { COMBAT_PHYSICALDAMAGE, -player->getHealth() };
	ASSERT_TRUE(g_game().combatChangeHealth(nullptr, player, damage));
	EXPECT_EQ(0, player->getHealth());
	EXPECT_FALSE(player->isRemoved());
	EXPECT_GT(deathTile->getItemCount(), itemCount);
	player->setOnline(false);
	const std::function<bool(const std::shared_ptr<Player> &)> noSave;
	EXPECT_EQ(ManagedPlayerRemovalResult::Complete, g_game().removeManagedPlayer(player, true, noSave));
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, RealMonsterDeathCreatesEligibleValueOnlyCorpseObservationWithoutTransfer) {
	PlayerBotDatabaseFixture fixture(g_database());
	const Position corpsePosition(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	for (int x = -1; x <= 2; ++x) for (int y = -1; y <= 1; ++y) createWalkableTile(Position(fixture.start.x + x, fixture.start.y + y, fixture.start.z));
	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	auto player = std::const_pointer_cast<Player>(session->getPlayer());
	auto monsterType = std::make_shared<MonsterType>("LootPerceptionMonster");
	monsterType->info.health = 20;
	monsterType->info.healthMax = 20;
	monsterType->info.lookcorpse = 3994;
	auto monster = std::make_shared<Monster>(monsterType);
	ASSERT_TRUE(g_game().placeCreature(monster, corpsePosition, false, true));
	const auto sourceCreatureId = monster->getID();
	UPDATE_OTSYS_TIME();
	CombatDamage damage;
	damage.primary = { COMBAT_PHYSICALDAMAGE, -100000 };
	ASSERT_TRUE(g_game().combatChangeHealth(player, monster, damage));
	ASSERT_EQ(0, monster->getHealth());
	// The integration harness does not run the dispatcher thread that normally
	// consumes Game::executeDeath, so advance the authoritative creature death
	// pipeline synchronously after real combat has reduced health to zero.
	monster->onDeath();
	ASSERT_TRUE(monster->isRemoved());
	ASSERT_TRUE(Item::items[3994].isCorpse);
	const auto tile = g_game().map.getTile(corpsePosition);
	ASSERT_NE(nullptr, tile);
	std::shared_ptr<Item> corpse;
	for (const auto &item : *tile->getItemList()) if (item && item->isCorpse() && item->getContainer()) { corpse = item; break; }
	ASSERT_NE(nullptr, corpse);
	auto gold = Item::CreateItem(3031, 50);
	// Production loot insertion uses this same fallback when the integration
	// configuration's global container-item limit rejects internalAddItem.
	corpse->getContainer()->internalAddThing(gold);
	ASSERT_EQ(corpse->getContainer(), gold->getParent());
	const auto corpseSize = corpse->getContainer()->size();
	const auto carriedBefore = std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031);
	BotLootPolicy policy { .rules = { { .itemTypeId = 3031, .valueCategory = 2, .priority = 10 } } };
	const auto result = manager.evaluateLoot(fixture.name, corpsePosition, sourceCreatureId, {}, policy);
	EXPECT_EQ(BotLootEligibility::Eligible, result.eligibility);
	EXPECT_EQ(sourceCreatureId, result.corpse.sourceCreatureId);
	EXPECT_EQ(player->getID(), result.corpse.ownerCreatureId);
	EXPECT_EQ(BotCorpseOwnership::Self, result.corpse.ownership);
	EXPECT_FALSE(result.corpse.containsWorldOwnership);
	ASSERT_TRUE(result.selected);
	EXPECT_EQ(3031, result.selected->item.itemTypeId);
	EXPECT_EQ(50U, result.selected->item.count);
	EXPECT_EQ(corpseSize, corpse->getContainer()->size());
	EXPECT_EQ(carriedBefore, std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031));
	auto platinum = Item::CreateItem(3035, 1);
	corpse->getContainer()->internalAddThing(platinum);
	ASSERT_EQ(corpse->getContainer(), platinum->getParent());
	EXPECT_EQ(BotLootEligibility::StaleObservation, manager.evaluateLoot(fixture.name, corpsePosition, sourceCreatureId, result.corpse.signature, policy).eligibility);
	ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalRemoveItem(corpse));
	EXPECT_EQ(BotLootEligibility::InvalidCorpse, manager.evaluateLoot(fixture.name, corpsePosition, sourceCreatureId, {}, policy).eligibility);
	EXPECT_TRUE(manager.logout(fixture.name, false));
	ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, RealCorpseLootRightsDenyUnrelatedPlayerBot) {
	PlayerBotDatabaseFixture observerFixture(g_database());
	PlayerBotDatabaseFixture killerFixture(g_database(), Position(observerFixture.start.x + 1, observerFixture.start.y, observerFixture.start.z));
	const Position corpsePosition(observerFixture.start.x, observerFixture.start.y + 1, observerFixture.start.z);
	for (int x = -1; x <= 2; ++x) for (int y = -1; y <= 2; ++y) createWalkableTile(Position(observerFixture.start.x + x, observerFixture.start.y + y, observerFixture.start.z));
	BotManager manager(g_game());
	const auto observer = loginBotOrReport(manager, observerFixture.name);
	const auto killer = loginBotOrReport(manager, killerFixture.name);
	ASSERT_NE(nullptr, observer);
	ASSERT_NE(nullptr, killer);
	auto killerPlayer = std::const_pointer_cast<Player>(killer->getPlayer());
	auto monsterType = std::make_shared<MonsterType>("LootRightsMonster");
	monsterType->info.health = 20; monsterType->info.healthMax = 20; monsterType->info.lookcorpse = 3994;
	auto monster = std::make_shared<Monster>(monsterType);
	ASSERT_TRUE(g_game().placeCreature(monster, corpsePosition, false, true));
	const auto sourceCreatureId = monster->getID();
	UPDATE_OTSYS_TIME();
	CombatDamage damage; damage.primary = { COMBAT_PHYSICALDAMAGE, -100000 };
	ASSERT_TRUE(g_game().combatChangeHealth(killerPlayer, monster, damage));
	ASSERT_EQ(0, monster->getHealth());
	monster->onDeath();
	ASSERT_TRUE(monster->isRemoved());
	BotLootPolicy policy { .rules = { { .itemTypeId = 3031, .valueCategory = 1, .priority = 1 } } };
	const auto denied = manager.evaluateLoot(observerFixture.name, corpsePosition, sourceCreatureId, {}, policy);
	EXPECT_EQ(BotLootEligibility::NoLootRights, denied.eligibility);
	EXPECT_EQ(BotCorpseOwnership::Denied, denied.corpse.ownership);
	EXPECT_TRUE(manager.logout(killerFixture.name, false));
	EXPECT_TRUE(manager.logout(observerFixture.name, false));
	ASSERT_TRUE(killerFixture.cleanup());
	ASSERT_TRUE(observerFixture.cleanup());
}

TEST(PlayerBotIntegrationTest, CorpseOutsideReachableM2KnowledgeIsRejected) {
	PlayerBotDatabaseFixture fixture(g_database());
	const Position corpsePosition(fixture.start.x + 2, fixture.start.y, fixture.start.z);
	for (int x = 0; x <= 2; ++x) for (int y = -1; y <= 1; ++y) createWalkableTile(Position(fixture.start.x + x, fixture.start.y + y, fixture.start.z));
	for (int y = -1; y <= 1; ++y) {
		auto wall = Item::CreateItem(1025);
		ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(g_game().map.getTile(Position(fixture.start.x + 1, fixture.start.y + y, fixture.start.z)), wall, INDEX_WHEREEVER, FLAG_NOLIMIT));
	}
	auto corpse = Item::CreateItem(ITEM_MALE_CORPSE);
	ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(g_game().map.getTile(corpsePosition), corpse, INDEX_WHEREEVER, FLAG_NOLIMIT));
	BotManager manager(g_game());
	ASSERT_NE(nullptr, loginBotOrReport(manager, fixture.name));
	EXPECT_EQ(BotLootEligibility::Unreachable, manager.evaluateLoot(fixture.name, corpsePosition, 99).eligibility);
	EXPECT_TRUE(manager.logout(fixture.name, false));
	ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, OrdinaryNetworkPlayerCorpseOpeningRemainsUnchanged) {
	PlayerBotDatabaseFixture fixture(g_database());
	createWalkableTile(fixture.start);
	const auto player = std::make_shared<Player>();
	player->setName(fixture.name);
	ASSERT_TRUE(IOLoginDataLoad::preLoadPlayer(player, fixture.name));
	ASSERT_TRUE(IOLoginData::loadPlayerById(player, fixture.playerId, false));
	player->setID(); player->setOnline(true);
	ASSERT_TRUE(g_game().placeCreature(player, fixture.start, false, true));
	auto corpse = Item::CreateItem(ITEM_MALE_CORPSE);
	ASSERT_NE(nullptr, corpse->getContainer());
	corpse->setAttribute(ItemAttribute_t::CORPSEOWNER, player->getID());
	const auto tile = g_game().map.getTile(fixture.start);
	ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(tile, corpse, INDEX_WHEREEVER, FLAG_NOLIMIT));
	ASSERT_TRUE(g_actions().useItem(player, fixture.start, static_cast<uint8_t>(tile->getThingIndex(corpse)), corpse, false));
	EXPECT_NE(-1, player->getContainerID(corpse->getContainer()));
	player->setOnline(false);
	const std::function<bool(const std::shared_ptr<Player> &)> noSave;
	EXPECT_EQ(ManagedPlayerRemovalResult::Complete, g_game().removeManagedPlayer(player, true, noSave));
	ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, LootTransferOpensRealCorpseAndMovesStackThroughOrdinaryPlayerPath) {
	PlayerBotDatabaseFixture fixture(g_database());
	const Position corpsePosition(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	for (int x=0;x<=1;++x) for(int y=-1;y<=1;++y) createWalkableTile(Position(fixture.start.x+x,fixture.start.y+y,fixture.start.z));
	BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(std::static_pointer_cast<Cylinder>(player),backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));
	auto corpse=Item::CreateItem(3994);ASSERT_NE(nullptr,corpse->getContainer());corpse->setAttribute(ItemAttribute_t::CORPSEOWNER,player->getID());ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(g_game().map.getTile(corpsePosition),corpse,INDEX_WHEREEVER,FLAG_NOLIMIT));corpse->startDecaying();
	auto gold=Item::CreateItem(3031,30);corpse->getContainer()->internalAddThing(gold);
	BotLootPolicy policy{.rules={{{.itemTypeId=3031,.valueCategory=2,.priority=10}}}};const auto selected=manager.evaluateLoot(fixture.name,corpsePosition,77,{},policy);ASSERT_EQ(BotLootEligibility::Eligible,selected.eligibility);ASSERT_TRUE(selected.selected);
	const auto before=std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031);BotLootTransferRequest request{.corpsePosition=corpsePosition,.sourceCreatureId=77,.corpseSignature=selected.corpse.signature,.itemTypeId=3031,.itemSignature=selected.selected->item.signature,.count=30};
	const auto pending=manager.executeLoot(fixture.name,request,std::chrono::milliseconds(0),policy);
	EXPECT_EQ(BotLootTransferOutcome::Pending,pending.outcome);EXPECT_TRUE(pending.ordinaryOpenAccepted);EXPECT_TRUE(pending.ordinaryMoveDispatched);EXPECT_GE(player->getContainerID(corpse->getContainer()),0);
	const auto result=manager.executeLoot(fixture.name,request,std::chrono::milliseconds(1),policy);EXPECT_EQ(BotLootTransferOutcome::Succeeded,result.outcome);EXPECT_EQ(30U,result.movedCount);EXPECT_EQ(0U,corpse->getContainer()->getItemTypeCount(3031));EXPECT_EQ(before+30,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031));
	EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalRemoveItem(corpse));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, LootTransferPreservesRealRightsDenialWithoutMutation) {
	PlayerBotDatabaseFixture fixture(g_database());const Position corpsePosition(fixture.start.x+1,fixture.start.y,fixture.start.z);createWalkableTile(fixture.start);createWalkableTile(corpsePosition);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto corpse=Item::CreateItem(3994);corpse->setAttribute(ItemAttribute_t::CORPSEOWNER,player->getID()+1000);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(g_game().map.getTile(corpsePosition),corpse,INDEX_WHEREEVER,FLAG_NOLIMIT));corpse->startDecaying();auto gold=Item::CreateItem(3031,5);corpse->getContainer()->internalAddThing(gold);BotLootPolicy policy{.rules={{{.itemTypeId=3031,.valueCategory=1,.priority=1}}}};BotLootTransferRequest request{.corpsePosition=corpsePosition,.sourceCreatureId=8,.itemTypeId=3031,.count=5};const auto before=std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031);const auto result=manager.executeLoot(fixture.name,request,std::chrono::milliseconds(0),policy);EXPECT_EQ(BotLootTransferOutcome::NoLootRights,result.outcome);EXPECT_EQ(5U,corpse->getContainer()->getItemTypeCount(3031));EXPECT_EQ(before,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalRemoveItem(corpse));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, LootTransferSessionCloseClearsValueOnlyExecutionState) {
	PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);BotLootTransferRequest request{.corpsePosition=Position(fixture.start.x+20,fixture.start.y,fixture.start.z),.sourceCreatureId=9,.itemTypeId=3031,.count=1};const auto result=manager.executeLoot(fixture.name,request,std::chrono::milliseconds(0));EXPECT_NE(BotLootTransferOutcome::Succeeded,result.outcome);EXPECT_TRUE(manager.logout(fixture.name,false));EXPECT_EQ(nullptr,manager.getSession(fixture.name));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, CriticalSurvivalReconcilesAuthoritativeLootBoundaryBeforeHealingAndFreshResume) {
	static const bool coreLoaded = g_scripts().loadEventSchedulerScripts("data/core.lua"); ASSERT_TRUE(coreLoaded); g_actions().clear(); ASSERT_TRUE(g_scripts().loadEventSchedulerScripts("tests/fixture/playerbots/survival_actions.lua"));
	PlayerBotDatabaseFixture fixture(g_database()); const Position corpsePosition(fixture.start.x+1,fixture.start.y,fixture.start.z); createWalkableTile(fixture.start); createWalkableTile(corpsePosition);
	BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto player=std::const_pointer_cast<Player>(session->getPlayer());
	auto backpack=Item::CreateItem(ITEM_BACKPACK); ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT)); auto potion=Item::CreateItem(266,2); ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),potion,INDEX_WHEREEVER,FLAG_NOLIMIT));
	auto corpse=Item::CreateItem(3994); ASSERT_NE(nullptr,corpse->getContainer()); corpse->setAttribute(ItemAttribute_t::CORPSEOWNER,player->getID()); ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(g_game().map.getTile(corpsePosition),corpse,INDEX_WHEREEVER,FLAG_NOLIMIT)); corpse->startDecaying(); auto gold=Item::CreateItem(3031,10); corpse->getContainer()->internalAddThing(gold);
	BotLootPolicy lootPolicy{.rules={{{.itemTypeId=3031,.valueCategory=2,.priority=10}}}}; const auto selected=manager.evaluateLoot(fixture.name,corpsePosition,77,{},lootPolicy); ASSERT_EQ(BotLootEligibility::Eligible,selected.eligibility); ASSERT_TRUE(selected.selected);
	BotLootTransferRequest request{.corpsePosition=corpsePosition,.sourceCreatureId=77,.corpseSignature=selected.corpse.signature,.itemTypeId=3031,.itemSignature=selected.selected->item.signature,.count=10}; EXPECT_EQ(BotLootTransferOutcome::Pending,manager.executeLoot(fixture.name,request,std::chrono::milliseconds(1),lootPolicy).outcome);
	CombatDamage damage; damage.primary={COMBAT_PHYSICALDAMAGE,-(player->getHealth()-std::max(1,player->getMaxHealth()/10))}; ASSERT_TRUE(g_game().combatChangeHealth(nullptr,player,damage)); BotHealingOption heal{.kind=BotHealingKind::HealthPotion,.itemTypeId=266,.availableCount=2,.minimumHealing=40,.maximumHealing=40}; BotSurvivalPolicy survivalPolicy; survivalPolicy.lowHealthPercent=40; survivalPolicy.moderateHealthPercent=30; survivalPolicy.highHealthPercent=25; survivalPolicy.recentDamageWeight=0;
	const auto critical=manager.evaluateSurvival(fixture.name,survivalPolicy,{heal}); ASSERT_EQ(BotSurvivalUrgency::Critical,critical.urgency); ASSERT_EQ(BotSurvivalDecision::Heal,critical.decision); ASSERT_EQ(BotLootPriorityState::WaitingForAuthoritativeBoundary,session->getLootProgress()->priority);
	const auto blocked=manager.executeLoot(fixture.name,request,std::chrono::milliseconds(2),lootPolicy); EXPECT_EQ(BotLootTransferOutcome::Cancelled,blocked.outcome); EXPECT_EQ(0U,corpse->getContainer()->getItemTypeCount(3031));
	EXPECT_EQ(BotHealingOutcome::Pending,manager.executeHealing(fixture.name,heal,std::chrono::milliseconds(3),survivalPolicy).outcome); EXPECT_EQ(BotLootPriorityState::HealingPriority,session->getLootProgress()->priority); EXPECT_EQ(BotLootTransferOutcome::Succeeded,session->getLootProgress()->authoritativeBoundaryOutcome); EXPECT_EQ(10U,session->getLootProgress()->authoritativeBoundaryMovedCount); EXPECT_EQ(BotHealingOutcome::Succeeded,manager.executeHealing(fixture.name,heal,std::chrono::milliseconds(4),survivalPolicy).outcome);
	EXPECT_NE(BotSurvivalUrgency::Critical,manager.evaluateSurvival(fixture.name,survivalPolicy,{}).urgency); const auto disappeared=manager.evaluateLoot(fixture.name,corpsePosition,77,{},lootPolicy); EXPECT_NE(BotLootEligibility::Eligible,disappeared.eligibility); EXPECT_EQ(BotLootPriorityState::LootAbandoned,session->getLootProgress()->priority);
	EXPECT_TRUE(manager.logout(fixture.name,false)); ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalRemoveItem(corpse)); ASSERT_TRUE(fixture.cleanup()); g_actions().clear();
}

TEST(PlayerBotIntegrationTest, LootTransferSelectsNestedMergeAndReconcilesRealPartialStack) {
	PlayerBotDatabaseFixture fixture(g_database());const Position corpsePosition(fixture.start.x+1,fixture.start.y,fixture.start.z);createWalkableTile(fixture.start);createWalkableTile(corpsePosition);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());
	auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));auto nested=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),nested,INDEX_WHEREEVER,FLAG_NOLIMIT));auto existing=Item::CreateItem(3031,40);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(nested->getContainer(),existing,INDEX_WHEREEVER,FLAG_NOLIMIT));
	auto corpse=Item::CreateItem(3994);corpse->setAttribute(ItemAttribute_t::CORPSEOWNER,player->getID());ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(g_game().map.getTile(corpsePosition),corpse,INDEX_WHEREEVER,FLAG_NOLIMIT));corpse->startDecaying();auto gold=Item::CreateItem(3031,30);corpse->getContainer()->internalAddThing(gold);BotLootPolicy policy{.rules={{{.itemTypeId=3031,.valueCategory=1,.priority=1}}}};const auto selected=manager.evaluateLoot(fixture.name,corpsePosition,81,{},policy);ASSERT_TRUE(selected.selected);BotLootTransferRequest request{.corpsePosition=corpsePosition,.sourceCreatureId=81,.corpseSignature=selected.corpse.signature,.itemTypeId=3031,.itemSignature=selected.selected->item.signature,.count=10};EXPECT_EQ(BotLootTransferOutcome::Pending,manager.executeLoot(fixture.name,request,std::chrono::milliseconds(1),policy).outcome);const auto result=manager.executeLoot(fixture.name,request,std::chrono::milliseconds(2),policy);EXPECT_EQ(BotLootTransferOutcome::Succeeded,result.outcome);EXPECT_EQ(10U,result.movedCount);EXPECT_EQ(20U,corpse->getContainer()->getItemTypeCount(3031));EXPECT_EQ(50U,nested->getContainer()->getItemTypeCount(3031));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalRemoveItem(corpse));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, LootTransferReconcilesCorpseRemovalAtAuthoritativeBoundary) {
	PlayerBotDatabaseFixture fixture(g_database());const Position corpsePosition(fixture.start.x+1,fixture.start.y,fixture.start.z);createWalkableTile(fixture.start);createWalkableTile(corpsePosition);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));auto corpse=Item::CreateItem(3994);corpse->setAttribute(ItemAttribute_t::CORPSEOWNER,player->getID());ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(g_game().map.getTile(corpsePosition),corpse,INDEX_WHEREEVER,FLAG_NOLIMIT));corpse->startDecaying();auto gold=Item::CreateItem(3031,5);corpse->getContainer()->internalAddThing(gold);BotLootPolicy policy{.rules={{{.itemTypeId=3031,.valueCategory=1,.priority=1}}}};const auto selected=manager.evaluateLoot(fixture.name,corpsePosition,82,{},policy);ASSERT_TRUE(selected.selected);BotLootTransferRequest request{.corpsePosition=corpsePosition,.sourceCreatureId=82,.corpseSignature=selected.corpse.signature,.itemTypeId=3031,.itemSignature=selected.selected->item.signature,.count=5};const auto before=std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031);EXPECT_EQ(BotLootTransferOutcome::Pending,manager.executeLoot(fixture.name,request,std::chrono::milliseconds(1),policy).outcome);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalRemoveItem(corpse));const auto result=manager.executeLoot(fixture.name,request,std::chrono::milliseconds(2),policy);EXPECT_EQ(BotLootTransferOutcome::CorpseExpired,result.outcome);EXPECT_EQ(5U,result.movedCount);EXPECT_EQ(before+5,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031));EXPECT_EQ(BotLootEligibility::InvalidCorpse,manager.evaluateLoot(fixture.name,corpsePosition,82,{},policy).eligibility);EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, LootTransferReportsAllCarriedDestinationsFullWithoutMutation) {
	PlayerBotDatabaseFixture fixture(g_database());
	const Position corpsePosition(fixture.start.x+1,fixture.start.y,fixture.start.z);createWalkableTile(fixture.start);createWalkableTile(corpsePosition);
	BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());
	auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));
	for(uint8_t slot=CONST_SLOT_FIRST;slot<=CONST_SLOT_LAST;++slot){const auto carried=player->getInventoryItem(static_cast<Slots_t>(slot));if(carried&&carried!=backpack&&carried->getContainer())ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalRemoveItem(carried));}
	for(uint32_t i=0;i<backpack->getContainer()->capacity();++i) backpack->getContainer()->internalAddThing(Item::CreateItem(3031,100));
	ASSERT_EQ(backpack->getContainer()->capacity(),backpack->getContainer()->size());const auto before=std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031);
	const auto fullInventory=BotLootTransfer::observeInventory(player);ASSERT_EQ(BotDestinationOutcome::AllDestinationsFull,BotLootTransfer::selectDestination(fullInventory,3031,5).outcome);
	auto corpse=Item::CreateItem(3994);corpse->setAttribute(ItemAttribute_t::CORPSEOWNER,player->getID());ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(g_game().map.getTile(corpsePosition),corpse,INDEX_WHEREEVER,FLAG_NOLIMIT));corpse->startDecaying();auto gold=Item::CreateItem(3031,5);corpse->getContainer()->internalAddThing(gold);
	BotLootPolicy policy{.rules={{{.itemTypeId=3031,.valueCategory=1,.priority=1}}}};const auto selected=manager.evaluateLoot(fixture.name,corpsePosition,83,{},policy);ASSERT_TRUE(selected.selected);BotLootTransferRequest request{.corpsePosition=corpsePosition,.sourceCreatureId=83,.corpseSignature=selected.corpse.signature,.itemTypeId=3031,.itemSignature=selected.selected->item.signature,.count=5};
	const auto result=manager.executeLoot(fixture.name,request,std::chrono::milliseconds(1),policy);EXPECT_EQ(BotLootTransferOutcome::AllDestinationsFull,result.outcome);EXPECT_EQ(5U,corpse->getContainer()->getItemTypeCount(3031));EXPECT_EQ(before,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031));
	EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalRemoveItem(corpse));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, RealCarriedSuppliesAndHealingConsumptionAreObservedAuthoritatively) {
	static const bool coreLoaded = g_scripts().loadEventSchedulerScripts("data/core.lua"); ASSERT_TRUE(coreLoaded); g_actions().clear(); ASSERT_TRUE(g_scripts().loadEventSchedulerScripts("tests/fixture/playerbots/survival_actions.lua"));
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto player=std::const_pointer_cast<Player>(session->getPlayer());
	auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));auto nested=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),nested,INDEX_WHEREEVER,FLAG_NOLIMIT));auto potion=Item::CreateItem(266,2);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),potion,INDEX_WHEREEVER,FLAG_NOLIMIT));auto arrows=Item::CreateItem(3447,25);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(nested->getContainer(),arrows,INDEX_WHEREEVER,FLAG_NOLIMIT));
	BotSupplyPolicy policy{.rules={{266,BotSupplyCategory::HealthHealing},{3447,BotSupplyCategory::Ammunition}},.thresholds={{BotSupplyCategory::HealthHealing,1,0,true},{BotSupplyCategory::Ammunition,10,0,false}}};const auto before=BotSupply::observe(player,policy);const auto potionEntry=std::ranges::find(before.entries,266,&BotSupplyEntry::itemTypeId);const auto arrowEntry=std::ranges::find(before.entries,3447,&BotSupplyEntry::itemTypeId);ASSERT_NE(before.entries.end(),potionEntry);ASSERT_NE(before.entries.end(),arrowEntry);EXPECT_EQ(2U,potionEntry->count);EXPECT_EQ(25U,arrowEntry->count);EXPECT_EQ(1,arrowEntry->depth);EXPECT_FALSE(before.containsWorldOwnership);EXPECT_EQ(BotSupplyIntent::Continue,manager.evaluateSupplies(fixture.name,policy,before.inventorySignature).intent);
	CombatDamage damage;damage.primary={COMBAT_PHYSICALDAMAGE,-50};ASSERT_TRUE(g_game().combatChangeHealth(nullptr,player,damage));BotHealingOption option{.kind=BotHealingKind::HealthPotion,.itemTypeId=266,.availableCount=2,.minimumHealing=40,.maximumHealing=40};EXPECT_EQ(BotHealingOutcome::Pending,manager.executeHealing(fixture.name,option,std::chrono::milliseconds(1)).outcome);EXPECT_EQ(BotHealingOutcome::Succeeded,manager.executeHealing(fixture.name,option,std::chrono::milliseconds(2)).outcome);const auto after=BotSupply::observe(player,policy);const auto afterPotion=std::ranges::find(after.entries,266,&BotSupplyEntry::itemTypeId);ASSERT_NE(after.entries.end(),afterPotion);EXPECT_EQ(1U,afterPotion->count);EXPECT_NE(before.inventorySignature,after.inventorySignature);EXPECT_EQ(BotSupplyFailure::ObservationStale,manager.evaluateSupplies(fixture.name,policy,before.inventorySignature).failure);
	EXPECT_TRUE(manager.logout(fixture.name,false));EXPECT_EQ(nullptr,manager.getSession(fixture.name));ASSERT_TRUE(fixture.cleanup());g_actions().clear();
}

TEST(PlayerBotIntegrationTest, OrdinaryNetworkPlayerInventoryObservationIsReadOnly) {
	PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);const auto player=std::make_shared<Player>();player->setName(fixture.name);ASSERT_TRUE(IOLoginDataLoad::preLoadPlayer(player,fixture.name));ASSERT_TRUE(IOLoginData::loadPlayerById(player,fixture.playerId,false));player->setID();player->setOnline(true);ASSERT_TRUE(g_game().placeCreature(player,fixture.start,false,true));auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));auto arrows=Item::CreateItem(3447,12);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),arrows,INDEX_WHEREEVER,FLAG_NOLIMIT));BotSupplyPolicy policy{.rules={{3447,BotSupplyCategory::Ammunition}}};const auto before=std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3447);const auto observed=BotSupply::observe(player,policy);EXPECT_EQ(before,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3447));EXPECT_NE(observed.entries.end(),std::ranges::find(observed.entries,3447,&BotSupplyEntry::itemTypeId));player->setOnline(false);const std::function<bool(const std::shared_ptr<Player>&)> noSave;EXPECT_EQ(ManagedPlayerRemovalResult::Complete,g_game().removeManagedPlayer(player,true,noSave));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, BoundedAdventureCoordinatorComposesRealMovementCombatDeathLootAndSupplies) {
	PlayerBotDatabaseFixture fixture(g_database());for(int x=-1;x<=3;++x)for(int y=-1;y<=1;++y)createWalkableTile(Position(fixture.start.x+x,fixture.start.y+y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));auto potion=Item::CreateItem(266,2);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),potion,INDEX_WHEREEVER,FLAG_NOLIMIT));
	const Position hunt(fixture.start.x+2,fixture.start.y,fixture.start.z);BotAdventurePolicy loopPolicy{.huntRegion={hunt,0},.returnRegion={fixture.start,0},.maximumDuration=std::chrono::seconds(60),.maximumCombatCount=2,.maximumRepeatedFailures=3,.minimumHealthPercent=20};uint64_t revision=1;auto observation=[&](Position position){return BotAdventureObservation{.revision=revision++,.position=position,.level=player->getLevel(),.experience=player->getExperience(),.healthPercent=BotSurvival::percentage(player->getHealth(),player->getMaxHealth())};};
	EXPECT_EQ(BotAdventureState::Preparing,manager.advanceAdventure(fixture.name,observation(player->getPosition()),std::chrono::milliseconds(1),loopPolicy).state);EXPECT_EQ(BotAdventureState::TravelingToArea,manager.advanceAdventure(fixture.name,observation(player->getPosition()),std::chrono::milliseconds(2),loopPolicy).state);ASSERT_EQ(BotRouteState::Ready,manager.startRoute(fixture.name,hunt,std::chrono::milliseconds(3)).state);auto route=manager.advanceRoute(fixture.name,std::chrono::milliseconds(4));for(uint8_t i=0;i<6&&route.state!=BotRouteState::Arrived;++i)route=manager.advanceRoute(fixture.name,std::chrono::milliseconds(100+i*100));ASSERT_EQ(BotRouteState::Arrived,route.state);ASSERT_EQ(hunt,player->getPosition());EXPECT_EQ(BotAdventureState::Searching,manager.advanceAdventure(fixture.name,observation(player->getPosition()),std::chrono::milliseconds(800),loopPolicy).state);
	const Position monsterPosition(hunt.x+1,hunt.y,hunt.z);auto monsterType=std::make_shared<MonsterType>("AdventureLoopMonster");monsterType->info.health=20;monsterType->info.healthMax=20;monsterType->info.experience=100;monsterType->info.lookcorpse=3994;auto monster=std::make_shared<Monster>(monsterType);ASSERT_TRUE(g_game().placeCreature(monster,monsterPosition,false,true));const auto sourceId=monster->getID();const auto selection=manager.evaluateCombat(fixture.name);ASSERT_EQ(sourceId,selection.selectedCreatureId);auto engage=observation(player->getPosition());engage.visibleTargetId=sourceId;EXPECT_EQ(BotAdventureState::Engaging,manager.advanceAdventure(fixture.name,engage,std::chrono::milliseconds(900),loopPolicy).state);const auto base=BotPerception::observe(player);ASSERT_TRUE(base);const auto combat=BotCombat::observe(player,*base);ASSERT_TRUE(combat);const auto target=std::ranges::find(combat->creatures,sourceId,&BotCombatCreatureObservation::id);ASSERT_NE(combat->creatures.end(),target);BotCombatExecutionRequest attack{sourceId,combat->revision,target->signature,player->getPosition()};ASSERT_EQ(BotCombatExecutionOutcome::TargetAcquired,manager.executeCombat(fixture.name,attack,std::chrono::milliseconds(901)).outcome);auto fighting=observation(player->getPosition());fighting.visibleTargetId=sourceId;fighting.combatActive=player->getAttackedCreature()!=nullptr;EXPECT_EQ(BotAdventureState::Fighting,manager.advanceAdventure(fixture.name,fighting,std::chrono::milliseconds(902),loopPolicy).state);
	const auto experienceBefore=player->getExperience();UPDATE_OTSYS_TIME();CombatDamage fatal;fatal.primary={COMBAT_PHYSICALDAMAGE,-100000};ASSERT_TRUE(g_game().combatChangeHealth(player,monster,fatal));ASSERT_EQ(0,monster->getHealth());monster->onDeath();ASSERT_TRUE(monster->isRemoved());EXPECT_GT(player->getExperience(),experienceBefore);const auto corpseTile=g_game().map.getTile(monsterPosition);ASSERT_NE(nullptr,corpseTile);std::shared_ptr<Item> corpse;for(const auto &item:*corpseTile->getItemList())if(item&&item->isCorpse()&&item->getContainer()){corpse=item;break;}ASSERT_NE(nullptr,corpse);auto gold=Item::CreateItem(3031,5);corpse->getContainer()->internalAddThing(gold);auto defeated=observation(player->getPosition());defeated.targetDefeated=true;defeated.corpseAvailable=true;EXPECT_EQ(BotAdventureState::Looting,manager.advanceAdventure(fixture.name,defeated,std::chrono::milliseconds(1000),loopPolicy).state);
	BotLootPolicy lootPolicy{.rules={{{.itemTypeId=3031,.valueCategory=1,.priority=1}}}};const auto selected=manager.evaluateLoot(fixture.name,monsterPosition,sourceId,{},lootPolicy);ASSERT_EQ(BotLootEligibility::Eligible,selected.eligibility);ASSERT_TRUE(selected.selected);BotLootTransferRequest request{.corpsePosition=monsterPosition,.sourceCreatureId=sourceId,.corpseSignature=selected.corpse.signature,.itemTypeId=3031,.itemSignature=selected.selected->item.signature,.count=5};EXPECT_EQ(BotLootTransferOutcome::Pending,manager.executeLoot(fixture.name,request,std::chrono::milliseconds(1001),lootPolicy).outcome);EXPECT_EQ(BotLootTransferOutcome::Succeeded,manager.executeLoot(fixture.name,request,std::chrono::milliseconds(1002),lootPolicy).outcome);auto looted=observation(player->getPosition());looted.lootComplete=true;EXPECT_EQ(BotAdventureState::EvaluatingSupplies,manager.advanceAdventure(fixture.name,looted,std::chrono::milliseconds(1003),loopPolicy).state);BotSupplyPolicy supplyPolicy{.rules={{266,BotSupplyCategory::HealthHealing}},.thresholds={{BotSupplyCategory::HealthHealing,1,0,true}}};EXPECT_EQ(BotSupplyIntent::Continue,manager.evaluateSupplies(fixture.name,supplyPolicy).intent);EXPECT_EQ(BotAdventureState::Searching,manager.advanceAdventure(fixture.name,observation(player->getPosition()),std::chrono::milliseconds(1004),loopPolicy).state);
	auto depleted=observation(player->getPosition());depleted.supplyIntent=BotSupplyIntent::ReturnRequired;EXPECT_EQ(BotAdventureState::Returning,manager.advanceAdventure(fixture.name,depleted,std::chrono::milliseconds(1005),loopPolicy).state);ASSERT_EQ(BotRouteState::Ready,manager.startRoute(fixture.name,fixture.start,std::chrono::milliseconds(1006)).state);auto blocker=std::make_shared<RemovalCountingCreature>();blocker->setID();ASSERT_TRUE(g_game().placeCreature(blocker,Position(hunt.x-1,hunt.y,hunt.z),false,true));auto blocked=manager.advanceRoute(fixture.name,std::chrono::milliseconds(1007));EXPECT_EQ(BotRouteReason::DynamicBlocker,blocked.reason);auto blockedObservation=observation(player->getPosition());blockedObservation.dynamicBlocker=true;EXPECT_EQ(BotAdventureState::Returning,manager.advanceAdventure(fixture.name,blockedObservation,std::chrono::milliseconds(1008),loopPolicy).state);ASSERT_TRUE(g_game().removeCreature(blocker,true));route=manager.advanceRoute(fixture.name,blocked.backoffDeadline);for(uint8_t i=0;i<8&&route.state!=BotRouteState::Arrived;++i)route=manager.advanceRoute(fixture.name,std::chrono::milliseconds(2000+i*100));ASSERT_EQ(BotRouteState::Arrived,route.state);EXPECT_EQ(BotAdventureState::Completed,manager.advanceAdventure(fixture.name,observation(player->getPosition()),std::chrono::milliseconds(3000),loopPolicy).state);EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, AdventureDeathAndSessionCloseAreTerminalAndOwnershipSafe) {
	PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());BotAdventureObservation active{.revision=1,.position=fixture.start};EXPECT_EQ(BotAdventureState::Preparing,manager.advanceAdventure(fixture.name,active,std::chrono::milliseconds(1)).state);CombatDamage damage;damage.primary={COMBAT_PHYSICALDAMAGE,-player->getHealth()};ASSERT_TRUE(g_game().combatChangeHealth(nullptr,player,damage));EXPECT_EQ(BotSurvivalState::Dead,manager.observeDeath(fixture.name).state);ASSERT_NE(nullptr,session->getAdventureProgress());EXPECT_EQ(BotAdventureState::Dead,session->getAdventureProgress()->state);EXPECT_FALSE(session->getAdventureProgress()->containsWorldOwnership);EXPECT_TRUE(manager.logout(fixture.name,false));EXPECT_EQ(nullptr,session->getAdventureProgress());ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, BoundedCampaignCompletesTenAuthoritativeCombatLootAndSupplyCycles) {
	PlayerBotDatabaseFixture fixture(g_database());
	const Position hunt(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	createWalkableTile(fixture.start);
	createWalkableTile(hunt);
	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	const auto player = std::const_pointer_cast<Player>(session->getPlayer());
	auto backpack = Item::CreateItem(ITEM_BACKPACK);
	ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(player, backpack, CONST_SLOT_BACKPACK, FLAG_NOLIMIT));

	BotCampaignPolicy campaignPolicy { .allowedRegions = { { "nearby-hunt", { hunt, 1 }, 4 } } };
	auto campaign = BotCampaign::begin(campaignPolicy);
	ASSERT_EQ(BotCampaignState::Running, campaign.state);
	BotLootPolicy lootPolicy { .rules = { { { .itemTypeId = 3031, .valueCategory = 1, .priority = 1 } } } };
	BotSupplyPolicy supplyPolicy;
	const auto experienceBefore = player->getExperience();
	uint32_t expectedGold = 0;

	for (uint16_t cycle = 0; cycle < campaignPolicy.targetKillCount; ++cycle) {
		campaign = BotCampaign::recordAttempt(campaign, campaignPolicy);
		ASSERT_EQ(BotCampaignState::Combat, campaign.state);
		auto monsterType = std::make_shared<MonsterType>(fmt::format("CampaignMonster{}", cycle));
		monsterType->info.health = 1;
		monsterType->info.healthMax = 1;
		monsterType->info.experience = 100;
		monsterType->info.lookcorpse = 3994;
		auto monster = std::make_shared<Monster>(monsterType);
		ASSERT_TRUE(g_game().placeCreature(monster, hunt, false, true));
		const auto sourceId = monster->getID();
		const auto selection = manager.evaluateCombat(fixture.name);
		ASSERT_EQ(sourceId, selection.selectedCreatureId);
		const auto base = BotPerception::observe(player);
		ASSERT_TRUE(base);
		const auto combat = BotCombat::observe(player, *base);
		ASSERT_TRUE(combat);
		const auto target = std::ranges::find(combat->creatures, sourceId, &BotCombatCreatureObservation::id);
		ASSERT_NE(combat->creatures.end(), target);
		ASSERT_EQ(BotCombatExecutionOutcome::TargetAcquired, manager.executeCombat(fixture.name, { sourceId, combat->revision, target->signature, player->getPosition() }, std::chrono::milliseconds(cycle * 10 + 1)).outcome);
		UPDATE_OTSYS_TIME();
		CombatDamage attackDamage;
		attackDamage.primary = { COMBAT_PHYSICALDAMAGE, -1 };
		Combat::doCombatHealth(player, monster, attackDamage, {});
		ASSERT_EQ(0, monster->getHealth()) << "ordinary Player attack did not defeat campaign fixture";
		// The integration dispatcher is intentionally stopped; complete the already
		// authoritative zero-health death boundary synchronously.
		monster->onDeath();
		ASSERT_TRUE(monster->isRemoved());
		campaign = BotCampaign::recordAuthoritativeKill(campaign, sourceId, campaignPolicy);

		const auto corpseTile = g_game().map.getTile(hunt);
		ASSERT_NE(nullptr, corpseTile);
		std::shared_ptr<Item> corpse;
		for (const auto &item : *corpseTile->getItemList()) if (item && item->isCorpse() && item->getContainer()) { corpse = item; break; }
		ASSERT_NE(nullptr, corpse);
		auto gold = Item::CreateItem(3031, 1);
		corpse->getContainer()->internalAddThing(gold);
		const auto selected = manager.evaluateLoot(fixture.name, hunt, sourceId, {}, lootPolicy);
		ASSERT_TRUE(selected.selected);
		BotLootTransferRequest request { .corpsePosition = hunt, .sourceCreatureId = sourceId, .corpseSignature = selected.corpse.signature, .itemTypeId = 3031, .itemSignature = selected.selected->item.signature, .count = 1 };
		ASSERT_EQ(BotLootTransferOutcome::Pending, manager.executeLoot(fixture.name, request, std::chrono::milliseconds(cycle * 10 + 2), lootPolicy).outcome);
		ASSERT_EQ(BotLootTransferOutcome::Succeeded, manager.executeLoot(fixture.name, request, std::chrono::milliseconds(cycle * 10 + 3), lootPolicy).outcome);
		++expectedGold;
		EXPECT_EQ(expectedGold, std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031));
		EXPECT_EQ(BotSupplyIntent::Continue, manager.evaluateSupplies(fixture.name, supplyPolicy).intent);
		ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalRemoveItem(corpse));
	}

	EXPECT_EQ(10U, campaign.authoritativeKills);
	EXPECT_EQ(BotCampaignState::Completed, campaign.state);
	EXPECT_GT(player->getExperience(), experienceBefore);
	EXPECT_TRUE(BotCampaign::safeSaveBoundary(false, false, false, false));
	EXPECT_TRUE(manager.logout(fixture.name, true));
	const auto loadedSession = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, loadedSession);
	const auto loaded = std::const_pointer_cast<Player>(loadedSession->getPlayer());
	EXPECT_GT(loaded->getExperience(), experienceBefore);
	EXPECT_EQ(expectedGold, std::static_pointer_cast<Cylinder>(loaded)->getItemTypeCount(3031));
	ASSERT_NE(nullptr, loadedSession->getAdventureProgress());
	ASSERT_NE(nullptr, loadedSession->getAttackExecutionState());
	EXPECT_EQ(BotAdventureState::Idle, loadedSession->getAdventureProgress()->state);
	EXPECT_EQ(BotAttackState::Idle, loadedSession->getAttackExecutionState()->state);
	EXPECT_TRUE(manager.logout(fixture.name, false));
	ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, AuthoritativeDeathPersistsTempleRecoveryAndRebuildsSessionState) {
	PlayerBotDatabaseFixture fixture(g_database());
	const Position temple(fixture.start.x + 2, fixture.start.y, fixture.start.z);
	createWalkableTile(fixture.start);
	createWalkableTile(Position(fixture.start.x + 1, fixture.start.y, fixture.start.z));
	createWalkableTile(temple);
	g_game().map.towns.getOrCreateTown(1)->setTemplePos(temple);
	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	const auto player = std::const_pointer_cast<Player>(session->getPlayer());
	BotCampaignPolicy policy { .allowedRegions = { { "temple-recovery", { temple, 0 }, 4 } } };
	auto campaign = BotCampaign::begin(policy);

	auto killerType = std::make_shared<MonsterType>("CampaignRecoveryKiller");
	auto killer = std::make_shared<Monster>(killerType);
	ASSERT_TRUE(g_game().placeCreature(killer, Position(fixture.start.x + 1, fixture.start.y, fixture.start.z), false, true));
	CombatDamage lethal;
	lethal.primary = { COMBAT_PHYSICALDAMAGE, -player->getHealth() };
	ASSERT_TRUE(g_game().combatChangeHealth(killer, player, lethal));
	ASSERT_EQ(0, player->getHealth());
	EXPECT_EQ(BotSurvivalState::Dead, manager.observeDeath(fixture.name).state);
	player->onDeath();
	EXPECT_EQ(temple, player->getLoginPosition());
	campaign = BotCampaign::recordDeath(campaign, player->getID(), policy);
	EXPECT_EQ(BotCampaignState::Recovering, campaign.state);
	EXPECT_TRUE(manager.logout(fixture.name, false));
	EXPECT_EQ(nullptr, session->getAdventureProgress());
	EXPECT_EQ(nullptr, session->getAttackExecutionState());
	EXPECT_TRUE(g_game().removeCreature(killer, true));

	const auto recoveredSession = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, recoveredSession);
	const auto recovered = std::const_pointer_cast<Player>(recoveredSession->getPlayer());
	EXPECT_EQ(temple, recovered->getPosition());
	campaign = BotCampaign::reconstruct(campaign, 1, recovered->getPosition(), policy.allowedRegions.front(), policy);
	EXPECT_EQ(BotCampaignState::Resumed, campaign.state);
	EXPECT_FALSE(campaign.freshObservationRequired);
	ASSERT_NE(nullptr, recoveredSession->getAdventureProgress());
	ASSERT_NE(nullptr, recoveredSession->getRouteProgress());
	EXPECT_EQ(BotAdventureState::Idle, recoveredSession->getAdventureProgress()->state);
	EXPECT_EQ(BotRouteState::Idle, recoveredSession->getRouteProgress()->state);
	EXPECT_TRUE(manager.logout(fixture.name, false));
	ASSERT_TRUE(fixture.cleanup());
}

namespace {
	uint16_t equipmentType(const std::function<bool(const ItemType &)> &predicate) {
		for (const auto &type : Item::items.getItems()) if (type.id && type.loaded && type.pickupable && predicate(type)) return type.id;
		return 0;
	}
}

TEST(PlayerBotIntegrationTest, EquipmentAssessmentObservesRealEquippedAndCarriedItemsWithoutMutation) {
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto player=std::const_pointer_cast<Player>(session->getPlayer());
	const auto armorId=equipmentType([](const ItemType &type){return type.isArmor()&&type.armor>0;}); ASSERT_NE(0,armorId); auto armor=Item::CreateItem(armorId); std::static_pointer_cast<Cylinder>(player)->addThing(CONST_SLOT_ARMOR,armor);
	const auto weaponId=equipmentType([](const ItemType &type){return type.isWeapon()&&type.attack>0&&(type.slotPosition&SLOTP_TWO_HAND)==0;}); ASSERT_NE(0,weaponId); auto weapon=Item::CreateItem(weaponId); auto backpack=Item::CreateItem(ITEM_BACKPACK); ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT)); ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),weapon,INDEX_WHEREEVER,FLAG_NOLIMIT));
	const auto armorBefore=player->getInventoryItem(CONST_SLOT_ARMOR); const auto carriedBefore=backpack->getContainer()->getItemTypeCount(weaponId); const auto observed=manager.evaluateEquipment(fixture.name);
	EXPECT_NE(observed.items.end(),std::ranges::find_if(observed.items,[&](const auto &item){return item.itemTypeId==armorId&&item.equipped&&item.armor==armor->getArmor();}));
	EXPECT_NE(observed.items.end(),std::ranges::find_if(observed.items,[&](const auto &item){return item.itemTypeId==weaponId&&!item.equipped&&item.attack==weapon->getAttack();}));
	EXPECT_EQ(armorBefore,player->getInventoryItem(CONST_SLOT_ARMOR)); EXPECT_EQ(carriedBefore,backpack->getContainer()->getItemTypeCount(weaponId)); EXPECT_TRUE(manager.logout(fixture.name,false)); ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, EquipmentAssessmentReadsAuthoritativeRequirementsStatsAndCapacity) {
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto player=std::const_pointer_cast<Player>(session->getPlayer()); auto backpack=Item::CreateItem(ITEM_BACKPACK); ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));
	const auto weaponId=equipmentType([](const ItemType &type){return type.isWeapon()&&type.attack>0&&type.minReqLevel>0;}); const auto armorId=equipmentType([](const ItemType &type){return type.isArmor()&&type.armor>0;}); const auto shieldId=equipmentType([](const ItemType &type){return type.isShield()&&type.defense>0;}); ASSERT_NE(0,weaponId);ASSERT_NE(0,armorId);ASSERT_NE(0,shieldId);
	ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),Item::CreateItem(weaponId),INDEX_WHEREEVER,FLAG_NOLIMIT));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),Item::CreateItem(armorId),INDEX_WHEREEVER,FLAG_NOLIMIT));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),Item::CreateItem(shieldId),INDEX_WHEREEVER,FLAG_NOLIMIT));
	const auto observed=manager.evaluateEquipment(fixture.name); EXPECT_EQ(player->getFreeCapacity(),observed.freeCapacity); EXPECT_EQ(player->getVocationId(),observed.vocationId);
	const auto weapon=std::ranges::find(observed.items,weaponId,&BotItemObservation::itemTypeId);ASSERT_NE(observed.items.end(),weapon);EXPECT_EQ(Item::items[weaponId].minReqLevel,weapon->minimumLevel);EXPECT_EQ(Item::items[weaponId].attack,weapon->attack);
	const auto armor=std::ranges::find(observed.items,armorId,&BotItemObservation::itemTypeId);ASSERT_NE(observed.items.end(),armor);EXPECT_EQ(Item::items[armorId].armor,armor->armor);
	const auto shield=std::ranges::find(observed.items,shieldId,&BotItemObservation::itemTypeId);ASSERT_NE(observed.items.end(),shield);EXPECT_EQ(Item::items[shieldId].defense,shield->defense);
	EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, EquipmentAssessmentTraversesOnlyBoundedOwnedContainersAndClassifiesSupplies) {
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game()); const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto player=std::const_pointer_cast<Player>(session->getPlayer()); auto backpack=Item::CreateItem(ITEM_BACKPACK),nested=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),nested,INDEX_WHEREEVER,FLAG_NOLIMIT));auto supply=Item::CreateItem(3031,12);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(nested->getContainer(),supply,INDEX_WHEREEVER,FLAG_NOLIMIT));
	auto unrelated=Item::CreateItem(ITEM_BACKPACK);auto hidden=Item::CreateItem(3031,9);unrelated->getContainer()->internalAddThing(hidden);
	BotEquipmentPolicy policy{.supplyItemTypeIds={3031},.maximumInventoryContainers=16,.maximumNestingDepth=2};const auto observed=manager.evaluateEquipment(fixture.name,policy);const auto found=std::ranges::find(observed.items,3031,&BotItemObservation::itemTypeId);ASSERT_NE(observed.items.end(),found);EXPECT_TRUE(found->supply);EXPECT_EQ(2U,found->path.childIndices.size());EXPECT_FALSE(observed.containerBudgetExceeded);
	EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, EquipmentAssessmentUsesExplicitNpcPricesAndPreservesOrdinaryPlayerInventory) {
	PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);auto player=std::make_shared<Player>();player->setName(fixture.name);ASSERT_TRUE(IOLoginDataLoad::preLoadPlayer(player,fixture.name));ASSERT_TRUE(IOLoginData::loadPlayerById(player,fixture.playerId,false));player->setID();player->setOnline(true);ASSERT_TRUE(g_game().placeCreature(player,fixture.start,false,true));auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));auto item=Item::CreateItem(3031,5);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),item,INDEX_WHEREEVER,FLAG_NOLIMIT));
	BotEquipmentPolicy policy{.knownPrices={{3031,100,50,BotItemValueSource::NpcObservation}}};const auto before=std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031);const auto observed=BotEquipment::observe(player,policy);const auto gold=std::ranges::find(observed.items,3031,&BotItemObservation::itemTypeId);ASSERT_NE(observed.items.end(),gold);EXPECT_EQ(100U,gold->knownNpcBuyPrice);EXPECT_EQ(50U,gold->knownNpcSellPrice);EXPECT_EQ(BotItemValueSource::NpcObservation,gold->priceSource);EXPECT_EQ(before,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031));player->setOnline(false);const std::function<bool(const std::shared_ptr<Player>&)> noSave;EXPECT_EQ(ManagedPlayerRemovalResult::Complete,g_game().removeManagedPlayer(player,true,noSave));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, EquipmentAssessmentSessionCloseClearsStateAndFixtures) {
	PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);EXPECT_FALSE(manager.evaluateEquipment(fixture.name).containsWorldOwnership());ASSERT_NE(nullptr,session->getEquipmentObservation());EXPECT_TRUE(session->getEquipmentObservation()->has_value());EXPECT_TRUE(manager.logout(fixture.name,false));EXPECT_EQ(nullptr,session->getEquipmentObservation());EXPECT_TRUE(fixture.cleanup());EXPECT_FALSE(fixture.hasCommittedRows());
}

	constexpr uint16_t playerBotShopItemId = 266;

	std::shared_ptr<Npc> createPlayerBotShopNpc(const Position &position) {
		static const bool loaded = g_scripts().loadEventSchedulerScripts("data/core.lua")
			&& g_scripts().loadEventSchedulerScripts("data/npclib/load.lua")
			&& g_scripts().loadScripts("tests/fixture/playerbots/npcs", false, false);
		if (!loaded) return nullptr;
		const auto type = g_npcs().getNpcType("PlayerBotShopFixtureNpc");
		if (!type) return nullptr;
		auto npc = std::make_shared<Npc>(type);
		if (!g_game().placeCreature(npc, position, false, true)) return nullptr;
		return npc;
	}

	BotShopTransactionRequest shopRequest(const BotShopObservation &observation, BotShopTransactionKind kind, uint16_t amount = 1) {
		const auto offer = std::ranges::find(observation.offers, playerBotShopItemId, &BotShopOffer::itemTypeId);
		if (offer == observation.offers.end()) return {};
		return { .kind=kind,.npcId=observation.npcId,.itemTypeId=offer->itemTypeId,.subType=offer->subType,.amount=amount,.observedPrice=kind==BotShopTransactionKind::Buy?offer->buyPrice:offer->sellPrice,.shopRevision=observation.revision };
	}

TEST(PlayerBotIntegrationTest, ShopObservationUsesOnlyRealExposedOffersAndAuthoritativeMoney) {
	PlayerBotDatabaseFixture fixture(g_database());for(int x=0;x<=3;++x)createWalkableTile(Position(fixture.start.x+x,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());const auto npc=createPlayerBotShopNpc(Position(fixture.start.x+3,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,npc);auto route=manager.startRoute(fixture.name,Position(fixture.start.x+2,fixture.start.y,fixture.start.z),std::chrono::milliseconds(900));ASSERT_EQ(BotRouteState::Ready,route.state);for(uint8_t attempt=0;attempt<8&&route.state!=BotRouteState::Arrived;++attempt)route=manager.advanceRoute(fixture.name,std::chrono::milliseconds(1000+attempt*1000));ASSERT_EQ(BotRouteState::Arrived,route.state);EXPECT_EQ(Position(fixture.start.x+2,fixture.start.y,fixture.start.z),player->getPosition());
	EXPECT_FALSE(manager.observeShop(fixture.name,npc->getID()).open);ASSERT_TRUE(player->openShopWindow(npc,npc->getShopItemVector(player->getGUID())));const auto observed=manager.observeShop(fixture.name,npc->getID());EXPECT_TRUE(observed.open);EXPECT_TRUE(observed.npcVisible);EXPECT_EQ(player->getMoney(),observed.money.carried);EXPECT_EQ(player->getBankBalance(),observed.money.bank);EXPECT_NE(observed.offers.end(),std::ranges::find(observed.offers,playerBotShopItemId,&BotShopOffer::itemTypeId));
	EXPECT_TRUE(g_game().removeCreature(npc,true));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, AuthoritativeShopBuyRequiresObservedMoneyAndInventoryChanges) {
	PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);createWalkableTile(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));const auto [added,addResult]=g_game().addMoney(player,500);ASSERT_EQ(500U,added);ASSERT_EQ(RETURNVALUE_NOERROR,addResult);const auto npc=createPlayerBotShopNpc(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,npc);ASSERT_TRUE(player->openShopWindow(npc,npc->getShopItemVector(player->getGUID())));
	const auto observed=manager.observeShop(fixture.name,npc->getID());const auto request=shopRequest(observed,BotShopTransactionKind::Buy,2);ASSERT_EQ(playerBotShopItemId,request.itemTypeId);const auto moneyBefore=player->getMoney()+player->getBankBalance();const auto itemsBefore=std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(playerBotShopItemId);UPDATE_OTSYS_TIME();EXPECT_EQ(BotShopOutcome::Pending,manager.executeShop(fixture.name,request,std::chrono::milliseconds(1)).outcome);const auto verified=manager.executeShop(fixture.name,request,std::chrono::milliseconds(2));EXPECT_EQ(BotShopOutcome::Succeeded,verified.outcome);EXPECT_EQ(itemsBefore+2,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(playerBotShopItemId));EXPECT_EQ(moneyBefore-100,player->getMoney()+player->getBankBalance());
	EXPECT_TRUE(g_game().removeCreature(npc,true));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, AuthoritativeShopRejectsInsufficientMoneyAndStaleOfferWithoutMutation) {
	PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);createWalkableTile(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());const auto npc=createPlayerBotShopNpc(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,npc);ASSERT_TRUE(player->openShopWindow(npc,npc->getShopItemVector(player->getGUID())));const auto observed=manager.observeShop(fixture.name,npc->getID());auto request=shopRequest(observed,BotShopTransactionKind::Buy);const auto before=std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(playerBotShopItemId);EXPECT_EQ(BotShopOutcome::InsufficientMoney,manager.executeShop(fixture.name,request,std::chrono::milliseconds(1)).outcome);++request.shopRevision;EXPECT_EQ(BotShopFailure::StaleObservation,manager.executeShop(fixture.name,request,std::chrono::milliseconds(2)).failure);EXPECT_EQ(before,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(playerBotShopItemId));
	EXPECT_TRUE(g_game().removeCreature(npc,true));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, AuthoritativeShopSellPreservesReservedItemsAndReconcilesProceeds) {
	PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);createWalkableTile(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),Item::CreateItem(playerBotShopItemId,3),INDEX_WHEREEVER,FLAG_NOLIMIT));const auto npc=createPlayerBotShopNpc(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,npc);ASSERT_TRUE(player->openShopWindow(npc,npc->getShopItemVector(player->getGUID())));const auto observed=manager.observeShop(fixture.name,npc->getID());const auto request=shopRequest(observed,BotShopTransactionKind::Sell,2);BotShopPolicy reserved{.reservedItemTypeIds={playerBotShopItemId}};EXPECT_EQ(BotShopOutcome::ReservedItem,manager.executeShop(fixture.name,request,std::chrono::milliseconds(1),reserved).outcome);const auto moneyBefore=player->getMoney()+player->getBankBalance();UPDATE_OTSYS_TIME();EXPECT_EQ(BotShopOutcome::Pending,manager.executeShop(fixture.name,request,std::chrono::milliseconds(2)).outcome);const auto verified=manager.executeShop(fixture.name,request,std::chrono::milliseconds(3));EXPECT_EQ(BotShopOutcome::Succeeded,verified.outcome);EXPECT_EQ(1U,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(playerBotShopItemId));EXPECT_EQ(moneyBefore+40,player->getMoney()+player->getBankBalance());
	EXPECT_TRUE(g_game().removeCreature(npc,true));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, ShopFocusLossMovementAndSessionCloseCancelSafely) {
	PlayerBotDatabaseFixture fixture(g_database());for(int x=0;x<=6;++x)createWalkableTile(Position(fixture.start.x+x,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());const auto npc=createPlayerBotShopNpc(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,npc);ASSERT_TRUE(player->openShopWindow(npc,npc->getShopItemVector(player->getGUID())));const auto request=shopRequest(manager.observeShop(fixture.name,npc->getID()),BotShopTransactionKind::Buy);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalTeleport(player,Position(fixture.start.x+6,fixture.start.y,fixture.start.z),true));const auto lost=manager.executeShop(fixture.name,request,std::chrono::milliseconds(1));EXPECT_TRUE(lost.outcome==BotShopOutcome::ShopClosed||lost.outcome==BotShopOutcome::NpcUnavailable);EXPECT_EQ(nullptr,player->getShopOwner());EXPECT_TRUE(manager.logout(fixture.name,false));EXPECT_EQ(nullptr,session->getShopProgress());EXPECT_TRUE(g_game().removeCreature(npc,true));ASSERT_TRUE(fixture.cleanup());EXPECT_FALSE(fixture.hasCommittedRows());
}

namespace {
std::shared_ptr<DepotChest> openPlayerBotDepot(const std::shared_ptr<Player> &player,uint16_t depotId=7){const Position position(player->getPosition().x+1,player->getPosition().y,player->getPosition().z);createWalkableTile(position);const auto tile=g_game().map.getTile(position);auto lockerItem=Item::CreateItem(ITEM_LOCKER);if(!tile||!lockerItem||!lockerItem->getContainer()||!lockerItem->getContainer()->getDepotLocker())return nullptr;lockerItem->getContainer()->getDepotLocker()->setDepotId(depotId);tile->internalAddThing(lockerItem);if(!g_actions().useItem(player,position,tile->getThingIndex(lockerItem),lockerItem,false))return nullptr;return player->getDepotChest(1,false);}
BotResupplyRequest firstTransfer(const std::shared_ptr<Player> &player,uint32_t depotId,const BotResupplyPolicy &policy){const auto inventory=BotEquipment::observe(player);const auto depot=BotResupply::observeDepot(player,depotId,policy);const auto plan=BotResupply::plan(inventory,depot,policy);return plan.transfers.empty()?BotResupplyRequest{}:plan.transfers.front();}
}

TEST(PlayerBotIntegrationTest, ResupplyOpensAndObservesOnlyOwnRealDepotWithinBounds){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());const auto chest=openPlayerBotDepot(player);ASSERT_NE(nullptr,chest);chest->internalAddThing(Item::CreateItem(7618,20));BotResupplyPolicy policy{.targets={{7618,5,10,2}},.maximumDepotContainers=2,.maximumNestingDepth=1,.maximumItemsObserved=8};const auto observed=BotResupply::observeDepot(player,7,policy);EXPECT_TRUE(observed.open);EXPECT_EQ(7,observed.depotId);EXPECT_NE(observed.items.end(),std::ranges::find(observed.items,7618,&BotDepotItemObservation::itemTypeId));EXPECT_FALSE(BotResupply::observeDepot(player,8,policy).open);EXPECT_FALSE(observed.containsWorldOwnership);EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, ResupplyMovesRealDepotStackToInventoryAndUpdatesSupply){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));const auto chest=openPlayerBotDepot(player);ASSERT_NE(nullptr,chest);chest->internalAddThing(Item::CreateItem(3031,20));BotResupplyPolicy policy{.targets={{3031,5,10,2}},.maximumTransferCount=8};const auto request=firstTransfer(player,7,policy);ASSERT_EQ(BotResupplyDirection::DepotToInventory,request.direction);const auto result=BotResupply::executeTransfer(g_game(),player,request,policy);EXPECT_EQ(BotResupplyOutcome::Succeeded,result.outcome);EXPECT_EQ(8,result.movedCount);EXPECT_EQ(12,chest->getItemTypeCount(3031));EXPECT_EQ(8,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031));BotSupplyPolicy supplies{.rules={{3031,BotSupplyCategory::HealthHealing}},.thresholds={{BotSupplyCategory::HealthHealing,5,0,true}}};EXPECT_NE(BotSupplyIntent::NoHealingSupplies,manager.evaluateSupplies(fixture.name,supplies).intent);EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, ResupplyDepositsRealInventoryStackAndReconcilesPartialMovement){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),Item::CreateItem(3031,20),INDEX_WHEREEVER,FLAG_NOLIMIT));const auto chest=openPlayerBotDepot(player);ASSERT_NE(nullptr,chest);BotResupplyPolicy policy{.depositItemTypeIds={3031},.maximumTransferCount=7};const auto request=firstTransfer(player,7,policy);ASSERT_EQ(BotResupplyDirection::InventoryToDepot,request.direction);const auto result=BotResupply::executeTransfer(g_game(),player,request,policy);EXPECT_EQ(BotResupplyOutcome::Succeeded,result.outcome);EXPECT_EQ(7,result.movedCount);EXPECT_EQ(13,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031));EXPECT_EQ(7,chest->getItemTypeCount(3031));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, ResupplyRejectsStaleAndUnavailableDepotWithoutMutation){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());const auto chest=openPlayerBotDepot(player);ASSERT_NE(nullptr,chest);chest->internalAddThing(Item::CreateItem(7618,10));BotResupplyPolicy policy{.targets={{7618,5,5,0}}};auto request=firstTransfer(player,7,policy);++request.depotRevision;const auto before=chest->getItemTypeCount(7618);EXPECT_EQ(BotResupplyOutcome::SourceStale,BotResupply::executeTransfer(g_game(),player,request,policy).outcome);player->setLastDepotId(-1);EXPECT_EQ(BotResupplyOutcome::DepotUnavailable,BotResupply::executeTransfer(g_game(),player,request,policy).outcome);EXPECT_EQ(before,chest->getItemTypeCount(7618));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, EquipmentExecutionUsesOrdinarySlotExchangeAndPreservesPreviousItem){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));const auto oldId=equipmentType([](const ItemType&t){return t.isArmor()&&t.armor>0&&t.minReqLevel==0;});const auto newId=equipmentType([&](const ItemType&t){return t.isArmor()&&t.id!=oldId&&t.minReqLevel==0;});ASSERT_NE(0,oldId);ASSERT_NE(0,newId);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,Item::CreateItem(oldId),CONST_SLOT_ARMOR,FLAG_NOLIMIT));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),Item::CreateItem(newId),INDEX_WHEREEVER,FLAG_NOLIMIT));BotEquipmentPolicy policy{.upgradeThreshold=-100000};const auto observed=BotEquipment::observe(player,policy);const auto upgrades=BotEquipment::upgrades(observed,policy);const auto candidate=std::ranges::find(upgrades,newId,&BotUpgradeCandidate::itemTypeId);ASSERT_NE(upgrades.end(),candidate);const auto result=BotResupply::executeEquipment(g_game(),player,{*candidate,{CONST_SLOT_BACKPACK,{}}},policy);EXPECT_EQ(BotResupplyOutcome::Succeeded,result.outcome);ASSERT_NE(nullptr,player->getInventoryItem(CONST_SLOT_ARMOR));EXPECT_EQ(newId,player->getInventoryItem(CONST_SLOT_ARMOR)->getID());EXPECT_EQ(1,backpack->getContainer()->getItemTypeCount(oldId));EXPECT_TRUE(manager.save(fixture.name));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, ResupplySessionCloseIsSafeAndOrdinaryInventoryRemainsAvailable){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());ASSERT_NE(nullptr,openPlayerBotDepot(player));EXPECT_TRUE(BotResupply::observeDepot(player,7).open);EXPECT_TRUE(manager.logout(fixture.name,false));EXPECT_EQ(nullptr,session->getPlayer());EXPECT_TRUE(fixture.cleanup());EXPECT_FALSE(fixture.hasCommittedRows());}

namespace{
std::shared_ptr<Npc> createPlayerBotDialogueNpc(const Position&position){const auto loader=createPlayerBotShopNpc(position);if(loader)(void)g_game().removeCreature(loader,true);const auto type=g_npcs().getNpcType("PlayerBotDialogueFixtureNpc");if(!type)return nullptr;auto npc=std::make_shared<Npc>(type);if(!g_game().placeCreature(npc,position,false,true))return nullptr;return npc;}
BotNpcDialoguePolicy integrationDialoguePolicy(){return{.configuredNpcNames={"playerbotdialoguefixturenpc"},.phrases={{BotDialogueIntent::Greeting,"hi","welcome"},{BotDialogueIntent::ConfiguredTopic,"job","guide"},{BotDialogueIntent::Yes,"yes","confirmed"},{BotDialogueIntent::No,"no","cancelled"},{BotDialogueIntent::Farewell,"bye","farewell"}},.maximumPhrases=6,.maximumRetries=2,.maximumTopics=3,.maximumResponseLength=64,.responseWait=std::chrono::milliseconds(10)};}
std::shared_ptr<Npc> createPlayerBotQuestWitnessNpc(const Position&position){const auto type=g_npcs().getNpcType("PlayerBotQuestWitnessFixtureNpc");if(!type)return nullptr;auto npc=std::make_shared<Npc>(type);if(!g_game().placeCreature(npc,position,false,true))return nullptr;return npc;}
BotNpcDialoguePolicy questGuideStartPolicy(){auto policy=integrationDialoguePolicy();policy.phrases.push_back({BotDialogueIntent::ConfiguredFollowUp,"mission","started"});policy.maximumPhrases=7;return policy;}
BotNpcDialoguePolicy questWitnessPolicy(){return{.configuredNpcNames={"playerbotquestwitnessfixturenpc"},.phrases={{BotDialogueIntent::Greeting,"hi","welcome"},{BotDialogueIntent::ConfiguredTopic,"witness","witnessed"},{BotDialogueIntent::Farewell,"bye","farewell"}},.maximumPhrases=4,.maximumRetries=2,.maximumTopics=2,.maximumResponseLength=64,.responseWait=std::chrono::milliseconds(10)};}
}

TEST(PlayerBotIntegrationTest, DialogueObservesApproachesAndGreetsRealNpcThroughOrdinarySpeech){PlayerBotDatabaseFixture fixture(g_database());for(int x=0;x<=3;++x)createWalkableTile(Position(fixture.start.x+x,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());const auto npc=createPlayerBotDialogueNpc(Position(fixture.start.x+3,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,npc);auto route=manager.startRoute(fixture.name,Position(fixture.start.x+2,fixture.start.y,fixture.start.z),std::chrono::milliseconds(1));for(uint8_t i=0;i<8&&route.state!=BotRouteState::Arrived;++i)route=manager.advanceRoute(fixture.name,std::chrono::milliseconds(100+i*100));ASSERT_EQ(BotRouteState::Arrived,route.state);const auto policy=integrationDialoguePolicy();const auto observed=manager.observeDialogue(fixture.name,policy);const auto selected=BotDialogue::select(observed,policy);ASSERT_TRUE(selected);EXPECT_EQ(npc->getID(),selected->npcId);EXPECT_EQ(BotDialogueState::AwaitingResponse,manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::Greeting,std::chrono::milliseconds(1000),policy).state);const auto result=manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::Greeting,std::chrono::milliseconds(1001),policy);EXPECT_EQ(BotDialogueResponse::GreetingAccepted,result.response);EXPECT_TRUE(npc->isInteractingWithPlayer(player->getID()));EXPECT_TRUE(g_game().removeCreature(npc,true));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, DialogueVisibleResponseBufferIsBoundedValueOnly){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);createWalkableTile(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());const auto npc=createPlayerBotDialogueNpc(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,npc);for(int i=0;i<20;++i)ASSERT_TRUE(g_game().internalCreatureSay(npc,TALKTYPE_PRIVATE_NP,fmt::format("visible {}",i),false));EXPECT_EQ(16,player->getVisibleSpeech().size());EXPECT_EQ("visible 19",player->getVisibleSpeech().back().text);EXPECT_TRUE(g_game().removeCreature(npc,true));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, DialogueTimeoutMovementAndNpcRemovalCancelFinitely){PlayerBotDatabaseFixture fixture(g_database());for(int x=0;x<=7;++x)createWalkableTile(Position(fixture.start.x+x,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());const auto npc=createPlayerBotDialogueNpc(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,npc);auto policy=integrationDialoguePolicy();policy.phrases.push_back({BotDialogueIntent::ConfiguredFollowUp,"silence","never"});EXPECT_EQ(BotDialogueState::AwaitingResponse,manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::ConfiguredFollowUp,std::chrono::milliseconds(1),policy).state);EXPECT_EQ(BotDialogueResponse::RetryScheduled,manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::ConfiguredFollowUp,std::chrono::milliseconds(20),policy).response);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalTeleport(player,Position(fixture.start.x+7,fixture.start.y,fixture.start.z),true));EXPECT_EQ(BotDialogueState::ConversationLost,manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::ConfiguredFollowUp,std::chrono::milliseconds(21),policy).state);EXPECT_TRUE(g_game().removeCreature(npc,true));EXPECT_EQ(BotDialogueResponse::NpcUnavailable,manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::Greeting,std::chrono::milliseconds(22),policy).response);EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, DialogueDoesNotMutateStorageAndShopPathRemainsFunctional){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);createWalkableTile(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());const auto storageBefore=player->getStorageValue(900001);const auto dialogue=createPlayerBotDialogueNpc(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,dialogue);const auto policy=integrationDialoguePolicy();(void)manager.advanceDialogue(fixture.name,dialogue->getID(),BotDialogueIntent::Greeting,std::chrono::milliseconds(1),policy);(void)manager.advanceDialogue(fixture.name,dialogue->getID(),BotDialogueIntent::Greeting,std::chrono::milliseconds(2),policy);EXPECT_EQ(storageBefore,player->getStorageValue(900001));EXPECT_TRUE(g_game().removeCreature(dialogue,true));const auto shop=createPlayerBotShopNpc(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,shop);EXPECT_TRUE(player->openShopWindow(shop,shop->getShopItemVector(player->getGUID())));EXPECT_TRUE(manager.observeShop(fixture.name,shop->getID()).open);EXPECT_TRUE(g_game().removeCreature(shop,true));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, DialogueSessionCloseClearsConversationWithoutWorldOwnership){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);createWalkableTile(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);const auto npc=createPlayerBotDialogueNpc(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,npc);const auto policy=integrationDialoguePolicy();(void)manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::Greeting,std::chrono::milliseconds(1),policy);ASSERT_NE(nullptr,session->getDialogueProgress());EXPECT_FALSE(session->getDialogueProgress()->containsWorldOwnership);EXPECT_TRUE(manager.logout(fixture.name,false));EXPECT_EQ(nullptr,session->getDialogueProgress());EXPECT_TRUE(g_game().removeCreature(npc,true));ASSERT_TRUE(fixture.cleanup());EXPECT_FALSE(fixture.hasCommittedRows());}

TEST(PlayerBotIntegrationTest, DialogueConfiguredTopicConfirmationAndFarewellUseObservedResponses){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);createWalkableTile(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);const auto npc=createPlayerBotDialogueNpc(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,npc);const auto policy=integrationDialoguePolicy();auto exchange=[&](BotDialogueIntent intent,int tick){EXPECT_EQ(BotDialogueState::AwaitingResponse,manager.advanceDialogue(fixture.name,npc->getID(),intent,std::chrono::milliseconds(tick),policy).state);return manager.advanceDialogue(fixture.name,npc->getID(),intent,std::chrono::milliseconds(tick+1),policy);};EXPECT_EQ(BotDialogueResponse::GreetingAccepted,exchange(BotDialogueIntent::Greeting,1).response);EXPECT_EQ(BotDialogueResponse::TopicAccepted,exchange(BotDialogueIntent::ConfiguredTopic,3).response);EXPECT_EQ(BotDialogueResponse::TopicAccepted,exchange(BotDialogueIntent::Yes,5).response);EXPECT_EQ(BotDialogueResponse::ConversationComplete,exchange(BotDialogueIntent::Farewell,7).response);EXPECT_FALSE(npc->isInteractingWithPlayer(std::const_pointer_cast<Player>(session->getPlayer())->getID()));EXPECT_TRUE(g_game().removeCreature(npc,true));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

namespace {
BotQuestDefinition integrationQuest(uint16_t vocationId,const Position &position){return{.id=7001,.visibleName="PlayerBot Visible Quest",.revision=11,.missions={{.id=7002,.visibleName="Visible Mission",.visibleDescription="Kill, collect, visit, and report.",.stateStorage={900100,-1,3},.availableValue=-1,.startedValue=1,.objectiveCompleteValue=2,.completedValue=3,.turnInRequired=true,.prerequisites={{.type=BotQuestPrerequisiteType::MinimumLevel,.value=1},{.type=BotQuestPrerequisiteType::Vocation,.value=vocationId},{.type=BotQuestPrerequisiteType::ItemPresent,.value=1,.itemTypeId=3031},{.type=BotQuestPrerequisiteType::StoragePredicate,.storage={900101,5,5}}},.objectives={{.type=BotQuestObjectiveType::KillCreatureType,.requiredCount=1,.creatureTypeId=77},{.type=BotQuestObjectiveType::CollectItem,.requiredCount=1,.itemTypeId=3031},{.type=BotQuestObjectiveType::VisitLocation,.requiredCount=1,.position=position,.radius=0},{.type=BotQuestObjectiveType::TalkToNpc,.requiredCount=1,.configuredNpcName="playerbotdialoguefixturenpc",.requiredDialogueToken="welcome"}},.rewards={{BotQuestRewardType::ItemReceived,266,1},{BotQuestRewardType::ExperienceIncreased,0,50},{BotQuestRewardType::MissionStateChanged,0,1}}}}};}
std::vector<BotGoal> integrationPlannerGoals(const Position&region){return{{.id=1,.type=BotGoalType::Survive,.priority=BotGoalPriority::CriticalSurvival,.status=BotGoalStatus::Eligible,.policyRevision=1},{.id=2,.type=BotGoalType::Recover,.priority=BotGoalPriority::DeathRecovery,.status=BotGoalStatus::Eligible,.policyRevision=1},{.id=3,.type=BotGoalType::Resupply,.priority=BotGoalPriority::UrgentResupply,.status=BotGoalStatus::Eligible,.policyRevision=1},{.id=4,.type=BotGoalType::CompleteConfiguredQuest,.priority=BotGoalPriority::ActiveQuest,.status=BotGoalStatus::Eligible,.policyRevision=1,.configuredTargetId=7001,.configuredRegion=region},{.id=5,.type=BotGoalType::HuntConfiguredRegion,.priority=BotGoalPriority::Progression,.status=BotGoalStatus::Eligible,.policyRevision=1,.configuredRegion=region},{.id=6,.type=BotGoalType::IdleSafely,.priority=BotGoalPriority::Idle,.status=BotGoalStatus::Eligible,.policyRevision=1}};}
}

TEST(PlayerBotIntegrationTest, QuestObservationUsesRealOwnStorageQuestLogAndPrerequisites){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),Item::CreateItem(3031,1),INDEX_WHEREEVER,FLAG_NOLIMIT));player->storage().add(900101,5);auto definition=integrationQuest(player->getVocationId(),fixture.start);auto unavailable=definition;unavailable.missions[0].availableValue=0;EXPECT_EQ(BotMissionState::Unavailable,manager.observeQuest(fixture.name,unavailable).missions.front().state);const auto available=manager.observeQuest(fixture.name,definition);EXPECT_EQ(BotMissionState::Available,available.missions.front().state);EXPECT_EQ("Visible Mission",available.missions.front().visibleName);EXPECT_TRUE(manager.evaluateQuest(fixture.name,7002,definition).eligible());player->storage().add(900100,1);EXPECT_EQ(BotMissionState::Started,manager.observeQuest(fixture.name,definition).missions.front().state);EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, QuestObservationUsesRealNpcDialogueItemAndLocationEvidence){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);createWalkableTile(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),Item::CreateItem(3031,1),INDEX_WHEREEVER,FLAG_NOLIMIT));player->storage().add(900100,1);player->storage().add(900101,5);const auto npc=createPlayerBotDialogueNpc(Position(fixture.start.x+1,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,npc);const auto dialoguePolicy=integrationDialoguePolicy();(void)manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::Greeting,std::chrono::milliseconds(1),dialoguePolicy);const auto response=manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::Greeting,std::chrono::milliseconds(2),dialoguePolicy);ASSERT_EQ(BotDialogueResponse::GreetingAccepted,response.response);std::vector<BotQuestEvidence> evidence={{.type=BotQuestObjectiveType::KillCreatureType,.subjectId=77,.count=1,.revision=1},{.type=BotQuestObjectiveType::TalkToNpc,.subjectId=npc->getID(),.normalizedSubjectName="playerbotdialoguefixturenpc",.count=1,.position=npc->getPosition(),.revision=response.responseRevision,.tokenHash=BotQuest::dialogueTokenHash("welcome")}};const auto observed=manager.observeQuest(fixture.name,integrationQuest(player->getVocationId(),fixture.start),evidence);ASSERT_EQ(4,observed.missions.front().progress.size());EXPECT_TRUE(std::ranges::all_of(observed.missions.front().progress,[](const auto&p){return p.complete;}));EXPECT_EQ(BotMissionState::TurnInRequired,observed.missions.front().state);EXPECT_TRUE(g_game().removeCreature(npc,true));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, QuestKillEvidenceFollowsOrdinaryRealCreatureDeath){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);const Position target(fixture.start.x+1,fixture.start.y,fixture.start.z);createWalkableTile(target);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto type=std::make_shared<MonsterType>("QuestEvidenceMonster");type->info.health=10;type->info.healthMax=10;type->info.experience=5;type->info.lookcorpse=3994;auto monster=std::make_shared<Monster>(type);ASSERT_TRUE(g_game().placeCreature(monster,target,false,true));const auto deathId=monster->getID();CombatDamage fatal;fatal.primary={COMBAT_PHYSICALDAMAGE,-100};ASSERT_TRUE(g_game().combatChangeHealth(player,monster,fatal));monster->onDeath();ASSERT_TRUE(monster->isRemoved());const auto observed=manager.observeQuest(fixture.name,integrationQuest(player->getVocationId(),fixture.start),{{.type=BotQuestObjectiveType::KillCreatureType,.subjectId=77,.count=1,.revision=deathId}});EXPECT_TRUE(observed.missions.front().progress.front().complete);EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, QuestRewardVerificationObservesRealAuthoritativeChanges){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));player->storage().add(900100,2);auto definition=integrationQuest(player->getVocationId(),fixture.start);auto beforeQuest=manager.observeQuest(fixture.name,definition);auto before=BotQuest::rewardObservation(beforeQuest,7002);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),Item::CreateItem(266,1),INDEX_WHEREEVER,FLAG_NOLIMIT));player->onGainExperience(50,nullptr);player->storage().add(900100,3);++definition.revision;auto after=BotQuest::rewardObservation(manager.observeQuest(fixture.name,definition),7002);const auto verified=BotQuest::verifyRewards(before,after,definition.missions.front().rewards);EXPECT_EQ(BotQuestVerificationResult::Verified,verified.result);EXPECT_EQ(3,verified.verified);EXPECT_EQ(1U,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(266));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, QuestObservationDoesNotMutateRewardsOrOrdinaryPlayerBehavior){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());const auto storageBefore=player->getStorageValue(900100);const auto experienceBefore=player->getExperience();const auto itemsBefore=std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(2160);const auto observed=manager.observeQuest(fixture.name,integrationQuest(player->getVocationId(),fixture.start));EXPECT_FALSE(observed.containsWorldOwnership());EXPECT_EQ(storageBefore,player->getStorageValue(900100));EXPECT_EQ(experienceBefore,player->getExperience());EXPECT_EQ(itemsBefore,std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(2160));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, QuestSessionCloseClearsAssessmentAndDatabaseRows){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);const auto player=std::const_pointer_cast<Player>(session->getPlayer());(void)manager.observeQuest(fixture.name,integrationQuest(player->getVocationId(),fixture.start));ASSERT_NE(nullptr,session->getQuestObservation());ASSERT_TRUE(session->getQuestObservation()->has_value());EXPECT_FALSE(session->getQuestObservation()->value().containsWorldOwnership());EXPECT_TRUE(manager.logout(fixture.name,false));EXPECT_EQ(nullptr,session->getQuestObservation());ASSERT_TRUE(fixture.cleanup());EXPECT_FALSE(fixture.hasCommittedRows());}

TEST(PlayerBotIntegrationTest, QuestExecutionDelegatesRealTravelDialogueAndVerifiedCheckpoints){PlayerBotDatabaseFixture fixture(g_database());for(int x=0;x<=3;++x)createWalkableTile(Position(fixture.start.x+x,fixture.start.y,fixture.start.z));BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);auto player=std::const_pointer_cast<Player>(session->getPlayer());auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(backpack->getContainer(),Item::CreateItem(3031,1),INDEX_WHEREEVER,FLAG_NOLIMIT));player->storage().add(900101,5);const auto definition=integrationQuest(player->getVocationId(),Position(fixture.start.x+2,fixture.start.y,fixture.start.z));auto observation=manager.observeQuest(fixture.name,definition);BotQuestPlan plan{.questId=definition.id,.missionId=7002,.revision=1,.steps={{.type=BotQuestStepType::TravelRegion,.region=Position(fixture.start.x+2,fixture.start.y,fixture.start.z)},{.type=BotQuestStepType::SendDialogue,.configuredName="playerbotdialoguefixturenpc"},{.type=BotQuestStepType::VerifyProgress,.expectedMissionState=BotMissionState::Started}}};auto execution=BotQuestExecution::start(plan,observation,manager.evaluateQuest(fixture.name,7002,definition));ASSERT_EQ(BotQuestExecutionState::Traveling,execution.state);auto route=manager.startRoute(fixture.name,Position(fixture.start.x+2,fixture.start.y,fixture.start.z),std::chrono::milliseconds(1));for(uint8_t i=0;i<8&&route.state!=BotRouteState::Arrived;++i)route=manager.advanceRoute(fixture.name,std::chrono::milliseconds(100+i*100));ASSERT_EQ(BotRouteState::Arrived,route.state);execution=BotQuestExecution::advance(plan,execution,{.revision=observation.revision+1,.regionReached=true,.missionState=BotMissionState::Available});ASSERT_EQ(1,execution.checkpoint.verifiedStepIndex);const auto npc=createPlayerBotDialogueNpc(Position(fixture.start.x+3,fixture.start.y,fixture.start.z));ASSERT_NE(nullptr,npc);const auto policy=integrationDialoguePolicy();(void)manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::Greeting,std::chrono::milliseconds(1000),policy);ASSERT_EQ(BotDialogueResponse::GreetingAccepted,manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::Greeting,std::chrono::milliseconds(1001),policy).response);execution=BotQuestExecution::advance(plan,execution,{.revision=observation.revision+2,.actionAccepted=true,.visibleDialogueResponse=true,.missionState=BotMissionState::Available});ASSERT_EQ(2,execution.checkpoint.verifiedStepIndex);player->storage().add(900100,1);observation=manager.observeQuest(fixture.name,definition);execution=BotQuestExecution::advance(plan,execution,{.revision=observation.revision+3,.authoritativeProgress=true,.missionState=BotMissionState::Started});EXPECT_EQ(BotQuestExecutionState::Completed,execution.state);EXPECT_EQ(3,execution.checkpoint.verifiedStepIndex);EXPECT_TRUE(g_game().removeCreature(npc,true));EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());}

TEST(PlayerBotIntegrationTest, QuestExecutionSurvivalDeathAndSessionReconstructionAreBounded){PlayerBotDatabaseFixture fixture(g_database());createWalkableTile(fixture.start);BotManager manager(g_game());const auto session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);const auto player=std::const_pointer_cast<Player>(session->getPlayer());auto definition=integrationQuest(player->getVocationId(),fixture.start);const auto observation=manager.observeQuest(fixture.name,definition);BotQuestPlan plan{.questId=definition.id,.missionId=7002,.revision=2,.steps={{.type=BotQuestStepType::TravelRegion,.region=fixture.start}}};auto execution=BotQuestExecution::start(plan,observation,{});auto suspended=BotQuestExecution::advance(plan,execution,{.revision=observation.revision+1,.survivalRequired=true});EXPECT_EQ(BotQuestExecutionState::Suspended,suspended.state);auto resumed=BotQuestExecution::advance(plan,suspended,{.revision=observation.revision+2,.regionReached=true});EXPECT_EQ(BotQuestExecutionState::Completed,resumed.state);auto dead=BotQuestExecution::advance(plan,execution,{.revision=observation.revision+1,.dead=true});EXPECT_EQ(BotQuestExecutionState::Dead,dead.state);EXPECT_FALSE(dead.containsWorldOwnership());EXPECT_TRUE(manager.logout(fixture.name,false));EXPECT_EQ(nullptr,session->getQuestObservation());ASSERT_TRUE(fixture.cleanup());EXPECT_FALSE(fixture.hasCommittedRows());}

TEST(PlayerBotIntegrationTest, QuestVerifiedCheckpointReconstructsAfterFreshSessionObservation) {
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game());
	const auto firstSession = loginBotOrReport(manager, fixture.name); ASSERT_NE(nullptr, firstSession);
	const auto firstPlayer = std::const_pointer_cast<Player>(firstSession->getPlayer());
	auto definition = integrationQuest(firstPlayer->getVocationId(), fixture.start);
	const auto firstObservation = manager.observeQuest(fixture.name, definition);
	BotQuestPlan plan { .questId = 7001, .missionId = 7002, .revision = 3, .steps = { { .type = BotQuestStepType::TravelRegion, .region = fixture.start }, { .type = BotQuestStepType::Finish } } };
	auto execution = manager.startQuestExecution(fixture.name, plan); ASSERT_EQ(BotQuestExecutionState::Traveling, execution.state);
	execution = manager.advanceQuestExecution(fixture.name, plan, { .revision = firstObservation.revision + 1, .regionReached = true, .missionState = BotMissionState::Available });
	ASSERT_EQ(1, execution.checkpoint.verifiedStepIndex); const auto checkpoint = execution.checkpoint;
	EXPECT_TRUE(manager.logout(fixture.name, false)); EXPECT_EQ(nullptr, firstSession->getQuestExecution());
	const auto secondSession = loginBotOrReport(manager, fixture.name); ASSERT_NE(nullptr, secondSession);
	definition.revision += 2; const auto fresh = manager.observeQuest(fixture.name, definition); ASSERT_GT(fresh.revision, checkpoint.observationRevision);
	auto reconstructed = BotQuestExecution::reconstruct(plan, checkpoint, fresh); EXPECT_EQ(BotQuestExecutionState::Checkpointing, reconstructed.state); EXPECT_EQ(1, reconstructed.checkpoint.verifiedStepIndex);
	reconstructed = BotQuestExecution::advance(plan, reconstructed, { .revision = fresh.revision + 1, .authoritativeProgress = true, .missionState = BotMissionState::Available }); EXPECT_EQ(BotQuestExecutionState::Completed, reconstructed.state);
	EXPECT_TRUE(manager.logout(fixture.name, false)); ASSERT_TRUE(fixture.cleanup()); EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, QuestExecutionCompletesOrdinaryMultiNpcKillCollectHandInAndRewardFlow) {
	PlayerBotDatabaseFixture fixture(g_database());
	for (int x = 0; x <= 6; ++x) createWalkableTile(Position(fixture.start.x + x, fixture.start.y, fixture.start.z));
	createWalkableTile(Position(fixture.start.x, fixture.start.y + 1, fixture.start.z));
	BotManager manager(g_game());
	const auto session = loginBotOrReport(manager, fixture.name);
	ASSERT_NE(nullptr, session);
	auto player = std::const_pointer_cast<Player>(session->getPlayer());
	auto backpack = Item::CreateItem(ITEM_BACKPACK);
	ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(player, backpack, CONST_SLOT_BACKPACK, FLAG_NOLIMIT));
	ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(backpack->getContainer(), Item::CreateItem(3031, 1), INDEX_WHEREEVER, FLAG_NOLIMIT));
	player->storage().add(900101, 5);
	const Position hunt(fixture.start.x + 2, fixture.start.y, fixture.start.z);
	auto definition = integrationQuest(player->getVocationId(), hunt);
	definition.missions.front().rewards.push_back({ BotQuestRewardType::ItemRemoved, 3031, 1 });
	definition.missions.front().rewards.push_back({ BotQuestRewardType::MoneyChanged, 0, 1 });
	auto quest = manager.observeQuest(fixture.name, definition);
	ASSERT_TRUE(manager.evaluateQuest(fixture.name, 7002, definition).eligible());
	BotQuestPlan plan {
		.questId = 7001, .missionId = 7002, .revision = 30,
		.steps = {
			{ .type = BotQuestStepType::SendDialogue, .missionId = 7002, .configuredName = "playerbotdialoguefixturenpc", .configuredPhrase = "mission" },
			{ .type = BotQuestStepType::TravelRegion, .missionId = 7002, .region = hunt },
			{ .type = BotQuestStepType::KillCreature, .missionId = 7002, .stableTargetId = 77 },
			{ .type = BotQuestStepType::CollectItem, .missionId = 7002, .itemTypeId = 3031, .requiredCount = 1 },
			{ .type = BotQuestStepType::SendDialogue, .missionId = 7002, .configuredName = "playerbotquestwitnessfixturenpc", .configuredPhrase = "witness" },
			{ .type = BotQuestStepType::DeliverItem, .missionId = 7002, .itemTypeId = 3031, .requiredCount = 1 },
			{ .type = BotQuestStepType::VerifyReward, .missionId = 7002 },
			{ .type = BotQuestStepType::Finish, .missionId = 7002 },
		}
	};
	auto execution = manager.startQuestExecution(fixture.name, plan);
	ASSERT_EQ(BotQuestExecutionState::StartingDialogue, execution.state);
	uint64_t revision = quest.revision;

	const auto guide = createPlayerBotDialogueNpc(Position(fixture.start.x, fixture.start.y + 1, fixture.start.z));
	ASSERT_NE(nullptr, guide);
	const auto startPolicy = questGuideStartPolicy();
	EXPECT_EQ(BotDialogueState::AwaitingResponse, manager.advanceDialogue(fixture.name, guide->getID(), BotDialogueIntent::Greeting, std::chrono::milliseconds(1), startPolicy).state);
	EXPECT_EQ(BotDialogueResponse::GreetingAccepted, manager.advanceDialogue(fixture.name, guide->getID(), BotDialogueIntent::Greeting, std::chrono::milliseconds(2), startPolicy).response);
	EXPECT_EQ(BotDialogueState::AwaitingResponse, manager.advanceDialogue(fixture.name, guide->getID(), BotDialogueIntent::ConfiguredFollowUp, std::chrono::milliseconds(3), startPolicy).state);
	EXPECT_EQ(BotDialogueResponse::TopicAccepted, manager.advanceDialogue(fixture.name, guide->getID(), BotDialogueIntent::ConfiguredFollowUp, std::chrono::milliseconds(4), startPolicy).response);
	ASSERT_EQ(1, player->getStorageValue(900100));
	execution = manager.advanceQuestExecution(fixture.name, plan, { .revision = ++revision, .actionAccepted = true, .authoritativeProgress = true, .visibleDialogueResponse = true, .missionState = BotMissionState::Started });
	ASSERT_EQ(1, execution.checkpoint.verifiedStepIndex);
	(void)manager.advanceDialogue(fixture.name, guide->getID(), BotDialogueIntent::Farewell, std::chrono::milliseconds(5), startPolicy);
	(void)manager.advanceDialogue(fixture.name, guide->getID(), BotDialogueIntent::Farewell, std::chrono::milliseconds(6), startPolicy);
	(void)manager.cancelDialogue(fixture.name);

	auto route = manager.startRoute(fixture.name, hunt, std::chrono::milliseconds(10));
	for (uint8_t i = 0; i < 10 && route.state != BotRouteState::Arrived; ++i) route = manager.advanceRoute(fixture.name, std::chrono::milliseconds(100 + i * 100));
	ASSERT_EQ(BotRouteState::Arrived, route.state);
	execution = manager.advanceQuestExecution(fixture.name, plan, { .revision = ++revision, .regionReached = true, .missionState = BotMissionState::Started });
	ASSERT_EQ(2, execution.checkpoint.verifiedStepIndex);

	auto monsterType = std::make_shared<MonsterType>("QuestExecutionMonster");
	monsterType->info.health = 10; monsterType->info.healthMax = 10; monsterType->info.experience = 5; monsterType->info.lookcorpse = 3994;
	auto monster = std::make_shared<Monster>(monsterType);
	const Position monsterPosition(hunt.x + 1, hunt.y, hunt.z);
	ASSERT_TRUE(g_game().placeCreature(monster, monsterPosition, false, true));
	ASSERT_EQ(monster->getID(), manager.evaluateCombat(fixture.name).selectedCreatureId);
	const auto perception = BotPerception::observe(player); ASSERT_TRUE(perception);
	const auto combat = BotCombat::observe(player, *perception); ASSERT_TRUE(combat);
	const auto target = std::ranges::find(combat->creatures, monster->getID(), &BotCombatCreatureObservation::id); ASSERT_NE(combat->creatures.end(), target);
	ASSERT_EQ(BotCombatExecutionOutcome::TargetAcquired, manager.executeCombat(fixture.name, { monster->getID(), combat->revision, target->signature, player->getPosition() }, std::chrono::milliseconds(1200)).outcome);
	const auto monsterId = monster->getID();
	CombatDamage fatal; fatal.primary = { COMBAT_PHYSICALDAMAGE, -100 };
	ASSERT_TRUE(g_game().combatChangeHealth(player, monster, fatal));
	monster->onDeath(); ASSERT_TRUE(monster->isRemoved());
	execution = manager.advanceQuestExecution(fixture.name, plan, { .revision = ++revision, .authoritativeProgress = true, .deathObserved = true, .missionState = BotMissionState::InProgress });
	ASSERT_EQ(3, execution.checkpoint.verifiedStepIndex);

	const auto corpseTile = g_game().map.getTile(monsterPosition); ASSERT_NE(nullptr, corpseTile);
	std::shared_ptr<Item> corpse;
	for (const auto &item : *corpseTile->getItemList()) if (item && item->isCorpse() && item->getContainer()) { corpse = item; break; }
	ASSERT_NE(nullptr, corpse);
	corpse->getContainer()->internalAddThing(Item::CreateItem(3031, 1));
	BotLootPolicy lootPolicy { .rules = { { { .itemTypeId = 3031, .valueCategory = 1, .priority = 1 } } } };
	const auto selected = manager.evaluateLoot(fixture.name, monsterPosition, monsterId, {}, lootPolicy); ASSERT_TRUE(selected.selected);
	BotLootTransferRequest lootRequest { .corpsePosition = monsterPosition, .sourceCreatureId = monsterId, .corpseSignature = selected.corpse.signature, .itemTypeId = 3031, .itemSignature = selected.selected->item.signature, .count = 1 };
	EXPECT_EQ(BotLootTransferOutcome::Pending, manager.executeLoot(fixture.name, lootRequest, std::chrono::milliseconds(1300), lootPolicy).outcome);
	EXPECT_EQ(BotLootTransferOutcome::Succeeded, manager.executeLoot(fixture.name, lootRequest, std::chrono::milliseconds(1301), lootPolicy).outcome);
	execution = manager.advanceQuestExecution(fixture.name, plan, { .revision = ++revision, .itemAdded = true, .missionState = BotMissionState::InProgress });
	ASSERT_EQ(4, execution.checkpoint.verifiedStepIndex);

	const auto witness = createPlayerBotQuestWitnessNpc(monsterPosition); ASSERT_NE(nullptr, witness);
	const auto witnessPolicy = questWitnessPolicy();
	quest = manager.observeQuest(fixture.name, definition);
	const auto beforeReward = BotQuest::rewardObservation(quest, 7002);
	const auto itemBefore = std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031);
	ASSERT_GE(itemBefore, 1);
	(void)manager.advanceDialogue(fixture.name, witness->getID(), BotDialogueIntent::Greeting, std::chrono::milliseconds(1400), witnessPolicy);
	ASSERT_EQ(BotDialogueResponse::GreetingAccepted, manager.advanceDialogue(fixture.name, witness->getID(), BotDialogueIntent::Greeting, std::chrono::milliseconds(1401), witnessPolicy).response);
	(void)manager.advanceDialogue(fixture.name, witness->getID(), BotDialogueIntent::ConfiguredTopic, std::chrono::milliseconds(1402), witnessPolicy);
	ASSERT_EQ(BotDialogueResponse::TopicAccepted, manager.advanceDialogue(fixture.name, witness->getID(), BotDialogueIntent::ConfiguredTopic, std::chrono::milliseconds(1403), witnessPolicy).response);
	ASSERT_EQ(3, player->getStorageValue(900100));
	execution = manager.advanceQuestExecution(fixture.name, plan, { .revision = ++revision, .actionAccepted = true, .authoritativeProgress = true, .visibleDialogueResponse = true, .missionState = BotMissionState::Completed });
	ASSERT_EQ(5, execution.checkpoint.verifiedStepIndex);
	ASSERT_EQ(3, player->getStorageValue(900100));
	EXPECT_EQ(itemBefore - 1, std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031));
	execution = manager.advanceQuestExecution(fixture.name, plan, { .revision = ++revision, .actionAccepted = true, .authoritativeProgress = true, .visibleDialogueResponse = true, .itemRemoved = true, .missionState = BotMissionState::Completed });
	ASSERT_EQ(6, execution.checkpoint.verifiedStepIndex);
	(void)manager.advanceDialogue(fixture.name, witness->getID(), BotDialogueIntent::Farewell, std::chrono::milliseconds(1404), witnessPolicy);
	(void)manager.advanceDialogue(fixture.name, witness->getID(), BotDialogueIntent::Farewell, std::chrono::milliseconds(1405), witnessPolicy);
	(void)manager.cancelDialogue(fixture.name);
	ASSERT_TRUE(g_game().removeCreature(witness, true));
	++definition.revision;
	const auto afterReward = BotQuest::rewardObservation(manager.observeQuest(fixture.name, definition), 7002);
	const auto verified = BotQuest::verifyRewards(beforeReward, afterReward, definition.missions.front().rewards);
	ASSERT_EQ(BotQuestVerificationResult::Verified, verified.result);
	execution = manager.advanceQuestExecution(fixture.name, plan, { .revision = ++revision, .missionState = BotMissionState::Completed, .reward = verified.result });
	ASSERT_EQ(7, execution.checkpoint.verifiedStepIndex);
	execution = manager.advanceQuestExecution(fixture.name, plan, { .revision = ++revision, .authoritativeProgress = true, .missionState = BotMissionState::Completed });
	EXPECT_EQ(BotQuestExecutionState::Completed, execution.state);
	EXPECT_EQ(8, execution.checkpoint.verifiedStepIndex);
	EXPECT_FALSE(execution.containsWorldOwnership());
	EXPECT_TRUE(g_game().removeCreature(guide, true));
	EXPECT_TRUE(manager.logout(fixture.name, false));
	EXPECT_EQ(nullptr, session->getQuestExecution());
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, OrdinaryNetworkPlayerUsesSameRegisteredQuestNpcFlowUnchanged) {
	PlayerBotDatabaseFixture fixture(g_database());
	createWalkableTile(fixture.start);
	createWalkableTile(Position(fixture.start.x + 1, fixture.start.y, fixture.start.z));
	auto player = std::make_shared<Player>();
	player->setName(fixture.name);
	ASSERT_TRUE(IOLoginDataLoad::preLoadPlayer(player, fixture.name));
	ASSERT_TRUE(IOLoginData::loadPlayerById(player, fixture.playerId, false));
	player->setID(); player->setOnline(true);
	ASSERT_TRUE(g_game().placeCreature(player, fixture.start, false, true));
	auto backpack = Item::CreateItem(ITEM_BACKPACK);
	ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(player, backpack, CONST_SLOT_BACKPACK, FLAG_NOLIMIT));
	ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(backpack->getContainer(), Item::CreateItem(3031, 1), INDEX_WHEREEVER, FLAG_NOLIMIT));
	const auto guide = createPlayerBotDialogueNpc(Position(fixture.start.x + 1, fixture.start.y, fixture.start.z)); ASSERT_NE(nullptr, guide);
	g_game().playerSay(player->getID(), 0, TALKTYPE_SAY, "", "hi");
	g_game().playerSay(player->getID(), 0, TALKTYPE_SAY, "", "mission");
	ASSERT_EQ(1, player->getStorageValue(900100));
	ASSERT_TRUE(g_game().removeCreature(guide, true));
	const auto witness = createPlayerBotQuestWitnessNpc(Position(fixture.start.x + 1, fixture.start.y, fixture.start.z)); ASSERT_NE(nullptr, witness);
	const auto experienceBefore = player->getExperience();
	g_game().playerSay(player->getID(), 0, TALKTYPE_SAY, "", "hi");
	g_game().playerSay(player->getID(), 0, TALKTYPE_SAY, "", "witness");
	EXPECT_EQ(3, player->getStorageValue(900100));
	EXPECT_EQ(0U, std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(3031));
	EXPECT_EQ(1U, std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(266));
	EXPECT_EQ(experienceBefore + 50, player->getExperience());
	EXPECT_TRUE(g_game().removeCreature(witness, true));
	player->setOnline(false);
	const std::function<bool(const std::shared_ptr<Player>&)> noSave;
	EXPECT_EQ(ManagedPlayerRemovalResult::Complete, g_game().removeManagedPlayer(player, true, noSave));
	ASSERT_TRUE(fixture.cleanup());
	EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, QuestExecutionUseStepDelegatesProductionLadderTransition) {
	ProductionTransitionActionFixture actionFixture; ASSERT_TRUE(actionFixture.isLoaded());
	PlayerBotDatabaseFixture fixture(g_database());
	const Position ladderPosition(fixture.start.x + 1, fixture.start.y, fixture.start.z);
	const Position destination(ladderPosition.x, ladderPosition.y + 1, ladderPosition.z - 1);
	createWalkableTile(fixture.start); createWalkableTile(ladderPosition); createWalkableTile(destination);
	const auto ladderTile = g_game().map.getTile(ladderPosition); ASSERT_NE(nullptr, ladderTile);
	const auto ladder = Item::CreateItem(1948); ASSERT_NE(nullptr, ladder); ladderTile->internalAddThing(ladder);
	BotManager manager(g_game()); const auto session = loginBotOrReport(manager, fixture.name); ASSERT_NE(nullptr, session);
	auto player = std::const_pointer_cast<Player>(session->getPlayer());
	auto backpack = Item::CreateItem(ITEM_BACKPACK); ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(player, backpack, CONST_SLOT_BACKPACK, FLAG_NOLIMIT));
	ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(backpack->getContainer(), Item::CreateItem(3031, 1), INDEX_WHEREEVER, FLAG_NOLIMIT)); player->storage().add(900101, 5);
	const auto definition = integrationQuest(player->getVocationId(), destination); const auto observation = manager.observeQuest(fixture.name, definition);
	BotQuestPlan plan { .questId = 7001, .missionId = 7002, .revision = 40, .steps = { { .type = BotQuestStepType::UseObject, .missionId = 7002, .stableTargetId = 1948 }, { .type = BotQuestStepType::Finish, .missionId = 7002 } } };
	auto execution = manager.startQuestExecution(fixture.name, plan); ASSERT_EQ(BotQuestExecutionState::UsingObject, execution.state);
	BotInteractionTarget target { ladderPosition, static_cast<uint8_t>(ladderTile->getThingIndex(ladder)), 1948, 0, BotInteractionType::UseLadder }; target.signature = BotInteraction::signature(target);
	const BotTransitionRequest request { .target = target, .expectedDestination = destination, .maxAttempts = 2, .timeout = std::chrono::milliseconds(50) };
	ASSERT_EQ(BotTransitionState::AwaitingTransition, manager.startTransition(fixture.name, request, std::chrono::milliseconds(1)).state);
	ASSERT_EQ(BotTransitionState::Completed, manager.advanceTransition(fixture.name, std::chrono::milliseconds(2)).state);
	ASSERT_EQ(destination, player->getPosition());
	execution = manager.advanceQuestExecution(fixture.name, plan, { .revision = observation.revision + 1, .authoritativeProgress = true, .missionState = BotMissionState::InProgress });
	ASSERT_EQ(1, execution.checkpoint.verifiedStepIndex);
	execution = manager.advanceQuestExecution(fixture.name, plan, { .revision = observation.revision + 2, .authoritativeProgress = true, .missionState = BotMissionState::InProgress });
	EXPECT_EQ(BotQuestExecutionState::Completed, execution.state);
	ladderTile->removeThing(ladder, 1); EXPECT_TRUE(manager.logout(fixture.name, false)); ASSERT_TRUE(fixture.cleanup());
}

TEST(PlayerBotIntegrationTest, LongHorizonPlannerCoordinatesM2ThroughM7AndDeterministicReplans) {
	PlayerBotDatabaseFixture fixture(g_database());
	for (int x = 0; x <= 4; ++x) createWalkableTile(Position(fixture.start.x + x, fixture.start.y, fixture.start.z));
	BotManager manager(g_game()); const auto session = loginBotOrReport(manager, fixture.name); ASSERT_NE(nullptr, session);
	auto player = std::const_pointer_cast<Player>(session->getPlayer());
	auto backpack = Item::CreateItem(ITEM_BACKPACK); ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(player, backpack, CONST_SLOT_BACKPACK, FLAG_NOLIMIT));
	ASSERT_EQ(RETURNVALUE_NOERROR, g_game().internalAddItem(backpack->getContainer(), Item::CreateItem(3031, 1), INDEX_WHEREEVER, FLAG_NOLIMIT)); player->storage().add(900101, 5);
	const Position region(fixture.start.x + 2, fixture.start.y, fixture.start.z);
	auto goals = integrationPlannerGoals(region);
	BotPlannerObservation observation { .revision = 1, .sessionGeneration = 1, .activeQuest = true, .progressionConfigured = true };
	auto decision = BotPlanner::select(goals, observation); ASSERT_EQ(BotGoalType::CompleteConfiguredQuest, decision.goalType); ASSERT_LE(decision.plan.steps.size(), BotPlannerPolicy{}.maximumSteps);
	EXPECT_NE(decision.plan.steps.end(), std::ranges::find(decision.plan.steps, BotPlanStepType::Travel, &BotPlanStep::type));
	EXPECT_NE(decision.plan.steps.end(), std::ranges::find(decision.plan.steps, BotPlanStepType::Fight, &BotPlanStep::type));
	EXPECT_NE(decision.plan.steps.end(), std::ranges::find(decision.plan.steps, BotPlanStepType::Loot, &BotPlanStep::type));
	EXPECT_NE(decision.plan.steps.end(), std::ranges::find(decision.plan.steps, BotPlanStepType::Converse, &BotPlanStep::type));
	decision = BotPlanner::verifyStep(decision, 0, 2, true); ASSERT_EQ(1, decision.checkpoint.verifiedStepIndex);

	auto route = manager.startRoute(fixture.name, region, std::chrono::milliseconds(1)); for(uint8_t i=0;i<8&&route.state!=BotRouteState::Arrived;++i) route=manager.advanceRoute(fixture.name,std::chrono::milliseconds(100+i*100)); ASSERT_EQ(BotRouteState::Arrived, route.state);
	decision = BotPlanner::verifyStep(decision, 1, 3, true); ASSERT_EQ(2, decision.checkpoint.verifiedStepIndex);
	const auto npc = createPlayerBotDialogueNpc(Position(fixture.start.x + 3, fixture.start.y, fixture.start.z)); ASSERT_NE(nullptr, npc); const auto dialoguePolicy = integrationDialoguePolicy();
	(void)manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::Greeting,std::chrono::milliseconds(1000),dialoguePolicy); ASSERT_EQ(BotDialogueResponse::GreetingAccepted,manager.advanceDialogue(fixture.name,npc->getID(),BotDialogueIntent::Greeting,std::chrono::milliseconds(1001),dialoguePolicy).response);
	decision = BotPlanner::verifyStep(decision, 2, 4, true); ASSERT_EQ(3, decision.checkpoint.verifiedStepIndex); (void)manager.cancelDialogue(fixture.name); ASSERT_TRUE(g_game().removeCreature(npc,true));

	auto monsterType=std::make_shared<MonsterType>("PlannerMonster");monsterType->info.health=10;monsterType->info.healthMax=10;monsterType->info.experience=5;monsterType->info.lookcorpse=3994;auto monster=std::make_shared<Monster>(monsterType);const Position monsterPosition(region.x+1,region.y,region.z);ASSERT_TRUE(g_game().placeCreature(monster,monsterPosition,false,true));ASSERT_EQ(monster->getID(),manager.evaluateCombat(fixture.name).selectedCreatureId);const auto perception=BotPerception::observe(player);ASSERT_TRUE(perception);const auto combat=BotCombat::observe(player,*perception);ASSERT_TRUE(combat);const auto target=std::ranges::find(combat->creatures,monster->getID(),&BotCombatCreatureObservation::id);ASSERT_NE(combat->creatures.end(),target);ASSERT_EQ(BotCombatExecutionOutcome::TargetAcquired,manager.executeCombat(fixture.name,{monster->getID(),combat->revision,target->signature,player->getPosition()},std::chrono::milliseconds(1100)).outcome);const auto monsterId=monster->getID();CombatDamage fatal;fatal.primary={COMBAT_PHYSICALDAMAGE,-100};ASSERT_TRUE(g_game().combatChangeHealth(player,monster,fatal));monster->onDeath();ASSERT_TRUE(monster->isRemoved());decision=BotPlanner::verifyStep(decision,3,5,true);ASSERT_EQ(4,decision.checkpoint.verifiedStepIndex);
	const auto corpseTile=g_game().map.getTile(monsterPosition);ASSERT_NE(nullptr,corpseTile);std::shared_ptr<Item> corpse;for(const auto&item:*corpseTile->getItemList())if(item&&item->isCorpse()&&item->getContainer()){corpse=item;break;}ASSERT_NE(nullptr,corpse);corpse->getContainer()->internalAddThing(Item::CreateItem(3031,1));BotLootPolicy lootPolicy{.rules={{{.itemTypeId=3031,.valueCategory=1,.priority=1}}}};const auto selected=manager.evaluateLoot(fixture.name,monsterPosition,monsterId,{},lootPolicy);ASSERT_TRUE(selected.selected);BotLootTransferRequest lootRequest{.corpsePosition=monsterPosition,.sourceCreatureId=monsterId,.corpseSignature=selected.corpse.signature,.itemTypeId=3031,.itemSignature=selected.selected->item.signature,.count=1};EXPECT_EQ(BotLootTransferOutcome::Pending,manager.executeLoot(fixture.name,lootRequest,std::chrono::milliseconds(1200),lootPolicy).outcome);EXPECT_EQ(BotLootTransferOutcome::Succeeded,manager.executeLoot(fixture.name,lootRequest,std::chrono::milliseconds(1201),lootPolicy).outcome);decision=BotPlanner::verifyStep(decision,4,6,true);ASSERT_EQ(5,decision.checkpoint.verifiedStepIndex);
	const auto quest=manager.observeQuest(fixture.name,integrationQuest(player->getVocationId(),region),{{.type=BotQuestObjectiveType::KillCreatureType,.subjectId=77,.count=1,.revision=monsterId}});EXPECT_EQ(7001,quest.questId);decision=BotPlanner::verifyStep(decision,5,7,true);ASSERT_EQ(6,decision.checkpoint.verifiedStepIndex);

	ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalRemoveItem(corpse));BotSupplyPolicy supplyPolicy{.rules={{3031,BotSupplyCategory::HealthHealing}},.thresholds={{BotSupplyCategory::HealthHealing,1,0,true}}};EXPECT_NE(BotSupplyIntent::NoHealingSupplies,manager.evaluateSupplies(fixture.name,supplyPolicy).intent);
	auto replanObservation=observation;replanObservation.revision=8;replanObservation.supplyUrgent=true;auto resupply=BotPlanner::replan(goals,replanObservation,decision,BotReplanReason::SupplyDepleted);EXPECT_EQ(BotGoalType::Resupply,resupply.goalType);
	replanObservation.revision=9;replanObservation.supplyUrgent=false;replanObservation.survivalCritical=true;auto survival=BotPlanner::replan(goals,replanObservation,resupply,BotReplanReason::SurvivalOverride);EXPECT_EQ(BotGoalType::Survive,survival.goalType);
	replanObservation.revision=10;replanObservation.survivalCritical=false;replanObservation.activeQuest=true;auto resumed=BotPlanner::replan(goals,replanObservation,survival,BotReplanReason::QuestStateChanged);EXPECT_EQ(BotGoalType::CompleteConfiguredQuest,resumed.goalType);
	replanObservation.revision=11;replanObservation.routeAvailable=false;EXPECT_EQ(BotReplanReason::RouteUnavailable,BotPlanner::replan(goals,replanObservation,resumed,BotReplanReason::RouteUnavailable).replanReason);
	EXPECT_FALSE(resumed.containsWorldOwnership());EXPECT_TRUE(manager.logout(fixture.name,false));ASSERT_TRUE(fixture.cleanup());EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, LongHorizonPlannerReconstructsOnlyVerifiedSafeCheckpointsWithoutGameplayMutation) {
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game());
	const auto firstSession = loginBotOrReport(manager, fixture.name); ASSERT_NE(nullptr, firstSession); auto firstPlayer = std::const_pointer_cast<Player>(firstSession->getPlayer());
	const auto storageBefore = firstPlayer->getStorageValue(900100); const auto experienceBefore = firstPlayer->getExperience(); const auto itemBefore = std::static_pointer_cast<Cylinder>(firstPlayer)->getItemTypeCount(2160);
	auto goals = integrationPlannerGoals(fixture.start); BotPlannerObservation initial { .revision = 1, .sessionGeneration = 1, .progressionConfigured = true };
	auto decision = BotPlanner::select(goals, initial); ASSERT_EQ(BotGoalType::HuntConfiguredRegion, decision.goalType); decision = BotPlanner::verifyStep(decision, 0, 2, true); ASSERT_TRUE(decision.checkpoint.safeSaveBoundary);
	EXPECT_EQ(storageBefore, firstPlayer->getStorageValue(900100)); EXPECT_EQ(experienceBefore, firstPlayer->getExperience()); EXPECT_EQ(itemBefore, std::static_pointer_cast<Cylinder>(firstPlayer)->getItemTypeCount(2160));
	EXPECT_TRUE(manager.logout(fixture.name, false));
	const auto secondSession = loginBotOrReport(manager, fixture.name); ASSERT_NE(nullptr, secondSession); BotPlannerObservation rebuilt { .revision = 3, .sessionGeneration = 2, .progressionConfigured = true };
	const auto resumed = BotPlanner::reconstruct(decision, rebuilt); EXPECT_EQ(BotGoalStatus::Active, resumed.status); EXPECT_EQ(2, resumed.checkpoint.sessionGeneration); EXPECT_EQ(1, resumed.checkpoint.verifiedStepIndex);
	auto invalidObservation = rebuilt; invalidObservation.revision = 4; invalidObservation.checkpointValid = false; const auto invalid = BotPlanner::reconstruct(decision, invalidObservation); EXPECT_EQ(BotPlannerFailure::CheckpointInvalid, invalid.failure);
	const auto replanned = BotPlanner::replan(goals, invalidObservation, invalid, BotReplanReason::CheckpointInvalid); EXPECT_EQ(BotReplanReason::CheckpointInvalid, replanned.replanReason); EXPECT_EQ(0, replanned.checkpoint.verifiedStepIndex);
	EXPECT_FALSE(resumed.containsWorldOwnership()); EXPECT_TRUE(manager.logout(fixture.name, false)); ASSERT_TRUE(fixture.cleanup()); EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, PlanExecutionDelegatesRealTravelAndWaitsForAuthoritativeCompletion) {
	PlayerBotDatabaseFixture fixture(g_database());
	for (int x = 0; x <= 2; ++x) createWalkableTile(Position(fixture.start.x + x, fixture.start.y, fixture.start.z));
	BotManager manager(g_game()); const auto session = loginBotOrReport(manager, fixture.name); ASSERT_NE(nullptr, session);
	const Position destination(fixture.start.x + 2, fixture.start.y, fixture.start.z);
	BotGoal goal { .id = 90, .type = BotGoalType::ReturnHomeRegion, .priority = BotGoalPriority::Progression, .status = BotGoalStatus::Eligible, .policyRevision = 1, .configuredRegion = destination };
	BotPlannerObservation plannerObservation { .revision = 1, .sessionGeneration = 7, .progressionConfigured = true };
	auto decision = BotPlanner::select({ goal }, plannerObservation); ASSERT_EQ(BotPlanStepType::Observe, decision.plan.steps.front().type);
	auto execution = BotPlanExecutor::start(500, decision, { .revision = 1, .sessionGeneration = 7, .placed = true });
	execution = BotPlanExecutor::delegate(execution, { .revision = 2, .sessionGeneration = 7, .placed = true }); ASSERT_TRUE(execution.delegatedStep);
	execution.delegatedStep->accepted = true; EXPECT_EQ(0, execution.decision.checkpoint.verifiedStepIndex);
	execution = BotPlanExecutor::observe(execution, BotPlanStepOutcome::Succeeded, 3, true); ASSERT_EQ(1, execution.decision.checkpoint.verifiedStepIndex);
	execution.freshObservationRequired = false;
	execution = BotPlanExecutor::delegate(execution, { .revision = 4, .sessionGeneration = 7, .placed = true }); ASSERT_EQ(BotPlanSubsystem::Navigation, execution.delegatedStep->intent.subsystem);
	auto route = manager.startRoute(fixture.name, destination, std::chrono::milliseconds(1)); for (uint8_t i=0;i<8&&route.state!=BotRouteState::Arrived;++i) route=manager.advanceRoute(fixture.name,std::chrono::milliseconds(100+i*100)); ASSERT_EQ(BotRouteState::Arrived,route.state);
	execution = BotPlanExecutor::observe(execution, BotPlanStepOutcome::Succeeded, 5, true); EXPECT_EQ(2,execution.decision.checkpoint.verifiedStepIndex); EXPECT_FALSE(execution.containsWorldOwnership());
	EXPECT_TRUE(manager.logout(fixture.name,false)); ASSERT_TRUE(fixture.cleanup()); EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, PlanExecutionArbitratesRecoveryDeathAndSessionCloseWithoutMutation) {
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game());
	const auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); const auto player=std::const_pointer_cast<Player>(session->getPlayer());
	const auto experienceBefore=player->getExperience(); const auto storageBefore=player->getStorageValue(900100);
	BotGoal goal{.id=91,.type=BotGoalType::IdleSafely,.status=BotGoalStatus::Eligible,.policyRevision=1}; BotPlannerObservation po{.revision=1,.sessionGeneration=8};
	auto execution=BotPlanExecutor::start(501,BotPlanner::select({goal},po),{.revision=1,.sessionGeneration=8,.placed=true});
	auto pending=BotPlanExecutor::arbitrate(execution,{.revision=2,.sessionGeneration=8,.placed=true,.pendingAuthoritativeAction=true,.shutdownRequested=true}); EXPECT_EQ(BotPlanArbitrationReason::PendingBoundary,pending.reason);
	auto recovered=BotPlanExecutor::recover(execution,BotPlanFailure::RouteUnavailable,77); EXPECT_EQ(BotPlanRecoveryDecision::RetryStep,recovered.recovery);
	auto survival=BotPlanExecutor::arbitrate(execution,{.revision=2,.sessionGeneration=8,.placed=true,.survivalCritical=true}); EXPECT_EQ(BotPlanArbitrationReason::CriticalSurvival,survival.reason);
	auto dead=BotPlanExecutor::delegate(execution,{.revision=2,.sessionGeneration=8,.placed=true,.dead=true}); EXPECT_EQ(BotPlanExecutionState::Dead,dead.state);
	EXPECT_EQ(experienceBefore,player->getExperience()); EXPECT_EQ(storageBefore,player->getStorageValue(900100)); EXPECT_TRUE(manager.logout(fixture.name,false)); EXPECT_EQ(nullptr,session->getPlayer()); ASSERT_TRUE(fixture.cleanup()); EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, PlannerCheckpointPersistsAcrossOrdinaryLogoutAndFreshSession) {
	PlayerBotDatabaseFixture fixture(g_database()); createWalkableTile(fixture.start); BotManager manager(g_game()); const auto first=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,first);
	BotGoal goal{.id=8,.type=BotGoalType::IdleSafely,.status=BotGoalStatus::Eligible,.policyRevision=1}; BotPlannerObservation po{.revision=1,.sessionGeneration=1}; auto decision=BotPlanner::select({goal},po); decision=BotPlanner::verifyStep(decision,0,2,true);
	auto execution=BotPlanExecutor::start(700,decision,{.revision=3,.sessionGeneration=1,.placed=true}); auto checkpoint=BotPlannerPersistence::capture(fixture.playerId,execution,BotPlanSubsystem::Observation); ASSERT_TRUE(checkpoint.safeSaveBoundary); ASSERT_TRUE(BotPlannerPersistence::persist(checkpoint));
	ASSERT_TRUE(manager.save(fixture.name)); ASSERT_TRUE(manager.logout(fixture.name,false)); const auto second=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,second);
	const auto loaded=BotPlannerPersistence::load(fixture.playerId); ASSERT_EQ(BotCheckpointLoadReason::Valid,loaded.reason); ASSERT_TRUE(loaded.durable); ASSERT_TRUE(loaded.checkpoint); EXPECT_EQ(checkpoint,*loaded.checkpoint);
	BotCheckpointValidation validation{.playerId=fixture.playerId,.schemaVersion=1,.policyRevision=1,.goalIds={8},.planRevisions={1},.planStepCount=static_cast<uint16_t>(decision.plan.steps.size()),.postconditionValid=true,.observationFresh=true}; EXPECT_EQ(BotCheckpointLoadReason::Valid,BotPlannerPersistence::validate(*loaded.checkpoint,validation).reason);
	validation.policyRevision=2; EXPECT_EQ(BotCheckpointLoadReason::PolicyRevisionChanged,BotPlannerPersistence::validate(*loaded.checkpoint,validation).reason); validation.policyRevision=1;validation.postconditionValid=false;EXPECT_EQ(BotCheckpointLoadReason::PostconditionInvalid,BotPlannerPersistence::validate(*loaded.checkpoint,validation).reason);
	EXPECT_TRUE(manager.logout(fixture.name,false)); EXPECT_TRUE(BotPlannerPersistence::erase(fixture.playerId)); ASSERT_TRUE(fixture.cleanup()); EXPECT_FALSE(fixture.hasCommittedRows());
}

TEST(PlayerBotIntegrationTest, PlannerLongCampaignIsBoundedDeterministicAndRejectsCorruption) {
	PlayerBotDatabaseFixture fixture(g_database());
	for (int x = 0; x <= 5; ++x) createWalkableTile(Position(fixture.start.x + x, fixture.start.y, fixture.start.z));
	BotManager manager(g_game()); auto session=loginBotOrReport(manager,fixture.name); ASSERT_NE(nullptr,session); auto player=std::const_pointer_cast<Player>(session->getPlayer());
	auto backpack=Item::CreateItem(ITEM_BACKPACK);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalAddItem(player,backpack,CONST_SLOT_BACKPACK,FLAG_NOLIMIT));
	const Position huntRegion(fixture.start.x+2,fixture.start.y,fixture.start.z); uint64_t revision=1; uint64_t generation=1; uint64_t executionId=800;
	BotLongCampaignBudget budget;budget.requiredCompletedGoals=3;budget.maximumPlannerTicks=64;BotLongCampaignProgress progress;
	bool ordinarySavePerformed=false;

	auto executeGoal = [&](BotGoal goal, BotPlanExecution &completed) {
		BotPlannerObservation observation{.revision=revision++,.sessionGeneration=generation,.placed=true,.progressionConfigured=true};
		auto decision=BotPlanner::select({goal},observation); EXPECT_EQ(goal.type,decision.goalType);
		auto execution=BotPlanExecutor::start(executionId++,decision,{.revision=revision++,.sessionGeneration=generation,.placed=true});
		uint32_t defeatedId=0; Position corpsePosition; std::shared_ptr<Item> corpse;
		while(!execution.terminal()) {
			execution=BotPlanExecutor::delegate(execution,{.revision=revision++,.sessionGeneration=generation,.placed=true}); ASSERT_TRUE(execution.delegatedStep);
			const auto step=execution.delegatedStep->intent.type;
			switch(step) {
				case BotPlanStepType::Observe: ASSERT_TRUE(BotPerception::observe(player)); break;
				case BotPlanStepType::Travel: { const Position destination=execution.delegatedStep->intent.region; auto route=manager.startRoute(fixture.name,destination,std::chrono::milliseconds(revision));for(uint8_t i=0;i<12&&route.state!=BotRouteState::Arrived;++i)route=manager.advanceRoute(fixture.name,std::chrono::milliseconds(revision+100+i*100));ASSERT_EQ(BotRouteState::Arrived,route.state);break; }
				case BotPlanStepType::Fight: { auto type=std::make_shared<MonsterType>("PersistentPlannerMonster");type->info.health=10;type->info.healthMax=10;type->info.experience=5;type->info.lookcorpse=3994;auto monster=std::make_shared<Monster>(type);corpsePosition=Position(player->getPosition().x+1,player->getPosition().y,player->getPosition().z);ASSERT_TRUE(g_game().placeCreature(monster,corpsePosition,false,true));ASSERT_EQ(monster->getID(),manager.evaluateCombat(fixture.name).selectedCreatureId);const auto perception=BotPerception::observe(player);ASSERT_TRUE(perception);const auto combat=BotCombat::observe(player,*perception);ASSERT_TRUE(combat);const auto target=std::ranges::find(combat->creatures,monster->getID(),&BotCombatCreatureObservation::id);ASSERT_NE(combat->creatures.end(),target);ASSERT_EQ(BotCombatExecutionOutcome::TargetAcquired,manager.executeCombat(fixture.name,{monster->getID(),combat->revision,target->signature,player->getPosition()},std::chrono::milliseconds(revision)).outcome);defeatedId=monster->getID();CombatDamage fatal;fatal.primary={COMBAT_PHYSICALDAMAGE,-100};ASSERT_TRUE(g_game().combatChangeHealth(player,monster,fatal));monster->onDeath();ASSERT_TRUE(monster->isRemoved());break; }
				case BotPlanStepType::Loot: { const auto tile=g_game().map.getTile(corpsePosition);ASSERT_NE(nullptr,tile);for(const auto&item:*tile->getItemList())if(item&&item->isCorpse()&&item->getContainer()){corpse=item;break;}ASSERT_NE(nullptr,corpse);corpse->getContainer()->internalAddThing(Item::CreateItem(3031,1));BotLootPolicy policy{.rules={{{.itemTypeId=3031,.valueCategory=1,.priority=1}}}};const auto selected=manager.evaluateLoot(fixture.name,corpsePosition,defeatedId,{},policy);ASSERT_TRUE(selected.selected);BotLootTransferRequest request{.corpsePosition=corpsePosition,.sourceCreatureId=defeatedId,.corpseSignature=selected.corpse.signature,.itemTypeId=3031,.itemSignature=selected.selected->item.signature,.count=1};ASSERT_EQ(BotLootTransferOutcome::Pending,manager.executeLoot(fixture.name,request,std::chrono::milliseconds(revision),policy).outcome);ASSERT_EQ(BotLootTransferOutcome::Succeeded,manager.executeLoot(fixture.name,request,std::chrono::milliseconds(revision+1),policy).outcome);ASSERT_EQ(RETURNVALUE_NOERROR,g_game().internalRemoveItem(corpse));break; }
				case BotPlanStepType::Resupply: { BotSupplyPolicy policy{.rules={{3031,BotSupplyCategory::HealthHealing}},.thresholds={{BotSupplyCategory::HealthHealing,1,0,true}}};const auto assessment=manager.evaluateSupplies(fixture.name,policy);ASSERT_NE(BotSupplyIntent::NoHealingSupplies,assessment.intent);break; }
				case BotPlanStepType::SaveLogout: if(!ordinarySavePerformed){ASSERT_TRUE(manager.save(fixture.name));ordinarySavePerformed=true;}else{ASSERT_EQ(BotSessionState::Placed,session->getState());}break;
				case BotPlanStepType::Complete: break;
				default: FAIL()<<"unexpected step in persisted campaign";
			}
			execution=BotPlanExecutor::observe(execution,BotPlanStepOutcome::Succeeded,revision++,true);
		}
		EXPECT_EQ(BotPlanExecutionState::Completed,execution.state); completed=std::move(execution);
	};

	BotPlanExecution first;executeGoal({.id=101,.type=BotGoalType::HuntConfiguredRegion,.priority=BotGoalPriority::Progression,.status=BotGoalStatus::Eligible,.policyRevision=1,.configuredRegion=huntRegion},first);
	progress=BotPlannerPersistence::advance(progress,budget,10,true,false,false,false,true,true,false);
	auto persisted=BotPlannerPersistence::capture(fixture.playerId,first,BotPlanSubsystem::Lifecycle);ASSERT_TRUE(persisted.safeSaveBoundary);ASSERT_TRUE(BotPlannerPersistence::persist(persisted));
	const auto oldPlayer=player.get();const auto oldSession=session;ASSERT_TRUE(manager.logout(fixture.name,false));EXPECT_EQ(nullptr,oldSession->getPlayer());session=loginBotOrReport(manager,fixture.name);ASSERT_NE(nullptr,session);player=std::const_pointer_cast<Player>(session->getPlayer());EXPECT_NE(oldPlayer,player.get());++generation;
	const auto loaded=BotPlannerPersistence::load(fixture.playerId);ASSERT_EQ(BotCheckpointLoadReason::Valid,loaded.reason);ASSERT_TRUE(loaded.checkpoint);const auto fresh=BotPerception::observe(player);ASSERT_TRUE(fresh);BotCheckpointValidation valid{.playerId=fixture.playerId,.schemaVersion=1,.policyRevision=1,.goalIds={101},.planRevisions={1},.planStepCount=static_cast<uint16_t>(first.decision.plan.steps.size()),.postconditionValid=player->getPosition()==huntRegion,.observationFresh=true};EXPECT_EQ(BotCheckpointLoadReason::Valid,BotPlannerPersistence::validate(*loaded.checkpoint,valid).reason);
	progress=BotPlannerPersistence::advance(progress,budget,10,false,false,false,false,false,false,true);

	CombatDamage damage;damage.primary={COMBAT_PHYSICALDAMAGE,-125};ASSERT_TRUE(g_game().combatChangeHealth(nullptr,player,damage));const auto survivalAssessment=manager.evaluateSurvival(fixture.name);EXPECT_GE(survivalAssessment.urgency,BotSurvivalUrgency::Critical);BotPlannerObservation interrupted{.revision=revision++,.sessionGeneration=generation,.survivalCritical=true,.supplyUrgent=true,.progressionConfigured=true};auto interruptedDecision=BotPlanner::select({{.id=1,.type=BotGoalType::Survive,.status=BotGoalStatus::Eligible,.policyRevision=1},{.id=102,.type=BotGoalType::Resupply,.status=BotGoalStatus::Eligible,.policyRevision=1,.configuredRegion=huntRegion}},interrupted);EXPECT_EQ(BotGoalType::Survive,interruptedDecision.goalType);CombatDamage healing;healing.primary={COMBAT_HEALING,125};ASSERT_TRUE(g_game().combatChangeHealth(nullptr,player,healing));auto replanObservation=interrupted;replanObservation.revision=revision++;replanObservation.survivalCritical=false;auto replanned=BotPlanner::replan({{.id=102,.type=BotGoalType::Resupply,.status=BotGoalStatus::Eligible,.policyRevision=1,.configuredRegion=huntRegion}},replanObservation,interruptedDecision,BotReplanReason::SurvivalOverride);EXPECT_EQ(BotGoalType::Resupply,replanned.goalType);progress=BotPlannerPersistence::advance(progress,budget,10,false,true,false,false,false,false,false);

	BotPlanExecution second;executeGoal({.id=102,.type=BotGoalType::Resupply,.priority=BotGoalPriority::UrgentResupply,.status=BotGoalStatus::Eligible,.policyRevision=1,.configuredRegion=huntRegion},second);EXPECT_TRUE(second.terminal());progress=BotPlannerPersistence::advance(progress,budget,10,true,false,false,false,true,false,false);
	BotPlanExecution third;executeGoal({.id=103,.type=BotGoalType::ReturnHomeRegion,.priority=BotGoalPriority::Progression,.status=BotGoalStatus::Eligible,.policyRevision=1,.configuredRegion=fixture.start},third);EXPECT_TRUE(third.terminal());progress=BotPlannerPersistence::advance(progress,budget,10,true,false,false,false,true,false,false);
	EXPECT_EQ(BotLongCampaignState::Success,progress.state);EXPECT_EQ(3,progress.completedGoals);EXPECT_EQ(1,progress.replans);EXPECT_EQ(1,progress.saveCycles);EXPECT_EQ(1,progress.sessionReconstructions);
	ASSERT_TRUE(g_database().executeQuery("UPDATE `player_bot_planner_state` SET `checksum`=`checksum`+1 WHERE `player_id`="+std::to_string(fixture.playerId)));EXPECT_EQ(BotCheckpointLoadReason::Corrupt,BotPlannerPersistence::load(fixture.playerId).reason);
	EXPECT_TRUE(manager.logout(fixture.name,false));EXPECT_TRUE(BotPlannerPersistence::erase(fixture.playerId));ASSERT_TRUE(fixture.cleanup());EXPECT_FALSE(fixture.hasCommittedRows());
}
