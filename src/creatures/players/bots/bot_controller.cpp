/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_controller.hpp"

#include "creatures/players/bots/bot_navigation.hpp"
#include "creatures/players/player.hpp"
#include "creatures/combat/combat.hpp"
#include "creatures/combat/spells.hpp"
#include "game/game.hpp"
#include "lua/creature/actions.hpp"
#include "lib/logging/log_with_spd_log.hpp"

BotController::BotController(Game &game, const std::shared_ptr<Player> &player, BotRuntimeLimits limits, DecisionLog decisionLog) :
	game(game), player(player), limits(limits), decisionLog(std::move(decisionLog)) {
}

namespace {
uint32_t carriedCount(Game &, const std::shared_ptr<Player> &player, uint16_t id) { return player ? std::static_pointer_cast<Cylinder>(player)->getItemTypeCount(id) : 0; }
uint64_t lootItemSignature(uint16_t id, uint32_t count, uint16_t stack) { uint64_t h=1469598103934665603ULL;h=(h^id)*1099511628211ULL;h=(h^count)*1099511628211ULL;return (h^stack)*1099511628211ULL; }
std::shared_ptr<Item> corpseAt(const Position &position) { const auto tile=g_game().map.getTile(position);if(!tile||!tile->getItemList())return nullptr;for(const auto &item:*tile->getItemList())if(item&&item->isCorpse()&&item->getContainer())return item;return nullptr; }
uint32_t corpseItemCount(const std::shared_ptr<Container> &container,uint16_t id){uint64_t total=0;if(container)for(const auto &item:container->getItemList())if(item&&item->getID()==id)total=std::min<uint64_t>(UINT32_MAX,total+std::max<uint16_t>(item->getItemCount(),1));return static_cast<uint32_t>(total);}
std::shared_ptr<Container> carriedContainerAt(const std::shared_ptr<Player> &player, uint8_t rootSlot, const std::vector<uint16_t> &path) { if(!player)return nullptr; auto item=player->getInventoryItem(static_cast<Slots_t>(rootSlot)); auto container=item?item->getContainer():nullptr; for(const auto index:path){if(!container||index>=container->getItemList().size())return nullptr;item=container->getItemList()[index];container=item?item->getContainer():nullptr;}return container; }
std::optional<std::pair<BotSurvivalObservation, BotCombatObservation>> survivalObservation(const std::shared_ptr<Player> &player) {
	const auto base = BotPerception::observe(player); if (!base) return std::nullopt;
	const auto combat = BotCombat::observe(player, *base); if (!combat) return std::nullopt;
	BotSurvivalObservation o { .revision = base->topologyRevision, .position = base->position, .health = base->health, .maxHealth = base->maxHealth, .mana = base->mana, .maxMana = base->maxMana, .healthPercent = BotSurvival::percentage(std::max(base->health, 0), std::max(base->maxHealth, 0)), .manaPercent = BotSurvival::percentage(base->mana, base->maxMana), .harmfulConditions = combat->self.activeConditions, .recentDamage = combat->self.recentDamage.amount, .dead = player->isRemoved() || base->health <= 0 };
	for (const auto &c : combat->creatures) if (c.kind == BotCombatCreatureKind::Monster && c.visibility == BotCombatVisibility::Visible && !c.deadOrRemoved) { ++o.visibleHostiles; o.attacked = o.attacked || c.attackingBot; }
	return std::pair(o, *combat);
}
}

BotSurvivalAssessment BotController::evaluateSurvival(const BotSurvivalPolicy &policy, std::vector<BotHealingOption> options) {
	const auto controlled = player.lock(); const auto observed = survivalObservation(controlled); if (!observed) return { .urgency = BotSurvivalUrgency::Fatal, .decision = BotSurvivalDecision::Dead };
	if (observed->first.dead) { (void)observeDeath(); return { .urgency = BotSurvivalUrgency::Fatal, .decision = BotSurvivalDecision::Dead }; }
	if (survivalProgress.state == BotSurvivalState::Failed || survivalProgress.state == BotSurvivalState::Recovering || survivalProgress.state == BotSurvivalState::Safe || survivalProgress.state == BotSurvivalState::Exhausted) survivalProgress = {};
	const auto base = BotPerception::observe(controlled); const auto flee = base ? BotSurvival::selectFlee(*base, observed->second, policy) : BotFleeResult {};
	auto assessment = BotSurvival::assess(observed->first, policy, std::move(options), flee.outcome == BotFleeOutcome::SafePositionSelected);
	if (assessment.urgency == BotSurvivalUrgency::Critical || assessment.urgency == BotSurvivalUrgency::Fatal) {
		survivalLootBlocked = true;
		(void)BotLootTransfer::requestSurvivalInterrupt(lootProgress, assessment.decision, assessment.decision == BotSurvivalDecision::Dead);
	} else if (assessment.urgency == BotSurvivalUrgency::None || assessment.urgency == BotSurvivalUrgency::Low) {
		survivalLootBlocked = false;
	}
	return assessment;
}

BotHealingResult BotController::executeHealing(const BotHealingOption &option, std::chrono::milliseconds now, const BotSurvivalPolicy &policy) {
	BotHealingResult result; const auto controlled = player.lock(); const auto observed = survivalObservation(controlled);
	if (controlled && (controlled->isRemoved() || controlled->getHealth() <= 0)) { (void)observeDeath(); result.outcome = BotHealingOutcome::Dead; return result; }
	if (!observed) { result.outcome = BotHealingOutcome::InvalidLifecycle; return result; }
	if (observed->first.dead) { (void)observeDeath(); result.outcome = BotHealingOutcome::Dead; return result; }
	if (survivalProgress.state == BotSurvivalState::Failed) { result.outcome = BotHealingOutcome::RetryExhausted; return result; }
	if (survivalProgress.state == BotSurvivalState::Cancelled) { result.outcome = BotHealingOutcome::Cancelled; return result; }
	if (survivalProgress.state == BotSurvivalState::Fleeing || survivalProgress.state == BotSurvivalState::FleePlanning) { result.outcome = BotHealingOutcome::WorldRejected; return result; }
	if (lootProgress.priority == BotLootPriorityState::WaitingForAuthoritativeBoundary) {
		const auto corpse = lootProgress.request ? corpseAt(lootProgress.request->corpsePosition) : nullptr;
		const auto sourceAfter = lootProgress.request ? corpseItemCount(corpse ? corpse->getContainer() : nullptr, lootProgress.request->itemTypeId) : 0;
		const auto destinationAfter = lootProgress.request ? carriedCount(game, controlled, lootProgress.request->itemTypeId) : 0;
		const auto sourceDelta = lootProgress.sourceBefore > sourceAfter ? lootProgress.sourceBefore - sourceAfter : 0;
		const auto destinationDelta = destinationAfter > lootProgress.destinationBefore ? destinationAfter - lootProgress.destinationBefore : 0;
		lootProgress.authoritativeBoundaryMovedCount = std::min(sourceDelta, destinationDelta);
		lootProgress.authoritativeBoundaryOutcome = lootProgress.authoritativeBoundaryMovedCount == 0 ? BotLootTransferOutcome::NoEffect : lootProgress.authoritativeBoundaryMovedCount < lootProgress.request->count ? BotLootTransferOutcome::Partial : BotLootTransferOutcome::Succeeded;
		lootProgress.state = BotLootExecutionState::Cancelled;
		lootProgress.request.reset();
		lootProgress.priority = BotLootPriorityState::HealingPriority;
	}
	if (survivalProgress.healing) {
		result.request = *survivalProgress.healing; result.attempts = survivalProgress.attempts;
		result.observedHealthDelta = observed->first.health - result.request.preHealth; result.observedManaDelta = static_cast<int32_t>(observed->first.mana) - static_cast<int32_t>(result.request.preMana); result.removedConditions = result.request.preConditions & ~observed->first.harmfulConditions;
		if (option.itemTypeId) result.observedItemDelta = static_cast<int32_t>(result.request.preItemCount) - static_cast<int32_t>(carriedCount(game, controlled, option.itemTypeId));
		const bool healingObserved = result.observedHealthDelta > 0
			|| (result.request.option.kind == BotHealingKind::ManaPotion && result.observedManaDelta > 0)
			|| result.removedConditions != 0;
		if (healingObserved) { result.outcome = BotHealingOutcome::Succeeded; survivalProgress = { .state = BotSurvivalState::Recovering }; return result; }
		if (now - survivalProgress.startedAt >= policy.timeout) { result.outcome = BotHealingOutcome::NoEffect; survivalProgress.state = BotSurvivalState::Failed; survivalProgress.healing.reset(); return result; }
		result.outcome = BotHealingOutcome::Pending; return result;
	}
	if (option.kind == BotHealingKind::None || !option.requirementsMet) { result.outcome = BotHealingOutcome::RequirementNotMet; return result; }
	if (option.cooldownActive) { survivalProgress.state = BotSurvivalState::Exhausted; result.outcome = BotHealingOutcome::CooldownActive; return result; }
	uint32_t authoritativeManaCost = option.manaCost;
	if (!option.spell.empty()) {
		const auto spell = g_spells().getInstantSpell(option.spell);
		if (!spell) { result.outcome = BotHealingOutcome::RequirementNotMet; return result; }
		authoritativeManaCost = spell->getManaCost(controlled);
		if (controlled->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, spell->getGroup()) || controlled->hasCondition(CONDITION_SPELLCOOLDOWN, spell->getSpellId()) || (spell->getSecondaryGroup() != SPELLGROUP_NONE && controlled->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, spell->getSecondaryGroup()))) { survivalProgress.state = BotSurvivalState::Exhausted; result.outcome = BotHealingOutcome::CooldownActive; return result; }
		if (controlled->getLevel() < spell->getLevel() || controlled->getMagicLevel() < spell->getMagicLevel() || !spell->canCast(controlled)) { result.outcome = BotHealingOutcome::RequirementNotMet; return result; }
	}
	if (authoritativeManaCost > observed->first.mana) { result.outcome = BotHealingOutcome::MissingMana; return result; }
	if (option.kind == BotHealingKind::ConditionRemoval && !(option.removesConditions & observed->first.harmfulConditions)) { result.outcome = BotHealingOutcome::ConditionNotPresent; return result; }
	if (option.itemTypeId && carriedCount(game, controlled, option.itemTypeId) == 0) { result.outcome = BotHealingOutcome::MissingItem; return result; }
	auto authoritativeOption = option; authoritativeOption.manaCost = authoritativeManaCost;
	result.request = { authoritativeOption, observed->first.revision, observed->first.health, observed->first.mana, observed->first.harmfulConditions, option.itemTypeId ? carriedCount(game, controlled, option.itemTypeId) : 0 };
	survivalProgress.state = BotSurvivalState::HealingRequired;
	survivalProgress = { .state = BotSurvivalState::HealingPending, .healing = result.request, .attempts = 1, .startedAt = now, .nextActionAt = now + policy.initialBackoff };
	cancelCombat(); (void)cancelRoute();
	if (!option.spell.empty()) game.playerSay(controlled->getID(), 0, TALKTYPE_SAY, "", option.spell);
	else if (option.itemTypeId) { const auto item = game.findItemOfType(controlled, option.itemTypeId, true); if (!item || !item->getParent()) { survivalProgress = {}; result.outcome = BotHealingOutcome::MissingItem; return result; } if (Item::items[item->getID()].triggerExhaustion() && !controlled->canDoPotionAction()) { survivalProgress = { .state = BotSurvivalState::Exhausted }; result.outcome = BotHealingOutcome::Exhausted; return result; } const auto from = item->getPosition(); const auto to = controlled->getPosition(); if (!g_actions().useItemEx(controlled, from, to, 0, item, false, controlled)) { survivalProgress.state = BotSurvivalState::Failed; survivalProgress.healing.reset(); result.outcome = BotHealingOutcome::WorldRejected; return result; } }
	result.outcome = BotHealingOutcome::Pending; result.attempts = 1; return result;
}

