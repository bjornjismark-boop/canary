/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_manager.hpp"

#include "creatures/players/bots/bot_session.hpp"
#include "creatures/players/grouping/party.hpp"
#include "creatures/players/player.hpp"
#include "game/game.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "lib/logging/log_with_spd_log.hpp"
#include "utils/tools.hpp"

BotManager::BotManager(Game &game, BotSessionOperations operations) :
	game(game), operations(std::move(operations)) {
}

BotManager::~BotManager() noexcept {
	try {
		fleetController.stopping = true;
		if (fleetEventId != 0) g_dispatcher().stopEvent(fleetEventId);
		fleetEventId = 0;
		fleetController.wakeupPending = false;
		fleetLifetime.reset();
		fleetLifecycle.clear();
		clearCoordination();
		bool success = true;
		for (const auto &[name, session] : sessions) {
			if (!session->close(false)) {
				success = false;
			}
			session->ownerAttemptedDestructorCleanup = true;
		}
		if (!success) {
			g_logger().error("[BotManager::~BotManager] One or more bots could not be removed; Game must outlive BotManager");
		}
	} catch (const std::exception &exception) {
		g_logger().error("[BotManager::~BotManager] Exception during best-effort cleanup: {}", exception.what());
	} catch (...) {
		g_logger().error("[BotManager::~BotManager] Unknown exception during best-effort cleanup");
	}
}

std::shared_ptr<const BotSession> BotManager::login(const std::string &name) {
	const auto key = asLowerCaseString(name);
	if (key.empty()) {
		g_logger().warn("[BotManager::login] Rejected bot login with an empty name");
		return nullptr;
	}
	if (sessions.contains(key)) {
		g_logger().warn("[BotManager::login] Rejected duplicate managed bot login for '{}'", name);
		return nullptr;
	}
	if (game.getPlayerByName(name)) {
		g_logger().warn("[BotManager::login] Rejected bot login for '{}': name is already in the world", name);
		return nullptr;
	}

	auto session = std::shared_ptr<BotSession>(new BotSession(game, operations));
	if (!session->load(name)) {
		g_logger().warn("[BotManager::login] Failed to load bot '{}'", name);
		return nullptr;
	}
	if (!session->place()) {
		g_logger().warn("[BotManager::login] Failed to place bot '{}'", name);
		return nullptr;
	}
	sessions.emplace(key, session);
	auto &generation = sessionGenerations[session->player->getGUID()];
	if (generation != std::numeric_limits<uint64_t>::max()) {
		++generation;
	}
	return session;
}

bool BotManager::logout(const std::string &name, bool savePlayer) {
	const auto it = sessions.find(asLowerCaseString(name));
	if (it == sessions.end()) {
		return false;
	}
	const auto memberId = it->second->player ? it->second->player->getGUID() : 0;
	if (!it->second->close(savePlayer)) {
		return false;
	}
	for (auto &[groupId, group] : coordinationGroups) {
		(void)groupId;
		BotCoordination::invalidate(group.reservations, memberId);
	}
	sessions.erase(it);
	return true;
}

ReturnValue BotManager::move(const std::string &name, Direction direction) {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end() ? RETURNVALUE_NOTPOSSIBLE : it->second->move(direction);
}

BotWalkabilityResult BotManager::assess(const std::string &name, Direction direction) const {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end()
		? BotWalkabilityResult { .candidate = { .direction = direction }, .outcome = BotWalkability::WorldRejected, .movementCost = std::numeric_limits<uint32_t>::max() }
		: it->second->assess(direction);
}

BotActionResult BotManager::executeMovement(const std::string &name, const BotWalkabilityResult &assessment, std::chrono::milliseconds now) {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end()
		? BotActionResult { BotActionStatus::Rejected, BotActionFailure::InvalidLifecycle }
		: it->second->executeMovement(assessment, now);
}

BotActionResult BotManager::tick(const std::string &name, std::chrono::milliseconds now) {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end()
		? BotActionResult { BotActionStatus::Rejected, BotActionFailure::InvalidLifecycle }
		: it->second->tick(now);
}

BotActionResult BotManager::execute(const std::string &name, const BotAction &action, std::chrono::milliseconds now) {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end()
		? BotActionResult { BotActionStatus::Rejected, BotActionFailure::InvalidLifecycle }
		: it->second->execute(action, now);
}

