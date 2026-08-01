/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#pragma once

#include "creatures/players/bots/bot_equipment.hpp"

class Player;
class Game;

enum class BotResupplyState : uint8_t { Idle, TravelingToDepotOrShop, OpeningDepot, ObservingInventory, ObservingDepot, PlanningResupply, TransferPending, VerifyingTransfer, Equipping, VerifyingEquipment, ResupplyComplete, Backoff, Failed, Cancelled };
enum class BotResupplyOutcome : uint8_t { Succeeded, Partial, Pending, NoEffect, DepotOpened, DepotUnavailable, DepotOwnershipRejected, SourceStale, DestinationStale, CapacityInsufficient, ContainerFull, RequirementNotMet, SlotConflict, TwoHandedConflict, EquipmentNotChanged, SupplyTargetReached, SupplyUnavailable, WorldRejected, TimedOut, RetryScheduled, RetryExhausted, Cancelled };
enum class BotEquipmentFailure : uint8_t { None, InvalidLifecycle, FreshAssessmentRequired, ItemUnavailable, RequirementNotMet, SlotConflict, TwoHandedConflict, CapacityInsufficient, WorldRejected, TimedOut, RetryExhausted, Cancelled };
enum class BotResupplyDirection : uint8_t { DepotToInventory, InventoryToDepot };

struct BotDepotPath { uint16_t boxId=0; std::vector<uint16_t> childIndices; auto operator<=>(const BotDepotPath &) const=default; };
struct BotDepotItemObservation { uint16_t itemTypeId=0; uint32_t count=0; BotDepotPath path; uint64_t signature=0; auto operator<=>(const BotDepotItemObservation &) const=default; };
struct BotDepotContainerObservation { BotDepotPath path; uint16_t capacity=0; uint16_t size=0; uint8_t depth=0; auto operator<=>(const BotDepotContainerObservation &) const=default; };
struct BotDepotObservation { uint32_t depotId=0; uint64_t revision=0; std::vector<BotDepotContainerObservation> containers; std::vector<BotDepotItemObservation> items; bool open=false; bool containerBudgetExceeded=false; bool itemBudgetExceeded=false; bool containsWorldOwnership=false; auto operator<=>(const BotDepotObservation &) const=default; };
struct BotResupplyTarget { uint16_t itemTypeId=0; uint32_t minimum=0; uint32_t target=0; uint32_t reserve=0; auto operator<=>(const BotResupplyTarget &) const=default; };
struct BotResupplyPolicy { std::vector<BotResupplyTarget> targets; std::vector<uint16_t> depositItemTypeIds; uint32_t minimumFreeCapacity=0; uint16_t maximumTransferCount=100; uint8_t maximumDepotContainers=16; uint8_t maximumNestingDepth=2; uint16_t maximumItemsObserved=256; uint8_t maximumRetries=3; std::chrono::milliseconds timeout{2000}; std::chrono::milliseconds initialBackoff{200}; std::chrono::milliseconds maximumBackoff{1600}; };
struct BotResupplyRequest { BotResupplyDirection direction=BotResupplyDirection::DepotToInventory; uint32_t depotId=0; uint16_t itemTypeId=0; uint32_t count=0; BotInventoryPath inventoryPath; BotDepotPath depotPath; uint64_t inventoryRevision=0; uint64_t depotRevision=0; [[nodiscard]] bool containsWorldOwnership() const{return false;} auto operator<=>(const BotResupplyRequest &) const=default; };
struct BotResupplyPlan { BotResupplyOutcome outcome=BotResupplyOutcome::SupplyUnavailable; std::vector<BotResupplyRequest> transfers; uint32_t operationCount=0; auto operator<=>(const BotResupplyPlan &) const=default; };
struct BotResupplyResult { BotResupplyOutcome outcome=BotResupplyOutcome::WorldRejected; BotResupplyState state=BotResupplyState::Idle; BotResupplyRequest request; uint32_t sourceBefore=0; uint32_t sourceAfter=0; uint32_t destinationBefore=0; uint32_t destinationAfter=0; uint32_t movedCount=0; uint16_t worldReturnValue=0; uint8_t attempts=0; auto operator<=>(const BotResupplyResult &) const=default; };
struct BotEquipmentExecutionRequest { BotUpgradeCandidate candidate; BotInventoryPath replacementDestination; auto operator<=>(const BotEquipmentExecutionRequest &) const=default; };
struct BotEquipmentExecutionResult { BotResupplyOutcome outcome=BotResupplyOutcome::WorldRejected; BotEquipmentFailure failure=BotEquipmentFailure::None; BotResupplyState state=BotResupplyState::Idle; BotEquipmentExecutionRequest request; uint16_t equippedBefore=0; uint16_t equippedAfter=0; uint16_t replacedItemTypeId=0; uint16_t worldReturnValue=0; uint8_t attempts=0; auto operator<=>(const BotEquipmentExecutionResult &) const=default; };
struct BotResupplyProgress { BotResupplyState state=BotResupplyState::Idle; std::optional<BotResupplyRequest> transfer; std::optional<BotEquipmentExecutionRequest> equipment; uint32_t sourceBefore=0; uint32_t destinationBefore=0; uint16_t equippedBefore=0; uint8_t attempts=0; std::chrono::milliseconds startedAt{0}; std::chrono::milliseconds nextAttemptAt{0}; bool containsWorldOwnership=false; };

class BotResupply final {
public:
	static BotDepotObservation observeDepot(const std::shared_ptr<Player> &, uint32_t depotId, const BotResupplyPolicy & = {});
	static BotResupplyPlan plan(const BotEquipmentObservation &, const BotDepotObservation &, const BotResupplyPolicy & = {});
	static BotResupplyResult executeTransfer(Game &, const std::shared_ptr<Player> &, const BotResupplyRequest &, const BotResupplyPolicy & = {});
	static BotEquipmentExecutionResult executeEquipment(Game &, const std::shared_ptr<Player> &, const BotEquipmentExecutionRequest &, const BotEquipmentPolicy & = {});
	static uint64_t itemSignature(const BotDepotItemObservation &);
	static std::chrono::milliseconds backoff(const BotResupplyPolicy &, uint8_t attempt);
};
