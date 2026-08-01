/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_fleet_telemetry.hpp"

#include "creatures/players/bots/bot_manager.hpp"
#include "lib/metrics/metrics.hpp"
#include "utils/tools.hpp"

std::shared_ptr<const BotFleetTelemetrySnapshot> BotFleetTelemetry::collect(const BotManager &manager, const BotFleetAdministration &administration) {
	const auto &fleet = manager.lastFleetReconciliation();
	auto result = std::make_shared<BotFleetTelemetrySnapshot>();
	result->sequence = ++sequence;
	if (const auto active = administration.activeRevision()) result->policyRevision = active->revision;
	result->controllerState = manager.fleetState().state;
	result->reconciliationFailure = fleet.failure;
	result->managedSessions = static_cast<uint32_t>(manager.size());
	result->placed = fleet.placed;
	result->pendingLogins = fleet.pendingLogins;
	result->pendingLogouts = fleet.pendingLogouts;
	result->plannerHealthy = result->managedSessions;
	result->coordinationGroups = static_cast<uint32_t>(manager.coordinationGroupCount());
	result->coordinationReservations = static_cast<uint32_t>(manager.coordinationReservationCount());
	result->queuedCommands = static_cast<uint32_t>(administration.queued());
	result->auditEntries = static_cast<uint32_t>(administration.audit().size());
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