bool BotManager::save(const std::string &name) {
	const auto it = sessions.find(asLowerCaseString(name));
	if (it == sessions.end()) {
		return false;
	}
	if (it->second->getState() != BotSessionState::PendingSave) {
		return it->second->save();
	}
	const auto memberId = it->second->player ? it->second->player->getGUID() : 0;
	if (!it->second->close(true)) {
		return false;
	}
	for (auto &[groupId, group] : coordinationGroups) {
		(void)groupId;
		BotCoordination::invalidate(group.reservations, memberId);
	}
	sessions.erase(it);
	return true;
}

std::shared_ptr<const BotSession> BotManager::getSession(const std::string &name) const {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end() ? nullptr : it->second;
}

bool BotManager::clear(bool savePlayers) {
	bool success = true;
	for (auto it = sessions.begin(); it != sessions.end();) {
		const auto memberId = it->second->player ? it->second->player->getGUID() : 0;
		if (it->second->close(savePlayers)) {
			for (auto &[groupId, group] : coordinationGroups) {
				(void)groupId;
				BotCoordination::invalidate(group.reservations, memberId);
			}
			it = sessions.erase(it);
		} else {
			success = false;
			++it;
		}
	}
	if (sessions.empty()) {
		clearCoordination();
	}
	return success;
}

BotRouteProgress BotManager::startRoute(const std::string &name, const Position &destination, std::chrono::milliseconds now, BotRouteLimits limits) {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end() ? BotRouteProgress { .state = BotRouteState::Failed, .reason = BotRouteReason::InvalidLifecycle } : it->second->startRoute(destination, now, limits);
}

BotRouteProgress BotManager::advanceRoute(const std::string &name, std::chrono::milliseconds now) {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end() ? BotRouteProgress { .state = BotRouteState::Failed, .reason = BotRouteReason::InvalidLifecycle } : it->second->advanceRoute(now);
}

BotRouteProgress BotManager::cancelRoute(const std::string &name) {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end() ? BotRouteProgress { .state = BotRouteState::Failed, .reason = BotRouteReason::InvalidLifecycle } : it->second->cancelRoute();
}

BotTransitionResult BotManager::startTransition(const std::string &name, const BotTransitionRequest &request, std::chrono::milliseconds now) {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end() ? BotTransitionResult { { BotInteractionOutcome::InvalidLifecycle, BotTransitionFailure::InvalidLifecycle }, BotTransitionState::Failed } : it->second->startTransition(request, now);
}

BotTransitionResult BotManager::advanceTransition(const std::string &name, std::chrono::milliseconds now) {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end() ? BotTransitionResult { { BotInteractionOutcome::InvalidLifecycle, BotTransitionFailure::InvalidLifecycle }, BotTransitionState::Failed } : it->second->advanceTransition(now);
}

BotTransitionResult BotManager::cancelTransition(const std::string &name) {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end() ? BotTransitionResult { { BotInteractionOutcome::InvalidLifecycle, BotTransitionFailure::InvalidLifecycle }, BotTransitionState::Failed } : it->second->cancelTransition();
}

BotTargetSelectionResult BotManager::evaluateCombat(const std::string &name, const BotCombatPolicy &policy) {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end() ? BotTargetSelectionResult { .failure = BotCombatFailure::InvalidLifecycle, .reason = BotCombatEligibility::InvalidLifecycle } : it->second->evaluateCombat(policy);
}

BotCombatExecutionResult BotManager::executeCombat(const std::string &name, const BotCombatExecutionRequest &request, std::chrono::milliseconds now, const BotCombatExecutionPolicy &policy) {
	const auto it = sessions.find(asLowerCaseString(name));
	return it == sessions.end() ? BotCombatExecutionResult { BotCombatExecutionOutcome::InvalidLifecycle, BotAttackFailure::InvalidLifecycle, BotAttackState::Failed } : it->second->executeCombat(request, now, policy);
}

