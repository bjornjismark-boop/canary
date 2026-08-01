#pragma once

#include "creatures/players/bots/bot_progression.hpp"

using BotCoordinationGroupId = uint32_t;
using BotCoordinationMemberId = uint32_t;

enum class BotCoordinationRole:uint8_t{Leader,Frontline,Damage,Support,Healer,Scout,Carrier,Unassigned};
enum class BotCoordinationState:uint8_t{Inactive,Joining,Active,LeaderUnavailable,Regrouping,Engaging,Retreating,Recovering,ObjectiveComplete,Degraded,Leaving,Failed,Cancelled};
enum class BotCoordinationReservationType:uint8_t{CombatTarget,LootCorpse,NpcInteraction,InteractionPoint,QuestObjective,RegroupRegion};
enum class BotCoordinationFailure:uint8_t{None,InvalidGroup,MemberExcluded,HumanControlForbidden,MemberLimit,ReservationLimit,StaleObservation,LeaderUnavailable,RoleIncompatible,Conflict,BudgetReached,Expired,GenerationMismatch,Cancelled};
enum class BotCoordinationIntentType:uint8_t{None,Follow,Formation,Engage,FocusFire,Loot,Retreat,Regroup,SharedObjective};

struct BotCoordinationMemberObservation{BotCoordinationMemberId id=0;uint64_t sessionGeneration=0;uint64_t revision=0;uint16_t vocationId=0;Position position;uint8_t healthPercent=100;bool configured=false;bool partyMember=false;bool playerBot=false;bool alive=true;bool placed=true;bool visible=true;auto operator<=>(const BotCoordinationMemberObservation&)const=default;};
struct BotCoordinationGroupObservation{BotCoordinationGroupId id=0;uint64_t revision=0;std::vector<BotCoordinationMemberObservation> members;bool containsWorldOwnership=false;auto operator<=>(const BotCoordinationGroupObservation&)const=default;};
struct BotCoordinationPolicy{BotCoordinationGroupId id=0;std::vector<BotCoordinationMemberId> configuredMembers;BotCoordinationMemberId configuredLeader=0;bool fallbackElection=true;bool focusFire=false;uint64_t revision=0;uint8_t maximumMembers=8;uint8_t maximumReservations=16;uint8_t maximumLeaderChanges=4;uint8_t maximumRegroupAttempts=3;uint16_t maximumReservationDuration=32;};
struct BotCoordinationIntent{BotCoordinationMemberId memberId=0;BotCoordinationIntentType type=BotCoordinationIntentType::None;uint64_t targetSignature=0;Position region;int8_t offsetX=0;int8_t offsetY=0;uint64_t policyRevision=0;auto operator<=>(const BotCoordinationIntent&)const=default;};
struct BotCoordinationReservation{BotCoordinationGroupId groupId=0;BotCoordinationMemberId memberId=0;BotCoordinationReservationType type=BotCoordinationReservationType::CombatTarget;uint64_t targetSignature=0;uint64_t observationRevision=0;uint64_t sessionGeneration=0;uint64_t expiresAt=0;uint64_t policyRevision=0;auto operator<=>(const BotCoordinationReservation&)const=default;};
struct BotCoordinationBudget{uint8_t maximumEventsPerTick=16;uint8_t maximumIntentsPerMember=2;uint8_t maximumLeaderChanges=4;uint8_t maximumRegroupAttempts=3;};
struct BotCoordinationDecision{BotCoordinationState state=BotCoordinationState::Inactive;BotCoordinationFailure failure=BotCoordinationFailure::None;BotCoordinationMemberId leaderId=0;std::vector<std::pair<BotCoordinationMemberId,BotCoordinationRole>> roles;std::vector<BotCoordinationIntent> intents;bool localTargetValidationRequired=true;bool directGameplayAction=false;auto operator<=>(const BotCoordinationDecision&)const=default;};

class BotCoordination final{
public:
	static BotCoordinationDecision evaluate(const BotCoordinationPolicy&,const BotCoordinationGroupObservation&,BotCoordinationMemberId previousLeader=0,uint8_t leaderChanges=0);
	static BotCoordinationFailure reserve(std::vector<BotCoordinationReservation>&,BotCoordinationReservation,const BotCoordinationPolicy&,uint64_t now);
	static void invalidate(std::vector<BotCoordinationReservation>&,BotCoordinationMemberId,uint64_t sessionGeneration=0);
	static void expire(std::vector<BotCoordinationReservation>&,uint64_t now);
};
