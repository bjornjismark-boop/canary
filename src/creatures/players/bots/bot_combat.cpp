/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_combat.hpp"

#include "creatures/creature.hpp"
#include "creatures/monsters/monster.hpp"
#include "creatures/npcs/npc.hpp"
#include "creatures/players/player.hpp"
#include "creatures/players/bots/bot_navigation.hpp"
#include "game/game.hpp"
#include "items/item.hpp"
#include "utils/tools.hpp"

namespace {
int32_t bounded(int64_t value, int32_t limit) { return static_cast<int32_t>(std::clamp<int64_t>(value, -std::max(limit, 0), std::max(limit, 0))); }
uint8_t percent(uint64_t value, uint64_t maximum) { return maximum == 0 ? 0 : static_cast<uint8_t>(std::min<uint64_t>(100, value * 100 / maximum)); }
uint16_t distance(const Position &a, const Position &b) { return static_cast<uint16_t>(std::max(Position::getDistanceX(a, b), Position::getDistanceY(a, b))); }
}

uint64_t BotCombat::signature(const BotCombatCreatureObservation &c) {
	uint64_t value = 1469598103934665603ULL;
	const std::array<uint64_t, 8> parts { c.id, c.position.x, c.position.y, c.position.z, static_cast<uint64_t>(c.kind), c.healthPercent, c.summonMasterId, c.revision };
	for (const auto part : parts) { value ^= part; value *= 1099511628211ULL; }
	return value;
}