BotSurvivalAssessment BotManager::evaluateSurvival(const std::string &n, const BotSurvivalPolicy &p, std::vector<BotHealingOption> o) { const auto s = sessions.find(asLowerCaseString(n)); return s == sessions.end() ? BotSurvivalAssessment { .urgency = BotSurvivalUrgency::Fatal, .decision = BotSurvivalDecision::Dead } : s->second->evaluateSurvival(p, std::move(o)); }
BotHealingResult BotManager::executeHealing(const std::string &n, const BotHealingOption &o, std::chrono::milliseconds now, const BotSurvivalPolicy &p) { const auto s = sessions.find(asLowerCaseString(n)); return s == sessions.end() ? BotHealingResult { .outcome = BotHealingOutcome::InvalidLifecycle } : s->second->executeHealing(o, now, p); }
BotFleeResult BotManager::executeFlee(const std::string &n, std::chrono::milliseconds now, const BotSurvivalPolicy &p) { const auto s = sessions.find(asLowerCaseString(n)); return s == sessions.end() ? BotFleeResult { .outcome = BotFleeOutcome::Cancelled } : s->second->executeFlee(now, p); }
BotDeathResult BotManager::observeDeath(const std::string &n) { const auto s = sessions.find(asLowerCaseString(n)); return s == sessions.end() ? BotDeathResult { .state = BotSurvivalState::Dead } : s->second->observeDeath(); }
BotLootSelectionResult BotManager::evaluateLoot(const std::string &name, const Position &position, uint32_t sourceCreatureId, BotCorpseSignature expectedSignature, const BotLootPolicy &policy) { const auto session = sessions.find(asLowerCaseString(name)); return session == sessions.end() ? BotLootSelectionResult { .eligibility = BotLootEligibility::InvalidLifecycle, .failure = BotLootFailure::InvalidLifecycle } : session->second->evaluateLoot(position, sourceCreatureId, expectedSignature, policy); }
BotLootTransferResult BotManager::executeLoot(const std::string &name,const BotLootTransferRequest &r,std::chrono::milliseconds n,const BotLootPolicy &p,const BotLootTransferPolicy &t){const auto s=sessions.find(asLowerCaseString(name));return s==sessions.end()?BotLootTransferResult{.outcome=BotLootTransferOutcome::Cancelled,.failure=BotLootTransferFailure::InvalidLifecycle,.state=BotLootExecutionState::Failed,.request=r}:s->second->executeLoot(r,n,p,t);}
BotSupplyAssessment BotManager::evaluateSupplies(const std::string &name, const BotSupplyPolicy &policy, uint64_t expectedInventorySignature) { const auto session=sessions.find(asLowerCaseString(name)); return session==sessions.end()?BotSupplyAssessment{.urgency=BotSupplyUrgency::Critical,.intent=BotSupplyIntent::ObservationStale,.failure=BotSupplyFailure::InvalidLifecycle}:session->second->evaluateSupplies(policy,expectedInventorySignature); }
BotAdventureProgress BotManager::advanceAdventure(const std::string &name,const BotAdventureObservation &observation,std::chrono::milliseconds now,const BotAdventurePolicy &policy){const auto session=sessions.find(asLowerCaseString(name));return session==sessions.end()?BotAdventureProgress{.state=BotAdventureState::Failed,.failure=BotAdventureFailure::InvalidLifecycle}:session->second->advanceAdventure(observation,now,policy);}
BotEquipmentObservation BotManager::evaluateEquipment(const std::string &name, const BotEquipmentPolicy &policy) { const auto session = sessions.find(asLowerCaseString(name)); return session == sessions.end() ? BotEquipmentObservation {} : session->second->evaluateEquipment(policy); }
BotShopObservation BotManager::observeShop(const std::string &name, uint32_t npcId, uint16_t maximumOffers) { const auto session=sessions.find(asLowerCaseString(name)); return session==sessions.end()?BotShopObservation{}:session->second->observeShop(npcId,maximumOffers); }
BotShopTransactionResult BotManager::executeShop(const std::string &name,const BotShopTransactionRequest &request,std::chrono::milliseconds now,const BotShopPolicy &policy){const auto session=sessions.find(asLowerCaseString(name));return session==sessions.end()?BotShopTransactionResult{.outcome=BotShopOutcome::Cancelled,.failure=BotShopFailure::InvalidLifecycle,.state=BotShopState::Failed,.request=request}:session->second->executeShop(request,now,policy);}
BotShopTransactionResult BotManager::cancelShop(const std::string &name){const auto session=sessions.find(asLowerCaseString(name));return session==sessions.end()?BotShopTransactionResult{.outcome=BotShopOutcome::Cancelled,.failure=BotShopFailure::InvalidLifecycle,.state=BotShopState::Failed}:session->second->cancelShop();}
BotDepotObservation BotManager::observeDepot(const std::string&name,uint32_t id,const BotResupplyPolicy&p){const auto s=sessions.find(asLowerCaseString(name));return s==sessions.end()?BotDepotObservation{}:s->second->observeDepot(id,p);}
BotResupplyResult BotManager::executeResupply(const std::string&name,const BotResupplyRequest&r,std::chrono::milliseconds n,const BotResupplyPolicy&p){const auto s=sessions.find(asLowerCaseString(name));return s==sessions.end()?BotResupplyResult{.outcome=BotResupplyOutcome::Cancelled,.state=BotResupplyState::Failed,.request=r}:s->second->executeResupply(r,n,p);}
BotEquipmentExecutionResult BotManager::executeEquipment(const std::string&name,const BotEquipmentExecutionRequest&r,std::chrono::milliseconds n,const BotEquipmentPolicy&p){const auto s=sessions.find(asLowerCaseString(name));return s==sessions.end()?BotEquipmentExecutionResult{.outcome=BotResupplyOutcome::Cancelled,.failure=BotEquipmentFailure::InvalidLifecycle,.state=BotResupplyState::Failed,.request=r}:s->second->executeEquipment(r,n,p);}
BotResupplyResult BotManager::cancelResupply(const std::string&name){const auto s=sessions.find(asLowerCaseString(name));return s==sessions.end()?BotResupplyResult{.outcome=BotResupplyOutcome::Cancelled,.state=BotResupplyState::Failed}:s->second->cancelResupply();}
BotDialogueObservation BotManager::observeDialogue(const std::string&name,const BotNpcDialoguePolicy&p){const auto s=sessions.find(asLowerCaseString(name));return s==sessions.end()?BotDialogueObservation{}:s->second->observeDialogue(p);}
BotConversationResult BotManager::advanceDialogue(const std::string&name,uint32_t id,BotDialogueIntent i,std::chrono::milliseconds n,const BotNpcDialoguePolicy&p){const auto s=sessions.find(asLowerCaseString(name));return s==sessions.end()?BotConversationResult{.state=BotDialogueState::Failed,.failure=BotDialogueFailure::InvalidLifecycle,.npcId=id,.intent=i}:s->second->advanceDialogue(id,i,n,p);}
BotConversationResult BotManager::cancelDialogue(const std::string&name){const auto s=sessions.find(asLowerCaseString(name));return s==sessions.end()?BotConversationResult{.state=BotDialogueState::Cancelled,.response=BotDialogueResponse::Cancelled,.failure=BotDialogueFailure::InvalidLifecycle}:s->second->cancelDialogue();}
BotQuestObservation BotManager::observeQuest(const std::string&name,const BotQuestDefinition&d,std::vector<BotQuestEvidence>e,const BotQuestBounds&b){const auto s=sessions.find(asLowerCaseString(name));return s==sessions.end()?BotQuestObservation{}:s->second->observeQuest(d,std::move(e),b);}
BotQuestEligibility BotManager::evaluateQuest(const std::string&name,BotMissionId id,const BotQuestDefinition&d,const BotNpcDialoguePolicy&p,const BotQuestBounds&b){const auto s=sessions.find(asLowerCaseString(name));return s==sessions.end()?BotQuestEligibility{.result=BotQuestFailure::InvalidLifecycle}:s->second->evaluateQuest(id,d,p,b);}
BotQuestExecutionResult BotManager::startQuestExecution(const std::string&name,const BotQuestPlan&p,const BotQuestExecutionPolicy&policy){const auto s=sessions.find(asLowerCaseString(name));return s==sessions.end()?BotQuestExecutionResult{.state=BotQuestExecutionState::Failed,.failure=BotQuestExecutionFailure::InvalidLifecycle}:s->second->startQuestExecution(p,policy);}
BotQuestExecutionResult BotManager::advanceQuestExecution(const std::string&name,const BotQuestPlan&p,const BotQuestStepObservation&o,const BotQuestExecutionPolicy&policy){const auto s=sessions.find(asLowerCaseString(name));return s==sessions.end()?BotQuestExecutionResult{.state=BotQuestExecutionState::Failed,.failure=BotQuestExecutionFailure::InvalidLifecycle}:s->second->advanceQuestExecution(p,o,policy);}

