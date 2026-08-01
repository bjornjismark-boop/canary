/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */
#pragma once

#include "creatures/players/bots/bot_plan_execution.hpp"

enum class BotCheckpointLoadReason : uint8_t { Valid, Missing, Corrupt, UnsupportedVersion, PolicyRevisionChanged, PlanMissing, GoalUnavailable, StepOutOfRange, PostconditionInvalid, PlayerMismatch, UnsafeBoundary, ObservationRequired, PersistenceUnavailable };
enum class BotPlannerMigrationResult : uint8_t { Current, Migrated, UnsupportedOlderVersion, UnsupportedFutureVersion, Corrupt, MissingRequiredField, PlayerMismatch, PolicyRevalidationRequired, PlanRevalidationRequired, PostconditionVerificationRequired, Rejected };
enum class BotPlannerDiagnosticType : uint8_t { GoalSelected, PlanSelected, StepDelegated, StepVerified, CheckpointCreated, CheckpointPersisted, CheckpointLoaded, CheckpointRejected, ReplanReason, RecoverySelected, TerminalResult };
enum class BotLongCampaignState : uint8_t { Idle, Running, Success, BudgetReached, Failed, Cancelled };

struct BotPersistedPlanCheckpoint {
	uint32_t playerId = 0; uint16_t schemaVersion = 2; uint64_t checkpointRevision = 0; uint64_t policyRevision = 0; BotGoalId goalId = 0; BotGoalType goalType = BotGoalType::IdleSafely; uint64_t planRevision = 0; uint16_t verifiedStepIndex = 0; BotPlanSubsystem verifiedSubsystem = BotPlanSubsystem::None; uint8_t failureCount = 0; uint8_t retryCount = 0; uint64_t configuredTargetId = 0; Position configuredRegion; bool safeSaveBoundary = false; uint64_t checksum = 0;
	[[nodiscard]] bool containsWorldOwnership() const { return false; }
	bool operator==(const BotPersistedPlanCheckpoint &) const = default;
};
struct BotCheckpointLoadResult { BotCheckpointLoadReason reason = BotCheckpointLoadReason::Missing; std::optional<BotPersistedPlanCheckpoint> checkpoint; bool freshObservationRequired = true; bool durable = false; };
struct BotCheckpointBoundary {
	bool lifecyclePlaced = true; bool stepVerified = true; bool combatPending = false; bool movementPending = false; bool lootPending = false; bool dialoguePending = false; bool shopPending = false; bool depotPending = false; bool equipmentPending = false; bool policyKnown = true; bool planKnown = true;
	[[nodiscard]] bool safe() const { return lifecyclePlaced && stepVerified && policyKnown && planKnown && !combatPending && !movementPending && !lootPending && !dialoguePending && !shopPending && !depotPending && !equipmentPending; }
};
struct BotCheckpointValidation { uint32_t playerId = 0; uint16_t schemaVersion = 2; uint64_t policyRevision = 1; std::vector<BotGoalId> goalIds; std::vector<uint64_t> planRevisions; uint16_t planStepCount = 0; bool postconditionValid = false; bool observationFresh = false; };
struct BotPlannerMigration { BotPlannerMigrationResult result = BotPlannerMigrationResult::Rejected; std::optional<BotPersistedPlanCheckpoint> checkpoint; bool freshObservationRequired = true; uint8_t migratedFields = 0; auto operator<=>(const BotPlannerMigration &) const = default; };
struct BotPlannerDiagnostic { BotPlannerDiagnosticType type = BotPlannerDiagnosticType::GoalSelected; uint64_t revision = 0; uint64_t value = 0; auto operator<=>(const BotPlannerDiagnostic &) const = default; };
struct BotPlannerDiagnostics { uint16_t maximumEntries = 32; std::vector<BotPlannerDiagnostic> entries; void record(BotPlannerDiagnostic); void clear() { entries.clear(); } };
struct BotLongCampaignBudget { uint32_t maximumPlannerTicks = 4096; uint32_t maximumAuthoritativeTime = 3600000; uint16_t maximumCompletedGoals = 16; uint16_t maximumReplans = 16; uint16_t maximumSubsystemFailures = 32; uint16_t maximumConsecutiveIdleCycles = 32; uint16_t maximumPersistedCheckpoints = 64; uint16_t maximumSaveCycles = 16; uint16_t maximumSessionReconstructions = 16; uint16_t requiredCompletedGoals = 3; };
struct BotLongCampaignProgress { BotLongCampaignState state = BotLongCampaignState::Idle; uint32_t plannerTicks = 0; uint32_t authoritativeTime = 0; uint16_t completedGoals = 0; uint16_t replans = 0; uint16_t subsystemFailures = 0; uint16_t consecutiveIdleCycles = 0; uint16_t persistedCheckpoints = 0; uint16_t saveCycles = 0; uint16_t sessionReconstructions = 0; bool containsWorldOwnership = false; auto operator<=>(const BotLongCampaignProgress &) const = default; };

class BotPlannerPersistence final {
public:
	[[nodiscard]] static BotPersistedPlanCheckpoint capture(uint32_t, const BotPlanExecution &, BotPlanSubsystem, const BotCheckpointBoundary & = {});
	[[nodiscard]] static uint64_t checksum(const BotPersistedPlanCheckpoint &);
	[[nodiscard]] static bool persist(const BotPersistedPlanCheckpoint &);
	[[nodiscard]] static BotCheckpointLoadResult load(uint32_t);
	[[nodiscard]] static bool erase(uint32_t);
	[[nodiscard]] static BotCheckpointLoadResult acknowledge(BotPersistedPlanCheckpoint, bool persistenceSucceeded);
	[[nodiscard]] static BotCheckpointLoadResult validate(BotPersistedPlanCheckpoint, const BotCheckpointValidation &);
	[[nodiscard]] static BotPlannerMigration migrate(BotPersistedPlanCheckpoint, uint32_t expectedPlayerId);
	[[nodiscard]] static BotLongCampaignProgress advance(BotLongCampaignProgress, const BotLongCampaignBudget &, uint32_t authoritativeDelta, bool goalCompleted, bool replanned, bool subsystemFailed, bool idle, bool checkpointPersisted, bool saveCycle, bool reconstructed);
};
