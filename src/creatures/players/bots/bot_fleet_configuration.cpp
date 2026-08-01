/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (c) 2019-present OpenTibiaBR
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

#include "creatures/players/bots/bot_fleet_configuration.hpp"

#include "creatures/players/bots/bot_manager.hpp"
#include "utils/tools.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <fstream>
	#include <nlohmann/json.hpp>
#endif

namespace {
using json = nlohmann::json;

template <typename T>
bool unsignedValue(const json &source, std::string_view key, T &target) {
	const auto found = source.find(key);
	if (found == source.end() || !found->is_number_unsigned()) return false;
	const auto value = found->get<uint64_t>();
	if (value > std::numeric_limits<T>::max()) return false;
	target = static_cast<T>(value);
	return true;
}

bool containsSecretKey(const json &value) {
	if (value.is_object()) for (const auto &[key, child] : value.items()) {
		const auto lowered = asLowerCaseString(key);
		if (lowered.contains("password") || lowered.contains("secret") || lowered.contains("token")) return true;
		if (containsSecretKey(child)) return true;
	}
	if (value.is_array()) for (const auto &child : value) if (containsSecretKey(child)) return true;
	return false;
}

BotFleetDistributionDimension dimension(std::string_view value) {
	if (value == "level-range") return BotFleetDistributionDimension::LevelRange;
	if (value == "planner-profile") return BotFleetDistributionDimension::PlannerPolicy;
	if (value == "role") return BotFleetDistributionDimension::Role;
	if (value == "coordination-group") return BotFleetDistributionDimension::CoordinationGroup;
	if (value == "region") return BotFleetDistributionDimension::Region;
	if (value == "party-profile") return BotFleetDistributionDimension::PartyProfile;
	return BotFleetDistributionDimension::Vocation;
}
}

