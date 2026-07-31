local potion = Action()

function potion.onUse(player, item, fromPosition, target)
	if not target or not target:isPlayer() or target:getId() ~= player:getId() then
		return false
	end
	doTargetCombatHealth(player, target, COMBAT_HEALING, 40, 40, CONST_ME_MAGIC_BLUE)
	item:remove(1)
	return true
end

potion:id(266)
potion:register()

local noEffect = Action()

function noEffect.onUse(player, item, fromPosition, target)
	if not target or not target:isPlayer() or target:getId() ~= player:getId() then
		return false
	end
	item:remove(1)
	return true
end

noEffect:id(2854)
noEffect:register()

local healing = Spell("instant")

function healing.onCastSpell(creature, variant)
	doTargetCombatHealth(creature, creature, COMBAT_HEALING, 30, 30, CONST_ME_MAGIC_BLUE)
	return true
end

healing:name("PlayerBot Test Healing")
healing:words("playerbot test heal")
healing:group("healing")
healing:level(1)
healing:mana(10)
healing:cooldown(1000)
healing:groupCooldown(1000)
healing:needLearn(false)
healing:isAggressive(false)
healing:register()

local expensiveHealing = Spell("instant")

function expensiveHealing.onCastSpell(creature, variant)
	doTargetCombatHealth(creature, creature, COMBAT_HEALING, 30, 30, CONST_ME_MAGIC_BLUE)
	return true
end

expensiveHealing:name("PlayerBot Test Expensive Healing")
expensiveHealing:words("playerbot test expensive heal")
expensiveHealing:group("healing")
expensiveHealing:level(1)
expensiveHealing:mana(1000)
expensiveHealing:cooldown(1000)
expensiveHealing:groupCooldown(1000)
expensiveHealing:needLearn(false)
expensiveHealing:isAggressive(false)
expensiveHealing:register()

local cleanse = Spell("instant")

function cleanse.onCastSpell(creature, variant)
	creature:removeCondition(CONDITION_PARALYZE)
	return true
end

cleanse:name("PlayerBot Test Cleanse")
cleanse:words("playerbot test cleanse")
cleanse:group("support")
cleanse:level(1)
cleanse:mana(5)
cleanse:cooldown(0)
cleanse:groupCooldown(0)
cleanse:needLearn(false)
cleanse:isAggressive(false)
cleanse:register()
