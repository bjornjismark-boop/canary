/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */
#include "creatures/players/bots/bot_shop.hpp"

#include "creatures/npcs/npc.hpp"
#include "creatures/players/player.hpp"
#include "game/game.hpp"
#include "items/item.hpp"

uint64_t BotShop::offerSignature(const BotShopOffer &offer) {
	uint64_t value = 1469598103934665603ULL;
	const std::array<uint64_t, 5> parts {
		offer.itemTypeId,
		offer.subType,
		offer.buyPrice,
		offer.sellPrice,
		offer.unitWeight,
	};
	for (const uint64_t part : parts) {
		value ^= part;
		value *= 1099511628211ULL;
	}
	return value;
}

BotShopObservation BotShop::observe(const std::shared_ptr<Player> &player, uint32_t npcId, uint16_t maximumOffers) {
	BotShopObservation result; if(!player)return result;result.npcId=npcId;result.freeCapacity=player->getFreeCapacity();const auto carried=player->getMoney();const auto bank=player->getBankBalance();result.money={carried,bank,bank>UINT64_MAX-carried?UINT64_MAX:carried+bank,0};
	const auto npc=g_game().getNpcByID(npcId);result.npcVisible=npc&&player->canSee(npc->getPosition());if(!npc)return result;result.npcPosition=npc->getPosition();result.open=player->getShopOwner()==npc&&npc->isShopPlayer(player->getGUID());if(!result.open)return result;
	const auto &offers=npc->getShopItemVector(player->getGUID());for(size_t index=0;index<std::min<size_t>(offers.size(),maximumOffers);++index){const auto &entry=offers[index];BotShopOffer offer{entry.itemId,static_cast<uint8_t>(entry.itemSubType),entry.itemBuyPrice,entry.itemSellPrice,Item::items[entry.itemId].weight};offer.signature=offerSignature(offer);result.offers.push_back(offer);}
	std::ranges::sort(result.offers,{},&BotShopOffer::signature);uint64_t revision=1469598103934665603ULL;for(const auto &offer:result.offers){revision^=offer.signature;revision*=1099511628211ULL;}result.revision=revision;result.money.revision=revision;return result;
}

BotPurchasePlan BotShop::planBuy(const BotShopObservation &observation,uint16_t itemId,uint8_t subType,uint16_t amount,uint32_t expectedPrice,uint32_t currentCount,const BotShopPolicy &policy){BotPurchasePlan result;result.request={BotShopTransactionKind::Buy,observation.npcId,itemId,subType,amount,expectedPrice,observation.revision};if(!observation.open){result.outcome=BotShopOutcome::ShopClosed;result.failure=BotShopFailure::ShopClosed;return result;}if(amount==0){result.outcome=BotShopOutcome::ItemUnavailable;result.failure=BotShopFailure::AmountChanged;return result;}const auto offer=std::ranges::find_if(observation.offers,[&](const auto&o){return o.itemTypeId==itemId&&o.subType==subType&&o.buyPrice;});if(offer==observation.offers.end()){result.failure=BotShopFailure::HiddenOffer;return result;}if(offer->buyPrice!=expectedPrice){result.outcome=BotShopOutcome::PriceChanged;result.failure=BotShopFailure::PriceChanged;return result;}result.request.amount=std::min<uint16_t>(amount,policy.maximumAmount);if(currentCount>=policy.maximumSupplyCount){result.outcome=BotShopOutcome::NoEffect;return result;}result.request.amount=std::min<uint32_t>(result.request.amount,policy.maximumSupplyCount-currentCount);if(result.request.amount!=amount){result.outcome=BotShopOutcome::OfferUnavailable;result.failure=BotShopFailure::AmountChanged;return result;}result.requiredMoney=static_cast<uint64_t>(result.request.amount)*expectedPrice;if(result.requiredMoney>observation.money.total){result.outcome=BotShopOutcome::InsufficientMoney;result.failure=BotShopFailure::InsufficientMoney;return result;}const uint64_t requiredCapacity=static_cast<uint64_t>(offer->unitWeight)*result.request.amount;if(requiredCapacity>observation.freeCapacity){result.outcome=BotShopOutcome::CapacityInsufficient;result.failure=BotShopFailure::CapacityInsufficient;return result;}result.outcome=BotShopOutcome::Pending;return result;}

BotSalePlan BotShop::planSell(const BotShopObservation &observation,const BotEquipmentObservation &inventory,uint16_t itemId,uint8_t subType,uint16_t amount,uint32_t expectedPrice,const BotShopPolicy &policy){BotSalePlan result;result.request={BotShopTransactionKind::Sell,observation.npcId,itemId,subType,amount,expectedPrice,observation.revision};if(!observation.open){result.outcome=BotShopOutcome::ShopClosed;result.failure=BotShopFailure::ShopClosed;return result;}if(amount==0){result.outcome=BotShopOutcome::ItemUnavailable;result.failure=BotShopFailure::AmountChanged;return result;}const auto offer=std::ranges::find_if(observation.offers,[&](const auto&o){return o.itemTypeId==itemId&&o.subType==subType&&o.sellPrice;});if(offer==observation.offers.end()){result.failure=BotShopFailure::HiddenOffer;return result;}if(offer->sellPrice!=expectedPrice){result.outcome=BotShopOutcome::PriceChanged;result.failure=BotShopFailure::PriceChanged;return result;}if(std::ranges::find(policy.reservedItemTypeIds,itemId)!=policy.reservedItemTypeIds.end()){result.outcome=BotShopOutcome::ReservedItem;result.failure=BotShopFailure::ReservedItem;return result;}for(const auto &item:inventory.items)if(item.itemTypeId==itemId){if(item.equipped){result.failure=BotShopFailure::EquippedItem;return result;}result.available=std::min<uint64_t>(UINT32_MAX,static_cast<uint64_t>(result.available)+item.countOrCharges);}for(const auto &[id,count]:policy.retainedMinimums)if(id==itemId)result.retained=count;if(result.available<=result.retained){result.outcome=BotShopOutcome::ReservedItem;result.failure=BotShopFailure::ReservedItem;return result;}result.request.amount=std::min<uint32_t>({amount,policy.maximumAmount,result.available-result.retained});if(result.request.amount!=amount){result.outcome=BotShopOutcome::OfferUnavailable;result.failure=BotShopFailure::AmountChanged;return result;}result.outcome=BotShopOutcome::Pending;return result;}

std::chrono::milliseconds BotShop::backoff(const BotShopPolicy &policy,uint8_t attempt){uint64_t factor=attempt>=31?UINT32_MAX:(1ULL<<attempt);return std::min(policy.maximumBackoff,std::chrono::milliseconds(std::min<uint64_t>(policy.initialBackoff.count()*factor,policy.maximumBackoff.count())));}
