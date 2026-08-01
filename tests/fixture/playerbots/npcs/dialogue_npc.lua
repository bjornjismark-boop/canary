local npcType = Game.createNpcType("PlayerBotDialogueFixtureNpc")
npcType:name("PlayerBotDialogueFixtureNpc")
npcType:nameDescription("PlayerBotDialogueFixtureNpc")
npcType:health(100)
npcType:maxHealth(100)
npcType:walkInterval(0)
npcType:walkRadius(0)

npcType.onSay = function(npc, creature, type, message)
	local text = message:lower()
	if text == "hi" then
		npc:setPlayerInteraction(creature, 0)
		npc:say("Welcome, adventurer.", TALKTYPE_PRIVATE_NP, false, creature)
	elseif text == "job" and npc:isInteractingWithPlayer(creature) then
		npc:say("I guide adventurers. Do you need help, yes or no?", TALKTYPE_PRIVATE_NP, false, creature)
	elseif text == "yes" and npc:isInteractingWithPlayer(creature) then
		npc:say("Confirmed. I will help.", TALKTYPE_PRIVATE_NP, false, creature)
	elseif text == "mission" and npc:isInteractingWithPlayer(creature) then
		if creature:getStorageValue(900100) < 1 then
			creature:setStorageValue(900100, 1)
		end
		npc:say("Your mission has started.", TALKTYPE_PRIVATE_NP, false, creature)
	elseif text == "report" and npc:isInteractingWithPlayer(creature) then
		if creature:getStorageValue(900100) == 2 and creature:removeItem(3031, 1) then
			creature:setStorageValue(900100, 3)
			npc:say("Mission complete. Take your reward.", TALKTYPE_PRIVATE_NP, false, creature)
			creature:addItem(266, 1)
			creature:addExperience(50, false)
		else
			npc:say("Your mission is not ready.", TALKTYPE_PRIVATE_NP, false, creature)
		end
	elseif text == "no" and npc:isInteractingWithPlayer(creature) then
		npc:say("Cancelled.", TALKTYPE_PRIVATE_NP, false, creature)
	elseif text == "bye" and npc:isInteractingWithPlayer(creature) then
		npc:say("Farewell.", TALKTYPE_PRIVATE_NP, false, creature)
		npc:removePlayerInteraction(creature)
	end
	return true
end

local witnessType = Game.createNpcType("PlayerBotQuestWitnessFixtureNpc")
witnessType:name("PlayerBotQuestWitnessFixtureNpc")
witnessType:nameDescription("PlayerBotQuestWitnessFixtureNpc")
witnessType:health(100)
witnessType:maxHealth(100)
witnessType:walkInterval(0)
witnessType:walkRadius(0)

witnessType.onSay = function(npc, creature, type, message)
	local text = message:lower()
	if text == "hi" then
		npc:setPlayerInteraction(creature, 0)
		npc:say("Welcome, hunter.", TALKTYPE_PRIVATE_NP, false, creature)
	elseif text == "witness" and npc:isInteractingWithPlayer(creature) and creature:getStorageValue(900100) == 1 then
		if creature:removeItem(3031, 1) then
			creature:setStorageValue(900100, 3)
			npc:say("I witnessed your work. Mission complete.", TALKTYPE_PRIVATE_NP, false, creature)
			creature:addItem(266, 1)
			creature:addExperience(50, false)
		end
	elseif text == "report" and npc:isInteractingWithPlayer(creature) then
		if creature:getStorageValue(900100) == 2 and creature:removeItem(3031, 1) then
			creature:setStorageValue(900100, 3)
			npc:say("Mission complete. Take your reward.", TALKTYPE_PRIVATE_NP, false, creature)
			creature:addItem(266, 1)
			creature:addExperience(50, false)
		end
	elseif text == "bye" and npc:isInteractingWithPlayer(creature) then
		npc:say("Farewell.", TALKTYPE_PRIVATE_NP, false, creature)
		npc:removePlayerInteraction(creature)
	end
	return true
end
