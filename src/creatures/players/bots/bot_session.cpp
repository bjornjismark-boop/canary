/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_session.hpp"

#include "creatures/players/bots/bot_navigation.hpp"
#include "creatures/players/player.hpp"
#include "game/game.hpp"
#include "io/functions/iologindata_load_player.hpp"
#include "io/iologindata.hpp"
#include "lib/logging/log_with_spd_log.hpp"

BotSession::BotSession(Game &game, BotSessionOperations operations) :
	game(game), operations(std::move(operations)) {
	if (!this->operations.admitLoad) {
		this->operations.admitLoad = [](std::string_view) { return true; };
	}
	if (!this->operations.admitPlacement) {
		this->operations.admitPlacement = [](const std::shared_ptr<Player> &) { return true; };
	}
	if (!this->operations.save) {
		this->operations.save = [](const std::shared_ptr<Player> &player) {
			return IOLoginData::savePlayer(player);
		};
	}
	if (!this->operations.remove) {
		this->operations.remove = [&game](const std::shared_ptr<Player> &player, bool savePlayer, const auto &saveOperation) {
			const std::function<bool(const std::shared_ptr<Player> &)> noSave;
			return game.removeManagedPlayer(player, true, savePlayer ? saveOperation : noSave);
		};
	}
}

BotSession::~BotSession() noexcept {
	try {
		if (ownerAttemptedDestructorCleanup) {
			return;
		}
		if (state == BotSessionState::PendingSave) {
			g_logger().error("[BotSession::~BotSession] Discarding pending save for removed bot '{}'", player ? player->getName() : "<unknown>");
			return;
		}
		if (!close(false)) {
			g_logger().error("[BotSession::~BotSession] Failed to remove bot '{}' from the world", player ? player->getName() : "<unknown>");
		}
	} catch (const std::exception &exception) {
		g_logger().error("[BotSession::~BotSession] Exception during best-effort bot removal: {}", exception.what());
	} catch (...) {
		g_logger().error("[BotSession::~BotSession] Unknown exception during best-effort bot removal");
	}
}

bool BotSession::load(const std::string &name) {
	if (state != BotSessionState::Created) {
		g_logger().warn("[BotSession::load] Invalid runtime state while loading '{}'", name);
		return false;
	}
	if (name.empty()) {
		g_logger().warn("[BotSession::load] Rejected an empty player name");
		return false;
	}
	if (game.getPlayerByName(name)) {
		g_logger().warn("[BotSession::load] Player '{}' is already in the world", name);
		return false;
	}
	if (!operations.admitLoad(name)) {
		g_logger().warn("[BotSession::load] Load admission rejected for '{}'", name);
		return false;
	}

	auto loadedPlayer = std::make_shared<Player>(nullptr, PlayerControlType::Bot);
	loadedPlayer->setName(name);
	if (!IOLoginDataLoad::preLoadPlayer(loadedPlayer, name)) {
		g_logger().warn("[BotSession::load] preLoadPlayer failed for '{}'", name);
		return false;
	}
	if (game.getPlayerByGUID(loadedPlayer->getGUID())) {
		g_logger().warn("[BotSession::load] Player '{}' has a GUID already present in the world", name);
		return false;
	}
	if (!IOLoginData::loadPlayerById(loadedPlayer, loadedPlayer->getGUID(), false)) {
		g_logger().warn("[BotSession::load] loadPlayerById failed for '{}'", name);
		return false;
	}

	loadedPlayer->setID();
	loadedPlayer->setOnline(true);
	player = std::move(loadedPlayer);
	controller = std::make_unique<BotController>(game, player);
	state = BotSessionState::Loaded;
	return true;
}

