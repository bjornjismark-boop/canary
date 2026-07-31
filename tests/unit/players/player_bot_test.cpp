/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include <gtest/gtest.h>

#include "creatures/players/player.hpp"
TEST(PlayerBotTest, ClassifiesNetworkAndBotControlExplicitly) {
	const auto networkPlayer = std::make_shared<Player>();
	const auto botPlayer = std::make_shared<Player>(PlayerControlType::Bot);

	EXPECT_EQ(PlayerControlType::Network, networkPlayer->getControlType());
	EXPECT_TRUE(networkPlayer->isNetworkControlled());
	EXPECT_FALSE(networkPlayer->isBotControlled());

	EXPECT_EQ(PlayerControlType::Bot, botPlayer->getControlType());
	EXPECT_FALSE(botPlayer->isNetworkControlled());
	EXPECT_TRUE(botPlayer->isBotControlled());
}

TEST(PlayerBotTest, BotWithoutClientIsNotADisconnectedNetworkSession) {
	const auto networkPlayer = std::make_shared<Player>();
	const auto botPlayer = std::make_shared<Player>(PlayerControlType::Bot);

	EXPECT_TRUE(networkPlayer->isDisconnected());
	EXPECT_FALSE(botPlayer->isDisconnected());
}