BotCoordinationFailure BotManager::configureCoordinationGroup(BotCoordinationPolicy policy) {
	if (const auto failure = BotCoordination::validate(policy); failure != BotCoordinationFailure::None) {
		return failure;
	}
	if (!coordinationGroups.contains(policy.id) && coordinationGroups.size() >= policy.maximumGroups) {
		return BotCoordinationFailure::BudgetReached;
	}
	std::ranges::sort(policy.configuredMembers);
	std::ranges::sort(policy.configuredRoles);
	coordinationGroups.insert_or_assign(policy.id, BotCoordinationGroupState { .policy = std::move(policy) });
	return BotCoordinationFailure::None;
}

BotCoordinationGroupObservation BotManager::observeCoordinationGroup(BotCoordinationGroupId groupId) {
	const auto group = coordinationGroups.find(groupId);
	if (group == coordinationGroups.end()) {
		return {};
	}
	auto &state = group->second;
	if (state.observationRevision != std::numeric_limits<uint64_t>::max()) {
		++state.observationRevision;
	}
	BotCoordinationGroupObservation observation { .id = groupId, .revision = state.observationRevision };
	std::shared_ptr<Party> leaderParty;
	for (const auto &[name, session] : sessions) {
		(void)name;
		if (!session->player || session->player->getGUID() != state.policy.configuredLeader) {
			continue;
		}
		leaderParty = session->player->getParty();
		break;
	}
	for (const auto memberId : state.policy.configuredMembers) {
		for (const auto &[name, session] : sessions) {
			(void)name;
			if (!session->player || session->player->getGUID() != memberId) {
				continue;
			}
			const auto &player = session->player;
			const auto maximumHealth = std::max<int32_t>(1, player->getMaxHealth());
			observation.members.push_back({
				.id = memberId,
				.sessionGeneration = sessionGenerations[memberId],
				.revision = observation.revision,
				.vocationId = player->getVocationId(),
				.position = player->getPosition(),
				.healthPercent = static_cast<uint8_t>(std::clamp<int32_t>(player->getHealth() * 100 / maximumHealth, 0, 100)),
				.configured = true,
				.partyMember = leaderParty && player->getParty() == leaderParty,
				.playerBot = true,
				.alive = player->getHealth() > 0,
				.placed = session->state == BotSessionState::Placed && !player->isRemoved(),
				.visible = true,
			});
			break;
		}
		if (observation.members.size() >= state.policy.maximumMemberObservations) {
			break;
		}
	}
	return observation;
}