std::optional<BotCombatObservation> BotCombat::observe(const std::shared_ptr<Player> &player, const BotObservation &base) {
	if (!player || !player->isBotControlled() || player->isRemoved() || !player->getTile()) return std::nullopt;
	BotCombatObservation result;
	result.revision = base.topologyRevision;
	result.self = { .position = base.position, .health = base.health, .maxHealth = base.maxHealth, .mana = base.mana, .maxMana = base.maxMana,
		.healthPercent = percent(std::max(base.health, 0), std::max(base.maxHealth, 0)), .manaPercent = percent(base.mana, base.maxMana),
		.attackedCreatureId = player->getAttackedCreature() ? player->getAttackedCreature()->getID() : 0,
		.followedCreatureId = player->getFollowCreature() ? player->getFollowCreature()->getID() : 0, .revision = result.revision };
	const auto weapon = player->getWeapon(true);
	if (weapon) result.self.attackRange = std::max<uint8_t>(weapon->getShootRange(), 1);
	switch (player->getWeaponType()) {
		case WEAPON_SWORD: case WEAPON_CLUB: case WEAPON_AXE: case WEAPON_FIST: result.self.weapon = BotWeaponCategory::Melee; result.self.archetype = BotCombatArchetype::Melee; break;
		case WEAPON_DISTANCE: case WEAPON_AMMO: case WEAPON_MISSILE: result.self.weapon = BotWeaponCategory::Distance; result.self.archetype = BotCombatArchetype::Distance; break;
		case WEAPON_WAND: result.self.weapon = BotWeaponCategory::Wand; result.self.archetype = BotCombatArchetype::Magic; break;
		case WEAPON_NONE: result.self.weapon = BotWeaponCategory::None; result.self.archetype = BotCombatArchetype::None; break;
		default: break;
	}
	const std::array<ConditionType_t, 5> observableConditions { CONDITION_POISON, CONDITION_FIRE, CONDITION_ENERGY, CONDITION_PARALYZE, CONDITION_INVISIBLE };
	for (size_t index = 0; index < observableConditions.size(); ++index) if (player->hasCondition(observableConditions[index])) result.self.activeConditions |= 1ULL << index;
	const auto damageMap = player->getDamageMap();
	for (const auto &visible : base.visibleCreatures) {
		const auto creature = g_game().getCreatureByID(visible.id);
		if (!creature || creature->isRemoved() || !player->canSeeCreature(creature)) continue;
		BotCombatCreatureObservation observed { .id = visible.id, .position = visible.position, .healthPercent = visible.healthPercent,
			.attackingBot = creature->getAttackedCreature() == player, .followingBot = creature->getFollowCreature() == player,
			.recentlyDamagedBot = damageMap.contains(visible.id),
			.deadOrRemoved = creature->getHealth() <= 0, .directDistance = distance(base.position, visible.position),
			.reachability = BotCombatReachability::Unknown, .visibility = BotCombatVisibility::Visible, .revision = result.revision };
		if (creature->isSummon()) { observed.kind = BotCombatCreatureKind::Summon; const auto master = creature->getMaster(); if (master == player || (master && std::ranges::find(base.visibleCreatures, master->getID(), &BotCreatureObservation::id) != base.visibleCreatures.end())) observed.summonMasterId = master->getID(); }
		else if (creature->getPlayer()) observed.kind = BotCombatCreatureKind::Player;
		else if (creature->getMonster()) observed.kind = BotCombatCreatureKind::Monster;
		else if (creature->getNpc()) observed.kind = BotCombatCreatureKind::Npc;
		else observed.kind = BotCombatCreatureKind::Unknown;
		const auto tileIt = std::ranges::find(base.visibleTiles, visible.position, &BotTileObservation::position);
		observed.protectedByZone = tileIt != base.visibleTiles.end() && tileIt->protectionZone;
		observed.reachability = BotCombatReachability::Unreachable;
		observed.routeCost = std::numeric_limits<uint32_t>::max();
		if (tileIt != base.visibleTiles.end() && tileIt->hasGround && !tileIt->terrainBlocked && !tileIt->blockingItemTypeId) {
			if (observed.directDistance <= 1) { observed.reachability = BotCombatReachability::Reachable; observed.routeCost = 0; }
			else for (uint8_t raw = DIRECTION_NORTH; raw <= DIRECTION_LAST; ++raw) {
				const auto destination = getNextPosition(static_cast<Direction>(raw), visible.position);
				const auto route = BotNavigation::findRoute(base, { base.position, destination, BotRouteLimits { .maxExpandedNodes = 128, .maxRouteLength = 32, .maxPlanningOperations = 1024 } });
				if (!route.found()) continue;
				uint32_t cost = 0; Position previous = base.position; bool valid = true;
				for (const auto &step : route.positions) { const auto tile = std::ranges::find(base.visibleTiles, step, &BotTileObservation::position); if (tile == base.visibleTiles.end()) { valid = false; break; } const bool diagonal = step.x != previous.x && step.y != previous.y; const uint32_t stepCost = (diagonal ? BotNavigation::DiagonalCost : BotNavigation::CardinalCost) + (tile->hazardous ? BotNavigation::HazardCost : 0); if (cost > std::numeric_limits<uint32_t>::max() - stepCost) { valid = false; break; } cost += stepCost; previous = step; }
				if (valid && cost < observed.routeCost) { observed.routeCost = cost; observed.reachability = BotCombatReachability::Reachable; }
			}
		}
		observed.signature = signature(observed);
		if (observed.recentlyDamagedBot) { const auto &damage = damageMap.at(visible.id); if (damage.total >= static_cast<int32_t>(result.self.recentDamage.amount)) result.self.recentDamage = { visible.id, static_cast<uint32_t>(std::max(damage.total, 0)), result.revision }; }
		result.creatures.push_back(observed);
	}
	std::ranges::sort(result.creatures, {}, &BotCombatCreatureObservation::id);
	return result;
}

BotCombatEligibility BotCombat::eligible(const BotCombatObservation &o, const BotCombatCreatureObservation &c, const BotCombatPolicy &p) {
	if (c.id == 0) return BotCombatEligibility::InvalidTarget;
	if (c.revision != o.revision || c.signature != signature(c)) return BotCombatEligibility::StaleObservation;
	if (c.deadOrRemoved || c.healthPercent == 0) return BotCombatEligibility::DeadOrRemoved;
	if (c.visibility == BotCombatVisibility::Hidden) return BotCombatEligibility::NotVisible;
	if (c.visibility == BotCombatVisibility::OutsideKnownArea) return BotCombatEligibility::OutsideKnownArea;
	if (c.position.z != o.self.position.z) return BotCombatEligibility::DifferentFloor;
	if (c.reachability == BotCombatReachability::BudgetExceeded) return BotCombatEligibility::EvaluationBudgetExceeded;
	if (c.reachability != BotCombatReachability::Reachable) return BotCombatEligibility::Unreachable;
	if (c.friendly) return BotCombatEligibility::Friendly;
	if (c.protectedByZone) return BotCombatEligibility::ProtectedByZone;
	if (c.secureModeRejected) return BotCombatEligibility::SecureModeRejected;
	if (c.kind == BotCombatCreatureKind::Player && !p.allowPlayers) return BotCombatEligibility::PlayerTargetDisallowed;
	if (c.kind == BotCombatCreatureKind::Npc && !p.allowNpcs) return BotCombatEligibility::NpcTargetDisallowed;
	if (c.kind == BotCombatCreatureKind::Summon && !p.allowOwnedSummons) return BotCombatEligibility::OwnedSummonDisallowed;
	if (c.kind == BotCombatCreatureKind::Unknown && !p.allowUnknown) return BotCombatEligibility::PolicyRejected;
	return BotCombatEligibility::Eligible;
}

