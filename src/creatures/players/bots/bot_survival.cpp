/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_survival.hpp"

#include "creatures/players/bots/bot_navigation.hpp"

namespace {
int32_t cap(int64_t value, int32_t limit) { return static_cast<int32_t>(std::clamp<int64_t>(value, 0, std::max(limit, 0))); }
uint16_t distance(const Position &a, const Position &b) { return static_cast<uint16_t>(std::max(Position::getDistanceX(a, b), Position::getDistanceY(a, b))); }
}

uint8_t BotSurvival::percentage(uint64_t value, uint64_t maximum) {
	if (maximum == 0) {
		return 0;
	}
	if (value >= maximum) {
		return 100;
	}
	if (value <= std::numeric_limits<uint64_t>::max() / 100) {
		return static_cast<uint8_t>(value * 100 / maximum);
	}
	return static_cast<uint8_t>(static_cast<long double>(value) * 100.0L / static_cast<long double>(maximum));
}

BotHealingOption BotSurvival::selectHealing(const BotSurvivalObservation &o, const BotSurvivalPolicy &p, std::vector<BotHealingOption> options) {
	std::erase_if(options, [&](const auto &v) { return v.kind == BotHealingKind::None || !v.requirementsMet || v.cooldownActive || (v.itemTypeId && v.availableCount == 0) || v.manaCost > o.mana || (v.kind == BotHealingKind::ConditionRemoval && (v.removesConditions & o.harmfulConditions) == 0) || (v.kind == BotHealingKind::Regeneration && !p.permitRegeneration); });
	std::ranges::sort(options, [&](const auto &a, const auto &b) {
		auto rank = [&](const BotHealingOption &v) { const uint32_t deficit = static_cast<uint32_t>(std::max(o.maxHealth - o.health, 0)); const uint32_t useful = std::min(v.maximumHealing, deficit); const uint32_t condition = std::popcount(v.removesConditions & o.harmfulConditions); return std::tuple(condition, useful, std::numeric_limits<uint32_t>::max() - v.manaCost, std::numeric_limits<uint16_t>::max() - v.itemTypeId, v.spell); };
		return rank(a) > rank(b);
	});
	return options.empty() ? BotHealingOption {} : options.front();
}

BotSurvivalAssessment BotSurvival::assess(const BotSurvivalObservation &o, const BotSurvivalPolicy &p, std::vector<BotHealingOption> options, bool fleeAvailable) {
	BotSurvivalAssessment r; r.fleeAvailable = fleeAvailable;
	if (o.dead || o.health <= 0) { r.urgency = BotSurvivalUrgency::Fatal; r.decision = BotSurvivalDecision::Dead; return r; }
	r.score.health = o.healthPercent <= p.fatalHealthPercent ? 600 : o.healthPercent <= p.criticalHealthPercent ? 500 : o.healthPercent <= p.highHealthPercent ? 350 : o.healthPercent <= p.moderateHealthPercent ? 200 : o.healthPercent <= p.lowHealthPercent ? 80 : 0;
	r.score.recentDamage = cap(static_cast<int64_t>(o.recentDamage) * p.recentDamageWeight, p.scoreLimit);
	r.score.conditions = o.harmfulConditions ? cap(static_cast<int64_t>(std::popcount(o.harmfulConditions)) * p.conditionWeight, p.scoreLimit) : 0;
	r.score.hostiles = cap(static_cast<int64_t>(o.visibleHostiles) * p.hostileWeight, p.scoreLimit); r.score.attacked = o.attacked ? cap(p.attackedWeight, p.scoreLimit) : 0; r.score.blocked = o.escapeBlocked ? cap(p.blockedWeight, p.scoreLimit) : 0;
	r.score.total = cap(static_cast<int64_t>(r.score.health) + r.score.recentDamage + r.score.conditions + r.score.hostiles + r.score.attacked + r.score.blocked, p.scoreLimit);
	r.urgency = r.score.total >= 500 ? BotSurvivalUrgency::Critical : r.score.total >= 350 ? BotSurvivalUrgency::High : r.score.total >= 200 ? BotSurvivalUrgency::Moderate : r.score.total > 0 ? BotSurvivalUrgency::Low : BotSurvivalUrgency::None;
	r.healing = selectHealing(o, p, std::move(options));
	if (r.urgency == BotSurvivalUrgency::Critical) r.decision = r.healing.kind != BotHealingKind::None ? BotSurvivalDecision::Heal : fleeAvailable ? BotSurvivalDecision::Flee : BotSurvivalDecision::Wait;
	else if (r.urgency == BotSurvivalUrgency::High && r.healing.kind != BotHealingKind::None) r.decision = BotSurvivalDecision::Heal;
	return r;
}