BotCoordinationDecision BotManager::evaluateCoordinationGroup(BotCoordinationGroupId groupId) {
	const auto group = coordinationGroups.find(groupId);
	if (group == coordinationGroups.end()) {
		return { .state = BotCoordinationState::Failed, .failure = BotCoordinationFailure::InvalidGroup };
	}
	auto &state = group->second;
	const auto previousLeader = state.leaderId;
	const auto observation = observeCoordinationGroup(groupId);
	std::erase_if(state.reservations, [&](const auto &reservation) {
		const auto member = std::ranges::find(observation.members, reservation.memberId, &BotCoordinationMemberObservation::id);
		return member == observation.members.end() || !member->alive || !member->placed || member->sessionGeneration != reservation.sessionGeneration;
	});
	auto decision = BotCoordination::evaluate(state.policy, observation, previousLeader, state.leaderChanges, state.regroupAttempts);
	if (decision.leaderId != 0 && previousLeader != 0 && decision.leaderId != previousLeader) {
		++state.leaderChanges;
		++state.regroupAttempts;
		BotCoordination::invalidate(state.reservations, previousLeader);
	}
	state.leaderId = decision.leaderId;
	return decision;
}

BotCoordinationFailure BotManager::reserveCoordinationTarget(BotCoordinationReservation reservation, uint64_t now) {
	const auto group = coordinationGroups.find(reservation.groupId);
	if (group == coordinationGroups.end()) {
		return BotCoordinationFailure::InvalidGroup;
	}
	const auto generation = sessionGenerations.find(reservation.memberId);
	if (generation == sessionGenerations.end() || generation->second != reservation.sessionGeneration) {
		return BotCoordinationFailure::GenerationMismatch;
	}
	if (reservation.observationRevision != group->second.observationRevision) {
		return BotCoordinationFailure::StaleObservation;
	}
	return BotCoordination::reserve(group->second.reservations, reservation, group->second.policy, now);
}