BotFleeResult BotController::executeFlee(std::chrono::milliseconds now, const BotSurvivalPolicy &policy) {
	const auto controlled = player.lock();
	if (controlled && (controlled->isRemoved() || controlled->getHealth() <= 0)) { (void)observeDeath(); return { .outcome = BotFleeOutcome::Dead }; }
	const auto observed = survivalObservation(controlled); if (!observed) return { .outcome = BotFleeOutcome::Cancelled };
	if (observed->first.dead) { (void)observeDeath(); return { .outcome = BotFleeOutcome::Dead }; }
	if (survivalProgress.state == BotSurvivalState::Failed) return { .outcome = BotFleeOutcome::RetryExhausted };
	if (survivalProgress.state == BotSurvivalState::Cancelled) return { .outcome = BotFleeOutcome::Cancelled };
	if (survivalProgress.healing) return { .outcome = BotFleeOutcome::Blocked };
	if (lootProgress.priority == BotLootPriorityState::WaitingForAuthoritativeBoundary) {
		const auto corpse = lootProgress.request ? corpseAt(lootProgress.request->corpsePosition) : nullptr;
		const auto sourceAfter = lootProgress.request ? corpseItemCount(corpse ? corpse->getContainer() : nullptr, lootProgress.request->itemTypeId) : 0;
		const auto destinationAfter = lootProgress.request ? carriedCount(game, controlled, lootProgress.request->itemTypeId) : 0;
		const auto sourceDelta = lootProgress.sourceBefore > sourceAfter ? lootProgress.sourceBefore - sourceAfter : 0;
		const auto destinationDelta = destinationAfter > lootProgress.destinationBefore ? destinationAfter - lootProgress.destinationBefore : 0;
		lootProgress.authoritativeBoundaryMovedCount = std::min(sourceDelta, destinationDelta);
		lootProgress.authoritativeBoundaryOutcome = lootProgress.authoritativeBoundaryMovedCount == 0 ? BotLootTransferOutcome::NoEffect : lootProgress.authoritativeBoundaryMovedCount < lootProgress.request->count ? BotLootTransferOutcome::Partial : BotLootTransferOutcome::Succeeded;
		lootProgress.state = BotLootExecutionState::Cancelled;
		lootProgress.request.reset();
		lootProgress.priority = BotLootPriorityState::FleePriority;
	}
	if (survivalProgress.state == BotSurvivalState::Fleeing) { if (now - survivalProgress.startedAt >= policy.timeout) { (void)cancelRoute(); survivalProgress.state=BotSurvivalState::Failed; return { .outcome=BotFleeOutcome::TimedOut,.request=*survivalProgress.flee,.attempts=survivalProgress.attempts,.noProgress=survivalProgress.noProgress }; } auto progress = advanceRoute(now); const bool active = progress.state == BotRouteState::Ready || progress.state == BotRouteState::StepPending || progress.state == BotRouteState::ReplanRequired || progress.state == BotRouteState::Backoff; BotFleeResult r { .outcome = progress.state == BotRouteState::Arrived ? BotFleeOutcome::Safe : active ? BotFleeOutcome::Progressing : progress.reason == BotRouteReason::NoProgress ? BotFleeOutcome::NoProgress : BotFleeOutcome::Blocked, .request = *survivalProgress.flee, .attempts = survivalProgress.attempts, .noProgress = static_cast<uint8_t>(progress.consecutiveNoProgress) }; if (r.outcome == BotFleeOutcome::Safe) { const bool nearby=std::ranges::any_of(observed->second.creatures,[&](const auto &c){ return c.kind==BotCombatCreatureKind::Monster && c.visibility==BotCombatVisibility::Visible && !c.deadOrRemoved && std::max(Position::getDistanceX(controlled->getPosition(),c.position),Position::getDistanceY(controlled->getPosition(),c.position))<=2; }); if (nearby) { r.outcome=BotFleeOutcome::ThreatStillPresent; survivalProgress.state=BotSurvivalState::Failed; } else survivalProgress = { .state = BotSurvivalState::Safe }; } return r; }
	survivalProgress.state = BotSurvivalState::FleeRequired;
	const auto base = BotPerception::observe(controlled); if (!base) return { .outcome = BotFleeOutcome::Cancelled }; survivalProgress.state = BotSurvivalState::FleePlanning; auto result = BotSurvival::selectFlee(*base, observed->second, policy); if (result.outcome != BotFleeOutcome::SafePositionSelected) { survivalProgress.state = BotSurvivalState::Failed; return result; }
	game.playerCancelAttackAndFollow(controlled->getID()); cancelCombat(); (void)cancelRoute(); (void)startRoute(result.request.destination, now, result.request.limits); survivalProgress = { .state = BotSurvivalState::Fleeing, .flee = result.request, .attempts = 1, .startedAt = now }; result.outcome = BotFleeOutcome::FleeStarted; return result;
}