BotFleeResult BotSurvival::selectFlee(const BotObservation &o, const BotCombatObservation &combat, const BotSurvivalPolicy &p) {
	BotFleeResult result; uint64_t best = 0; Position bestPosition; BotRouteResult bestRoute;
	const auto limit = std::min<size_t>(p.maxCandidates, o.visibleTiles.size());
	const BotRouteLimits limits { .maxExpandedNodes = p.maxCandidates, .maxRouteLength = p.maxRouteLength, .maxPlanningOperations = static_cast<uint32_t>(p.maxCandidates) * 16, .maxReplans = p.maxAttempts, .maxNoProgress = p.maxNoProgress, .initialBackoff = p.initialBackoff, .maximumBackoff = p.maximumBackoff };
	for (size_t i = 0; i < limit; ++i) { const auto &tile = o.visibleTiles[i]; if (tile.position == o.position || tile.position.z != o.position.z || !tile.hasGround || tile.terrainBlocked || tile.blockingItemTypeId || tile.blockingCreatureId || tile.hazardous || distance(o.position, tile.position) > p.maxFleeRadius) continue;
		auto route = BotNavigation::findRoute(o, { o.position, tile.position, limits }); if (!route.found()) continue;
		uint64_t nearest = p.maxFleeRadius + 1; uint64_t adjacent = 0; for (const auto &hostile : combat.creatures) { if (hostile.visibility != BotCombatVisibility::Visible || hostile.kind != BotCombatCreatureKind::Monster) continue; const auto d = distance(tile.position, hostile.position); nearest = std::min(nearest, static_cast<uint64_t>(d)); if (d <= 1) ++adjacent; }
		const uint64_t distanceScore = nearest * 1000;
		const uint64_t routeScore = route.positions.size() <= p.maxRouteLength ? (p.maxRouteLength - route.positions.size()) * 10 : 0;
		const uint64_t penalty = std::min<uint64_t>(adjacent * 100, distanceScore + routeScore);
		const uint64_t score = distanceScore + routeScore - penalty;
		if (score > best || (score == best && tile.position < bestPosition)) { best = score; bestPosition = tile.position; bestRoute = std::move(route); }
	}
	result.request = { o.position, bestPosition, limits, static_cast<uint16_t>(limit) };
	if (best == 0) return result;
	result.route = std::move(bestRoute);
	result.outcome = BotFleeOutcome::SafePositionSelected;
	return result;
}

std::chrono::milliseconds BotSurvival::backoff(const BotSurvivalPolicy &p, uint32_t attempt) { if (!attempt) return {}; return std::min(p.maximumBackoff, p.initialBackoff * (1U << std::min<uint32_t>(attempt - 1, 10))); }

bool BotSurvival::legalTransition(BotSurvivalState from, BotSurvivalState to) {
	if (to == BotSurvivalState::DeathDetected) return from != BotSurvivalState::Dead && from != BotSurvivalState::Cancelled;
	if (from == BotSurvivalState::DeathDetected) return to == BotSurvivalState::Dead;
	if (from == BotSurvivalState::Dead || from == BotSurvivalState::Cancelled) return false;
	if (to == BotSurvivalState::Cancelled || to == BotSurvivalState::Failed) return true;
	switch (from) { case BotSurvivalState::Stable: return to == BotSurvivalState::Threatened || to == BotSurvivalState::HealingRequired || to == BotSurvivalState::FleeRequired || to == BotSurvivalState::Safe; case BotSurvivalState::Threatened: return to == BotSurvivalState::Stable || to == BotSurvivalState::HealingRequired || to == BotSurvivalState::FleeRequired; case BotSurvivalState::HealingRequired: return to == BotSurvivalState::HealingPending; case BotSurvivalState::HealingPending: return to == BotSurvivalState::Recovering || to == BotSurvivalState::Exhausted; case BotSurvivalState::Recovering: return to == BotSurvivalState::Stable || to == BotSurvivalState::FleeRequired || to == BotSurvivalState::Safe; case BotSurvivalState::FleeRequired: return to == BotSurvivalState::FleePlanning; case BotSurvivalState::FleePlanning: return to == BotSurvivalState::Fleeing; case BotSurvivalState::Fleeing: return to == BotSurvivalState::Safe || to == BotSurvivalState::FleePlanning; case BotSurvivalState::Safe: return to == BotSurvivalState::Stable; case BotSurvivalState::Exhausted: return to == BotSurvivalState::HealingRequired || to == BotSurvivalState::FleeRequired; default: return false; }
}
