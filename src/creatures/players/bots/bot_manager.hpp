/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_coordination.hpp"
#include "creatures/players/bots/bot_fleet.hpp"
#include "creatures/players/bots/bot_fleet_hardening.hpp"
#include "creatures/players/bots/bot_session.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <memory>
	#include <string>
	#include <unordered_map>
#endif

class Game;
class Player;

class BotManager final {
public:
	// Game owns the world registries used during cleanup and must outlive BotManager.
	// Call clear(true) during normal shutdown; the destructor only removes from the
	// world and deliberately performs no database saves.
	explicit BotManager(Game &game, BotSessionOperations operations = {});
	~BotManager() noexcept;

	BotManager(const BotManager &) = delete;
	BotManager &operator=(const BotManager &) = delete;

	[[nodiscard]] std::shared_ptr<const BotSession> login(const std::string &name);
	[[nodiscard]] bool logout(const std::string &name, bool savePlayer = true);
	[[nodiscard]] ReturnValue move(const std::string &name, Direction direction);
	[[nodiscard]] BotWalkabilityResult assess(const std::string &name, Direction direction) const;
	[[nodiscard]] BotActionResult executeMovement(const std::string &name, const BotWalkabilityResult &assessment, std::chrono::milliseconds now);
	[[nodiscard]] BotActionResult tick(const std::string &name, std::chrono::milliseconds now);
	[[nodiscard]] BotActionResult execute(const std::string &name, const BotAction &action, std::chrono::milliseconds now);
	[[nodiscard]] bool save(const std::string &name);
	[[nodiscard]] std::shared_ptr<const BotSession> getSession(const std::string &name) const;
	[[nodiscard]] bool clear(bool savePlayers = true);
	[[nodiscard]] BotRouteProgress startRoute(const std::string &name, const Position &destination, std::chrono::milliseconds now, BotRouteLimits limits = {});
	[[nodiscard]] BotRouteProgress advanceRoute(const std::string &name, std::chrono::milliseconds now);
	[[nodiscard]] BotRouteProgress cancelRoute(const std::string &name);
	[[nodiscard]] BotTransitionResult startTransition(const std::string &name, const BotTransitionRequest &request, std::chrono::milliseconds now);
	[[nodiscard]] BotTransitionResult advanceTransition(const std::string &name, std::chrono::milliseconds now);
	[[nodiscard]] BotTransitionResult cancelTransition(const std::string &name);
	[[nodiscard]] BotTargetSelectionResult evaluateCombat(const std::string &name, const BotCombatPolicy &policy = {});
	[[nodiscard]] BotCombatExecutionResult executeCombat(const std::string &name, const BotCombatExecutionRequest &request, std::chrono::milliseconds now, const BotCombatExecutionPolicy &policy = {});
	[[nodiscard]] BotSurvivalAssessment evaluateSurvival(const std::string &, const BotSurvivalPolicy & = {}, std::vector<BotHealingOption> = {});
	[[nodiscard]] BotHealingResult executeHealing(const std::string &, const BotHealingOption &, std::chrono::milliseconds, const BotSurvivalPolicy & = {});
	[[nodiscard]] BotFleeResult executeFlee(const std::string &, std::chrono::milliseconds, const BotSurvivalPolicy & = {});
	[[nodiscard]] BotDeathResult observeDeath(const std::string &);
	[[nodiscard]] BotLootSelectionResult evaluateLoot(const std::string &, const Position &, uint32_t sourceCreatureId, BotCorpseSignature expectedSignature = {}, const BotLootPolicy &policy = {});
	[[nodiscard]] BotLootTransferResult executeLoot(const std::string &, const BotLootTransferRequest &, std::chrono::milliseconds, const BotLootPolicy & = {}, const BotLootTransferPolicy & = {});
	[[nodiscard]] BotSupplyAssessment evaluateSupplies(const std::string &, const BotSupplyPolicy & = {}, uint64_t expectedInventorySignature = 0);
	[[nodiscard]] BotAdventureProgress advanceAdventure(const std::string &, const BotAdventureObservation &, std::chrono::milliseconds, const BotAdventurePolicy & = {});
	[[nodiscard]] BotEquipmentObservation evaluateEquipment(const std::string &, const BotEquipmentPolicy & = {});
	[[nodiscard]] BotShopObservation observeShop(const std::string &, uint32_t npcId, uint16_t maximumOffers = 256);
	[[nodiscard]] BotShopTransactionResult executeShop(const std::string &, const BotShopTransactionRequest &, std::chrono::milliseconds, const BotShopPolicy & = {});
	[[nodiscard]] BotShopTransactionResult cancelShop(const std::string &);
	[[nodiscard]] BotDepotObservation observeDepot(const std::string &, uint32_t, const BotResupplyPolicy & = {});
	[[nodiscard]] BotResupplyResult executeResupply(const std::string &, const BotResupplyRequest &, std::chrono::milliseconds, const BotResupplyPolicy & = {});
	[[nodiscard]] BotEquipmentExecutionResult executeEquipment(const std::string &, const BotEquipmentExecutionRequest &, std::chrono::milliseconds, const BotEquipmentPolicy & = {});
	[[nodiscard]] BotResupplyResult cancelResupply(const std::string &);
	[[nodiscard]] BotDialogueObservation observeDialogue(const std::string &, const BotNpcDialoguePolicy & = {});
	[[nodiscard]] BotConversationResult advanceDialogue(const std::string &, uint32_t, BotDialogueIntent, std::chrono::milliseconds, const BotNpcDialoguePolicy & = {});
	[[nodiscard]] BotConversationResult cancelDialogue(const std::string &);
	[[nodiscard]] BotQuestObservation observeQuest(const std::string &, const BotQuestDefinition &, std::vector<BotQuestEvidence> = {}, const BotQuestBounds & = {});
	[[nodiscard]] BotQuestEligibility evaluateQuest(const std::string &, BotMissionId, const BotQuestDefinition &, const BotNpcDialoguePolicy & = {}, const BotQuestBounds & = {});
	[[nodiscard]] BotQuestExecutionResult startQuestExecution(const std::string &, const BotQuestPlan &, const BotQuestExecutionPolicy & = {});
	[[nodiscard]] BotQuestExecutionResult advanceQuestExecution(const std::string &, const BotQuestPlan &, const BotQuestStepObservation &, const BotQuestExecutionPolicy & = {});
	[[nodiscard]] BotCoordinationFailure configureCoordinationGroup(BotCoordinationPolicy);
	[[nodiscard]] BotCoordinationGroupObservation observeCoordinationGroup(BotCoordinationGroupId);
	[[nodiscard]] BotCoordinationDecision evaluateCoordinationGroup(BotCoordinationGroupId);
	[[nodiscard]] BotCoordinationFailure reserveCoordinationTarget(BotCoordinationReservation, uint64_t now);
	void invalidateCoordinationTarget(BotCoordinationGroupId, BotCoordinationReservationType, uint64_t targetSignature);
	[[nodiscard]] BotRouteProgress executeCoordinationMovement(const std::string &, const BotCoordinationIntent &, std::chrono::milliseconds now, BotRouteLimits limits = {});
	[[nodiscard]] size_t coordinationGroupCount() const { return coordinationGroups.size(); }
	[[nodiscard]] size_t coordinationReservationCount(BotCoordinationGroupId) const;
	[[nodiscard]] size_t coordinationReservationCount() const;
	void clearCoordination();
	[[nodiscard]] BotFleetFailure configureFleet(BotFleetPopulationPolicy, BotFleetDistributionPolicy, std::vector<BotFleetMemberProfile>, uint32_t intervalTicks = 1000);
	[[nodiscard]] BotFleetReconciliation reconcileFleet(uint64_t now, bool overloaded = false);
	[[nodiscard]] BotFleetReconciliation reconcileFleet(uint64_t now, const BotFleetResourceObservation &);
	[[nodiscard]] bool configureFleetResources(BotFleetResourcePolicy);
	[[nodiscard]] bool startFleet(uint64_t now = 0);
	void pauseFleet();
	void resumeFleet();
	void drainFleet(uint32_t target = 0);
	void stopFleet(bool savePlayers = true);
	[[nodiscard]] bool loginFleetMember(const std::string &name);
	[[nodiscard]] bool logoutFleetMember(const std::string &name);
	[[nodiscard]] const BotFleetControllerStateValue &fleetState() const { return fleetController; }
	[[nodiscard]] const BotFleetReconciliation &lastFleetReconciliation() const { return lastFleetResult; }
	[[nodiscard]] const BotFleetLoadSheddingDecision &lastFleetLoadShedding() const { return fleetLoadShedding; }

