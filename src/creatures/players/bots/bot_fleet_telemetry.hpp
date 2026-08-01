/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_fleet_configuration.hpp"
#include "creatures/players/bots/bot_fleet_hardening.hpp"
#include "game/scheduling/dispatcher_telemetry.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <array>
	#include <deque>
	#include <functional>
	#include <filesystem>
	#include <memory>
	#include <string>
#endif

class BotManager;

enum class BotFleetTelemetryFailure : uint8_t { None, InvalidRequest, Unauthorized, Disabled, RateLimited, TimedOut, RequestTooLarge, ResponseTooLarge, Stopping, Count };
enum class BotFleetAdminOperation : uint8_t { Status, Audit, Events, Command };
enum class BotFleetAdminTransport : uint8_t { Disabled, Localhost, UnixSocket };

struct BotFleetTelemetrySnapshot {
	uint64_t sequence = 0;
	uint64_t policyRevision = 0;
	BotFleetControllerState controllerState = BotFleetControllerState::Disabled;
	BotFleetPressureState pressureState = BotFleetPressureState::Normal;
	BotFleetFailure reconciliationFailure = BotFleetFailure::None;
	uint32_t managedSessions = 0;
	uint32_t desiredPopulation = 0;
	uint32_t hardMaximum = 0;
	uint32_t ordinaryPlayers = 0;
	uint32_t placed = 0;
	std::array<uint32_t, static_cast<size_t>(BotFleetMemberState::Failed) + 1> lifecycleCounts {};
	uint32_t duplicateSessions = 0;
	uint32_t pendingLogins = 0;
	uint32_t pendingLogouts = 0;
	uint32_t plannerHealthy = 0;
	uint32_t coordinationGroups = 0;
	uint32_t coordinationReservations = 0;
	uint32_t queuedCommands = 0;
	uint32_t auditEntries = 0;
	uint16_t reconciliationWorkBudget = 0;
	std::array<uint64_t, static_cast<size_t>(BotFleetTelemetryFailure::Count)> failures {};
	bool stopping = false;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	bool operator==(const BotFleetTelemetrySnapshot &) const = default;
};

struct BotFleetTelemetryEvent {
	uint64_t sequence = 0;
	BotFleetTelemetryFailure failure = BotFleetTelemetryFailure::None;
	std::string detail;
};

struct BotFleetAdminServicePolicy {
	BotFleetAdminTransport transport = BotFleetAdminTransport::Disabled;
	uint16_t maximumRequestsPerWindow = 32;
	uint32_t maximumRequestAgeTicks = 64;
	uint32_t maximumRequestBytes = 4096;
	uint32_t maximumResponseBytes = 65536;
	uint16_t maximumAuditResults = 64;
	uint16_t maximumEventResults = 64;
};

struct BotFleetAdminRequest {
	BotFleetAdminOperation operation = BotFleetAdminOperation::Status;
	uint64_t operatorId = 0;
	uint64_t window = 0;
	uint64_t submittedAtTick = 0;
	uint64_t handledAtTick = 0;
	uint32_t encodedSize = 0;
	uint16_t limit = 0;
	std::string credential;
	BotFleetCommand command;
};

struct BotFleetAdminResponse {
	BotFleetTelemetryFailure failure = BotFleetTelemetryFailure::None;
	std::shared_ptr<const BotFleetTelemetrySnapshot> snapshot;
	std::vector<BotFleetAuditEntry> audit;
	std::vector<BotFleetTelemetryEvent> events;
	BotFleetCommand command;
	uint32_t encodedSize = 0;
	[[nodiscard]] bool success() const { return failure == BotFleetTelemetryFailure::None; }
};

class BotFleetTelemetry final {
public:
	static constexpr size_t MaximumEvents = 256;
	[[nodiscard]] std::shared_ptr<const BotFleetTelemetrySnapshot> collect(const BotManager &, const BotFleetAdministration &);
	void record(BotFleetTelemetryFailure, std::string detail = {});
	[[nodiscard]] std::vector<BotFleetTelemetryEvent> recent(size_t limit) const;
	void stop();

private:
	uint64_t sequence = 0;
	std::array<uint64_t, static_cast<size_t>(BotFleetTelemetryFailure::Count)> failures {};
	std::deque<BotFleetTelemetryEvent> events;
	bool stopping = false;
};

class BotFleetAdminService final {
public:
	using AuthenticationFunction = std::function<bool(uint64_t, std::string_view)>;
	BotFleetAdminService(BotFleetAdministration &, BotFleetTelemetry &, BotFleetAdminServicePolicy = {}, AuthenticationFunction = {});
	[[nodiscard]] BotFleetAdminResponse handle(const BotManager &, BotFleetAdminRequest);
	void stop();

private:
	BotFleetAdministration &administration;
	BotFleetTelemetry &telemetry;
	BotFleetAdminServicePolicy policy;
	AuthenticationFunction authenticate;
	uint64_t activeWindow = 0;
	uint16_t requestsInWindow = 0;
	bool stopping = false;
};

// Explicit soak-mode, local-filesystem-only adapter. It is never constructed
// unless Canary receives PLAYERBOTS_SOAK_OUTPUT. All calls run on the dispatcher
// thread and only copy value snapshots or enqueue normal M9C commands.
class BotFleetLocalSoakAdapter final {
public:
	static constexpr uintmax_t MaximumCommandFileBytes = 1024 * 1024;
	BotFleetLocalSoakAdapter(std::filesystem::path, BotManager &, BotFleetAdministration &, BotFleetTelemetry &);
	[[nodiscard]] bool valid() const { return enabled; }
	void sample(const dispatcher::telemetry::LatencySnapshot &, uint64_t timestampMilliseconds);
	void stop();

private:
	void pollCommands(uint64_t timestampMilliseconds);
	std::filesystem::path directory;
	BotManager &manager;
	BotFleetAdministration &administration;
	BotFleetTelemetry &telemetry;
	uintmax_t commandOffset = 0;
	std::deque<uint64_t> requestIds;
	bool enabled = false;
};
