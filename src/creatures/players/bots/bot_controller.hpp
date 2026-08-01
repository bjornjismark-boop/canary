/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_interaction.hpp"
#include "creatures/players/bots/bot_combat.hpp"
#include "creatures/players/bots/bot_navigation.hpp"
#include "creatures/players/bots/bot_survival.hpp"
#include "creatures/players/bots/bot_loot.hpp"
#include "creatures/players/bots/bot_supply.hpp"
#include "creatures/players/bots/bot_adventure.hpp"
#include "creatures/players/bots/bot_equipment.hpp"
#include "creatures/players/bots/bot_shop.hpp"
#include "creatures/players/bots/bot_resupply.hpp"
#include "creatures/players/bots/bot_dialogue.hpp"
#include "creatures/players/bots/bot_quest.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <cstdint>
	#include <functional>
	#include <memory>
#endif

enum Direction : uint8_t;
enum ReturnValue : uint16_t;

class Game;
class Player;

class BotController final {
public:
	using DecisionLog = std::function<void(const BotObservation &, BotRootState, const BotAction &, const BotActionResult &)>;

	BotController(Game &game, const std::shared_ptr<Player> &player, BotRuntimeLimits limits = {}, DecisionLog decisionLog = {});

	[[nodiscard]] ReturnValue move(Direction direction) const;
	[[nodiscard]] BotWalkabilityResult assess(Direction direction) const;
	[[nodiscard]] BotActionResult executeMovement(const BotWalkabilityResult &assessment, std::chrono::milliseconds now);
	[[nodiscard]] BotActionResult tick(std::chrono::milliseconds now);
	[[nodiscard]] BotActionResult execute(const BotAction &action, std::chrono::milliseconds now);
	[[nodiscard]] const BotBlackboard &getBlackboard() const { return blackboard; }
	[[nodiscard]] size_t getLastTickWork() const { return lastTickWork; }
	[[nodiscard]] BotRouteProgress startRoute(const Position &destination, std::chrono::milliseconds now, BotRouteLimits routeLimits = {});
	[[nodiscard]] BotRouteProgress advanceRoute(std::chrono::milliseconds now);
	[[nodiscard]] BotRouteProgress cancelRoute();
	[[nodiscard]] const BotRouteProgress &getRouteProgress() const { return routeProgress; }
	[[nodiscard]] BotTransitionResult startTransition(const BotTransitionRequest &request, std::chrono::milliseconds now);
	[[nodiscard]] BotTransitionResult advanceTransition(std::chrono::milliseconds now);
	[[nodiscard]] BotTransitionResult cancelTransition();
	[[nodiscard]] const BotTransitionProgress &getTransitionProgress() const { return transitionProgress; }
	[[nodiscard]] BotTargetSelectionResult evaluateCombat(const BotCombatPolicy &policy = {});
	[[nodiscard]] BotCombatExecutionResult executeCombat(const BotCombatExecutionRequest &request, std::chrono::milliseconds now, const BotCombatExecutionPolicy &policy = {});
	void cancelCombat();
	[[nodiscard]] const BotTargetLock &getCombatLock() const { return combatLock; }
	[[nodiscard]] const BotAttackExecutionState &getAttackExecutionState() const { return attackExecution; }
	[[nodiscard]] BotSurvivalAssessment evaluateSurvival(const BotSurvivalPolicy &policy = {}, std::vector<BotHealingOption> options = {});
	[[nodiscard]] BotHealingResult executeHealing(const BotHealingOption &, std::chrono::milliseconds now, const BotSurvivalPolicy &policy = {});
	[[nodiscard]] BotFleeResult executeFlee(std::chrono::milliseconds now, const BotSurvivalPolicy &policy = {});
	[[nodiscard]] BotDeathResult observeDeath();
	void cancelSurvival();
	[[nodiscard]] const BotSurvivalProgress &getSurvivalProgress() const { return survivalProgress; }
	[[nodiscard]] BotLootSelectionResult evaluateLoot(const Position &position, uint32_t sourceCreatureId, BotCorpseSignature expectedSignature = {}, const BotLootPolicy &policy = {});
	[[nodiscard]] BotLootTransferResult executeLoot(const BotLootTransferRequest &, std::chrono::milliseconds now, const BotLootPolicy & = {}, const BotLootTransferPolicy & = {});
	[[nodiscard]] BotLootExecutionProgress cancelLoot();
	[[nodiscard]] const BotLootExecutionProgress &getLootProgress() const { return lootProgress; }
	[[nodiscard]] BotSupplyAssessment evaluateSupplies(const BotSupplyPolicy & = {}, uint64_t expectedInventorySignature = 0);
	[[nodiscard]] BotAdventureProgress advanceAdventure(const BotAdventureObservation &, std::chrono::milliseconds, const BotAdventurePolicy & = {});
	[[nodiscard]] BotAdventureProgress cancelAdventure();
	[[nodiscard]] const BotAdventureProgress &getAdventureProgress() const { return adventureProgress; }
	[[nodiscard]] BotEquipmentObservation evaluateEquipment(const BotEquipmentPolicy & = {});
	[[nodiscard]] const std::optional<BotEquipmentObservation> &getEquipmentObservation() const { return equipmentObservation; }
	[[nodiscard]] BotShopObservation observeShop(uint32_t npcId, uint16_t maximumOffers = 256);
	[[nodiscard]] BotShopTransactionResult executeShop(const BotShopTransactionRequest &, std::chrono::milliseconds now, const BotShopPolicy & = {});
	[[nodiscard]] BotShopTransactionResult cancelShop();
	[[nodiscard]] const BotShopProgress &getShopProgress() const { return shopProgress; }
	[[nodiscard]] BotDepotObservation observeDepot(uint32_t depotId, const BotResupplyPolicy & = {});
	[[nodiscard]] BotResupplyResult executeResupply(const BotResupplyRequest &, std::chrono::milliseconds now, const BotResupplyPolicy & = {});
	[[nodiscard]] BotEquipmentExecutionResult executeEquipment(const BotEquipmentExecutionRequest &, std::chrono::milliseconds now, const BotEquipmentPolicy & = {});
	[[nodiscard]] BotResupplyResult cancelResupply();
	[[nodiscard]] const BotResupplyProgress &getResupplyProgress() const { return resupplyProgress; }
	[[nodiscard]] BotDialogueObservation observeDialogue(const BotNpcDialoguePolicy & = {});
	[[nodiscard]] BotConversationResult advanceDialogue(uint32_t npcId, BotDialogueIntent, std::chrono::milliseconds, const BotNpcDialoguePolicy & = {});
	[[nodiscard]] BotConversationResult cancelDialogue();
	[[nodiscard]] const BotDialogueProgress &getDialogueProgress() const { return dialogueProgress; }
	[[nodiscard]] BotQuestObservation observeQuest(const BotQuestDefinition &, std::vector<BotQuestEvidence> = {}, const BotQuestBounds & = {});
	[[nodiscard]] BotQuestEligibility evaluateQuest(BotMissionId, const BotQuestDefinition &, const BotNpcDialoguePolicy & = {}, const BotQuestBounds & = {});
	void clearQuestAssessment() { questObservation.reset(); }
	[[nodiscard]] const std::optional<BotQuestObservation> &getQuestObservation() const { return questObservation; }

private:
	[[nodiscard]] BotAction selectAction(const BotObservation &observation) const;
	[[nodiscard]] BotActionResult perform(const BotAction &action) const;
	[[nodiscard]] BotWalkabilityResult revalidateAndMove(const BotAction &action) const;

	Game &game;
	std::weak_ptr<Player> player;
	BotRuntimeLimits limits;
	DecisionLog decisionLog;
	BotBlackboard blackboard;
	std::chrono::milliseconds nextTickAt { 0 };
	std::chrono::milliseconds nextLogAt { 0 };
	size_t lastTickWork = 0;
	BotRouteLimits routeLimits;
	BotRouteResult route;
	BotRouteProgress routeProgress;
	BotTransitionProgress transitionProgress;
	BotTargetLock combatLock;
	BotAttackExecutionState attackExecution;
	BotSurvivalProgress survivalProgress;
	BotLootExecutionProgress lootProgress;
	bool survivalLootBlocked = false;
	BotAdventureProgress adventureProgress;
	std::optional<BotEquipmentObservation> equipmentObservation;
	BotShopProgress shopProgress;
	BotResupplyProgress resupplyProgress;
	BotDialogueProgress dialogueProgress;
	std::optional<BotQuestObservation> questObservation;
};