BotDeathResult BotController::observeDeath() {
	BotDeathResult result; const auto controlled = player.lock(); if (!controlled) { result.state = BotSurvivalState::Dead; return result; }
	result.observation = { 0, controlled->getPosition(), controlled->getHealth(), controlled->isRemoved() || controlled->getHealth() <= 0 };
	if (!result.observation.authoritativeDead) { result.state = survivalProgress.state; return result; }
	survivalProgress.state = BotSurvivalState::DeathDetected; cancelCombat(); result.combatCancelled = true; (void)cancelRoute(); (void)cancelTransition(); (void)BotLootTransfer::requestSurvivalInterrupt(lootProgress, BotSurvivalDecision::Dead, true); adventureProgress = BotAdventure::advance(adventureProgress, { .revision=std::max<uint64_t>(adventureProgress.lastObservationRevision+1,1),.position=controlled->getPosition(),.dead=true }, std::chrono::milliseconds(0)); result.movementCancelled = true; survivalProgress = { .state = BotSurvivalState::Dead, .death = result.observation }; result.state = BotSurvivalState::Dead; return result;
}

void BotController::cancelSurvival() { if (survivalProgress.state != BotSurvivalState::Dead) survivalProgress = { .state = BotSurvivalState::Cancelled }; }

BotLootSelectionResult BotController::evaluateLoot(const Position &position, uint32_t sourceCreatureId, BotCorpseSignature expectedSignature, const BotLootPolicy &policy) {
	const auto controlled = player.lock();
	const auto observation = BotPerception::observe(controlled);
	if (!observation) return { .eligibility = BotLootEligibility::InvalidLifecycle, .failure = BotLootFailure::InvalidLifecycle };
	auto result = BotLoot::observe(controlled, *observation, position, sourceCreatureId, expectedSignature, policy);
	if (!survivalLootBlocked && (lootProgress.freshCorpseRequired || lootProgress.freshInventoryRequired)) {
		const auto inventory = BotLootTransfer::observeInventory(controlled);
		const bool eligible = result.eligibility == BotLootEligibility::Eligible;
		(void)BotLootTransfer::observeFresh(lootProgress, result.corpse.observationRevision, inventory.revision, eligible);
	}
	return result;
}

BotLootTransferResult BotController::executeLoot(const BotLootTransferRequest &request, std::chrono::milliseconds now, const BotLootPolicy &lootPolicy, const BotLootTransferPolicy &policy) {
	BotLootTransferResult result { .request = request };
	const auto controlled = player.lock();
	if (!controlled || controlled->isRemoved() || controlled->getHealth() <= 0) { result.failure=BotLootTransferFailure::InvalidLifecycle;result.state=BotLootExecutionState::Failed;return result; }
	if (survivalLootBlocked || !BotLootTransfer::mayDispatch(lootProgress)) { result.outcome=BotLootTransferOutcome::Cancelled;result.failure=BotLootTransferFailure::Cancelled;result.state=lootProgress.state;return result; }
	if (lootProgress.state == BotLootExecutionState::Cancelled) { result.outcome=BotLootTransferOutcome::Cancelled;result.failure=BotLootTransferFailure::Cancelled;result.state=lootProgress.state;return result; }
	if (lootProgress.state == BotLootExecutionState::Completed) { result.outcome=BotLootTransferOutcome::Succeeded;result.state=lootProgress.state;result.attempts=lootProgress.attempts;return result; }
	if (lootProgress.state == BotLootExecutionState::Failed && lootProgress.request && *lootProgress.request==request) { result.outcome=BotLootTransferOutcome::RetryExhausted;result.failure=BotLootTransferFailure::RetryExhausted;result.state=lootProgress.state;result.attempts=lootProgress.attempts;return result; }
	if (lootProgress.request && *lootProgress.request != request && lootProgress.state != BotLootExecutionState::Completed && lootProgress.state != BotLootExecutionState::Failed) { result.outcome=BotLootTransferOutcome::Pending;result.state=lootProgress.state;result.attempts=lootProgress.attempts;return result; }
	if (lootProgress.state==BotLootExecutionState::ApproachingCorpse&&lootProgress.request&&*lootProgress.request==request) { const auto routeState=advanceRoute(now);if(routeState.state!=BotRouteState::Arrived){result.outcome=BotLootTransferOutcome::Pending;result.state=lootProgress.state;result.attempts=lootProgress.attempts;return result;}lootProgress.state=BotLootExecutionState::OpeningCorpse; }
	auto verify = [&]() -> std::optional<BotLootTransferResult> {
		if (lootProgress.state != BotLootExecutionState::TransferPending && lootProgress.state != BotLootExecutionState::VerifyingTransfer) return std::nullopt;
		lootProgress.state=BotLootExecutionState::VerifyingTransfer;
		const auto corpse=corpseAt(request.corpsePosition);const auto container=corpse?corpse->getContainer():nullptr;
		const uint32_t sourceAfter=corpseItemCount(container,request.itemTypeId);const uint32_t destinationAfter=carriedCount(game,controlled,request.itemTypeId);
		BotLootTransferResult checked { .request=request,.sourceBefore=lootProgress.sourceBefore,.sourceAfter=sourceAfter,.destinationBefore=lootProgress.destinationBefore,.destinationAfter=destinationAfter,.attempts=lootProgress.attempts,.ordinaryOpenAccepted=true,.ordinaryMoveDispatched=true,.mergedStack=lootProgress.chosenDestination.outcome==BotDestinationOutcome::SelectedMerge,.createdStack=lootProgress.chosenDestination.outcome==BotDestinationOutcome::SelectedFreeSlot };
		const uint32_t sourceDelta=lootProgress.sourceBefore>sourceAfter?lootProgress.sourceBefore-sourceAfter:0;const uint32_t destinationDelta=destinationAfter>lootProgress.destinationBefore?destinationAfter-lootProgress.destinationBefore:0;checked.movedCount=std::min(sourceDelta,destinationDelta);
		if (!corpse || !container) { lootProgress.state=BotLootExecutionState::Failed;checked.state=lootProgress.state;checked.outcome=BotLootTransferOutcome::CorpseExpired;checked.failure=BotLootTransferFailure::CorpseExpired;return checked; }
		if (checked.movedCount>0) { lootProgress.state=BotLootExecutionState::Completed;checked.state=lootProgress.state;checked.outcome=checked.movedCount<request.count?BotLootTransferOutcome::Partial:BotLootTransferOutcome::Succeeded;return checked; }
		if (now-lootProgress.startedAt>=policy.timeout) { lootProgress.state=BotLootExecutionState::Failed;checked.state=lootProgress.state;checked.outcome=BotLootTransferOutcome::NoEffect;checked.failure=BotLootTransferFailure::WorldRejected;return checked; }
		checked.state=lootProgress.state;checked.outcome=BotLootTransferOutcome::Pending;return checked;
	};
	if (auto checked=verify()) return *checked;
	const auto eligibility=evaluateLoot(request.corpsePosition,request.sourceCreatureId,request.corpseSignature,lootPolicy);
	if (eligibility.eligibility!=BotLootEligibility::Eligible) { result.state=BotLootExecutionState::Failed;lootProgress={.state=BotLootExecutionState::Failed};switch(eligibility.eligibility){case BotLootEligibility::NoLootRights:result.outcome=BotLootTransferOutcome::NoLootRights;result.failure=BotLootTransferFailure::NoLootRights;break;case BotLootEligibility::StaleObservation:result.outcome=BotLootTransferOutcome::StaleCorpse;result.failure=BotLootTransferFailure::StaleCorpse;break;case BotLootEligibility::CorpseExpired:result.outcome=BotLootTransferOutcome::CorpseExpired;result.failure=BotLootTransferFailure::CorpseExpired;break;case BotLootEligibility::CapacityInsufficient:result.outcome=BotLootTransferOutcome::CapacityInsufficient;result.failure=BotLootTransferFailure::CapacityInsufficient;break;default:result.outcome=BotLootTransferOutcome::WorldRejected;result.failure=BotLootTransferFailure::WorldRejected;}return result; }
	if (!eligibility.selected || request.count==0 || eligibility.selected->item.itemTypeId!=request.itemTypeId || (request.itemSignature&&eligibility.selected->item.signature!=request.itemSignature)) { lootProgress={.state=BotLootExecutionState::Failed};result.state=lootProgress.state;result.outcome=BotLootTransferOutcome::StaleItem;result.failure=BotLootTransferFailure::StaleItem;return result; }
	const auto inventory=BotLootTransfer::observeInventory(controlled,policy.maxInventoryContainers,policy.maxInventoryDepth);if(request.destinationSignature&&request.destinationSignature!=inventory.signature){lootProgress={.state=BotLootExecutionState::Failed};result.state=lootProgress.state;result.outcome=BotLootTransferOutcome::StaleDestination;result.failure=BotLootTransferFailure::StaleDestination;return result;}const auto destination=BotLootTransfer::selectDestination(inventory,request.itemTypeId,request.count);if(!destination.selected()){lootProgress={.state=BotLootExecutionState::CapacityBlocked};result.state=lootProgress.state;if(destination.outcome==BotDestinationOutcome::DestinationBudgetExceeded){result.outcome=BotLootTransferOutcome::DestinationBudgetExceeded;result.failure=BotLootTransferFailure::DestinationBudgetExceeded;}else if(destination.outcome==BotDestinationOutcome::ItemIncompatible){result.outcome=BotLootTransferOutcome::ItemIncompatible;result.failure=BotLootTransferFailure::ItemIncompatible;}else{result.outcome=BotLootTransferOutcome::AllDestinationsFull;result.failure=BotLootTransferFailure::AllDestinationsFull;}return result;}const auto destinationContainer=carriedContainerAt(controlled,destination.rootSlot,destination.childIndices);if(!destinationContainer){lootProgress={.state=BotLootExecutionState::Failed};result.state=lootProgress.state;result.outcome=BotLootTransferOutcome::StaleDestination;result.failure=BotLootTransferFailure::StaleDestination;return result;}
	if (std::max(Position::getDistanceX(controlled->getPosition(),request.corpsePosition),Position::getDistanceY(controlled->getPosition(),request.corpsePosition))>1) { lootProgress={.state=BotLootExecutionState::ApproachingCorpse,.request=request,.attempts=1,.startedAt=now};(void)startRoute(request.corpsePosition,now,{.maxExpandedNodes=128,.maxRouteLength=lootPolicy.maxDistance,.maxPlanningOperations=2048});result.state=lootProgress.state;result.outcome=BotLootTransferOutcome::Pending;return result; }
	const auto corpse=corpseAt(request.corpsePosition);if(!corpse||!corpse->getContainer()){result.outcome=BotLootTransferOutcome::CorpseExpired;result.failure=BotLootTransferFailure::CorpseExpired;result.state=BotLootExecutionState::Failed;return result;}
	lootProgress={.state=BotLootExecutionState::OpeningCorpse,.request=request,.attempts=1,.startedAt=now,.priority=BotLootPriorityState::LootActive,.corpseObservationRevision=eligibility.corpse.observationRevision,.inventoryObservationRevision=inventory.revision};
	const bool alreadyOpen=controlled->getContainerID(corpse->getContainer())>=0;
	if(!alreadyOpen&&!g_actions().useItem(controlled,request.corpsePosition,0,corpse,false)){lootProgress.state=BotLootExecutionState::Failed;result.outcome=BotLootTransferOutcome::WorldRejected;result.failure=BotLootTransferFailure::WorldRejected;result.state=lootProgress.state;return result;}
	result.ordinaryOpenAccepted=true;
	if(controlled->getContainerID(corpse->getContainer())<0){result.outcome=BotLootTransferOutcome::Pending;result.state=lootProgress.state;return result;}
	lootProgress.state=BotLootExecutionState::ObservingContents;
	std::shared_ptr<Item> selected;uint16_t selectedStack=0;for(size_t i=0;i<corpse->getContainer()->getItemList().size();++i){const auto &item=corpse->getContainer()->getItemList()[i];if(item&&item->getID()==request.itemTypeId&&(!request.itemSignature||lootItemSignature(item->getID(),std::max<uint16_t>(item->getItemCount(),1),static_cast<uint16_t>(i))==request.itemSignature)){selected=item;selectedStack=static_cast<uint16_t>(i);break;}}
	if(!selected){lootProgress.state=BotLootExecutionState::Failed;result.outcome=BotLootTransferOutcome::ItemGone;result.failure=BotLootTransferFailure::ItemGone;result.state=lootProgress.state;return result;}
	lootProgress.state=BotLootExecutionState::SelectingItem;const uint32_t available=std::max<uint16_t>(selected->getItemCount(),1);const uint32_t wanted=std::min(request.count,available);const uint32_t unitWeight=available?selected->getWeight()/available:selected->getWeight();const auto capacity=BotLootTransfer::assessCapacity(controlled->getFreeCapacity(),unitWeight,wanted);if(capacity.movableCount==0){lootProgress.state=BotLootExecutionState::CapacityBlocked;result.outcome=BotLootTransferOutcome::CapacityInsufficient;result.failure=BotLootTransferFailure::CapacityInsufficient;result.state=lootProgress.state;return result;}uint32_t maxDestinationCount=0;const auto destinationQuery=destinationContainer->queryMaxCount(INDEX_WHEREEVER,selected,capacity.movableCount,maxDestinationCount,0);if(destinationQuery!=RETURNVALUE_NOERROR&&maxDestinationCount==0){lootProgress.state=BotLootExecutionState::CapacityBlocked;result.outcome=BotLootTransferOutcome::DestinationFull;result.failure=BotLootTransferFailure::DestinationFull;result.state=lootProgress.state;return result;}const uint32_t dispatchCount=std::min(capacity.movableCount,maxDestinationCount);
	lootProgress.state=BotLootExecutionState::TransferPending;lootProgress.sourceBefore=corpseItemCount(corpse->getContainer(),request.itemTypeId);lootProgress.destinationBefore=carriedCount(game,controlled,request.itemTypeId);lootProgress.chosenDestination=destination;const auto moveResult=game.internalMoveItem(corpse->getContainer(),destinationContainer,INDEX_WHEREEVER,selected,dispatchCount,nullptr,0,controlled);result.ordinaryMoveDispatched=moveResult==RETURNVALUE_NOERROR;result.mergedStack=destination.outcome==BotDestinationOutcome::SelectedMerge;result.createdStack=destination.outcome==BotDestinationOutcome::SelectedFreeSlot;if(!result.ordinaryMoveDispatched){lootProgress.state=BotLootExecutionState::Failed;result.state=lootProgress.state;result.outcome=BotLootTransferOutcome::WorldRejected;result.failure=BotLootTransferFailure::WorldRejected;return result;}
	result.outcome=BotLootTransferOutcome::Pending;result.state=lootProgress.state;return result;
}

