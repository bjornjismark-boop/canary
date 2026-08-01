/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_controller.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <functional>
	#include <memory>
	#include <string>
	#include <string_view>
#endif

class Game;
class Player;
class BotManager;
enum class ManagedPlayerRemovalResult : uint8_t;

struct BotSessionOperations {
	std::function<bool(std::string_view)> admitLoad;
	std::function<bool(const std::shared_ptr<Player> &)> admitPlacement;
	std::function<bool(const std::shared_ptr<Player> &)> save;
	std::function<ManagedPlayerRemovalResult(
		const std::shared_ptr<Player> &,
		bool,
		const std::function<bool(const std::shared_ptr<Player> &)> &
	)> remove;
};

enum class BotSessionState : uint8_t {
	Created,
	Loaded,
	Placed,
	PendingSave,
	Closed,
};

class BotSession final {
public:
	~BotSession() noexcept;

	BotSession(const BotSession &) = delete;
	BotSession &operator=(const BotSession &) = delete;

	[[nodiscard]] std::shared_ptr<const Player> getPlayer() const {
		return player;
	}

	[[nodiscard]] BotSessionState getState() const {
		return state;
	}

	[[nodiscard]] const BotBlackboard *getBlackboard() const {
		return controller ? &controller->getBlackboard() : nullptr;
	}
	[[nodiscard]] const BotRouteProgress *getRouteProgress() const { return controller ? &controller->getRouteProgress() : nullptr; }
	[[nodiscard]] const BotTransitionProgress *getTransitionProgress() const { return controller ? &controller->getTransitionProgress() : nullptr; }
	[[nodiscard]] const BotTargetLock *getCombatLock() const { return controller ? &controller->getCombatLock() : nullptr; }
	[[nodiscard]] const BotAttackExecutionState *getAttackExecutionState() const { return controller ? &controller->getAttackExecutionState() : nullptr; }
	[[nodiscard]] const BotSurvivalProgress *getSurvivalProgress() const { return controller ? &controller->getSurvivalProgress() : nullptr; }
	[[nodiscard]] const BotLootExecutionProgress *getLootProgress() const { return controller ? &controller->getLootProgress() : nullptr; }
	[[nodiscard]] const BotAdventureProgress *getAdventureProgress() const { return controller ? &controller->getAdventureProgress() : nullptr; }
	[[nodiscard]] const std::optional<BotEquipmentObservation> *getEquipmentObservation() const { return controller ? &controller->getEquipmentObservation() : nullptr; }
	[[nodiscard]] const BotShopProgress *getShopProgress() const { return controller ? &controller->getShopProgress() : nullptr; }
	[[nodiscard]] const BotResupplyProgress *getResupplyProgress() const { return controller ? &controller->getResupplyProgress() : nullptr; }
	[[nodiscard]] const BotDialogueProgress *getDialogueProgress() const { return controller ? &controller->getDialogueProgress() : nullptr; }
	[[nodiscard]] const std::optional<BotQuestObservation> *getQuestObservation() const { return controller ? &controller->getQuestObservation() : nullptr; }
	[[nodiscard]] const std::optional<BotQuestExecutionResult> *getQuestExecution() const { return controller ? &controller->getQuestExecution() : nullptr; }

private:
	friend class BotManager;

	explicit BotSession(Game &game, BotSessionOperations operations);

