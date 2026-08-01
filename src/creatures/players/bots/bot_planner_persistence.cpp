/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */
#include "creatures/players/bots/bot_planner_persistence.hpp"

#include "database/database.hpp"

namespace { template<typename T> void cappedIncrement(T &value, bool increment) { if (increment && value != std::numeric_limits<T>::max()) ++value; } }

void BotPlannerDiagnostics::record(BotPlannerDiagnostic value) { if (maximumEntries == 0) return; if (entries.size() >= maximumEntries) entries.erase(entries.begin()); entries.push_back(value); }

uint64_t BotPlannerPersistence::checksum(const BotPersistedPlanCheckpoint &c) { uint64_t h=1469598103934665603ULL; auto mix=[&](uint64_t v){h^=v;h*=1099511628211ULL;};mix(c.playerId);mix(c.schemaVersion);mix(c.checkpointRevision);mix(c.policyRevision);mix(c.goalId);mix(static_cast<uint8_t>(c.goalType));mix(c.planRevision);mix(c.verifiedStepIndex);mix(static_cast<uint8_t>(c.verifiedSubsystem));mix(c.failureCount);mix(c.retryCount);mix(c.configuredTargetId);mix(c.configuredRegion.x);mix(c.configuredRegion.y);mix(c.configuredRegion.z);mix(c.safeSaveBoundary);return h; }

BotPersistedPlanCheckpoint BotPlannerPersistence::capture(uint32_t playerId, const BotPlanExecution &execution, BotPlanSubsystem subsystem, const BotCheckpointBoundary &boundary) {
	BotPersistedPlanCheckpoint checkpoint;
	checkpoint.playerId = playerId;
	checkpoint.checkpointRevision = execution.decision.checkpoint.observationRevision;
	checkpoint.policyRevision = execution.decision.checkpoint.policyRevision;
	checkpoint.goalId = execution.decision.goalId;
	checkpoint.goalType = execution.decision.goalType;
	checkpoint.planRevision = execution.decision.plan.revision;
	checkpoint.verifiedStepIndex = execution.decision.checkpoint.verifiedStepIndex;
	checkpoint.verifiedSubsystem = subsystem;
	checkpoint.failureCount = execution.decision.checkpoint.failureCount;
	checkpoint.retryCount = execution.decision.checkpoint.retryCount;
	checkpoint.safeSaveBoundary = execution.decision.checkpoint.safeSaveBoundary && !execution.delegatedStep && boundary.safe();
	if (checkpoint.verifiedStepIndex < execution.decision.plan.steps.size()) {
		const auto &step = execution.decision.plan.steps[checkpoint.verifiedStepIndex];
		checkpoint.configuredTargetId = step.configuredTargetId;
		checkpoint.configuredRegion = step.region;
	}
	checkpoint.checksum = checksum(checkpoint);
	return checkpoint;
}

bool BotPlannerPersistence::persist(const BotPersistedPlanCheckpoint &c){if(c.playerId==0||c.schemaVersion!=1||c.checkpointRevision==0||c.policyRevision==0||c.goalId==0||c.goalType==BotGoalType::Unsupported||c.planRevision==0||c.verifiedSubsystem>BotPlanSubsystem::Lifecycle||!c.safeSaveBoundary||c.checksum!=checksum(c))return false;std::ostringstream q;q<<"INSERT INTO `player_bot_planner_state` (`player_id`,`schema_version`,`checkpoint_revision`,`policy_revision`,`goal_id`,`goal_type`,`plan_revision`,`verified_step_index`,`verified_subsystem`,`failure_count`,`retry_count`,`configured_target_id`,`region_x`,`region_y`,`region_z`,`safe_boundary`,`checksum`) VALUES ("<<c.playerId<<","<<c.schemaVersion<<","<<c.checkpointRevision<<","<<c.policyRevision<<","<<c.goalId<<","<<static_cast<uint16_t>(c.goalType)<<","<<c.planRevision<<","<<c.verifiedStepIndex<<","<<static_cast<uint16_t>(c.verifiedSubsystem)<<","<<static_cast<uint16_t>(c.failureCount)<<","<<static_cast<uint16_t>(c.retryCount)<<","<<c.configuredTargetId<<","<<c.configuredRegion.x<<","<<c.configuredRegion.y<<","<<static_cast<uint16_t>(c.configuredRegion.z)<<",1,"<<c.checksum<<") ON DUPLICATE KEY UPDATE `schema_version`=VALUES(`schema_version`),`checkpoint_revision`=VALUES(`checkpoint_revision`),`policy_revision`=VALUES(`policy_revision`),`goal_id`=VALUES(`goal_id`),`goal_type`=VALUES(`goal_type`),`plan_revision`=VALUES(`plan_revision`),`verified_step_index`=VALUES(`verified_step_index`),`verified_subsystem`=VALUES(`verified_subsystem`),`failure_count`=VALUES(`failure_count`),`retry_count`=VALUES(`retry_count`),`configured_target_id`=VALUES(`configured_target_id`),`region_x`=VALUES(`region_x`),`region_y`=VALUES(`region_y`),`region_z`=VALUES(`region_z`),`safe_boundary`=VALUES(`safe_boundary`),`checksum`=VALUES(`checksum`)";return g_database().executeQuery(q.str());}