BotLootExecutionProgress BotController::cancelLoot() { if(lootProgress.state!=BotLootExecutionState::Completed)lootProgress={.state=BotLootExecutionState::Cancelled};return lootProgress; }

BotSupplyAssessment BotController::evaluateSupplies(const BotSupplyPolicy &policy, uint64_t expectedInventorySignature) {
	const auto controlled = player.lock();
	if (!controlled || controlled->isRemoved() || controlled->getHealth() <= 0) {
		return { .urgency = BotSupplyUrgency::Critical, .intent = BotSupplyIntent::ObservationStale, .failure = BotSupplyFailure::InvalidLifecycle };
	}
	return BotSupply::assess(BotSupply::observe(controlled, policy), policy, expectedInventorySignature);
}

BotAdventureProgress BotController::advanceAdventure(const BotAdventureObservation &observation, std::chrono::milliseconds now, const BotAdventurePolicy &policy) {
	const auto controlled = player.lock();
	if (!controlled || controlled->isRemoved() || controlled->getHealth() <= 0) {
		adventureProgress = BotAdventure::advance(adventureProgress, { .revision=std::max<uint64_t>(observation.revision,1),.dead=true }, now, policy);
		return adventureProgress;
	}
	adventureProgress = BotAdventure::advance(adventureProgress, observation, now, policy);
	return adventureProgress;
}

BotAdventureProgress BotController::cancelAdventure() { adventureProgress = BotAdventure::cancel(adventureProgress); return adventureProgress; }

BotActionResult BotController::tick(std::chrono::milliseconds now) {
	lastTickWork = 0;
	if (now < nextTickAt) {
		return { BotActionStatus::Rejected, BotActionFailure::RateLimited };
	}
	nextTickAt = now + limits.tickInterval;
	const auto controlledPlayer = player.lock();
	if (controlledPlayer && (controlledPlayer->isRemoved() || controlledPlayer->getHealth() <= 0)) { (void)observeDeath(); return { BotActionStatus::Rejected, BotActionFailure::InvalidLifecycle }; }
	const auto observation = BotPerception::observe(controlledPlayer);
	if (!observation) {
		return { BotActionStatus::Rejected, BotActionFailure::InvalidLifecycle };
	}
	lastTickWork = std::min(observation->visibleCreatures.size(), limits.maxCreaturesPerTick);
	if (observation->visibleCreatures.size() > limits.maxCreaturesPerTick) {
		return { BotActionStatus::Rejected, BotActionFailure::TickBudgetExceeded };
	}
	const auto action = selectAction(*observation);
	blackboard.state = action.type == BotActionType::InspectCreature ? BotRootState::Observe : BotRootState::Idle;
	auto result = execute(action, now);
	if (now >= nextLogAt) {
		if (decisionLog) {
			decisionLog(*observation, blackboard.state, action, result);
		} else {
			g_logger().debug(
				"[BotDecision] guid={} creature={} state={} action={} reason={} status={} failure={} attempts={} work={}",
				observation->playerGuid,
				observation->playerCreatureId,
				static_cast<uint8_t>(blackboard.state),
				static_cast<uint8_t>(action.type),
				static_cast<uint8_t>(action.reason),
				static_cast<uint8_t>(result.status),
				static_cast<uint8_t>(result.failure),
				result.attempts,
				lastTickWork
			);
		}
		nextLogAt = now + limits.tickInterval;
	}
	return result;
}