	[[nodiscard]] bool load(const std::string &name);
	[[nodiscard]] bool place();
	[[nodiscard]] ReturnValue move(Direction direction);
	[[nodiscard]] BotWalkabilityResult assess(Direction direction) const;
	[[nodiscard]] BotActionResult executeMovement(const BotWalkabilityResult &assessment, std::chrono::milliseconds now);
	[[nodiscard]] BotActionResult tick(std::chrono::milliseconds now);
	[[nodiscard]] BotActionResult execute(const BotAction &action, std::chrono::milliseconds now);
	[[nodiscard]] BotRouteProgress startRoute(const Position &destination, std::chrono::milliseconds now, BotRouteLimits limits);
	[[nodiscard]] BotRouteProgress advanceRoute(std::chrono::milliseconds now);
	[[nodiscard]] BotRouteProgress cancelRoute();
	[[nodiscard]] BotTransitionResult startTransition(const BotTransitionRequest &request, std::chrono::milliseconds now);
	[[nodiscard]] BotTransitionResult advanceTransition(std::chrono::milliseconds now);
	[[nodiscard]] BotTransitionResult cancelTransition();
	[[nodiscard]] BotTargetSelectionResult evaluateCombat(const BotCombatPolicy &policy);
	[[nodiscard]] BotCombatExecutionResult executeCombat(const BotCombatExecutionRequest &request, std::chrono::milliseconds now, const BotCombatExecutionPolicy &policy);
	[[nodiscard]] BotSurvivalAssessment evaluateSurvival(const BotSurvivalPolicy &, std::vector<BotHealingOption>);
	[[nodiscard]] BotHealingResult executeHealing(const BotHealingOption &, std::chrono::milliseconds, const BotSurvivalPolicy &);
	[[nodiscard]] BotFleeResult executeFlee(std::chrono::milliseconds, const BotSurvivalPolicy &);
	[[nodiscard]] BotDeathResult observeDeath();
	[[nodiscard]] BotLootSelectionResult evaluateLoot(const Position &position, uint32_t sourceCreatureId, BotCorpseSignature expectedSignature = {}, const BotLootPolicy &policy = {});
	[[nodiscard]] BotLootTransferResult executeLoot(const BotLootTransferRequest &, std::chrono::milliseconds, const BotLootPolicy &, const BotLootTransferPolicy &);
	[[nodiscard]] BotSupplyAssessment evaluateSupplies(const BotSupplyPolicy &, uint64_t expectedInventorySignature);
	[[nodiscard]] BotAdventureProgress advanceAdventure(const BotAdventureObservation &, std::chrono::milliseconds, const BotAdventurePolicy &);
	[[nodiscard]] BotEquipmentObservation evaluateEquipment(const BotEquipmentPolicy &);
	[[nodiscard]] BotShopObservation observeShop(uint32_t npcId, uint16_t maximumOffers);
	[[nodiscard]] BotShopTransactionResult executeShop(const BotShopTransactionRequest &, std::chrono::milliseconds, const BotShopPolicy &);
	[[nodiscard]] BotShopTransactionResult cancelShop();
	[[nodiscard]] BotDepotObservation observeDepot(uint32_t, const BotResupplyPolicy &);
	[[nodiscard]] BotResupplyResult executeResupply(const BotResupplyRequest &, std::chrono::milliseconds, const BotResupplyPolicy &);
	[[nodiscard]] BotEquipmentExecutionResult executeEquipment(const BotEquipmentExecutionRequest &, std::chrono::milliseconds, const BotEquipmentPolicy &);
	[[nodiscard]] BotResupplyResult cancelResupply();
	[[nodiscard]] BotDialogueObservation observeDialogue(const BotNpcDialoguePolicy &);
	[[nodiscard]] BotConversationResult advanceDialogue(uint32_t,BotDialogueIntent,std::chrono::milliseconds,const BotNpcDialoguePolicy &);
	[[nodiscard]] BotConversationResult cancelDialogue();
	[[nodiscard]] BotQuestObservation observeQuest(const BotQuestDefinition &, std::vector<BotQuestEvidence>, const BotQuestBounds &);
	[[nodiscard]] BotQuestEligibility evaluateQuest(BotMissionId, const BotQuestDefinition &, const BotNpcDialoguePolicy &, const BotQuestBounds &);
	[[nodiscard]] BotQuestExecutionResult startQuestExecution(const BotQuestPlan &, const BotQuestExecutionPolicy &);
	[[nodiscard]] BotQuestExecutionResult advanceQuestExecution(const BotQuestPlan &, const BotQuestStepObservation &, const BotQuestExecutionPolicy &);
	[[nodiscard]] bool save() const;
	[[nodiscard]] bool close(bool savePlayer);
	[[nodiscard]] bool retryPendingSave();
	void finishClose();
	[[nodiscard]] bool isCompletelyAbsentFromWorld() const;

	Game &game;
	BotSessionOperations operations;
	std::shared_ptr<Player> player;
	std::unique_ptr<BotController> controller;
	BotSessionState state = BotSessionState::Created;
	bool ownerAttemptedDestructorCleanup = false;
};