bool BotSession::place() {
	if (state != BotSessionState::Loaded || !player) {
		g_logger().warn("[BotSession::place] Runtime initialization is incomplete");
		return false;
	}
	if (!operations.admitPlacement(player)) {
		g_logger().warn("[BotSession::place] Placement admission rejected for '{}'", player->getName());
		player->setOnline(false);
		return false;
	}

	if (!game.placeCreature(player, player->getLoginPosition())) {
		g_logger().warn("[BotSession::place] Primary placement failed for '{}' at {}", player->getName(), player->getLoginPosition().toString());
		if (!game.placeCreature(player, player->getTemplePosition(), false, true)) {
			g_logger().warn("[BotSession::place] Temple fallback placement failed for '{}' at {}", player->getName(), player->getTemplePosition().toString());
			return false;
		}
	}

	const auto registeredPlayer = game.getPlayerByName(player->getName());
	if (player->isRemoved() || !player->getParent() || !player->getTile() || registeredPlayer != player) {
		g_logger().warn(
			"[BotSession::place] Post-placement invariant failed for '{}': removed={}, parent={}, tile={}, registered={}",
			player->getName(),
			player->isRemoved(),
			player->getParent() != nullptr,
			player->getTile() != nullptr,
			registeredPlayer == player
		);
			if (!isCompletelyAbsentFromWorld() && operations.remove(player, false, operations.save) == ManagedPlayerRemovalResult::RemovalFailed) {
			g_logger().error("[BotSession::place] Failed to clean up rejected placement for '{}'", player->getName());
		}
		player->setOnline(false);
		return false;
	}

	state = BotSessionState::Placed;
	return true;
}

ReturnValue BotSession::move(Direction direction) {
	if (state != BotSessionState::Placed || !controller) {
		return RETURNVALUE_NOTPOSSIBLE;
	}
	return controller->move(direction);
}

BotWalkabilityResult BotSession::assess(Direction direction) const {
	if (state != BotSessionState::Placed || !controller) {
		return { .candidate = { .direction = direction }, .outcome = BotWalkability::WorldRejected, .movementCost = BotNavigation::BlockedCost };
	}
	return controller->assess(direction);
}

BotActionResult BotSession::executeMovement(const BotWalkabilityResult &assessment, std::chrono::milliseconds now) {
	if (state != BotSessionState::Placed || !controller) {
		return { BotActionStatus::Rejected, BotActionFailure::InvalidLifecycle };
	}
	return controller->executeMovement(assessment, now);
}

BotActionResult BotSession::tick(std::chrono::milliseconds now) {
	if (state != BotSessionState::Placed || !controller) {
		return { BotActionStatus::Rejected, BotActionFailure::InvalidLifecycle };
	}
	return controller->tick(now);
}

BotActionResult BotSession::execute(const BotAction &action, std::chrono::milliseconds now) {
	if (state != BotSessionState::Placed || !controller) {
		return { BotActionStatus::Rejected, BotActionFailure::InvalidLifecycle };
	}
	return controller->execute(action, now);
}

BotRouteProgress BotSession::startRoute(const Position &destination, std::chrono::milliseconds now, BotRouteLimits limits) {
	if (state != BotSessionState::Placed || !controller) return { .state = BotRouteState::Failed, .reason = BotRouteReason::InvalidLifecycle };
	return controller->startRoute(destination, now, limits);
}

BotRouteProgress BotSession::advanceRoute(std::chrono::milliseconds now) {
	if (state != BotSessionState::Placed || !controller) return { .state = BotRouteState::Failed, .reason = BotRouteReason::InvalidLifecycle };
	return controller->advanceRoute(now);
}

BotRouteProgress BotSession::cancelRoute() {
	if (state != BotSessionState::Placed || !controller) return { .state = BotRouteState::Failed, .reason = BotRouteReason::InvalidLifecycle };
	return controller->cancelRoute();
}

BotTransitionResult BotSession::startTransition(const BotTransitionRequest &request, std::chrono::milliseconds now) {
	if (state != BotSessionState::Placed || !controller) return { { BotInteractionOutcome::InvalidLifecycle, BotTransitionFailure::InvalidLifecycle }, BotTransitionState::Failed };
	return controller->startTransition(request, now);
}

