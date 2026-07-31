/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_navigation.hpp"

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
};
