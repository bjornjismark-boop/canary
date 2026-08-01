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
#endif

class Game;
class Player;
class BotManager;
enum class ManagedPlayerRemovalResult : uint8_t;

struct BotSessionOperations {
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