void BotManager::invalidateCoordinationTarget(BotCoordinationGroupId groupId, BotCoordinationReservationType type, uint64_t targetSignature) {
	if (const auto group = coordinationGroups.find(groupId); group != coordinationGroups.end()) {
		BotCoordination::invalidateTarget(group->second.reservations, type, targetSignature);
	}
}

BotRouteProgress BotManager::executeCoordinationMovement(const std::string &name, const BotCoordinationIntent &intent, std::chrono::milliseconds now, BotRouteLimits limits) {
	const auto session = sessions.find(asLowerCaseString(name));
	if (session == sessions.end() || !session->second->player || session->second->player->getGUID() != intent.memberId) {
		return { .state = BotRouteState::Failed, .reason = BotRouteReason::InvalidLifecycle };
	}
	const auto group = coordinationGroups.find(intent.groupId);
	if (group == coordinationGroups.end() || intent.policyRevision != group->second.policy.revision || intent.observationRevision != group->second.observationRevision || !std::ranges::contains(group->second.policy.configuredMembers, intent.memberId)) {
		return { .state = BotRouteState::Failed, .reason = BotRouteReason::StaleTopology };
	}
	auto destination = intent.region;
	if (intent.type == BotCoordinationIntentType::Formation) {
		destination.x = static_cast<uint16_t>(std::clamp<int32_t>(static_cast<int32_t>(destination.x) + intent.offsetX, 0, std::numeric_limits<uint16_t>::max()));
		destination.y = static_cast<uint16_t>(std::clamp<int32_t>(static_cast<int32_t>(destination.y) + intent.offsetY, 0, std::numeric_limits<uint16_t>::max()));
	}
	return session->second->startRoute(destination, now, limits);
}

size_t BotManager::coordinationReservationCount(BotCoordinationGroupId groupId) const {
	const auto group = coordinationGroups.find(groupId);
	return group == coordinationGroups.end() ? 0 : group->second.reservations.size();
}

size_t BotManager::coordinationReservationCount() const {
	size_t count = 0;
	for (const auto &[groupId, group] : coordinationGroups) count += group.reservations.size();
	return count;
}

void BotManager::clearCoordination() {
	coordinationGroups.clear();
}

BotFleetFailure BotManager::configureFleet(BotFleetPopulationPolicy population, BotFleetDistributionPolicy distribution, std::vector<BotFleetMemberProfile> members, uint32_t intervalTicks) {
	if (const auto failure = BotFleet::validate(population); failure != BotFleetFailure::None) return failure;
	if (const auto failure = BotFleet::validate(distribution, members); failure != BotFleetFailure::None) return failure;
	if (population.absoluteHardMaximum > fleetResourcePolicy.maximumManagedBots || population.maximumPendingLogins > fleetResourcePolicy.maximumPendingLogins || population.maximumPendingLogouts > fleetResourcePolicy.maximumPendingLogouts) return BotFleetFailure::InvalidPolicy;
	if (intervalTicks == 0 || intervalTicks > BotFleet::AbsoluteMaximumBackoffTicks) return BotFleetFailure::InvalidPolicy;
	if (fleetEventId != 0) g_dispatcher().stopEvent(fleetEventId);
	fleetEventId = 0;
	fleetPopulation = population;
	fleetDistribution = std::move(distribution);
	fleetMembers = std::move(members);
	fleetIntervalTicks = intervalTicks;
	fleetLifecycle.clear();
	BotFleet::clear(fleetController);
	fleetController.stopping = false;
	if (!fleetLifetime) fleetLifetime = std::make_shared<uint64_t>(fleetController.generation);
	fleetController.startedAtTick = g_dispatcher().getDispatcherCycle();
	return BotFleetFailure::None;
}

