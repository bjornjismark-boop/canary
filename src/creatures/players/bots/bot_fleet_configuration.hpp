/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_fleet.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <deque>
	#include <functional>
	#include <memory>
	#include <string>
#endif

class BotManager;

enum class BotFleetConfigurationFailure : uint8_t { None, FileUnavailable, ParseFailed, UnsupportedSchema, InvalidRevision, InvalidPolicy, InvalidMember, SecretRejected };
enum class BotFleetCommandType : uint8_t { Status, Pause, Resume, Drain, Reload, SetDesired, LoginMember, LogoutMember };
enum class BotFleetCommandState : uint8_t { Rejected, Accepted, Completed, Failed };
enum class BotFleetCommandFailure : uint8_t { None, Unauthorized, QueueFull, InvalidArgument, RevisionConflict, LifecycleRejected, ReloadFailed, Stopping };

struct BotFleetScheduleWindow {
	uint16_t startMinute = 0;
	uint16_t endMinute = 0;
	uint32_t desiredOnline = 0;
	bool operator==(const BotFleetScheduleWindow &) const = default;
};

struct BotFleetConfigurationRevision {
	uint16_t schemaVersion = 1;
	uint64_t revision = 0;
	BotFleetPopulationPolicy population;
	BotFleetDistributionPolicy distribution;
	std::vector<BotFleetMemberProfile> members;
	std::vector<BotFleetScheduleWindow> schedule;
};

struct BotFleetConfigurationResult {
	BotFleetConfigurationFailure failure = BotFleetConfigurationFailure::None;
	std::shared_ptr<const BotFleetConfigurationRevision> revision;
	std::string detail;
	[[nodiscard]] bool success() const { return failure == BotFleetConfigurationFailure::None && revision; }
};

struct BotFleetChangeSummary {
	uint64_t previousRevision = 0;
	uint64_t activeRevision = 0;
	uint32_t addedMembers = 0;
	uint32_t removedMembers = 0;
	uint32_t changedMembers = 0;
	bool populationChanged = false;
	bool distributionChanged = false;
	bool scheduleChanged = false;
	bool operator==(const BotFleetChangeSummary &) const = default;
};

struct BotFleetCommand {
	uint64_t id = 0;
	BotFleetCommandType type = BotFleetCommandType::Status;
	BotFleetCommandState state = BotFleetCommandState::Rejected;
	BotFleetCommandFailure failure = BotFleetCommandFailure::None;
	uint64_t operatorId = 0;
	uint64_t expectedRevision = 0;
	uint32_t value = 0;
	std::string memberName;
	std::string reason;
};

struct BotFleetAuditEntry {
	uint64_t commandId = 0;
	uint64_t operatorId = 0;
	BotFleetCommandType type = BotFleetCommandType::Status;
	BotFleetCommandState state = BotFleetCommandState::Rejected;
	BotFleetCommandFailure failure = BotFleetCommandFailure::None;
	uint64_t revision = 0;
	std::string detail;
};

class BotFleetConfiguration final {
public:
	static constexpr uint16_t CurrentSchemaVersion = 1;
	static constexpr uint16_t AbsoluteMaximumScheduleWindows = 64;
	[[nodiscard]] static BotFleetConfigurationResult parse(std::string_view jsonText);
	[[nodiscard]] static BotFleetConfigurationResult load(const std::string &path);
	[[nodiscard]] static BotFleetChangeSummary summarize(const BotFleetConfigurationRevision *previous, const BotFleetConfigurationRevision &next);
	[[nodiscard]] static uint32_t scheduledDesired(const BotFleetConfigurationRevision &, uint16_t minuteOfDay);
};

class BotFleetAdministration final {
public:
	static constexpr size_t MaximumQueuedCommands = 64;
	static constexpr size_t MaximumAuditEntries = 256;
	static constexpr size_t MaximumCommandsPerTick = 8;

	using ReloadFunction = std::function<BotFleetConfigurationResult()>;
	explicit BotFleetAdministration(ReloadFunction reload = {});
	[[nodiscard]] BotFleetConfigurationFailure install(BotManager &, std::shared_ptr<const BotFleetConfigurationRevision>);
	[[nodiscard]] BotFleetCommand submit(BotFleetCommand, bool authorized);
	[[nodiscard]] std::vector<BotFleetCommand> process(BotManager &);
	[[nodiscard]] std::shared_ptr<const BotFleetConfigurationRevision> activeRevision() const { return active; }
	[[nodiscard]] const std::deque<BotFleetAuditEntry> &audit() const { return audits; }
	[[nodiscard]] size_t queued() const { return commands.size(); }
	void stop();

private:
	void record(const BotFleetCommand &);
	ReloadFunction reloadFunction;
	std::shared_ptr<const BotFleetConfigurationRevision> active;
	std::deque<BotFleetCommand> commands;
	std::deque<BotFleetAuditEntry> audits;
	uint64_t sequence = 0;
	bool stopping = false;
};