BotTargetSelectionResult BotController::evaluateCombat(const BotCombatPolicy &policy) {
	const auto controlledPlayer = player.lock();
	const auto base = BotPerception::observe(controlledPlayer);
	if (!base) return { .failure = BotCombatFailure::InvalidLifecycle, .reason = BotCombatEligibility::InvalidLifecycle };
	const auto observation = BotCombat::observe(controlledPlayer, *base);
	if (!observation) return { .failure = BotCombatFailure::InvalidLifecycle, .reason = BotCombatEligibility::InvalidLifecycle };
	return BotCombat::select(*observation, policy, combatLock);
}

void BotController::cancelCombat() {
	if (const auto controlledPlayer = player.lock(); controlledPlayer && controlledPlayer->getAttackedCreature()) game.playerSetAttackedCreature(controlledPlayer->getID(), 0);
	combatLock.cancel();
	attackExecution.cancel();
	(void)cancelRoute();
}

BotCombatExecutionResult BotController::executeCombat(const BotCombatExecutionRequest &request, std::chrono::milliseconds now, const BotCombatExecutionPolicy &policy) {
	auto fail = [&](BotCombatExecutionOutcome outcome, BotAttackFailure failure, BotAttackState state, uint16_t world = 0) {
		attackExecution.state = state;
		return BotCombatExecutionResult { outcome, failure, state, BotRangeAssessment::UnknownWeaponRange, request.targetCreatureId, world, attackExecution.assignmentAttempts };
	};
	const auto controlledPlayer = player.lock();
	if (controlledPlayer && (controlledPlayer->isRemoved() || controlledPlayer->getHealth() <= 0)) { (void)observeDeath(); return fail(BotCombatExecutionOutcome::TargetLost, BotAttackFailure::InvalidTarget, BotAttackState::TargetLost); }
	if (!controlledPlayer || !controlledPlayer->isBotControlled() || !controlledPlayer->getTile()) return fail(BotCombatExecutionOutcome::InvalidLifecycle, BotAttackFailure::InvalidLifecycle, BotAttackState::Failed);
	if (request.targetCreatureId == 0 || combatLock.creatureId != request.targetCreatureId) return fail(BotCombatExecutionOutcome::InvalidTarget, BotAttackFailure::InvalidTarget, BotAttackState::Failed);
	const bool live = attackExecution.state == BotAttackState::Validating || attackExecution.state == BotAttackState::AcquiringTarget || attackExecution.state == BotAttackState::AttackPending || attackExecution.state == BotAttackState::Repositioning;
	if (live && attackExecution.request.targetCreatureId != request.targetCreatureId) return { BotCombatExecutionOutcome::Pending, BotAttackFailure::None, attackExecution.state, BotRangeAssessment::UnknownWeaponRange, attackExecution.request.targetCreatureId };
	if (attackExecution.request.targetCreatureId != request.targetCreatureId) {
		attackExecution = { .state = BotAttackState::TargetSelected, .request = request, .timing = { .timeoutDeadline = now + policy.timeout }, .lastObservedPosition = controlledPlayer->getPosition() };
	}
	if (now > attackExecution.timing.timeoutDeadline) { game.playerSetAttackedCreature(controlledPlayer->getID(), 0); return fail(BotCombatExecutionOutcome::TimedOut, BotAttackFailure::TimedOut, BotAttackState::Failed); }
	if (now < attackExecution.timing.nextPermittedReassessment) return { BotCombatExecutionOutcome::CooldownActive, BotAttackFailure::None, BotAttackState::Cooldown, BotRangeAssessment::UnknownWeaponRange, request.targetCreatureId, 0, attackExecution.assignmentAttempts, attackExecution.timing.nextPermittedReassessment - now };

	attackExecution.state = BotAttackState::Validating;
	const auto base = BotPerception::observe(controlledPlayer);
	const auto combat = base ? BotCombat::observe(controlledPlayer, *base) : std::nullopt;
	if (!base || !combat) return fail(BotCombatExecutionOutcome::InvalidLifecycle, BotAttackFailure::InvalidLifecycle, BotAttackState::Failed);
	const auto observed = std::ranges::find(combat->creatures, request.targetCreatureId, &BotCombatCreatureObservation::id);
	if (observed == combat->creatures.end()) { game.playerSetAttackedCreature(controlledPlayer->getID(), 0); return fail(BotCombatExecutionOutcome::TargetLost, BotAttackFailure::NotVisible, BotAttackState::TargetLost); }
	if (request.observationRevision != combat->revision || request.targetSignature != observed->signature) { game.playerSetAttackedCreature(controlledPlayer->getID(), 0); return fail(BotCombatExecutionOutcome::StaleObservation, BotAttackFailure::StaleObservation, BotAttackState::Failed); }
	if (observed->kind != BotCombatCreatureKind::Monster) { game.playerSetAttackedCreature(controlledPlayer->getID(), 0); return fail(BotCombatExecutionOutcome::PolicyRejected, BotAttackFailure::PolicyRejected, BotAttackState::Failed); }
	const auto eligibility = BotCombat::eligible(*combat, *observed, policy.targetPolicy);
	if (eligibility != BotCombatEligibility::Eligible) {
		game.playerSetAttackedCreature(controlledPlayer->getID(), 0);
		if (eligibility == BotCombatEligibility::DifferentFloor) return fail(BotCombatExecutionOutcome::DifferentFloor, BotAttackFailure::DifferentFloor, BotAttackState::TargetLost);
		if (eligibility == BotCombatEligibility::NotVisible) return fail(BotCombatExecutionOutcome::NotVisible, BotAttackFailure::NotVisible, BotAttackState::TargetLost);
		if (eligibility == BotCombatEligibility::ProtectedByZone) return fail(BotCombatExecutionOutcome::ProtectedByZone, BotAttackFailure::ProtectedByZone, BotAttackState::Failed);
		if (eligibility == BotCombatEligibility::Unreachable) return fail(BotCombatExecutionOutcome::Unreachable, BotAttackFailure::Unreachable, BotAttackState::Failed);
		return fail(BotCombatExecutionOutcome::PolicyRejected, BotAttackFailure::PolicyRejected, BotAttackState::Failed);
	}
	const auto target = game.getCreatureByID(request.targetCreatureId);
	if (!target || target->isRemoved() || target->getHealth() <= 0 || !controlledPlayer->canSeeCreature(target)) { game.playerSetAttackedCreature(controlledPlayer->getID(), 0); return fail(BotCombatExecutionOutcome::TargetLost, BotAttackFailure::InvalidTarget, BotAttackState::TargetLost); }
	const ReturnValue authority = Combat::canTargetCreature(controlledPlayer, target);
	if (authority != RETURNVALUE_NOERROR) {
		if (authority == RETURNVALUE_ACTIONNOTPERMITTEDINPROTECTIONZONE || authority == RETURNVALUE_ACTIONNOTPERMITTEDINANOPVPZONE) return fail(BotCombatExecutionOutcome::ProtectedByZone, BotAttackFailure::ProtectedByZone, BotAttackState::Failed, static_cast<uint16_t>(authority));
		if (authority == RETURNVALUE_TURNSECUREMODETOATTACKUNMARKEDPLAYERS) return fail(BotCombatExecutionOutcome::SecureModeRejected, BotAttackFailure::SecureModeRejected, BotAttackState::Failed, static_cast<uint16_t>(authority));
		return fail(BotCombatExecutionOutcome::WorldRejected, BotAttackFailure::WorldRejected, BotAttackState::Failed, static_cast<uint16_t>(authority));
	}
	if (combat->self.weapon == BotWeaponCategory::Distance && !combat->self.ammunitionAvailable) return fail(BotCombatExecutionOutcome::MissingAmmunition, BotAttackFailure::MissingAmmunition, BotAttackState::Failed);
	const bool lineOfSight = game.canThrowObjectTo(controlledPlayer->getPosition(), target->getPosition(), SightLine_CheckSightLineAndFloor, combat->self.attackRange, combat->self.attackRange);
	const auto range = BotCombat::assessRange(*combat, *observed, lineOfSight);
	if (range != BotRangeAssessment::InRange) {
		const auto originDistance = std::max(Position::getDistanceX(request.combatOrigin, controlledPlayer->getPosition()), Position::getDistanceY(request.combatOrigin, controlledPlayer->getPosition()));
		if (originDistance > policy.maxDistanceFromOrigin || attackExecution.repositionAttempts >= policy.maxRepositionAttempts) return fail(BotCombatExecutionOutcome::RetryExhausted, BotAttackFailure::RetryExhausted, BotAttackState::Failed);
		auto positioning = BotCombat::position(*base, *combat, *observed, policy, true);
		if (positioning.outcome != BotCombatExecutionOutcome::RepositionRequired) return { positioning.outcome, positioning.failure, BotAttackState::Failed, positioning.range, request.targetCreatureId, 0, attackExecution.repositionAttempts, {}, positioning };
		if (routeProgress.state == BotRouteState::Idle || routeProgress.destination != positioning.request.destination || attackExecution.lastTargetPosition != observed->position) {
			(void)startRoute(positioning.request.destination, now, policy.routeLimits); ++attackExecution.repositionAttempts;
		}
		attackExecution.lastTargetPosition = observed->position;
		attackExecution.state = BotAttackState::Repositioning;
		const auto before = controlledPlayer->getPosition();
		const auto progress = advanceRoute(now);
		const auto after = controlledPlayer->getPosition();
		if (after != before) attackExecution.noProgressCount = 0; else ++attackExecution.noProgressCount;
		attackExecution.timing.nextPermittedReassessment = now + policy.reassessmentInterval;
		if (progress.state == BotRouteState::Failed || attackExecution.noProgressCount > policy.maxNoProgress) return fail(BotCombatExecutionOutcome::Unreachable, BotAttackFailure::Unreachable, BotAttackState::Failed);
		return { BotCombatExecutionOutcome::RepositionStarted, BotAttackFailure::OutOfRange, BotAttackState::Repositioning, range, request.targetCreatureId, 0, attackExecution.repositionAttempts, policy.reassessmentInterval, positioning };
	}
	attackExecution.state = BotAttackState::InRange;
	if (const auto attacked = controlledPlayer->getAttackedCreature(); attacked && attacked->getID() == request.targetCreatureId) {
		attackExecution.timing.nextPermittedReassessment = now + policy.reassessmentInterval;
		attackExecution.timing.observedNextAttackTime = now + std::chrono::milliseconds(combat->self.attackSpeed);
		return { BotCombatExecutionOutcome::AlreadyTargeting, BotAttackFailure::None, BotAttackState::Cooldown, range, request.targetCreatureId };
	}
	attackExecution.state = BotAttackState::AcquiringTarget;
	attackExecution.timing.lastAssignmentAttempt = now;
	++attackExecution.assignmentAttempts;
	game.playerSetAttackedCreature(controlledPlayer->getID(), request.targetCreatureId);
	const auto assigned = controlledPlayer->getAttackedCreature();
	if (!assigned || assigned->getID() != request.targetCreatureId) {
		if (attackExecution.assignmentAttempts >= policy.maxAssignmentRetries) return fail(BotCombatExecutionOutcome::RetryExhausted, BotAttackFailure::RetryExhausted, BotAttackState::Failed);
		const auto delay = BotCombat::executionBackoff(policy, attackExecution.assignmentAttempts);
		attackExecution.timing.nextPermittedReassessment = now + delay;
		return { BotCombatExecutionOutcome::RetryScheduled, BotAttackFailure::WorldRejected, BotAttackState::AttackPending, range, request.targetCreatureId, static_cast<uint16_t>(authority), attackExecution.assignmentAttempts, delay };
	}
	attackExecution.timing.lastAcceptedAssignment = now;
	attackExecution.timing.nextPermittedReassessment = now + policy.reassessmentInterval;
	attackExecution.timing.observedNextAttackTime = now + std::chrono::milliseconds(combat->self.attackSpeed);
	attackExecution.state = BotAttackState::Cooldown;
	return { BotCombatExecutionOutcome::TargetAcquired, BotAttackFailure::None, BotAttackState::Cooldown, range, request.targetCreatureId, 0, attackExecution.assignmentAttempts };
}