BotFleetReconciliation BotManager::reconcileFleet(uint64_t now, bool overloaded) {
	std::vector<BotFleetObservation> observations;
	observations.reserve(fleetMembers.size());
	if (fleetObservationRevision != std::numeric_limits<uint64_t>::max()) ++fleetObservationRevision;
	for (const auto &member : fleetMembers) {
		auto observation = fleetLifecycle.contains(member.id) ? fleetLifecycle.at(member.id) : BotFleetObservation { .id = member.id };
		observation.observationRevision = fleetObservationRevision;
		const auto session = getSession(member.name);
		const auto worldPlayer = game.getPlayerByName(member.name);
		observation.ordinaryHuman = worldPlayer && worldPlayer->isNetworkControlled();
		observation.duplicateSession = session && (!session->getPlayer() || session->getPlayer()->getGUID() != member.id);
		if (session && session->getState() == BotSessionState::Placed && session->getPlayer() && session->getPlayer()->getGUID() == member.id) {
			observation.state = BotFleetMemberState::Placed;
			if (observation.placedAtTick == 0) observation.placedAtTick = now;
		} else if (!session && !observation.ordinaryHuman
		           && observation.state != BotFleetMemberState::LoginQueued && observation.state != BotFleetMemberState::Loading && observation.state != BotFleetMemberState::PlacementPending && observation.state != BotFleetMemberState::Recovering
		           && observation.state != BotFleetMemberState::DrainRequested && observation.state != BotFleetMemberState::Saving && observation.state != BotFleetMemberState::LogoutPending) {
			observation.state = BotFleetMemberState::Offline;
			observation.placedAtTick = 0;
		}
		observations.push_back(observation);
	}
	lastFleetResult = BotFleet::reconcile(fleetController, fleetPopulation, fleetDistribution, fleetMembers, observations, now, overloaded);
	for (const auto &request : lastFleetResult.requests) {
		const auto member = std::ranges::find(fleetMembers, request.memberId, &BotFleetMemberProfile::id);
		if (member == fleetMembers.end()) continue;
		auto &lifecycle = fleetLifecycle[member->id];
		lifecycle.id = member->id;
		if (request.type == BotFleetLifecycleRequestType::Login) {
			lifecycle.state = BotFleetMemberState::LoginQueued;
			if (const auto session = login(member->name); session && session->getState() == BotSessionState::Placed) {
				lifecycle.state = BotFleetMemberState::Placed;
				lifecycle.placedAtTick = now;
				lifecycle.regionIntent = request.regionIntent;
				lifecycle.loginFailures = 0;
			} else {
				lifecycle.state = BotFleetMemberState::Backoff;
				if (lifecycle.loginFailures != std::numeric_limits<uint16_t>::max()) ++lifecycle.loginFailures;
				lifecycle.nextEligibleTick = BotFleet::retryAt(now, lifecycle.loginFailures, fleetPopulation.maximumRetryBackoffTicks);
			}
		} else {
			lifecycle.state = BotFleetMemberState::DrainRequested;
			if (logout(member->name, true)) {
				lifecycle = { .id = member->id, .state = BotFleetMemberState::Offline, .observationRevision = fleetObservationRevision };
			} else {
				lifecycle.state = BotFleetMemberState::Backoff;
				if (lifecycle.logoutFailures != std::numeric_limits<uint16_t>::max()) ++lifecycle.logoutFailures;
				lifecycle.nextEligibleTick = BotFleet::retryAt(now, lifecycle.logoutFailures, fleetPopulation.maximumRetryBackoffTicks);
			}
		}
	}
	if (fleetController.draining && sessions.empty()) {
		clearCoordination();
	}
	return lastFleetResult;
}

bool BotManager::configureFleetResources(BotFleetResourcePolicy policy) {
	if (!BotFleetHardening::validate(policy) || policy.maximumManagedBots < fleetPopulation.absoluteHardMaximum || policy.maximumPendingLogins < fleetPopulation.maximumPendingLogins || policy.maximumPendingLogouts < fleetPopulation.maximumPendingLogouts) return false;
	fleetResourcePolicy = policy;
	fleetHealthyResourceObservations = 0;
	fleetLoadShedding = {};
	return true;
}

