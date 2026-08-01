/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_fleet_hardening.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <algorithm>
	#include <cctype>
	#include <filesystem>
	#include <fstream>
	#include <iostream>
	#include <ranges>
	#include <stdexcept>
	#include <string>
	#include <vector>
#endif

namespace {
BotFleetScaleProfile parseProfile(std::string_view value) {
	if (value == "smoke") return BotFleetScaleProfile::Smoke;
	if (value == "small") return BotFleetScaleProfile::Small;
	if (value == "medium") return BotFleetScaleProfile::Medium;
	if (value == "large") return BotFleetScaleProfile::Large;
	if (value == "release-candidate") return BotFleetScaleProfile::ReleaseCandidate;
	throw std::invalid_argument("unsupported profile");
}

std::string profileName(BotFleetScaleProfile profile) {
	switch (profile) {
		case BotFleetScaleProfile::Smoke: return "smoke";
		case BotFleetScaleProfile::Small: return "small";
		case BotFleetScaleProfile::Medium: return "medium";
		case BotFleetScaleProfile::Large: return "large";
		case BotFleetScaleProfile::ReleaseCandidate: return "release-candidate";
	}
	return "unknown";
}

bool disposableEnvironment(const std::filesystem::path &path) {
	std::ifstream input(path);
	std::string line;
	bool reset = false;
	bool testName = false;
	while (std::getline(input, line)) {
		if (line == "TEST_DB_ALLOW_RESET=1") reset = true;
		if (line.starts_with("TEST_DB_NAME=")) {
			auto name = line.substr(std::string("TEST_DB_NAME=").size());
			std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			testName = name.contains("test") || name.contains("playerbot");
		}
	}
	return reset && testName;
}
}

int main(int argc, char **argv) {
	try {
		std::string profileArgument;
		std::filesystem::path environment;
		std::filesystem::path output;
		uint32_t requestedTicks = 0;
		for (int i = 1; i < argc; ++i) {
			const std::string argument = argv[i];
			auto value = [&](std::string_view name) -> std::string {
				if (i + 1 >= argc) throw std::invalid_argument(std::string(name) + " requires a value");
				return argv[++i];
			};
			if (argument == "--profile") profileArgument = value(argument);
			else if (argument == "--environment") environment = value(argument);
			else if (argument == "--output") output = value(argument);
			else if (argument == "--ticks") requestedTicks = static_cast<uint32_t>(std::stoul(value(argument)));
			else throw std::invalid_argument("unknown argument");
		}
		if (profileArgument.empty() || environment.empty() || output.empty()) throw std::invalid_argument("--profile, --environment and --output are required");
		if (!std::filesystem::is_regular_file(environment) || !disposableEnvironment(environment)) throw std::invalid_argument("environment is not an explicitly disposable test database");
		auto policy = BotFleetHardening::profile(parseProfile(profileArgument));
		if (requestedTicks != 0) policy.tickBudget = requestedTicks;
		if (policy.tickBudget == 0 || policy.tickBudget > BotFleetHardening::AbsoluteMaximumSoakTicks) throw std::invalid_argument("tick budget is outside hard bounds");
		std::vector<BotFleetSoakObservation> observations;
		observations.reserve(policy.tickBudget);
		for (uint32_t tick = 1; tick <= policy.tickBudget; ++tick) observations.push_back({ .tick=tick, .placed=policy.desiredPopulation, .uniqueSessions=policy.desiredPopulation });
		const auto result = BotFleetHardening::run(policy, observations);
		const auto temporary = output.string() + ".tmp";
		{
			std::ofstream report(temporary, std::ios::trunc);
			if (!report) throw std::runtime_error("cannot create report");
			report << "PLAYERBOTS_SOAK_VERSION=1\n"
			       << "MODE=DETERMINISTIC\n"
			       << "PROFILE=" << profileName(policy.profile) << "\n"
			       << "SEED=" << policy.seed << "\n"
			       << "DESIRED_POPULATION=" << policy.desiredPopulation << "\n"
			       << "TICKS_REQUESTED=" << policy.tickBudget << "\n"
			       << "TICKS_COMPLETED=" << result.ticksCompleted << "\n"
			       << "SUMMARIES=" << result.summaries << "\n"
			       << "RESTART_CYCLES=" << result.restartCycles << "\n"
			       << "PROCESS_CPU=UNSUPPORTED\n"
			       << "PROCESS_RSS=UNSUPPORTED\n"
			       << "PRODUCTION_SOAK=NO\n"
			       << "RESULT=" << (result.passed ? "PASS" : "FAIL") << "\n";
		}
		std::filesystem::rename(temporary, output);
		std::cout << "SOAK_REPORT=" << output.string() << '\n';
		return result.passed ? 0 : 1;
	} catch (const std::exception &exception) {
		std::cerr << "playerbots_soak: " << exception.what() << '\n';
		return 64;
	}
}