BotTransitionResult BotSession::advanceTransition(std::chrono::milliseconds now) {
	if (state != BotSessionState::Placed || !controller) return { { BotInteractionOutcome::InvalidLifecycle, BotTransitionFailure::InvalidLifecycle }, BotTransitionState::Failed };
	return controller->advanceTransition(now);
}

BotTransitionResult BotSession::cancelTransition() {
	if (state != BotSessionState::Placed || !controller) return { { BotInteractionOutcome::InvalidLifecycle, BotTransitionFailure::InvalidLifecycle }, BotTransitionState::Failed };
	return controller->cancelTransition();
}

BotTargetSelectionResult BotSession::evaluateCombat(const BotCombatPolicy &policy) {
	if (state != BotSessionState::Placed || !controller) return { .failure = BotCombatFailure::InvalidLifecycle, .reason = BotCombatEligibility::InvalidLifecycle };
	return controller->evaluateCombat(policy);
}

BotCombatExecutionResult BotSession::executeCombat(const BotCombatExecutionRequest &request, std::chrono::milliseconds now, const BotCombatExecutionPolicy &policy) {
	if (state != BotSessionState::Placed || !controller) return { BotCombatExecutionOutcome::InvalidLifecycle, BotAttackFailure::InvalidLifecycle, BotAttackState::Failed };
	return controller->executeCombat(request, now, policy);
}

