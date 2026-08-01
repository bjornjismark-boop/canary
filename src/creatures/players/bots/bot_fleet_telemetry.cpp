/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_fleet_telemetry.hpp"

#include "creatures/players/bots/bot_manager.hpp"
#include "lib/metrics/metrics.hpp"
#include "utils/tools.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <fstream>
	#include <nlohmann/json.hpp>
#endif

using json = nlohmann::json;

std::shared_ptr<const BotFleetTelemetrySnapshot> BotFleetTelemetry::collect(const BotManager &manager, const BotFleetAdministration &administration) {
	const auto &fleet = manager.lastFleetReconciliation();
	auto result = std::make_shared<BotFleetTelemetrySnapshot>();
	result->sequence = ++sequence;
	if (const auto active = administration.activeRevision()) result->policyRevision = active->revision;
	result->controllerState = manager.fleetState().state;
	result->pressureState = manager.lastFleetLoadShedding().pressure;
	result->reconciliationFailure = fleet.failure;
	result->managedSessions = static_cast<uint32_t>(manager.size());
	result->desiredPopulation = manager.fleetPopulationPolicy().desiredOnline;
	result->hardMaximum = manager.fleetPopulationPolicy().absoluteHardMaximum;
	result->ordinaryPlayers = manager.ordinaryPlayerCount();
	result->placed = fleet.placed;
	result->lifecycleCounts[static_cast<size_t>(BotFleetMemberState::Offline)] = static_cast<uint32_t>(manager.fleetMemberCount());
	for (const auto &[id, observation] : manager.fleetObservations()) {
		(void)id;
		const auto state = static_cast<size_t>(observation.state);
		if (state < result->lifecycleCounts.size() && observation.state != BotFleetMemberState::Offline) {
			++result->lifecycleCounts[state];
			if (result->lifecycleCounts[static_cast<size_t>(BotFleetMemberState::Offline)] > 0) --result->lifecycleCounts[static_cast<size_t>(BotFleetMemberState::Offline)];
		}
		if (observation.duplicateSession) ++result->duplicateSessions;
	}
	result->pendingLogins = fleet.pendingLogins;
	result->pendingLogouts = fleet.pendingLogouts;
	result->plannerHealthy = result->managedSessions;
	result->coordinationGroups = static_cast<uint32_t>(manager.coordinationGroupCount());
	result->coordinationReservations = static_cast<uint32_t>(manager.coordinationReservationCount());
	result->queuedCommands = static_cast<uint32_t>(administration.queued());
	result->auditEntries = static_cast<uint32_t>(administration.audit().size());
	result->reconciliationWorkBudget = manager.lastFleetLoadShedding().reconciliationWork;
	result->failures = failures;
	result->stopping = stopping;
	g_metrics().addCounter("playerbots_fleet_telemetry_collections", 1);
	if (fleet.failure != BotFleetFailure::None) g_metrics().addCounter("playerbots_fleet_reconciliation_failures", 1, { { "reason", std::to_string(static_cast<uint8_t>(fleet.failure)) } });
	return result;
}

void BotFleetTelemetry::record(BotFleetTelemetryFailure failure, std::string detail) {
	if (failure == BotFleetTelemetryFailure::None || stopping) return;
	++failures[static_cast<size_t>(failure)];
	detail.resize(std::min<size_t>(detail.size(), 128));
	const auto lowered = asLowerCaseString(detail);
	if (lowered.contains("password") || lowered.contains("secret") || lowered.contains("token") || lowered.contains("credential")) detail = "[redacted]";
	events.push_back({ ++sequence, failure, std::move(detail) });
	while (events.size() > MaximumEvents) events.pop_front();
	g_metrics().addCounter("playerbots_fleet_admin_failures", 1, { { "reason", std::to_string(static_cast<uint8_t>(failure)) } });
}

std::vector<BotFleetTelemetryEvent> BotFleetTelemetry::recent(size_t limit) const {
	limit = std::min(limit, MaximumEvents);
	const auto first = events.size() > limit ? events.end() - static_cast<ptrdiff_t>(limit) : events.begin();
	return { first, events.end() };
}

