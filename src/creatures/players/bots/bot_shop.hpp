/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */
#pragma once

#include "creatures/players/bots/bot_equipment.hpp"
#include "game/movement/position.hpp"

class Player;

enum class BotShopState : uint8_t { Idle, ApproachingNpc, ConversationStarting, ShopOpening, ObservingOffers, PlanningTransaction, TransactionPending, VerifyingResult, ShopClosing, Completed, Backoff, Failed, Cancelled };
enum class BotShopTransactionKind : uint8_t { Buy, Sell };
enum class BotShopOutcome : uint8_t { Succeeded, Partial, Pending, NoEffect, OfferUnavailable, PriceChanged, InsufficientMoney, CapacityInsufficient, DestinationFull, ItemUnavailable, ReservedItem, RequirementNotMet, NpcUnavailable, ConversationLost, ShopClosed, WorldRejected, TimedOut, RetryScheduled, RetryExhausted, Cancelled };
enum class BotShopFailure : uint8_t { None, InvalidLifecycle, StaleObservation, HiddenOffer, PriceChanged, AmountChanged, InsufficientMoney, CapacityInsufficient, ItemUnavailable, ReservedItem, EquippedItem, NpcUnavailable, ShopClosed, TimedOut, RetryExhausted, Cancelled };

struct BotShopOffer { uint16_t itemTypeId=0; uint8_t subType=0; uint32_t buyPrice=0; uint32_t sellPrice=0; uint32_t unitWeight=0; uint64_t signature=0; auto operator<=>(const BotShopOffer &) const=default; };
struct BotMoneyObservation { uint64_t carried=0; uint64_t bank=0; uint64_t total=0; uint64_t revision=0; auto operator<=>(const BotMoneyObservation &) const=default; };
struct BotShopObservation {
	uint32_t npcId=0; uint64_t revision=0; Position npcPosition; std::vector<BotShopOffer> offers; BotMoneyObservation money; uint64_t inventorySignature=0; uint32_t freeCapacity=0; bool open=false; bool npcVisible=false; bool containsWorldOwnership=false;
	auto operator<=>(const BotShopObservation &) const=default;
};
struct BotShopPolicy { uint16_t maximumAmount=100; uint32_t maximumSupplyCount=1000; uint8_t maximumRetries=3; std::chrono::milliseconds timeout{2000}; std::chrono::milliseconds initialBackoff{200}; std::chrono::milliseconds maximumBackoff{1600}; std::vector<uint16_t> reservedItemTypeIds; std::vector<std::pair<uint16_t,uint32_t>> retainedMinimums; };
struct BotShopTransactionRequest { BotShopTransactionKind kind=BotShopTransactionKind::Buy; uint32_t npcId=0; uint16_t itemTypeId=0; uint8_t subType=0; uint16_t amount=0; uint32_t observedPrice=0; uint64_t shopRevision=0; bool inBackpacks=false; bool ignoreCapacity=false; auto operator<=>(const BotShopTransactionRequest &) const=default; };
struct BotPurchasePlan { BotShopOutcome outcome=BotShopOutcome::OfferUnavailable; BotShopFailure failure=BotShopFailure::None; BotShopTransactionRequest request; uint64_t requiredMoney=0; auto operator<=>(const BotPurchasePlan &) const=default; };
struct BotSalePlan { BotShopOutcome outcome=BotShopOutcome::OfferUnavailable; BotShopFailure failure=BotShopFailure::None; BotShopTransactionRequest request; uint32_t available=0; uint32_t retained=0; auto operator<=>(const BotSalePlan &) const=default; };
struct BotShopTransactionResult { BotShopOutcome outcome=BotShopOutcome::WorldRejected; BotShopFailure failure=BotShopFailure::None; BotShopState state=BotShopState::Idle; BotShopTransactionRequest request; BotMoneyObservation moneyBefore; BotMoneyObservation moneyAfter; uint32_t itemsBefore=0; uint32_t itemsAfter=0; uint32_t reconciledAmount=0; uint8_t attempts=0; auto operator<=>(const BotShopTransactionResult &) const=default; };
struct BotShopProgress { BotShopState state=BotShopState::Idle; std::optional<BotShopTransactionRequest> request; BotMoneyObservation moneyBefore; uint32_t itemsBefore=0; uint8_t attempts=0; std::chrono::milliseconds startedAt{0}; std::chrono::milliseconds nextAttemptAt{0}; bool containsWorldOwnership=false; };

class BotShop final {
public:
	static BotShopObservation observe(const std::shared_ptr<Player> &, uint32_t npcId, uint16_t maximumOffers=256);
	static BotPurchasePlan planBuy(const BotShopObservation &, uint16_t itemId, uint8_t subType, uint16_t amount, uint32_t expectedPrice, uint32_t currentCount, const BotShopPolicy & = {});
	static BotSalePlan planSell(const BotShopObservation &, const BotEquipmentObservation &, uint16_t itemId, uint8_t subType, uint16_t amount, uint32_t expectedPrice, const BotShopPolicy & = {});
	static uint64_t offerSignature(const BotShopOffer &);
	static std::chrono::milliseconds backoff(const BotShopPolicy &, uint8_t attempt);
};