BotSurvivalAssessment BotSession::evaluateSurvival(const BotSurvivalPolicy &p, std::vector<BotHealingOption> o) { if (state != BotSessionState::Placed || !controller) return { .urgency = BotSurvivalUrgency::Fatal, .decision = BotSurvivalDecision::Dead }; return controller->evaluateSurvival(p, std::move(o)); }
BotHealingResult BotSession::executeHealing(const BotHealingOption &o, std::chrono::milliseconds n, const BotSurvivalPolicy &p) { if (state != BotSessionState::Placed || !controller) return { .outcome = BotHealingOutcome::InvalidLifecycle }; return controller->executeHealing(o, n, p); }
BotFleeResult BotSession::executeFlee(std::chrono::milliseconds n, const BotSurvivalPolicy &p) { if (state != BotSessionState::Placed || !controller) return { .outcome = BotFleeOutcome::Cancelled }; return controller->executeFlee(n, p); }
BotDeathResult BotSession::observeDeath() { if (!controller) return { .state = BotSurvivalState::Dead }; return controller->observeDeath(); }
BotLootSelectionResult BotSession::evaluateLoot(const Position &position, uint32_t sourceCreatureId, BotCorpseSignature expectedSignature, const BotLootPolicy &policy) { if (state != BotSessionState::Placed || !controller) return { .eligibility = BotLootEligibility::InvalidLifecycle, .failure = BotLootFailure::InvalidLifecycle }; return controller->evaluateLoot(position, sourceCreatureId, expectedSignature, policy); }
BotLootTransferResult BotSession::executeLoot(const BotLootTransferRequest &r, std::chrono::milliseconds n, const BotLootPolicy &p, const BotLootTransferPolicy &t) { if(state!=BotSessionState::Placed||!controller)return{.outcome=BotLootTransferOutcome::Cancelled,.failure=BotLootTransferFailure::InvalidLifecycle,.state=BotLootExecutionState::Failed,.request=r};return controller->executeLoot(r,n,p,t); }
BotSupplyAssessment BotSession::evaluateSupplies(const BotSupplyPolicy &policy, uint64_t expectedInventorySignature) { if (state != BotSessionState::Placed || !controller) return { .urgency=BotSupplyUrgency::Critical,.intent=BotSupplyIntent::ObservationStale,.failure=BotSupplyFailure::InvalidLifecycle }; return controller->evaluateSupplies(policy, expectedInventorySignature); }
BotAdventureProgress BotSession::advanceAdventure(const BotAdventureObservation &observation, std::chrono::milliseconds now, const BotAdventurePolicy &policy) { if(state!=BotSessionState::Placed||!controller)return{.state=BotAdventureState::Failed,.failure=BotAdventureFailure::InvalidLifecycle};return controller->advanceAdventure(observation,now,policy); }
BotEquipmentObservation BotSession::evaluateEquipment(const BotEquipmentPolicy &policy) { if (state != BotSessionState::Placed || !controller) return {}; return controller->evaluateEquipment(policy); }
BotShopObservation BotSession::observeShop(uint32_t npcId, uint16_t maximumOffers) { if (state != BotSessionState::Placed || !controller) return {}; return controller->observeShop(npcId, maximumOffers); }
BotShopTransactionResult BotSession::executeShop(const BotShopTransactionRequest &request, std::chrono::milliseconds now, const BotShopPolicy &policy) { if (state != BotSessionState::Placed || !controller) return { .outcome=BotShopOutcome::Cancelled,.failure=BotShopFailure::InvalidLifecycle,.state=BotShopState::Failed,.request=request }; return controller->executeShop(request, now, policy); }
BotShopTransactionResult BotSession::cancelShop() { if (!controller) return { .outcome=BotShopOutcome::Cancelled,.failure=BotShopFailure::InvalidLifecycle,.state=BotShopState::Failed }; return controller->cancelShop(); }
BotDepotObservation BotSession::observeDepot(uint32_t id,const BotResupplyPolicy &p){if(state!=BotSessionState::Placed||!controller)return{};return controller->observeDepot(id,p);}
BotResupplyResult BotSession::executeResupply(const BotResupplyRequest&r,std::chrono::milliseconds n,const BotResupplyPolicy&p){if(state!=BotSessionState::Placed||!controller)return{.outcome=BotResupplyOutcome::Cancelled,.state=BotResupplyState::Failed,.request=r};return controller->executeResupply(r,n,p);}
BotEquipmentExecutionResult BotSession::executeEquipment(const BotEquipmentExecutionRequest&r,std::chrono::milliseconds n,const BotEquipmentPolicy&p){if(state!=BotSessionState::Placed||!controller)return{.outcome=BotResupplyOutcome::Cancelled,.failure=BotEquipmentFailure::InvalidLifecycle,.state=BotResupplyState::Failed,.request=r};return controller->executeEquipment(r,n,p);}
BotResupplyResult BotSession::cancelResupply(){if(!controller)return{.outcome=BotResupplyOutcome::Cancelled,.state=BotResupplyState::Failed};return controller->cancelResupply();}
BotDialogueObservation BotSession::observeDialogue(const BotNpcDialoguePolicy&p){if(state!=BotSessionState::Placed||!controller)return{};return controller->observeDialogue(p);}
BotConversationResult BotSession::advanceDialogue(uint32_t id,BotDialogueIntent i,std::chrono::milliseconds n,const BotNpcDialoguePolicy&p){if(state!=BotSessionState::Placed||!controller)return{.state=BotDialogueState::Failed,.failure=BotDialogueFailure::InvalidLifecycle,.npcId=id,.intent=i};return controller->advanceDialogue(id,i,n,p);}
BotConversationResult BotSession::cancelDialogue(){if(!controller)return{.state=BotDialogueState::Cancelled,.response=BotDialogueResponse::Cancelled,.failure=BotDialogueFailure::InvalidLifecycle};return controller->cancelDialogue();}
BotQuestObservation BotSession::observeQuest(const BotQuestDefinition&d,std::vector<BotQuestEvidence>e,const BotQuestBounds&b){if(state!=BotSessionState::Placed||!controller)return{};return controller->observeQuest(d,std::move(e),b);}
BotQuestEligibility BotSession::evaluateQuest(BotMissionId id,const BotQuestDefinition&d,const BotNpcDialoguePolicy&p,const BotQuestBounds&b){if(state!=BotSessionState::Placed||!controller)return{.result=BotQuestFailure::InvalidLifecycle};return controller->evaluateQuest(id,d,p,b);}
BotQuestExecutionResult BotSession::startQuestExecution(const BotQuestPlan&p,const BotQuestExecutionPolicy&policy){if(state!=BotSessionState::Placed||!controller)return{.state=BotQuestExecutionState::Failed,.failure=BotQuestExecutionFailure::InvalidLifecycle};return controller->startQuestExecution(p,policy);}
BotQuestExecutionResult BotSession::advanceQuestExecution(const BotQuestPlan&p,const BotQuestStepObservation&o,const BotQuestExecutionPolicy&policy){if(state!=BotSessionState::Placed||!controller)return{.state=BotQuestExecutionState::Failed,.failure=BotQuestExecutionFailure::InvalidLifecycle};return controller->advanceQuestExecution(p,o,policy);}