void BotFleetTelemetry::stop() { stopping = true; events.clear(); }

BotFleetAdminService::BotFleetAdminService(BotFleetAdministration &administration, BotFleetTelemetry &telemetry, BotFleetAdminServicePolicy policy, AuthenticationFunction authenticate) :
	administration(administration), telemetry(telemetry), policy(policy), authenticate(std::move(authenticate)) { }

BotFleetAdminResponse BotFleetAdminService::handle(const BotManager &manager, BotFleetAdminRequest request) {
	auto reject = [&](BotFleetTelemetryFailure failure) { telemetry.record(failure); BotFleetAdminResponse response; response.failure = failure; return response; };
	if (stopping) return reject(BotFleetTelemetryFailure::Stopping);
	if (policy.transport == BotFleetAdminTransport::Disabled) return reject(BotFleetTelemetryFailure::Disabled);
	if (request.encodedSize > policy.maximumRequestBytes) return reject(BotFleetTelemetryFailure::RequestTooLarge);
	if (!authenticate || !authenticate(request.operatorId, request.credential)) return reject(BotFleetTelemetryFailure::Unauthorized);
	request.credential.clear();
	if ((request.submittedAtTick != 0 || request.handledAtTick != 0)
	    && (request.handledAtTick < request.submittedAtTick || request.handledAtTick - request.submittedAtTick > policy.maximumRequestAgeTicks)) {
		return reject(BotFleetTelemetryFailure::TimedOut);
	}
	if (request.window != activeWindow) { activeWindow = request.window; requestsInWindow = 0; }
	if (requestsInWindow >= policy.maximumRequestsPerWindow) return reject(BotFleetTelemetryFailure::RateLimited);
	++requestsInWindow;
	BotFleetAdminResponse response;
	response.snapshot = telemetry.collect(manager, administration);
	if (request.operation == BotFleetAdminOperation::Audit) {
		const auto limit = std::min<size_t>(request.limit, policy.maximumAuditResults);
		const auto &source = administration.audit();
		const auto first = source.size() > limit ? source.end() - static_cast<ptrdiff_t>(limit) : source.begin();
		response.audit.assign(first, source.end());
	} else if (request.operation == BotFleetAdminOperation::Events) {
		response.events = telemetry.recent(std::min<size_t>(request.limit, policy.maximumEventResults));
	} else if (request.operation == BotFleetAdminOperation::Command) {
		request.command.operatorId = request.operatorId;
		response.command = administration.submit(std::move(request.command), true);
	}
	size_t responseSize = sizeof(BotFleetTelemetrySnapshot) + sizeof(BotFleetCommand);
	for (const auto &entry : response.audit) responseSize += sizeof(entry) + entry.detail.size();
	for (const auto &entry : response.events) responseSize += sizeof(entry) + entry.detail.size();
	responseSize += response.command.memberName.size() + response.command.reason.size();
	if (responseSize > std::numeric_limits<uint32_t>::max()) return reject(BotFleetTelemetryFailure::ResponseTooLarge);
	response.encodedSize = static_cast<uint32_t>(responseSize);
	if (response.encodedSize > policy.maximumResponseBytes) return reject(BotFleetTelemetryFailure::ResponseTooLarge);
	return response;
}

void BotFleetAdminService::stop() { stopping = true; authenticate = {}; }

BotFleetLocalSoakAdapter::BotFleetLocalSoakAdapter(std::filesystem::path output, BotManager &manager, BotFleetAdministration &administration, BotFleetTelemetry &telemetry) :
	directory(std::move(output)), manager(manager), administration(administration), telemetry(telemetry) {
	std::error_code error;
	if (!std::filesystem::is_directory(directory, error) || error) return;
	const auto permissions = std::filesystem::status(directory, error).permissions();
	if (error || (permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) != std::filesystem::perms::none) return;
	enabled = true;
}