BotFleetConfigurationResult BotFleetConfiguration::parse(std::string_view text) {
	json root;
	try { root = json::parse(text); } catch (const json::exception &error) { return { BotFleetConfigurationFailure::ParseFailed, {}, error.what() }; }
	if (!root.is_object()) return { BotFleetConfigurationFailure::ParseFailed, {}, "root must be an object" };
	if (containsSecretKey(root)) return { BotFleetConfigurationFailure::SecretRejected, {}, "secret-bearing keys are not permitted" };
	auto revision = std::make_shared<BotFleetConfigurationRevision>();
	if (!unsignedValue(root, "schemaVersion", revision->schemaVersion) || revision->schemaVersion != CurrentSchemaVersion) return { BotFleetConfigurationFailure::UnsupportedSchema, {}, "unsupported schemaVersion" };
	if (!unsignedValue(root, "revision", revision->revision) || revision->revision == 0) return { BotFleetConfigurationFailure::InvalidRevision, {}, "revision must be positive" };
	const auto population = root.find("population");
	if (population == root.end() || !population->is_object()) return { BotFleetConfigurationFailure::InvalidPolicy, {}, "population is required" };
	auto &p = revision->population;
	if (!unsignedValue(*population,"desiredOnline",p.desiredOnline)||!unsignedValue(*population,"minimumOnline",p.minimumOnline)||!unsignedValue(*population,"maximumOnline",p.maximumOnline)||!unsignedValue(*population,"absoluteHardMaximum",p.absoluteHardMaximum)) return { BotFleetConfigurationFailure::InvalidPolicy, {}, "population bounds are required unsigned values" };
	if (population->contains("enabled") && !(*population)["enabled"].is_boolean()) return { BotFleetConfigurationFailure::InvalidPolicy, {}, "enabled must be boolean" };
	p.enabled = population->contains("enabled") ? (*population)["enabled"].get<bool>() : false; p.revision = revision->revision;
	const auto optionalUnsigned = [&]<typename T>(std::string_view key, T &target) { return !population->contains(key) || unsignedValue(*population, key, target); };
	if (!optionalUnsigned("maximumLoginsPerInterval",p.maximumLoginsPerInterval)||!optionalUnsigned("maximumLogoutsPerInterval",p.maximumLogoutsPerInterval)||!optionalUnsigned("maximumPendingLogins",p.maximumPendingLogins)||!optionalUnsigned("maximumPendingLogouts",p.maximumPendingLogouts)||!optionalUnsigned("maximumRetries",p.maximumRetries)||!optionalUnsigned("startupDelayTicks",p.startupDelayTicks)||!optionalUnsigned("maximumRetryBackoffTicks",p.maximumRetryBackoffTicks)||!optionalUnsigned("drainTarget",p.drainTarget)||!optionalUnsigned("drainTimeoutTicks",p.drainTimeoutTicks)||!optionalUnsigned("maximumSessionTicks",p.maximumSessionTicks)||!optionalUnsigned("overloadRecoveryIntervals",p.overloadRecoveryIntervals)) return { BotFleetConfigurationFailure::InvalidPolicy, {}, "optional population values must be unsigned" };
	const auto members = root.find("members");
	if (members == root.end() || !members->is_array() || members->size() > BotFleet::AbsoluteMaximumMembers) return { BotFleetConfigurationFailure::InvalidMember, {}, "members must be a bounded array" };
	for (const auto &source : *members) {
		BotFleetMemberProfile member;
		if (!source.is_object() || !unsignedValue(source,"id",member.id) || !source.contains("name") || !source["name"].is_string()) return { BotFleetConfigurationFailure::InvalidMember, {}, "member id and name are required" };
		if ((source.contains("enabled")&&!source["enabled"].is_boolean())||(source.contains("alwaysOffline")&&!source["alwaysOffline"].is_boolean())) return { BotFleetConfigurationFailure::InvalidMember, {}, "member flags must be boolean" };
		member.name=source["name"].get<std::string>(); member.enabled=source.contains("enabled")?source["enabled"].get<bool>():true; member.alwaysOffline=source.contains("alwaysOffline")?source["alwaysOffline"].get<bool>():false;
		unsignedValue(source,"vocationCategory",member.vocationCategory); unsignedValue(source,"levelRangeCategory",member.levelRangeCategory); unsignedValue(source,"plannerPolicyId",member.plannerPolicyId); unsignedValue(source,"coordinationGroupId",member.coordinationGroupId); unsignedValue(source,"roleId",member.roleId); unsignedValue(source,"partyProfileId",member.partyProfileId); unsignedValue(source,"priority",member.priority); unsignedValue(source,"maximumSessionTicks",member.maximumSessionTicks);
		if (const auto regions=source.find("allowedRegionIds");regions!=source.end()) { if(!regions->is_array()||regions->size()>BotFleet::AbsoluteMaximumRegions)return{BotFleetConfigurationFailure::InvalidMember,{},"regions must be bounded"};for(const auto&region:*regions){if(!region.is_number_unsigned()||region.get<uint64_t>()>UINT32_MAX)return{BotFleetConfigurationFailure::InvalidMember,{},"invalid region"};member.allowedRegionIds.push_back(region.get<uint32_t>());} }
		revision->members.push_back(std::move(member));
	}
	if (const auto limits=root.find("distribution");limits!=root.end()) { if(!limits->is_array())return{BotFleetConfigurationFailure::InvalidPolicy,{},"distribution must be an array"};for(const auto&source:*limits){BotFleetDistributionLimit limit;if(!source.is_object()||!source.contains("dimension")||!source["dimension"].is_string()||!unsignedValue(source,"category",limit.category)||!unsignedValue(source,"minimum",limit.minimum)||!unsignedValue(source,"desired",limit.desired)||!unsignedValue(source,"maximum",limit.maximum))return{BotFleetConfigurationFailure::InvalidPolicy,{},"invalid distribution limit"};const auto dimensionName=source["dimension"].get<std::string>();if(!std::ranges::contains(std::array<std::string_view,7>{"vocation","level-range","planner-profile","role","coordination-group","region","party-profile"},dimensionName))return{BotFleetConfigurationFailure::InvalidPolicy,{},"unknown distribution dimension"};limit.dimension=dimension(dimensionName);revision->distribution.limits.push_back(limit);} }
	revision->distribution.revision=revision->revision;
	if (const auto schedule=root.find("schedule");schedule!=root.end()) { if(!schedule->is_array()||schedule->size()>AbsoluteMaximumScheduleWindows)return{BotFleetConfigurationFailure::InvalidPolicy,{},"schedule must be bounded"};for(const auto&source:*schedule){BotFleetScheduleWindow window;if(!source.is_object()||!unsignedValue(source,"startMinute",window.startMinute)||!unsignedValue(source,"endMinute",window.endMinute)||!unsignedValue(source,"desiredOnline",window.desiredOnline)||window.startMinute>=1440||window.endMinute>1440||window.startMinute>=window.endMinute||window.desiredOnline>p.maximumOnline)return{BotFleetConfigurationFailure::InvalidPolicy,{},"invalid schedule window"};revision->schedule.push_back(window);}std::ranges::sort(revision->schedule,{},&BotFleetScheduleWindow::startMinute);for(size_t i=1;i<revision->schedule.size();++i)if(revision->schedule[i-1].endMinute>revision->schedule[i].startMinute)return{BotFleetConfigurationFailure::InvalidPolicy,{},"overlapping schedule"};}
	if (BotFleet::validate(p)!=BotFleetFailure::None||BotFleet::validate(revision->distribution,revision->members)!=BotFleetFailure::None) return { BotFleetConfigurationFailure::InvalidPolicy, {}, "fleet policy validation failed" };
	return { BotFleetConfigurationFailure::None, std::move(revision), {} };
}

BotFleetConfigurationResult BotFleetConfiguration::load(const std::string &path) {
	std::ifstream stream(path); if(!stream)return{BotFleetConfigurationFailure::FileUnavailable,{},"configuration file unavailable"};std::ostringstream contents;contents<<stream.rdbuf();return parse(contents.str());
}