bool BotSession::save() const {
	if (!player || state == BotSessionState::Created || state == BotSessionState::PendingSave || state == BotSessionState::Closed) {
		return false;
	}

	const Position previousLoginPosition = player->loginPosition;
	player->loginPosition = player->isDead() ? player->getTemplePosition() : player->getPosition();
	try {
		if (operations.save(player)) {
			return true;
		}
	} catch (const std::exception &exception) {
		g_logger().error("[BotSession::save] Failed to save bot '{}': {}", player->getName(), exception.what());
	} catch (...) {
		g_logger().error("[BotSession::save] Failed to save bot '{}' with an unknown exception", player->getName());
	}
	player->loginPosition = previousLoginPosition;
	return false;
}

bool BotSession::close(bool savePlayer) {
	if (state == BotSessionState::Closed) {
		return true;
	}
	if (state == BotSessionState::PendingSave) {
		return savePlayer && retryPendingSave();
	}

	if (player) {
		if (!isCompletelyAbsentFromWorld()) {
			const auto removalResult = operations.remove(player, savePlayer, operations.save);
			if (removalResult == ManagedPlayerRemovalResult::RemovalFailed) {
				return false;
			}
			if (removalResult == ManagedPlayerRemovalResult::RemovedPendingSave) {
				state = BotSessionState::PendingSave;
				return false;
			}
		}
		if (!isCompletelyAbsentFromWorld()) {
			return false;
		}
	}

	finishClose();
	return true;
}

bool BotSession::retryPendingSave() {
	if (!player || state != BotSessionState::PendingSave) {
		return false;
	}
	player->setOnline(true);
	try {
		if (!operations.save(player)) {
			player->setOnline(false);
			return false;
		}
	} catch (const std::exception &exception) {
		player->setOnline(false);
		g_logger().error("[BotSession::retryPendingSave] Failed to save removed bot '{}': {}", player->getName(), exception.what());
		return false;
	} catch (...) {
		player->setOnline(false);
		g_logger().error("[BotSession::retryPendingSave] Failed to save removed bot '{}' with an unknown exception", player->getName());
		return false;
	}
	player->setOnline(false);
	finishClose();
	return true;
}

void BotSession::finishClose() {
	if (controller) { (void)controller->cancelRoute(); (void)controller->cancelTransition(); controller->cancelCombat(); controller->cancelSurvival(); (void)controller->cancelLoot(); (void)controller->cancelAdventure(); }
	controller.reset();
	player.reset();
	state = BotSessionState::Closed;
}

bool BotSession::isCompletelyAbsentFromWorld() const {
	if (!player) {
		return true;
	}
	const auto tile = player->getTile();
	return player->isRemoved()
		&& game.getPlayerByName(player->getName()) != player
		&& game.getPlayerByGUID(player->getGUID()) != player
		&& (!tile || tile->getThingIndex(player) == -1);
}
