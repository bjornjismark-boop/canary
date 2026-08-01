local npcType = Game.createNpcType("PlayerBotShopFixtureNpc")
npcType:name("PlayerBotShopFixtureNpc")
npcType:nameDescription("PlayerBotShopFixtureNpc")
npcType:health(100)
npcType:maxHealth(100)
npcType:walkInterval(0)
npcType:walkRadius(0)

local shop = Shop()
shop:setNameItem("health potion")
shop:setId(266)
shop:setBuyPrice(50)
shop:setSellPrice(20)
npcType:addShopItem(shop)

npcType.onBuyItem = function(npc, player, itemId, subType, amount, ignoreCapacity, inBackpacks)
	npc:sellItem(player, itemId, amount, subType, 0, ignoreCapacity, inBackpacks)
end

npcType.onSellItem = function(npc, player, itemId, subType, amount, ignoreEquipped, name, totalCost)
	player:sendTextMessage(MESSAGE_TRADE, string.format("Sold %ix %s for %i gold.", amount, name, totalCost))
end

npcType.onCheckItem = function() end