void BotFleetLocalSoakAdapter::sample(const dispatcher::telemetry::LatencySnapshot &latency, uint64_t timestampMilliseconds) {
	if (!enabled) return;
	const auto snapshot = telemetry.collect(manager, administration);
	json managedMembers = json::array();
	for (const auto &member : manager.managedSessionIdentities()) {
		managedMembers.push_back({ { "id", member.memberId }, { "name", member.name }, { "generation", member.sessionGeneration }, { "authoritativelyPlaced", member.authoritativelyPlaced } });
	}
	json fleet {
		{ "timestampMilliseconds", timestampMilliseconds }, { "snapshotRevision", snapshot->sequence },
		{ "configurationRevision", snapshot->policyRevision }, { "desiredPopulation", snapshot->desiredPopulation },
		{ "hardMaximum", snapshot->hardMaximum }, { "managedSessions", snapshot->managedSessions },
		{ "ordinaryPlayers", snapshot->ordinaryPlayers }, { "placed", snapshot->placed },
		{ "offline", snapshot->lifecycleCounts[static_cast<size_t>(BotFleetMemberState::Offline)] },
		{ "loginQueued", snapshot->lifecycleCounts[static_cast<size_t>(BotFleetMemberState::LoginQueued)] },
		{ "loading", snapshot->lifecycleCounts[static_cast<size_t>(BotFleetMemberState::Loading)] },
		{ "placementPending", snapshot->lifecycleCounts[static_cast<size_t>(BotFleetMemberState::PlacementPending)] },
		{ "draining", snapshot->lifecycleCounts[static_cast<size_t>(BotFleetMemberState::DrainRequested)] },
		{ "saving", snapshot->lifecycleCounts[static_cast<size_t>(BotFleetMemberState::Saving)] },
		{ "logoutPending", snapshot->lifecycleCounts[static_cast<size_t>(BotFleetMemberState::LogoutPending)] },
		{ "failed", snapshot->lifecycleCounts[static_cast<size_t>(BotFleetMemberState::Failed)] },
		{ "duplicateSessions", snapshot->duplicateSessions }, { "lifecycleQueueDepth", snapshot->pendingLogins + snapshot->pendingLogouts },
		{ "commandQueueDepth", snapshot->queuedCommands }, { "coordinationGroups", snapshot->coordinationGroups },
		{ "coordinationReservations", snapshot->coordinationReservations },
		{ "controllerState", static_cast<uint8_t>(snapshot->controllerState) }, { "pressureState", static_cast<uint8_t>(snapshot->pressureState) },
		{ "managedMembers", std::move(managedMembers) }
	};
	std::ofstream fleetOutput(directory / "fleet-snapshots.jsonl", std::ios::app);
	if (!fleetOutput) { enabled = false; return; }
	fleetOutput << fleet.dump() << '\n';
	std::ofstream lifecycleOutput(directory / "managed-lifecycle.jsonl", std::ios::app);
	if (!lifecycleOutput) { enabled = false; return; }
	for (const auto &event : manager.managedSessionEventsAfter(lifecycleEventSequence)) {
		const auto action = event.action == BotManagedSessionAction::Login ? "login" : event.action == BotManagedSessionAction::Logout ? "logout" : "unexpected_loss";
		const auto reason = event.reason == BotManagedSessionReason::Reconciliation ? "reconciliation" : event.reason == BotManagedSessionReason::Shutdown ? "shutdown" : event.reason == BotManagedSessionReason::WorldRemoval ? "world_removal" : "explicit";
		lifecycleOutput << json { { "timestampMilliseconds", timestampMilliseconds }, { "sequence", event.sequence }, { "action", action }, { "reason", reason },
		                         { "memberId", event.memberId }, { "name", event.name }, { "sessionGeneration", event.sessionGeneration } }.dump() << '\n';
		lifecycleEventSequence = event.sequence;
	}
	json ticks {
		{ "timestampMilliseconds", timestampMilliseconds }, { "samples", latency.samples },
		{ "bucketUpperBoundsUs", dispatcher::telemetry::LATENCY_BUCKET_UPPER_BOUNDS_US }, { "buckets", latency.buckets },
		{ "p50Us", latency.percentile(0.50).count() }, { "p95Us", latency.percentile(0.95).count() },
		{ "p99Us", latency.percentile(0.99).count() }, { "maxUs", latency.maxUs },
		{ "placed", snapshot->placed }, { "ordinaryPlayers", snapshot->ordinaryPlayers },
		{ "pressureState", static_cast<uint8_t>(snapshot->pressureState) }
	};
	std::ofstream tickOutput(directory / "ticks.jsonl", std::ios::app);
	if (!tickOutput) { enabled = false; return; }
	tickOutput << ticks.dump() << '\n';
	pollCommands(timestampMilliseconds);
}