BotCheckpointLoadResult BotPlannerPersistence::load(uint32_t id){if(id==0){BotCheckpointLoadResult result;result.reason=BotCheckpointLoadReason::PlayerMismatch;return result;}std::ostringstream q;q<<"SELECT * FROM `player_bot_planner_state` WHERE `player_id`="<<id;auto r=g_database().storeQuery(q.str());if(!r)return{};BotPersistedPlanCheckpoint c{.playerId=r->getNumber<uint32_t>("player_id"),.schemaVersion=r->getNumber<uint16_t>("schema_version"),.checkpointRevision=r->getNumber<uint64_t>("checkpoint_revision"),.policyRevision=r->getNumber<uint64_t>("policy_revision"),.goalId=r->getNumber<uint64_t>("goal_id"),.goalType=r->getNumber<BotGoalType>("goal_type"),.planRevision=r->getNumber<uint64_t>("plan_revision"),.verifiedStepIndex=r->getNumber<uint16_t>("verified_step_index"),.verifiedSubsystem=r->getNumber<BotPlanSubsystem>("verified_subsystem"),.failureCount=r->getNumber<uint8_t>("failure_count"),.retryCount=r->getNumber<uint8_t>("retry_count"),.configuredTargetId=r->getNumber<uint64_t>("configured_target_id"),.configuredRegion=Position(r->getNumber<uint16_t>("region_x"),r->getNumber<uint16_t>("region_y"),r->getNumber<uint8_t>("region_z")),.safeSaveBoundary=r->getNumber<uint8_t>("safe_boundary")!=0,.checksum=r->getNumber<uint64_t>("checksum")};return{.reason=c.checksum==checksum(c)?BotCheckpointLoadReason::Valid:BotCheckpointLoadReason::Corrupt,.checkpoint=c,.durable=true};}

bool BotPlannerPersistence::erase(uint32_t id){return id!=0&&g_database().executeQuery("DELETE FROM `player_bot_planner_state` WHERE `player_id`="+std::to_string(id));}

BotCheckpointLoadResult BotPlannerPersistence::acknowledge(BotPersistedPlanCheckpoint checkpoint, bool persistenceSucceeded) {
	BotCheckpointLoadResult result;
	result.reason = persistenceSucceeded ? BotCheckpointLoadReason::Valid : BotCheckpointLoadReason::PersistenceUnavailable;
	result.durable = persistenceSucceeded;
	if (persistenceSucceeded) result.checkpoint = std::move(checkpoint);
	return result;
}

BotCheckpointLoadResult BotPlannerPersistence::validate(BotPersistedPlanCheckpoint c,const BotCheckpointValidation&v){auto reject=[](BotCheckpointLoadReason reason){BotCheckpointLoadResult result;result.reason=reason;return result;};if(c.checksum!=checksum(c)||c.playerId==0||c.checkpointRevision==0||c.policyRevision==0||c.goalId==0||c.planRevision==0||c.goalType>BotGoalType::Unsupported||c.verifiedSubsystem>BotPlanSubsystem::Lifecycle)return reject(BotCheckpointLoadReason::Corrupt);if(c.playerId!=v.playerId)return reject(BotCheckpointLoadReason::PlayerMismatch);if(c.schemaVersion!=v.schemaVersion)return reject(BotCheckpointLoadReason::UnsupportedVersion);if(c.policyRevision!=v.policyRevision)return reject(BotCheckpointLoadReason::PolicyRevisionChanged);if(!std::ranges::contains(v.goalIds,c.goalId))return reject(BotCheckpointLoadReason::GoalUnavailable);if(!std::ranges::contains(v.planRevisions,c.planRevision))return reject(BotCheckpointLoadReason::PlanMissing);if(c.verifiedStepIndex>v.planStepCount)return reject(BotCheckpointLoadReason::StepOutOfRange);if(!c.safeSaveBoundary)return reject(BotCheckpointLoadReason::UnsafeBoundary);if(!v.observationFresh)return reject(BotCheckpointLoadReason::ObservationRequired);if(!v.postconditionValid)return reject(BotCheckpointLoadReason::PostconditionInvalid);return{.reason=BotCheckpointLoadReason::Valid,.checkpoint=c,.freshObservationRequired=false,.durable=false};}

BotLongCampaignProgress BotPlannerPersistence::advance(BotLongCampaignProgress p,const BotLongCampaignBudget&b,uint32_t dt,bool goal,bool replan,bool failure,bool idle,bool persisted,bool save,bool reconstructed){if(p.state==BotLongCampaignState::Success||p.state==BotLongCampaignState::BudgetReached||p.state==BotLongCampaignState::Failed||p.state==BotLongCampaignState::Cancelled)return p;p.state=BotLongCampaignState::Running;cappedIncrement(p.plannerTicks,true);p.authoritativeTime=dt>UINT32_MAX-p.authoritativeTime?UINT32_MAX:p.authoritativeTime+dt;cappedIncrement(p.completedGoals,goal);cappedIncrement(p.replans,replan);cappedIncrement(p.subsystemFailures,failure);p.consecutiveIdleCycles=idle&&p.consecutiveIdleCycles!=UINT16_MAX?p.consecutiveIdleCycles+1:0;cappedIncrement(p.persistedCheckpoints,persisted);cappedIncrement(p.saveCycles,save);cappedIncrement(p.sessionReconstructions,reconstructed);if(p.completedGoals>=b.requiredCompletedGoals){p.state=BotLongCampaignState::Success;return p;}if(p.plannerTicks>=b.maximumPlannerTicks||p.authoritativeTime>=b.maximumAuthoritativeTime||p.completedGoals>=b.maximumCompletedGoals||p.replans>=b.maximumReplans||p.subsystemFailures>=b.maximumSubsystemFailures||p.consecutiveIdleCycles>=b.maximumConsecutiveIdleCycles||p.persistedCheckpoints>=b.maximumPersistedCheckpoints||p.saveCycles>=b.maximumSaveCycles||p.sessionReconstructions>=b.maximumSessionReconstructions)p.state=BotLongCampaignState::BudgetReached;return p;}