	[[nodiscard]] size_t size() const {
		return sessions.size();
	}

private:
	Game &game;
	BotSessionOperations operations;
	std::unordered_map<std::string, std::shared_ptr<BotSession>> sessions;
	std::unordered_map<BotCoordinationGroupId, BotCoordinationGroupState> coordinationGroups;
	std::unordered_map<BotCoordinationMemberId, uint64_t> sessionGenerations;
	BotFleetPopulationPolicy fleetPopulation;
	BotFleetDistributionPolicy fleetDistribution;
	std::vector<BotFleetMemberProfile> fleetMembers;
	std::unordered_map<BotFleetMemberId, BotFleetObservation> fleetLifecycle;
	BotFleetControllerStateValue fleetController;
	BotFleetReconciliation lastFleetResult;
	BotFleetResourcePolicy fleetResourcePolicy;
	BotFleetLoadSheddingDecision fleetLoadShedding;
	uint16_t fleetHealthyResourceObservations = 0;
	std::shared_ptr<uint64_t> fleetLifetime = std::make_shared<uint64_t>(1);
	uint64_t fleetEventId = 0;
	uint64_t fleetObservationRevision = 0;
	uint32_t fleetIntervalTicks = 1000;
	void scheduleFleetReconciliation();
	void executeFleetReconciliation();
};