BotThreatAssessment BotCombat::assess(const BotCombatObservation &, const BotCombatCreatureObservation &c, const BotCombatPolicy &p, uint32_t count, uint32_t retained) {
	BotThreatAssessment b;
	b.attacker = c.attackingBot ? p.attackerWeight : 0; b.recentDamage = c.recentlyDamagedBot ? p.recentDamageWeight : 0;
	b.following = c.followingBot ? p.followingWeight : 0; b.retention = c.id == retained ? p.retainedTargetWeight : 0;
	b.distance = bounded(-static_cast<int64_t>(c.directDistance) * p.distanceWeight, p.scoreLimit);
	b.route = bounded(-static_cast<int64_t>(c.routeCost) * p.routeCostWeight, p.scoreLimit);
	b.visibleHealth = bounded(static_cast<int64_t>(100 - c.healthPercent) * p.missingHealthWeight, p.scoreLimit);
	b.crowdRisk = bounded(-static_cast<int64_t>(count > 0 ? count - 1 : 0) * p.crowdRiskWeight, p.scoreLimit);
	b.total = bounded(static_cast<int64_t>(b.attacker) + b.recentDamage + b.following + b.retention + b.distance + b.route + b.visibleHealth + b.crowdRisk, p.scoreLimit);
	return b;
}

BotTargetSelectionResult BotCombat::select(const BotCombatObservation &o, const BotCombatPolicy &p, BotTargetLock &lock, bool placed) {
	BotTargetSelectionResult result;
	if (!placed) { result.failure = BotCombatFailure::InvalidLifecycle; result.reason = BotCombatEligibility::InvalidLifecycle; return result; }
	std::vector<const BotCombatCreatureObservation *> eligibleCreatures;
	const auto limit = std::min<uint32_t>(p.maxCandidates, o.creatures.size()); result.truncated = o.creatures.size() > limit;
	for (uint32_t i = 0; i < limit; ++i) { ++result.evaluatedCandidates; if (eligible(o, o.creatures[i], p) == BotCombatEligibility::Eligible) eligibleCreatures.push_back(&o.creatures[i]); }
	if (result.truncated) result.reason = BotCombatEligibility::EvaluationBudgetExceeded;
	for (const auto *c : eligibleCreatures) {
		if (result.scoreOperations + 8 > p.maxScoreOperations) { result.failure = BotCombatFailure::EvaluationBudgetExceeded; result.reason = BotCombatEligibility::EvaluationBudgetExceeded; result.truncated = true; break; }
		auto breakdown = assess(o, *c, p, eligibleCreatures.size(), lock.creatureId); result.scoreOperations += 8;
		result.scores.push_back({ c->id, BotCombatEligibility::Eligible, breakdown, breakdown.total });
	}
	std::ranges::sort(result.scores, [](const auto &a, const auto &b) { return a.total != b.total ? a.total > b.total : a.creatureId < b.creatureId; });
	const auto current = std::ranges::find(result.scores, lock.creatureId, &BotTargetScore::creatureId);
	if (result.scores.empty()) { const bool had = lock.creatureId != 0; lock = {}; result.intent = had ? BotCombatIntent::ReleaseTarget : BotCombatIntent::NoCombat; if (result.failure == BotCombatFailure::None) result.failure = BotCombatFailure::NoTarget; return result; }
	const BotTargetScore *selected = &result.scores.front();
	if (current != result.scores.end() && selected->creatureId != current->creatureId && selected->total < current->total + p.switchThreshold) selected = &*current;
	const bool held = selected->creatureId == lock.creatureId; result.selectedCreatureId = selected->creatureId; result.intent = held ? BotCombatIntent::HoldTarget : BotCombatIntent::AcquireTarget; result.reason = BotCombatEligibility::Eligible;
	lock = { selected->creatureId, selected->total, o.revision, o.revision + 1, p.switchThreshold, false }; return result;
}