void BotFleetLocalSoakAdapter::pollCommands(uint64_t timestampMilliseconds) {
	const auto path = directory / "command-requests.jsonl";
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error) return;
	if (size > MaximumCommandFileBytes || size < commandOffset) { enabled = false; return; }
	std::ifstream input(path);
	input.seekg(static_cast<std::streamoff>(commandOffset));
	std::string line;
	while (std::getline(input, line)) {
		if (line.size() > 4096) { enabled = false; return; }
		try {
			const auto request = json::parse(line);
			BotFleetCommand command;
			const auto requestId = request.at("requestId").get<uint64_t>();
			if (std::ranges::contains(requestIds, requestId)) {
				telemetry.record(BotFleetTelemetryFailure::InvalidRequest, "duplicate soak request id");
				std::ofstream duplicateOutput(directory / "commands.jsonl", std::ios::app);
				if (!duplicateOutput) { enabled = false; return; }
				duplicateOutput << json { { "timestampMilliseconds", timestampMilliseconds }, { "requestId", requestId }, { "phase", "rejected" }, { "failure", "duplicate_request_id" } }.dump() << '\n';
				continue;
			}
			requestIds.push_back(requestId);
			while (requestIds.size() > BotFleetAdministration::MaximumAuditEntries) requestIds.pop_front();
			command.value = request.value("value", 0U);
			command.memberName = request.value("memberName", std::string {});
			const auto operation = request.at("operation").get<std::string>();
			if (operation == "status") command.type = BotFleetCommandType::Status;
			else if (operation == "pause") command.type = BotFleetCommandType::Pause;
			else if (operation == "resume") command.type = BotFleetCommandType::Resume;
			else if (operation == "drain") command.type = BotFleetCommandType::Drain;
			else if (operation == "reload") command.type = BotFleetCommandType::Reload;
			else if (operation == "setDesired") command.type = BotFleetCommandType::SetDesired;
			else continue;
			const auto accepted = administration.submit(command, true);
			std::ofstream output(directory / "commands.jsonl", std::ios::app);
			if (!output) { enabled = false; return; }
			output << json { { "timestampMilliseconds", timestampMilliseconds }, { "requestId", requestId }, { "commandId", accepted.id }, { "phase", "accepted" }, { "state", static_cast<uint8_t>(accepted.state) }, { "failure", static_cast<uint8_t>(accepted.failure) } }.dump() << '\n';
			for (const auto &completed : administration.process(manager)) {
				output << json { { "timestampMilliseconds", timestampMilliseconds }, { "requestId", completed.id == accepted.id ? requestId : 0 }, { "commandId", completed.id }, { "phase", "completed" }, { "state", static_cast<uint8_t>(completed.state) }, { "failure", static_cast<uint8_t>(completed.failure) } }.dump() << '\n';
			}
		} catch (const json::exception &) {
			telemetry.record(BotFleetTelemetryFailure::InvalidRequest, "malformed soak command");
		}
	}
	commandOffset = static_cast<uintmax_t>(input.tellg());
	if (input.fail() && input.eof()) commandOffset = size;
}

void BotFleetLocalSoakAdapter::stop() { enabled = false; }