BotActionResult BotController::execute(const BotAction &action, std::chrono::milliseconds now) {
	if (now < blackboard.nextActionAt) {
		return { BotActionStatus::Rejected, BotActionFailure::RateLimited, blackboard.attempts, blackboard.nextActionAt - now };
	}
	if (blackboard.pendingAction) {
		if (now - blackboard.actionStartedAt < limits.actionTimeout) {
			return { BotActionStatus::Pending, BotActionFailure::None, blackboard.attempts };
		}
		if (now - blackboard.actionStartedAt >= limits.actionTimeout) {
			blackboard.lastFailure = BotActionFailure::TimedOut;
			if (blackboard.attempts >= limits.maxAttempts) {
				blackboard.pendingAction.reset();
				blackboard.state = BotRootState::Recover;
				return { BotActionStatus::Rejected, BotActionFailure::TimedOut, blackboard.attempts };
			}
			const auto exponent = std::min<uint32_t>(blackboard.attempts - 1U, 10U);
			const auto backoff = limits.initialBackoff * (1U << exponent);
			blackboard.nextActionAt = now + backoff;
			blackboard.pendingAction.reset();
			return { BotActionStatus::RetryScheduled, BotActionFailure::TimedOut, blackboard.attempts, backoff };
		}
	}

	auto result = perform(action);
	result.attempts = ++blackboard.attempts;
	blackboard.nextActionAt = now + limits.actionInterval;
	blackboard.lastFailure = result.failure;
	if (result.status == BotActionStatus::Pending) {
		blackboard.pendingAction = action;
		blackboard.actionStartedAt = now;
	} else {
		blackboard.pendingAction.reset();
		blackboard.attempts = 0;
	}
	return result;
}

BotAction BotController::selectAction(const BotObservation &observation) const {
	if (!observation.visibleCreatures.empty()) {
		return { BotActionType::InspectCreature, BotActionReason::ObserveTarget, observation.visibleCreatures.front().id };
	}
	return {};
}

BotActionResult BotController::perform(const BotAction &action) const {
	const auto controlledPlayer = player.lock();
	if (!controlledPlayer || !controlledPlayer->isBotControlled() || controlledPlayer->isRemoved() || !controlledPlayer->getTile()) {
		return { BotActionStatus::Rejected, BotActionFailure::InvalidLifecycle };
	}
	switch (action.type) {
		case BotActionType::Wait:
			return { BotActionStatus::Succeeded, BotActionFailure::None };
		case BotActionType::Move:
		{
			auto movement = revalidateAndMove(action);
			BotActionFailure failure = BotActionFailure::None;
			switch (movement.outcome) {
				case BotWalkability::InvalidDirection: failure = BotActionFailure::InvalidDirection; break;
				case BotWalkability::DifferentFloor: failure = BotActionFailure::DifferentFloor; break;
				case BotWalkability::OutsideKnownOrVisibleArea: failure = BotActionFailure::OutsideKnownOrVisibleArea; break;
				case BotWalkability::StaleObservation: failure = BotActionFailure::StaleObservation; break;
				case BotWalkability::WorldRejected: failure = BotActionFailure::WorldRejected; break;
				case BotWalkability::BlockedByTerrain:
				case BotWalkability::BlockedByItem:
				case BotWalkability::BlockedByCreature: failure = BotActionFailure::WorldRejected; break;
				case BotWalkability::Walkable:
				case BotWalkability::WalkableWithRisk: break;
			}
			return { movement.walkable() ? BotActionStatus::Succeeded : BotActionStatus::Rejected, failure, 0, {}, movement };
		}
		case BotActionType::InspectCreature: {
			const auto target = game.getCreatureByID(action.targetCreatureId);
			if (!target || target->isRemoved()
				|| (target != controlledPlayer
					&& (!Position::areInRange<8, 6, 0>(controlledPlayer->getPosition(), target->getPosition())
						|| !controlledPlayer->canSeeCreature(target)))) {
				return { BotActionStatus::Rejected, BotActionFailure::InvalidTarget };
			}
			return { BotActionStatus::Pending, BotActionFailure::None };
		}
	}
	return { BotActionStatus::Rejected, BotActionFailure::WorldRejected };
}

