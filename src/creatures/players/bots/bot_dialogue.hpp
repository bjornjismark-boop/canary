/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */
#pragma once

#include "game/movement/position.hpp"

class Player;

enum class BotDialogueState:uint8_t{Idle,NpcSelected,Approaching,GreetingPending,AwaitingResponse,TopicActive,ReplyPending,ConversationComplete,ConversationLost,TimedOut,Backoff,Failed,Cancelled};
enum class BotDialogueIntent:uint8_t{None,Greeting,Farewell,Yes,No,Trade,ConfiguredTopic,ConfiguredFollowUp,Cancel};
enum class BotDialogueResponse:uint8_t{None,GreetingAccepted,GreetingRejected,TopicAccepted,TopicRejected,ConfirmationRequested,ConversationComplete,NpcBusy,NpcUnavailable,OutOfRange,FocusLost,UnexpectedResponse,NoResponse,TimedOut,RetryScheduled,RetryExhausted,Cancelled};
enum class BotDialogueFailure:uint8_t{None,InvalidLifecycle,NpcHidden,DifferentFloor,OutOfRange,UnconfiguredNpc,UnconfiguredPhrase,PhraseLimit,TopicLimit,ResponseLimit,FocusLost,NpcUnavailable,TimedOut,RetryExhausted,Cancelled};

struct BotNpcObservation{uint32_t npcId=0;std::string normalizedName;Position position;uint32_t distance=0;bool visible=false;bool sameFloor=false;bool focused=false;uint64_t revision=0;[[nodiscard]]bool containsWorldOwnership()const{return false;}auto operator<=>(const BotNpcObservation&)const=default;};
struct BotDialoguePhrase{BotDialogueIntent intent=BotDialogueIntent::None;std::string text;std::string expectedToken;auto operator<=>(const BotDialoguePhrase&)const=default;};
struct BotNpcDialoguePolicy{std::vector<std::string> configuredNpcNames;std::vector<BotDialoguePhrase> phrases;uint8_t maximumPhrases=8;uint8_t maximumRetries=2;uint8_t maximumTopics=8;uint16_t maximumResponseLength=512;std::chrono::milliseconds responseWait{2000};std::chrono::milliseconds initialBackoff{250};std::chrono::milliseconds maximumBackoff{1000};};
struct BotDialogueObservation{uint64_t revision=0;std::vector<BotNpcObservation> npcs;[[nodiscard]]bool containsWorldOwnership()const{return false;}};
struct BotDialogueResponseObservation{uint64_t revision=0;uint32_t npcId=0;BotDialogueResponse category=BotDialogueResponse::None;uint64_t normalizedTokenHash=0;std::string normalizedToken;auto operator<=>(const BotDialogueResponseObservation&)const=default;};
struct BotConversationResult{BotDialogueState state=BotDialogueState::Idle;BotDialogueResponse response=BotDialogueResponse::None;BotDialogueFailure failure=BotDialogueFailure::None;uint32_t npcId=0;BotDialogueIntent intent=BotDialogueIntent::None;uint8_t phrasesSent=0;uint8_t attempts=0;uint64_t responseRevision=0;uint64_t responseTokenHash=0;auto operator<=>(const BotConversationResult&)const=default;};
struct BotDialogueProgress{BotDialogueState state=BotDialogueState::Idle;uint32_t npcId=0;BotDialogueIntent intent=BotDialogueIntent::None;uint8_t phrasesSent=0;uint8_t attempts=0;uint64_t speechRevision=0;std::chrono::milliseconds sentAt{0};std::chrono::milliseconds nextAttemptAt{0};bool containsWorldOwnership=false;};

class BotDialogue final{public:static BotDialogueObservation observe(const std::shared_ptr<Player>&,const BotNpcDialoguePolicy& = {});static std::optional<BotNpcObservation> select(const BotDialogueObservation&,const BotNpcDialoguePolicy&);static BotDialogueResponseObservation response(const std::shared_ptr<Player>&,uint32_t,uint64_t,const BotDialoguePhrase&,uint16_t);static const BotDialoguePhrase* phrase(const BotNpcDialoguePolicy&,BotDialogueIntent);static std::chrono::milliseconds backoff(const BotNpcDialoguePolicy&,uint8_t);};
