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
	elseif text == "no" and npc:isInteractingWithPlayer(creature) then
		npc:say("Cancelled.", TALKTYPE_PRIVATE_NP, false, creature)
	elseif text == "bye" and npc:isInteractingWithPlayer(creature) then
		npc:say("Farewell.", TALKTYPE_PRIVATE_NP, false, creature)
		npc:removePlayerInteraction(creature)
	end
	return true
end