BotFleetChangeSummary BotFleetConfiguration::summarize(const BotFleetConfigurationRevision *previous,const BotFleetConfigurationRevision&next){BotFleetChangeSummary result{.previousRevision=previous?previous->revision:0,.activeRevision=next.revision,.populationChanged=!previous||previous->population!=next.population,.scheduleChanged=!previous||previous->schedule!=next.schedule};if(!previous){result.addedMembers=next.members.size();result.distributionChanged=true;return result;}for(const auto&m:next.members){const auto old=std::ranges::find(previous->members,m.id,&BotFleetMemberProfile::id);if(old==previous->members.end())++result.addedMembers;else if(*old!=m)++result.changedMembers;}for(const auto&m:previous->members)if(!std::ranges::contains(next.members,m.id,&BotFleetMemberProfile::id))++result.removedMembers;result.distributionChanged=previous->distribution!=next.distribution;return result;}
uint32_t BotFleetConfiguration::scheduledDesired(const BotFleetConfigurationRevision&r,uint16_t minute){for(const auto&w:r.schedule)if(minute>=w.startMinute&&minute<w.endMinute)return w.desiredOnline;return r.population.desiredOnline;}

BotFleetAdministration::BotFleetAdministration(ReloadFunction reload):reloadFunction(std::move(reload)){}
BotFleetConfigurationFailure BotFleetAdministration::install(BotManager&manager,std::shared_ptr<const BotFleetConfigurationRevision>revision){if(!revision)return BotFleetConfigurationFailure::InvalidRevision;if(active&&revision->revision<=active->revision)return BotFleetConfigurationFailure::InvalidRevision;const auto failure=manager.configureFleet(revision->population,revision->distribution,revision->members);if(failure!=BotFleetFailure::None)return BotFleetConfigurationFailure::InvalidPolicy;active=std::move(revision);return BotFleetConfigurationFailure::None;}
BotFleetCommand BotFleetAdministration::submit(BotFleetCommand command,bool authorized){command.id=++sequence;if(stopping){command.failure=BotFleetCommandFailure::Stopping;record(command);return command;}if(!authorized){command.failure=BotFleetCommandFailure::Unauthorized;record(command);return command;}if(commands.size()>=MaximumQueuedCommands){command.failure=BotFleetCommandFailure::QueueFull;record(command);return command;}command.state=BotFleetCommandState::Accepted;commands.push_back(command);record(command);return command;}
std::vector<BotFleetCommand> BotFleetAdministration::process(BotManager&manager){std::vector<BotFleetCommand>done;for(size_t count=0;count<MaximumCommandsPerTick&&!commands.empty();++count){auto command=std::move(commands.front());commands.pop_front();command.state=BotFleetCommandState::Completed;if(active&&command.expectedRevision&&command.expectedRevision!=active->revision){command.state=BotFleetCommandState::Failed;command.failure=BotFleetCommandFailure::RevisionConflict;}else switch(command.type){case BotFleetCommandType::Status:break;case BotFleetCommandType::Pause:manager.pauseFleet();break;case BotFleetCommandType::Resume:manager.resumeFleet();break;case BotFleetCommandType::Drain:manager.drainFleet(command.value);break;case BotFleetCommandType::SetDesired:{if(!active||command.value>active->population.maximumOnline||active->revision==std::numeric_limits<uint64_t>::max()){command.state=BotFleetCommandState::Failed;command.failure=BotFleetCommandFailure::InvalidArgument;break;}auto next=std::make_shared<BotFleetConfigurationRevision>(*active);next->revision=active->revision+1;next->population.revision=next->revision;next->distribution.revision=next->revision;next->population.desiredOnline=command.value;if(install(manager,next)!=BotFleetConfigurationFailure::None){command.state=BotFleetCommandState::Failed;command.failure=BotFleetCommandFailure::LifecycleRejected;}break;}case BotFleetCommandType::Reload:{auto loaded=reloadFunction?reloadFunction():BotFleetConfigurationResult{};if(!loaded.success()||install(manager,loaded.revision)!=BotFleetConfigurationFailure::None){command.state=BotFleetCommandState::Failed;command.failure=BotFleetCommandFailure::ReloadFailed;}break;}case BotFleetCommandType::LoginMember:if(!manager.loginFleetMember(command.memberName)){command.state=BotFleetCommandState::Failed;command.failure=BotFleetCommandFailure::LifecycleRejected;}break;case BotFleetCommandType::LogoutMember:if(!manager.logoutFleetMember(command.memberName)){command.state=BotFleetCommandState::Failed;command.failure=BotFleetCommandFailure::LifecycleRejected;}break;}record(command);done.push_back(std::move(command));}return done;}
void BotFleetAdministration::record(const BotFleetCommand&command){auto detail=command.reason.substr(0,128);const auto lowered=asLowerCaseString(detail);if(lowered.contains("password")||lowered.contains("secret")||lowered.contains("token"))detail="[redacted]";BotFleetAuditEntry entry{command.id,command.operatorId,command.type,command.state,command.failure,active?active->revision:0,std::move(detail)};audits.push_back(std::move(entry));while(audits.size()>MaximumAuditEntries)audits.pop_front();}
void BotFleetAdministration::stop(){stopping=true;commands.clear();reloadFunction={};}