BotFleetReconciliation BotManager::reconcileFleet(uint64_t now, const BotFleetResourceObservation &observation) {
	if (!BotFleetHardening::validate(fleetResourcePolicy)) {
		lastFleetResult = { .state = BotFleetControllerState::Failed, .failure = BotFleetFailure::InvalidPolicy };
		return lastFleetResult;
	}
	const auto previous = fleetLoadShedding.pressure;
	const bool healthy = observation.dispatcherPressure == BotFleetPressureState::Normal && observation.schedulerPressure == BotFleetPressureState::Normal && observation.databaseAvailable && observation.ordinaryPlayerResponsive;
	if (healthy && fleetHealthyResourceObservations < std::numeric_limits<uint16_t>::max()) ++fleetHealthyResourceObservations;
	else fleetHealthyResourceObservations = 0;
	fleetLoadShedding = BotFleetHardening::observe(fleetResourcePolicy, observation, previous, fleetHealthyResourceObservations);
	return reconcileFleet(now, !fleetLoadShedding.permitBotLogin);
}

bool BotManager::startFleet(uint64_t now) {
	if (!fleetPopulation.enabled || fleetController.stopping || fleetEventId != 0) return false;
	fleetController.startedAtTick = now;
	fleetController.freshObservationRequired = true;
	scheduleFleetReconciliation();
	return fleetEventId != 0;
}

void BotManager::scheduleFleetReconciliation() {
	if (fleetController.stopping || fleetController.wakeupPending || !fleetPopulation.enabled) return;
	const std::weak_ptr<uint64_t> lifetime = fleetLifetime;
	const auto generation = fleetController.generation;
	fleetController.wakeupPending = true;
	fleetEventId = g_dispatcher().scheduleEvent(fleetIntervalTicks, [this, lifetime, generation] {
		if (lifetime.expired() || fleetController.stopping || fleetController.generation != generation) return;
		fleetController.wakeupPending = false;
		fleetEventId = 0;
		executeFleetReconciliation();
	}, "BotManager::reconcileFleet", DispatcherLane::Maintenance);
	if (fleetEventId == 0) fleetController.wakeupPending = false;
}

void BotManager::executeFleetReconciliation() {
	const auto dispatcher = g_dispatcher().getLoadState();
	BotFleetResourceObservation observation {
		.managedBots = static_cast<uint32_t>(size()),
		.pendingLogins = static_cast<uint16_t>(std::min<uint32_t>(lastFleetResult.pendingLogins, std::numeric_limits<uint16_t>::max())),
		.pendingLogouts = static_cast<uint16_t>(std::min<uint32_t>(lastFleetResult.pendingLogouts, std::numeric_limits<uint16_t>::max())),
		.dispatcherPressure = dispatcher == DispatcherLoadState::Emergency || game.getGameState() == GAME_STATE_SHUTDOWN ? BotFleetPressureState::Critical : dispatcher == DispatcherLoadState::Constrained ? BotFleetPressureState::Overloaded : BotFleetPressureState::Normal,
	};
	(void)reconcileFleet(g_dispatcher().getDispatcherCycle(), observation);
	scheduleFleetReconciliation();
}

void BotManager::pauseFleet() { BotFleet::pause(fleetController); }
void BotManager::resumeFleet() { BotFleet::resume(fleetController); scheduleFleetReconciliation(); }
void BotManager::drainFleet(uint32_t target) { BotFleet::drain(fleetController, target); scheduleFleetReconciliation(); }

void BotManager::stopFleet(bool savePlayers) {
	fleetController.stopping = true;
	if (fleetEventId != 0) g_dispatcher().stopEvent(fleetEventId);
	fleetEventId = 0;
	fleetController.wakeupPending = false;
	fleetLifetime.reset();
	clearCoordination();
	(void)clear(savePlayers);
	fleetLifecycle.clear();
	BotFleet::clear(fleetController);
	fleetController.stopping = true;
}

bool BotManager::loginFleetMember(const std::string &name) {
	const auto member = std::ranges::find_if(fleetMembers, [&](const auto &entry) { return asLowerCaseString(entry.name) == asLowerCaseString(name); });
	return member != fleetMembers.end() && member->enabled && !member->alwaysOffline && sessions.size() < fleetPopulation.absoluteHardMaximum && login(member->name) != nullptr;
}

bool BotManager::logoutFleetMember(const std::string &name) {
	const auto member = std::ranges::find_if(fleetMembers, [&](const auto &entry) { return asLowerCaseString(entry.name) == asLowerCaseString(name); });
	return member != fleetMembers.end() && logout(member->name, true);
}