BotWalkabilityResult BotController::assess(Direction direction) const {
	const auto controlledPlayer = player.lock();
	const auto observation = BotPerception::observe(controlledPlayer);
	if (!observation) {
		BotMovementCandidate candidate { .direction = direction };
		return { .candidate = candidate, .outcome = BotWalkability::WorldRejected, .movementCost = BotNavigation::BlockedCost };
	}
	return BotNavigation::assess(*observation, direction);
}

BotActionResult BotController::executeMovement(const BotWalkabilityResult &assessment, std::chrono::milliseconds now) {
	const auto &candidate = assessment.candidate;
	return execute(
		BotAction {
			.type = BotActionType::Move,
			.reason = BotActionReason::Retry,
			.targetCreatureId = 0,
			.direction = candidate.direction,
			.observedOrigin = candidate.origin,
			.observedDestination = candidate.destination,
			.observationSignature = candidate.observationSignature,
		},
		now
	);
}

BotWalkabilityResult BotController::revalidateAndMove(const BotAction &action) const {
	const auto controlledPlayer = player.lock();
	if (!controlledPlayer || controlledPlayer->getPosition() != action.observedOrigin) {
		return { .candidate = { action.observedOrigin, action.observedDestination, action.direction, action.observationSignature }, .outcome = BotWalkability::StaleObservation, .movementCost = BotNavigation::BlockedCost };
	}
	const auto observation = BotPerception::observe(controlledPlayer);
	if (!observation) {
		return { .candidate = { action.observedOrigin, action.observedDestination, action.direction, action.observationSignature }, .outcome = BotWalkability::WorldRejected, .movementCost = BotNavigation::BlockedCost };
	}
	auto current = BotNavigation::assess(*observation, action.observedDestination);
	if (current.candidate.direction != action.direction || current.candidate.observationSignature != action.observationSignature) {
		current.outcome = BotWalkability::StaleObservation;
		current.movementCost = BotNavigation::BlockedCost;
		return current;
	}
	if (!current.walkable()) {
		return current;
	}
	const auto destinationTile = game.map.getTile(action.observedDestination);
	const ReturnValue worldResult = destinationTile
		? game.internalMoveCreature(controlledPlayer, destinationTile)
		: RETURNVALUE_NOTPOSSIBLE;
	current.worldReturnValue = static_cast<uint16_t>(worldResult);
	if (worldResult != RETURNVALUE_NOERROR) {
		current.outcome = BotWalkability::WorldRejected;
		current.movementCost = BotNavigation::BlockedCost;
	}
	return current;
}

ReturnValue BotController::move(Direction direction) const {
	const auto controlledPlayer = player.lock();
	if (!controlledPlayer || !controlledPlayer->isBotControlled() || controlledPlayer->isRemoved() || !controlledPlayer->getTile()) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	return game.internalMoveCreature(controlledPlayer, direction);
}

BotRouteProgress BotController::startRoute(const Position &destination, std::chrono::milliseconds now, BotRouteLimits requestedLimits) {
	route = {};
	routeLimits = requestedLimits;
	routeProgress = { .state = BotRouteState::Planning, .destination = destination, .lastProgressAt = now };
	const auto controlledPlayer = player.lock();
	const auto observation = BotPerception::observe(controlledPlayer);
	if (!observation) {
		routeProgress.state = BotRouteState::Failed; routeProgress.reason = BotRouteReason::InvalidLifecycle; return routeProgress;
	}
	route = BotNavigation::findRoute(*observation, { observation->position, destination, routeLimits });
	routeProgress.actualPosition = observation->position;
	routeProgress.reason = route.reason;
	if (route.reason == BotRouteReason::AlreadyAtDestination) routeProgress.state = BotRouteState::Arrived;
	else if (route.reason == BotRouteReason::RouteFound) routeProgress.state = BotRouteState::Ready;
	else routeProgress.state = BotRouteState::Failed;
	return routeProgress;
}

BotRouteProgress BotController::advanceRoute(std::chrono::milliseconds now) {
	if (routeProgress.state == BotRouteState::Cancelled || routeProgress.state == BotRouteState::Arrived || routeProgress.state == BotRouteState::Failed) return routeProgress;
	if (routeProgress.state == BotRouteState::Backoff && now < routeProgress.backoffDeadline) return routeProgress;
	const auto controlledPlayer = player.lock();
	const auto observation = BotPerception::observe(controlledPlayer);
	if (!observation) { routeProgress.state = BotRouteState::Failed; routeProgress.reason = BotRouteReason::InvalidLifecycle; return routeProgress; }
	routeProgress.actualPosition = observation->position;
	if (observation->position == routeProgress.destination) { routeProgress.state = BotRouteState::Arrived; routeProgress.reason = BotRouteReason::AlreadyAtDestination; return routeProgress; }
	if (routeProgress.state == BotRouteState::Ready && !route.positions.empty()) {
		routeProgress.expectedOrigin = observation->position;
		routeProgress.expectedNext = route.positions.front();
		const auto nextTile = std::ranges::find(observation->visibleTiles, routeProgress.expectedNext, &BotTileObservation::position);
		if (observation->topologyRevision != route.topologyRevision || nextTile == observation->visibleTiles.end() || BotNavigation::signature(*nextTile) != route.signatures.front()) {
			routeProgress.reason = nextTile != observation->visibleTiles.end() && nextTile->blockingCreatureId != 0 ? BotRouteReason::DynamicBlocker : BotRouteReason::StaleTopology;
			if (++routeProgress.totalReplans > routeLimits.maxReplans) { routeProgress.state = BotRouteState::Failed; routeProgress.reason = BotRouteReason::ReplanLimitExceeded; return routeProgress; }
			routeProgress.state = BotRouteState::Backoff;
			routeProgress.backoffDeadline = now + BotNavigation::backoff(routeLimits, routeProgress.totalReplans);
			return routeProgress;
		}
	}

	routeProgress.state = BotRouteState::Planning;
	route = BotNavigation::findRoute(*observation, { observation->position, routeProgress.destination, routeLimits });
	if (!route.found() || route.positions.empty()) { routeProgress.state = BotRouteState::Failed; routeProgress.reason = route.reason; return routeProgress; }
	routeProgress.state = BotRouteState::Ready;
	routeProgress.expectedOrigin = observation->position;
	routeProgress.expectedNext = route.positions.front();
	const auto currentTile = std::ranges::find(observation->visibleTiles, routeProgress.expectedNext, &BotTileObservation::position);
	if (currentTile == observation->visibleTiles.end() || BotNavigation::signature(*currentTile) != route.signatures.front()) {
		routeProgress.reason = BotRouteReason::StaleTopology;
	} else if (currentTile->blockingCreatureId != 0) {
		routeProgress.reason = BotRouteReason::DynamicBlocker;
	} else {
		routeProgress.state = BotRouteState::StepPending;
		const auto assessment = BotNavigation::assess(*observation, routeProgress.expectedNext);
		const auto actionResult = executeMovement(assessment, now);
		if (!actionResult.succeeded()) routeProgress.reason = actionResult.failure == BotActionFailure::StaleObservation ? BotRouteReason::StaleTopology : BotRouteReason::MovementRejected;
		else {
			const auto actual = controlledPlayer->getPosition();
			if (actual == routeProgress.expectedNext) {
				BotNavigation::observeProgress(routeProgress, actual, now, routeLimits); routeProgress.routeIndex++;
				if (actual == routeProgress.destination) { routeProgress.state = BotRouteState::Arrived; routeProgress.reason = BotRouteReason::RouteFound; }
				else { routeProgress.state = BotRouteState::ReplanRequired; routeProgress.reason = BotRouteReason::RouteFound; route = {}; }
				return routeProgress;
			}
			BotNavigation::observeProgress(routeProgress, actual, now, routeLimits);
		}
	}

	if (routeProgress.reason == BotRouteReason::NoProgress && routeProgress.consecutiveNoProgress >= routeLimits.maxNoProgress) {
		routeProgress.state = BotRouteState::Failed; return routeProgress;
	}
	if (++routeProgress.totalReplans > routeLimits.maxReplans) {
		routeProgress.state = BotRouteState::Failed; routeProgress.reason = BotRouteReason::ReplanLimitExceeded; return routeProgress;
	}
	routeProgress.state = BotRouteState::Backoff;
	routeProgress.backoffDeadline = now + BotNavigation::backoff(routeLimits, routeProgress.totalReplans);
	return routeProgress;
}

BotRouteProgress BotController::cancelRoute() {
	route = {};
	routeProgress.state = BotRouteState::Cancelled;
	routeProgress.reason = BotRouteReason::Cancelled;
	return routeProgress;
}

