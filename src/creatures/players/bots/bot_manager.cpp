/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_manager.hpp"

#include "creatures/players/bots/bot_session.hpp"
#include "game/game.hpp"
#include "lib/logging/log_with_spd_log.hpp"
#include "utils/tools.hpp"

BotManager::BotManager(Game &game, BotSessionOperations operations) :
	game(game), operations(std::move(operations)) {
}

BotManager::~BotManager() noexcept {
	try {
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
	return session;
}

bool BotManager::logout(const std::string &name, bool savePlayer) {
	const auto it = sessions.find(asLowerCaseString(name));
	if (it == sessions.end()) {
		return false;
	}
	if (!it->second->close(savePlayer)) {
		return false;
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
	if (!it->second->close(true)) {
		return false;
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
		if (it->second->close(savePlayers)) {
			it = sessions.erase(it);
		} else {
			success = false;
			++it;
		}
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
