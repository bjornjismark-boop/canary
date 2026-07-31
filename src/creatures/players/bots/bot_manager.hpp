/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

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

	[[nodiscard]] size_t size() const {
		return sessions.size();
	}

private:
	Game &game;
	BotSessionOperations operations;
	std::unordered_map<std::string, std::shared_ptr<BotSession>> sessions;
};