BotTransitionResult BotController::startTransition(const BotTransitionRequest &request, std::chrono::milliseconds now) {
	if (transitionProgress.state != BotTransitionState::Idle
		&& transitionProgress.state != BotTransitionState::Completed
		&& transitionProgress.state != BotTransitionState::ReplanRequired
		&& transitionProgress.state != BotTransitionState::Failed
		&& transitionProgress.state != BotTransitionState::Cancelled) {
		return { { BotInteractionOutcome::Pending, BotTransitionFailure::None, 0, {}, {}, transitionProgress.attempts }, transitionProgress.state };
	}
	const auto controlledPlayer = player.lock();
	const auto observation = BotPerception::observe(controlledPlayer);
	if (!observation) return { { BotInteractionOutcome::InvalidLifecycle, BotTransitionFailure::InvalidLifecycle }, BotTransitionState::Failed };
	if (!BotInteraction::supported(request.target.category)) return { { BotInteractionOutcome::UnsupportedInteraction, BotTransitionFailure::UnsupportedInteraction }, BotTransitionState::Failed };
	if (request.target.signature != BotInteraction::signature(request.target)) return { { BotInteractionOutcome::StaleObservation, BotTransitionFailure::StaleObservation }, BotTransitionState::Failed };
	transitionProgress = { .state = BotTransitionState::InteractionPending, .request = request, .positionBefore = observation->position, .startedAt = now };
	return advanceTransition(now);
}

BotTransitionResult BotController::advanceTransition(std::chrono::milliseconds now) {
	auto &progress = transitionProgress;
	if (progress.state == BotTransitionState::Completed || progress.state == BotTransitionState::ReplanRequired || progress.state == BotTransitionState::Failed || progress.state == BotTransitionState::Cancelled) return progress.result;
	const auto controlledPlayer = player.lock();
	const auto observation = BotPerception::observe(controlledPlayer);
	if (!observation) {
		progress.state = BotTransitionState::Failed;
		progress.result = { { BotInteractionOutcome::InvalidLifecycle, BotTransitionFailure::InvalidLifecycle }, progress.state };
		return progress.result;
	}
	if (progress.state == BotTransitionState::Backoff) {
		if (now < progress.nextAttemptAt) return progress.result;
		progress.state = BotTransitionState::InteractionPending;
	}
	if (progress.state == BotTransitionState::AwaitingTransition) {
		progress.state = BotTransitionState::VerifyingResult;
		const Position after = observation->position;
		if (after != progress.positionBefore) {
			const bool allowed = BotInteraction::destinationAllowed(progress.request, progress.positionBefore, after);
			progress.state = allowed ? BotTransitionState::Completed : BotTransitionState::Failed;
			progress.result = { {
				allowed ? BotInteractionOutcome::TransitionObserved : BotInteractionOutcome::UnexpectedDestination,
				allowed ? BotTransitionFailure::None : BotTransitionFailure::UnexpectedDestination,
				progress.result.worldReturnValue, progress.positionBefore, after, progress.attempts
			}, progress.state, true, true };
			route = {};
			routeProgress = { .state = BotRouteState::ReplanRequired, .reason = BotRouteReason::StaleTopology, .actualPosition = after };
			return progress.result;
		}
		if (progress.request.target.category == BotInteractionType::UseDoor) {
			const auto tile = game.map.getTile(progress.request.target.position);
			std::shared_ptr<Item> item;
			if (tile && tile->getItemList()) {
				for (const auto &candidate : *tile->getItemList()) if (candidate && candidate->getDoor()) { item = candidate; break; }
			}
			if (item && item->getID() != progress.request.target.itemTypeId && item->getDoor()) {
				progress.state = BotTransitionState::ReplanRequired;
				progress.result = { { BotInteractionOutcome::Succeeded, BotTransitionFailure::None, progress.result.worldReturnValue, progress.positionBefore, after, progress.attempts }, progress.state, true, true };
				route = {};
				routeProgress = { .state = BotRouteState::ReplanRequired, .reason = BotRouteReason::StaleTopology, .actualPosition = after };
				return progress.result;
			}
		}
		if (now - progress.startedAt < progress.request.timeout) return progress.result;
		if (progress.attempts >= progress.request.maxAttempts) {
			progress.state = BotTransitionState::Failed;
			const bool doorDenied = progress.request.target.category == BotInteractionType::UseDoor;
			progress.result = { { doorDenied ? BotInteractionOutcome::AccessDenied : BotInteractionOutcome::RetryExhausted, doorDenied ? BotTransitionFailure::AccessDenied : BotTransitionFailure::RetryExhausted, progress.result.worldReturnValue, progress.positionBefore, after, progress.attempts }, progress.state };
			return progress.result;
		}
		const auto delay = BotInteraction::backoff(progress.request, progress.attempts);
		progress.state = BotTransitionState::Backoff;
		progress.nextAttemptAt = now + delay;
		progress.result.outcome = BotInteractionOutcome::RetryScheduled;
		progress.result.failure = BotTransitionFailure::TimedOut;
		progress.result.retryAfter = delay;
		return progress.result;
	}

	const auto &target = progress.request.target;
	if (target.signature != BotInteraction::signature(target)) {
		progress.state = BotTransitionState::Failed;
		progress.result = { { BotInteractionOutcome::StaleObservation, BotTransitionFailure::StaleObservation }, progress.state };
		return progress.result;
	}
	if (progress.request.requiredItemTypeId && static_cast<const Cylinder &>(*controlledPlayer).getItemTypeCount(progress.request.requiredItemTypeId) == 0) {
		progress.state = BotTransitionState::Failed;
		progress.result = { { BotInteractionOutcome::MissingRequiredItem, BotTransitionFailure::MissingRequiredItem }, progress.state };
		return progress.result;
	}
	const Position before = controlledPlayer->getPosition();
	ReturnValue worldResult = RETURNVALUE_NOTPOSSIBLE;
	if (target.category == BotInteractionType::WalkOntoTransition) {
		if (!Position::areInRange<1, 1, 0>(before, target.position)) {
			progress.state = BotTransitionState::Failed;
			progress.result = { { BotInteractionOutcome::Blocked, BotTransitionFailure::Blocked }, progress.state };
			return progress.result;
		}
		worldResult = game.internalMoveCreature(controlledPlayer, getDirectionTo(before, target.position));
	} else {
		const auto targetTile = game.map.getTile(target.position);
		std::shared_ptr<Item> item;
		if (targetTile && targetTile->getItemList()) {
			for (const auto &candidate : *targetTile->getItemList()) if (candidate && candidate->getID() == target.itemTypeId) { item = candidate; break; }
		}
		if (!item || item->getID() != target.itemTypeId) {
			progress.state = BotTransitionState::Failed;
			progress.result = { { BotInteractionOutcome::StaleObservation, BotTransitionFailure::StaleObservation }, progress.state };
			return progress.result;
		}
		if (target.category == BotInteractionType::UseDoor && !item->getDoor()) {
			progress.state = BotTransitionState::Failed;
			progress.result = { { BotInteractionOutcome::InvalidTarget, BotTransitionFailure::InvalidTarget }, progress.state };
			return progress.result;
		}
		worldResult = g_actions().canUse(controlledPlayer, target.position);
		if (worldResult == RETURNVALUE_NOERROR) worldResult = g_actions().canUse(controlledPlayer, target.position, item);
		if (worldResult == RETURNVALUE_NOERROR) game.playerUseItem(controlledPlayer->getID(), target.position, target.stackPosition, 0, target.itemTypeId);
	}
	++progress.attempts;
	progress.startedAt = now;
	progress.positionBefore = before;
	progress.state = worldResult == RETURNVALUE_NOERROR ? BotTransitionState::AwaitingTransition : BotTransitionState::Failed;
	progress.result = { {
		worldResult == RETURNVALUE_NOERROR ? BotInteractionOutcome::Pending : BotInteractionOutcome::WorldRejected,
		worldResult == RETURNVALUE_NOERROR ? BotTransitionFailure::NoTransition : BotTransitionFailure::WorldRejected,
		static_cast<uint16_t>(worldResult), before, controlledPlayer->getPosition(), progress.attempts
	}, progress.state };
	return progress.result;
}

BotTransitionResult BotController::cancelTransition() {
	transitionProgress.state = BotTransitionState::Cancelled;
	transitionProgress.result = { { BotInteractionOutcome::Cancelled, BotTransitionFailure::Cancelled }, BotTransitionState::Cancelled };
	return transitionProgress.result;
}
