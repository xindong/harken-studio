#include "RuntimeCapabilityMatrix.h"

#include "Components/VMixNode.h"
#include "Components/VAttenuation.h"
#include "Components/VSelection.h"
#include "EffectChainSemantics.h"
#include "ItemTypeCatalog.h"
#include "CurrentWorldProjectAdapter.h"
#include "PlatformContextService.h"
#include "BindingRuntimeLowering.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"

#pragma push_macro("slots")
#undef slots
#include <harken_project_schema/FrontEnd.h>
#pragma pop_macro("slots")

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

namespace VNX {
namespace {

using F = RuntimeCapabilityFeature;
using O = RuntimeCapabilityOperation;
using S = RuntimeCapabilityState;
using B = RuntimeCapabilityBackend;
using T = ProjectItem::Type;

RuntimeCapabilityEntry row(T objectType, O operation, F feature, S state,
                           const QString& diagnostic = {})
{
    return {objectType, operation, feature, state, diagnostic};
}

const ProjectItem* findByStableId(const ProjectData& project, harkenstudio::ObjectId stableId)
{
    const ProjectItem* result = nullptr;
    std::set<const ProjectItem*> visited;
    std::function<void(const ProjectItem*)> visit = [&](const ProjectItem* item) {
        if (!item || result || !visited.insert(item).second) return;
        if (item->runtimeId() == stableId) {
            result = item;
            return;
        }
        for (const auto& child : item->children)
            visit(child.get());
    };
    if (project.root) visit(project.root.get());
    for (const auto* binding : project.allFileBindings())
        if (binding && binding->rootItem) visit(binding->rootItem.get());
    return result;
}

S worstState(S left, S right)
{
    const auto rank = [](S state) {
        switch (state) {
        case S::Ready: return 0;
        case S::CompiledPreviewOnly: return 1;
        case S::PackOnly: return 2;
        case S::UnavailableEngineContract: return 3;
        case S::InvalidAuthoring: return 4;
        }
        return std::numeric_limits<int>::max();
    };
    return rank(left) >= rank(right) ? left : right;
}

void appendIssue(RuntimeCapabilityAssessment& assessment,
                 F feature, S state, const QString& diagnostic,
                 const ProjectItem& item, const QString& field = {}, int stepIndex = -1)
{
    RuntimeCapabilityIssue issue;
    issue.feature = feature;
    issue.state = state;
    issue.diagnostic = diagnostic;
    issue.locator.objectStableId = item.runtimeId();
    issue.locator.itemGuid = item.guid;
    issue.locator.field = field;
    issue.locator.stepIndex = stepIndex;
    assessment.issues.push_back(std::move(issue));
}

void appendStaticIssue(RuntimeCapabilityAssessment& assessment,
                       const RuntimeCapabilityEntry& entry,
                       const ProjectItem& item,
                       const QString& field = {}, int stepIndex = -1)
{
    if (entry.state == S::Ready) return;
    appendIssue(assessment, entry.feature, entry.state,
                entry.diagnostic, item, field, stepIndex);
}

bool operationSupports(T type, O operation)
{
    const bool playable = type == T::Sound || type == T::Container
        || type == T::Track || type == T::Piece;
    switch (operation) {
    case O::ActionRegistration: return type == T::Action;
    case O::CompiledPreview:
    case O::AuditionPlay: return playable || type == T::Action;
    case O::ProjectInputCompile:
        return playable || type == T::Layer || type == T::Action
            || type == T::Variable || type == T::Bus;
    case O::PackBuild:
    case O::LiveSync:
    case O::Delete:
        return playable || type == T::Layer || type == T::Action
            || type == T::Variable || type == T::Bus || type == T::Attenuation;
    }
    return false;
}

B backendFor(O operation, T type)
{
    if (!operationSupports(type, operation)) return B::None;
    if (operation == O::PackBuild) return B::Pack;
    if (type == T::Sound || type == T::Container || type == T::Track
        || type == T::Piece || type == T::Layer || type == T::Action
        || type == T::Variable || type == T::Bus)
        return B::CurrentWorld;
    return B::Native;
}

RuntimeCapabilityEntry bindingCapability(const ProjectItem& item,
                                         const ProjectData& project,
                                         O operation)
{
    RuntimeCapabilityEntry result{item.type, operation, F::TypedBinding, S::Ready, {}};
    const auto refuse = [&](S state, const QString& diagnostic) {
        result.state = state;
        result.diagnostic = diagnostic;
        return result;
    };
    const auto* mix = item.comp<VMixNode>();
    if (!mix || mix->bindings.isEmpty()) return result;
    for (const auto& binding : mix->bindings.entries()) {
        if (!binding.typed || harkenstudio::IsNull(binding.runtimeId()))
            return refuse(S::InvalidAuthoring,
                          QStringLiteral("Binding is not typed or has no stable identity"));
        if (binding.address.hostClass != BindingContractSemantics::hostClassOf(item.type)
            || !BindingContractSemantics::engineTargetId(binding.address))
            return refuse(S::UnavailableEngineContract,
                          QStringLiteral("Binding target has no supported runtime lowering"));
        BindingRuntimeLowering lowered;
        const auto status = BindingRuntimeLoweringSemantics::appendLoweredBinding(
            project, item, binding, &lowered);
        if (status == BindingLowerStatus::Fail)
            return refuse(S::InvalidAuthoring,
                          QStringLiteral("Binding transitionMs is illegal or unsupported"));
        if (status != BindingLowerStatus::Ok)
            return refuse(S::InvalidAuthoring,
                          QStringLiteral("Binding Variable or curve is invalid"));
    }
    if (operation == O::ProjectInputCompile)
        return refuse(S::UnavailableEngineContract,
                      QStringLiteral("Current World input does not carry typed bindings; sync and Pack use the binding bridge"));
    return result;
}

QString scheduleAdmissionDiagnostic()
{
    return QStringLiteral("Schedule compilation is available; playback admission requires every materialized Clip start inside the active scheduling window after publication lead, within runtime capacities");
}

QString legacyOperationName(O operation)
{
    switch (operation) {
    case O::PackBuild: return QStringLiteral("packBuild");
    case O::LiveSync: return QStringLiteral("liveSync");
    case O::ActionRegistration: return QStringLiteral("actionRegistration");
    case O::ProjectInputCompile: return QStringLiteral("projectInputCompile");
    case O::CompiledPreview: return QStringLiteral("compiledPreview");
    case O::AuditionPlay: return QStringLiteral("auditionPlay");
    case O::Delete: return QStringLiteral("delete");
    }
    return {};
}

bool isUnsupportedState(S state)
{
    return state == S::UnavailableEngineContract || state == S::InvalidAuthoring;
}

}

QJsonObject RuntimeCapabilityLocator::toJson() const
{
    QJsonArray path;
    for (uint64_t id : referencePath)
        path.append(static_cast<qint64>(id));
    QJsonObject result{
        {QStringLiteral("objectStableId"), objectIdString(objectStableId)},
        {QStringLiteral("itemGuid"), itemGuid.toString(QUuid::WithoutBraces)},
        {QStringLiteral("entityGuid"), entityGuid.toString(QUuid::WithoutBraces)},
        {QStringLiteral("stepIndex"), stepIndex},
        {QStringLiteral("field"), field},
        {QStringLiteral("referencePath"), path}};
    if (itemGuid.isNull()) result.remove(QStringLiteral("itemGuid"));
    if (entityGuid.isNull()) result.remove(QStringLiteral("entityGuid"));
    if (stepIndex < 0) result.remove(QStringLiteral("stepIndex"));
    if (field.isEmpty()) result.remove(QStringLiteral("field"));
    if (referencePath.empty()) result.remove(QStringLiteral("referencePath"));
    return result;
}

QJsonObject RuntimeCapabilityIssue::toJson() const
{
    return {{QStringLiteral("feature"), RuntimeCapabilityMatrix::featureName(feature)},
            {QStringLiteral("state"), RuntimeCapabilityMatrix::stateName(state)},
            {QStringLiteral("diagnostic"), diagnostic},
            {QStringLiteral("locator"), locator.toJson()}};
}

bool RuntimeCapabilityAssessment::permitted() const
{
    if (state == S::UnavailableEngineContract || state == S::InvalidAuthoring)
        return false;
    for (const auto& issue : issues)
        if (isUnsupportedState(issue.state)) return false;
    if (state == S::PackOnly && operation != O::PackBuild)
        return false;
    if (state == S::CompiledPreviewOnly
        && operation != O::ProjectInputCompile
        && operation != O::CompiledPreview
        && operation != O::AuditionPlay)
        return false;
    return true;
}

QJsonObject RuntimeCapabilityAssessment::toJson() const
{
    QJsonArray issuesJson;
    for (const auto& issue : issues)
        issuesJson.append(issue.toJson());
    QJsonObject result{
        {QStringLiteral("operation"), RuntimeCapabilityMatrix::operationName(operation)},
        {QStringLiteral("rootStableId"), objectIdString(rootStableId)},
        {QStringLiteral("state"), RuntimeCapabilityMatrix::stateName(state)},
        {QStringLiteral("backend"), RuntimeCapabilityMatrix::backendName(backend)},
        {QStringLiteral("issues"), issuesJson},
        {QStringLiteral("permitted"), permitted()}};
    if (variant) {
        result.insert(QStringLiteral("variant"), QJsonObject{
            {QStringLiteral("platform"), variant->platform},
            {QStringLiteral("language"), variant->language}});
    }
    return result;
}

QJsonObject RuntimeCapabilityEntry::toJson() const
{
    return {{QStringLiteral("objectType"), ItemTypeCatalog::canonicalName(objectType)},
            {QStringLiteral("operation"), RuntimeCapabilityMatrix::operationName(operation)},
            {QStringLiteral("feature"), RuntimeCapabilityMatrix::featureName(feature)},
            {QStringLiteral("state"), RuntimeCapabilityMatrix::stateName(state)},
            {QStringLiteral("backend"), RuntimeCapabilityMatrix::backendName(
                feature == F::TypedBinding && operation != O::PackBuild
                    ? B::Native : backendFor(operation, objectType))},
            {QStringLiteral("diagnostic"), diagnostic}};
}

namespace RuntimeCapabilityMatrix {

std::optional<O> parseOperation(const QString& value)
{
    const QString name = value.trimmed();
    if (name == QStringLiteral("pack") || name == QStringLiteral("packBuild"))
        return O::PackBuild;
    if (name == QStringLiteral("sync") || name == QStringLiteral("liveSync"))
        return O::LiveSync;
    if (name == QStringLiteral("actionRegistration")) return O::ActionRegistration;
    if (name == QStringLiteral("execute")) return std::nullopt;
    if (name == QStringLiteral("compile") || name == QStringLiteral("projectInputCompile"))
        return O::ProjectInputCompile;
    if (name == QStringLiteral("preview") || name == QStringLiteral("compiledPreview"))
        return O::CompiledPreview;
    if (name == QStringLiteral("audition") || name == QStringLiteral("auditionPlay"))
        return O::AuditionPlay;
    if (name == QStringLiteral("delete")) return O::Delete;
    return std::nullopt;
}


RuntimeCapabilityEntry evaluate(const ProjectItem& item,
                                const ProjectData& project,
                                RuntimeCapabilityOperation operation,
                                RuntimeCapabilityFeature feature,
                                std::optional<BuildVariant> variant)
{
    RuntimeCapabilityEntry result{item.type, operation, feature, S::Ready, {}};
    const auto refuse = [&](S state, const QString& diagnostic) {
        result.state = state;
        result.diagnostic = diagnostic;
        return result;
    };
    if (feature == F::StableIdentity) {
        if (harkenstudio::IsNull(item.runtimeId()) || item.guid.isNull())
            return refuse(S::InvalidAuthoring,
                          QStringLiteral("Object requires a GUID and runtime stable identity"));
        return result;
    }
    if (!operationSupports(item.type, operation))
        return refuse(S::UnavailableEngineContract,
                      QStringLiteral("%1 does not support %2")
                          .arg(ItemTypeCatalog::canonicalName(item.type), operationName(operation)));
    if (!query(item.type, operation, feature))
        return refuse(S::UnavailableEngineContract,
                      QStringLiteral("The object has no runtime contract for this feature and operation"));
    if (operation == O::Delete) {
        if (const auto entry = query(item.type, operation, feature)) return *entry;
        return result;
    }
    if (item.type == T::Attenuation) {
        if (feature != F::ContentClosure)
            return refuse(S::UnavailableEngineContract,
                          QStringLiteral("Attenuation exposes its resource capability only"));
        const auto* attenuation = item.comp<VAttenuation>();
        if (!attenuation || item.guid.isNull() || harkenstudio::IsNull(item.runtimeId())
            || !std::isfinite(attenuation->maxDistance)
            || attenuation->maxDistance <= 0.0f)
            return refuse(S::InvalidAuthoring,
                          QStringLiteral("Attenuation requires its stored identity and a positive finite distance"));
        if (variant && !variant->platform.isEmpty()) {
            const auto platform = ProjectPlatformSemantics::parseId(variant->platform);
            if (!platform || !ProjectPlatformSemantics::find(project.platforms, *platform))
                return refuse(S::InvalidAuthoring,
                              QStringLiteral("Build platform ID does not belong to this project"));
        }
        for (const auto& dimension : attenuation->dimensions) {
            if (dimension.curve.isEmpty()) continue;
            if (dimension.type != AttenuationDimension::Volume)
                return refuse(S::UnavailableEngineContract,
                              QStringLiteral("Runtime Attenuation supports the Volume dimension only"));
            if (dimension.curve.size() > harkenstudiobuild::kMaxCurvePoints
                || dimension.handles.size() != dimension.curve.size() - 1)
                return refuse(S::InvalidAuthoring,
                              QStringLiteral("Attenuation curve points and handles are invalid"));
            std::vector<harkenstudiobuild::CurvePoint> points;
            std::vector<harkenstudiobuild::CurveHandlePair> handles;
            for (const auto& point : dimension.curve) {
                if (point.value < -96.0f || point.value > 0.0f)
                    return refuse(S::InvalidAuthoring,
                                  QStringLiteral("Attenuation Volume curve exceeds its gain range"));
                points.push_back({point.x, point.value});
            }
            for (const auto& handle : dimension.handles) {
                if (handle.h1y < -96.0f || handle.h1y > 0.0f
                    || handle.h2y < -96.0f || handle.h2y > 0.0f)
                    return refuse(S::InvalidAuthoring,
                                  QStringLiteral("Attenuation Volume handles exceed their gain range"));
                handles.push_back({handle.h1x, handle.h1y, handle.h2x, handle.h2y});
            }
            harkenstudiobuild::CurveV1 curve;
            if (!curve.curve.SetPoints(points.data(), handles.data(),
                                       static_cast<uint32_t>(points.size())))
                return refuse(S::InvalidAuthoring,
                              QStringLiteral("Attenuation curve does not satisfy the runtime curve contract"));
        }
        return result;
    }
    if (feature == F::TypedBinding)
        return bindingCapability(item, project, operation);
    if (feature == F::TypedEffect) {
        const auto* mix = item.comp<VMixNode>();
        if (!mix || mix->effects.isEmpty()) return result;
        for (const auto& effect : mix->effects.entries()) {
            QString diagnostic;
            if (!effect.typed || harkenstudio::IsNull(effect.runtimeId())
                || !EffectChainSemantics::validate(effect, &diagnostic))
                return refuse(S::InvalidAuthoring, diagnostic.isEmpty()
                    ? QStringLiteral("Effect chain contains an invalid typed entry") : diagnostic);
            if (effect.bypass && item.type != T::Bus)
                return refuse(S::UnavailableEngineContract,
                              QStringLiteral("Current World playable effect graphs do not carry effect bypass"));
        }
    }
    const bool currentWorldFeature = feature == F::ContentClosure
        || feature == F::SoundPlayback || feature == F::FlatContainer
        || feature == F::TrackProgram || feature == F::PieceProgram
        || feature == F::VariableDeclaration || feature == F::TypedEffect
        || feature == F::Transition || feature == F::ContinuousGrid
        || feature == F::MultiAxisGrid || feature == F::QueueMemory
        || feature == F::ShuffleMemory || feature == F::ResumeMemory
        || feature == F::GridMapping || feature == F::EntryPosition
        || feature == F::NestedPlaceholder || feature == F::ActionTarget
        || feature == F::ActionPlay || feature == F::ActionStop
        || feature == F::ActionPost || feature == F::ActionSetVariable
        || feature == F::ActionReset || feature == F::Quantize || feature == F::IfSilent;
    if (!currentWorldFeature) {
        if (const auto entry = query(item.type, operation, feature)) return *entry;
        return refuse(S::UnavailableEngineContract,
                      QStringLiteral("No runtime capability row exists"));
    }
    if ((feature == F::TrackProgram && item.type != T::Track)
        || (feature == F::PieceProgram && item.type != T::Piece))
        return refuse(S::InvalidAuthoring,
                      QStringLiteral("Schedule capability requires its matching Track or Piece type"));
    if ((feature == F::FlatContainer || feature == F::Transition
         || feature == F::ContinuousGrid || feature == F::MultiAxisGrid
         || feature == F::QueueMemory || feature == F::ShuffleMemory
         || feature == F::ResumeMemory || feature == F::GridMapping)
        && !item.comp<VSelection>())
        return refuse(S::InvalidAuthoring,
                      QStringLiteral("Container capability requires a Container selection"));
    if (item.type == T::Bus) {
        const auto bindings = bindingCapability(item, project, operation);
        if (bindings.state != S::Ready) return refuse(bindings.state, bindings.diagnostic);
    }
    PlatformScope scope = currentWorldPlatformScope(PlatformContextService::instance().context());
    CurrentWorldBuildOptions options;
    if (item.type == T::Action) options.actionRoot = &item;
    else options.definitionRoot = &item;
    if (variant) {
        options.language = variant->language;
        if (!variant->platform.isEmpty()) {
            const auto platform = ProjectPlatformSemantics::parseId(variant->platform);
            if (!platform)
                return refuse(S::InvalidAuthoring, QStringLiteral("Build platform ID is invalid"));
            scope = *platform;
        }
    }
    QString diagnostic;
    CurrentWorldBuildStatus status = CurrentWorldBuildStatus::InvalidAuthoring;
    auto request = buildCurrentWorldSyncRequest(
        const_cast<ProjectData&>(project), scope, &diagnostic, &status, &options);
    if (!request)
        return refuse(status == CurrentWorldBuildStatus::CapabilityUnavailable
                          ? S::UnavailableEngineContract : S::InvalidAuthoring,
                      diagnostic);
    const bool playable = item.type == T::Sound || item.type == T::Container
        || item.type == T::Track || item.type == T::Piece || item.type == T::Layer;
    if (playable && std::none_of(request->project.nodes.begin(), request->project.nodes.end(),
            [&](const auto& node) { return node.guid == item.runtimeId(); }))
        return refuse(S::InvalidAuthoring,
                      QStringLiteral("Object has no playable definition in the selected platform and language closure"));
    if (item.type == T::Action && std::none_of(request->actions.begin(), request->actions.end(),
            [&](const auto& action) { return action.actionStableId == item.runtimeId(); }))
        return refuse(S::InvalidAuthoring,
                      QStringLiteral("Action has no playable steps in the selected language closure"));
    for (const auto& node : request->project.nodes) {
        const auto* dependency = findByStableId(project, node.guid);
        if (!dependency) continue;
        for (const F dependencyFeature : {F::TypedBinding, F::TypedEffect}) {
            if (dependencyFeature == F::TypedEffect) {
                const auto* mix = dependency->comp<VMixNode>();
                if (!mix) continue;
                for (const auto& effect : mix->effects.entries()) {
                    if (!EffectChainSemantics::validate(effect, &diagnostic))
                        return refuse(S::InvalidAuthoring, diagnostic);
                    if (effect.bypass)
                        return refuse(S::UnavailableEngineContract,
                            QStringLiteral("Current World playable effect graphs do not carry effect bypass"));
                }
            } else {
                const auto binding = bindingCapability(*dependency, project, operation);
                if (binding.state != S::Ready) return refuse(binding.state, binding.diagnostic);
            }
        }
    }
    const auto frontEnd = harkenprojectschema::CompileFrontEnd(request->project);
    if (!frontEnd.ok()) {
        diagnostic = QStringLiteral("Current World project compilation failed");
        if (!frontEnd.diagnostics.empty())
            diagnostic += QStringLiteral(": %1").arg(QString::fromLatin1(
                harkenprojectschema::DiagnosticCodeName(frontEnd.diagnostics.front().code)));
        return refuse(S::InvalidAuthoring, diagnostic);
    }
    auto materializations = request->materializations;
    for (auto& definition : materializations)
        if (definition.kind == harkenplayableproperties::ContentPropertyKindV1::Sound
            && definition.contentRevision == 0)
            definition.contentRevision = 1;
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        *frontEnd.project, request->variables, materializations,
        request->containers, HarkenEditor::Bridge::kStudioCurrentWorldCapacity,
        request->transitions, request->actions,
        &request->busResult->idSet);
    if (!compiled.ok())
        return refuse(compiled.status == harkencurrentworldcompiler::CompileStatusV1::Capacity
                          || compiled.status == harkencurrentworldcompiler::CompileStatusV1::TransitionCapabilityUnavailable
                          ? S::UnavailableEngineContract : S::InvalidAuthoring,
                      QStringLiteral("Current World authoring compilation failed (%1, stable id %2)")
                          .arg(static_cast<uint32_t>(compiled.status))
                          .arg(objectIdString(compiled.relatedStableId)));
    if (compiled.world->View().trackScheduleCount != 0
        || compiled.world->View().pieceScheduleCount != 0)
        result.diagnostic = scheduleAdmissionDiagnostic();
    return result;
}

RuntimeCapabilityEntry evaluate(const ProjectItem& item,
                                const ProjectData& project,
                                const QString& operation,
                                RuntimeCapabilityFeature feature)
{
    auto parsed = parseOperation(operation);
    if (!parsed && operation == QStringLiteral("execute"))
        parsed = item.type == T::Action ? O::ActionRegistration : O::AuditionPlay;
    if (!parsed)
        return {item.type, O::LiveSync, feature, S::InvalidAuthoring,
                QStringLiteral("Unknown runtime capability operation")};
    return evaluate(item, project, *parsed, feature);
}

RuntimeCapabilityAssessment assess(const ProjectItem& item,
                                   const ProjectData& project,
                                   RuntimeCapabilityOperation operation,
                                   std::optional<BuildVariant> variant)
{
    RuntimeCapabilityAssessment assessment;
    assessment.operation = operation;
    assessment.rootStableId = item.runtimeId();
    assessment.variant = std::move(variant);
    assessment.backend = backendFor(operation, item.type);
    assessment.state = S::Ready;

    const auto add = [&](F feature) {
        const auto evaluated = evaluate(item, project, operation, feature, assessment.variant);
        if (evaluated.state == S::Ready) {
            if (!evaluated.diagnostic.isEmpty()
                && std::none_of(assessment.issues.begin(), assessment.issues.end(),
                    [&](const auto& issue) { return issue.diagnostic == evaluated.diagnostic; }))
                appendIssue(assessment, feature, S::Ready, evaluated.diagnostic, item);
            return;
        }
        appendStaticIssue(assessment, evaluated, item);
        assessment.state = worstState(assessment.state, evaluated.state);
    };
    add(F::StableIdentity);
    if (!operationSupports(item.type, operation)) {
        add(F::ContentClosure);
        return assessment;
    }
    if (operation == O::Delete) {
        if (item.type == T::Attenuation) add(F::AttenuationUnregister);
        return assessment;
    }
    switch (item.type) {
    case T::Action: add(F::ContentClosure); break;
    case T::Container:
        if (const auto* selection = item.comp<VSelection>(); selection
            && selection->transitions != canonicalTransitionTable())
            add(F::Transition);
        if (assessment.state == S::Ready) add(F::FlatContainer);
        break;
    case T::Track: add(F::TrackProgram); break;
    case T::Piece: add(F::PieceProgram); break;
    case T::Variable: add(F::VariableDeclaration); break;
    case T::Sound:
    case T::Layer:
    case T::Bus:
    case T::Attenuation: add(F::ContentClosure); break;
    default: add(F::ContentClosure); break;
    }
    return assessment;
}

RuntimeCapabilityAssessment assess(const ProjectData& project,
                                   harkenstudio::ObjectId rootStableId,
                                   RuntimeCapabilityOperation operation,
                                   std::optional<BuildVariant> variant)
{
    RuntimeCapabilityAssessment result;
    result.operation = operation;
    result.rootStableId = rootStableId;
    result.variant = std::move(variant);
    const ProjectItem* item = findByStableId(project, rootStableId);
    if (!item) {
        result.state = S::InvalidAuthoring;
        result.issues.push_back({F::StableIdentity, S::InvalidAuthoring,
                                 QStringLiteral("Root runtime stable identity does not resolve"),
                                 {rootStableId, {}, {}, -1, QStringLiteral("rootStableId"), {}}});
        return result;
    }
    return assess(*item, project, operation, std::move(result.variant));
}

QString featureName(RuntimeCapabilityFeature feature)
{
    switch (feature) {
    case F::ContentClosure: return QStringLiteral("contentClosure");
    case F::SoundPlayback: return QStringLiteral("soundPlayback");
    case F::FlatContainer: return QStringLiteral("flatContainer");
    case F::TrackProgram: return QStringLiteral("trackProgram");
    case F::PieceProgram: return QStringLiteral("pieceProgram");
    case F::VariableDeclaration: return QStringLiteral("variableDeclaration");
    case F::TypedBinding: return QStringLiteral("typedBinding");
    case F::TypedEffect: return QStringLiteral("typedEffect");
    case F::StableIdentity: return QStringLiteral("stableIdentity");
    case F::ActionPlay: return QStringLiteral("actionPlay");
    case F::ActionStop: return QStringLiteral("actionStop");
    case F::ActionPost: return QStringLiteral("actionPost");
    case F::ActionSetVariable: return QStringLiteral("actionSetVariable");
    case F::ActionReset: return QStringLiteral("actionReset");
    case F::ActionTarget: return QStringLiteral("actionTarget");
    case F::Quantize: return QStringLiteral("quantize");
    case F::IfSilent: return QStringLiteral("ifSilent");
    case F::Transition: return QStringLiteral("transition");
    case F::ContinuousGrid: return QStringLiteral("continuousGrid");
    case F::MultiAxisGrid: return QStringLiteral("multiAxisGrid");
    case F::QueueMemory: return QStringLiteral("queueMemory");
    case F::ShuffleMemory: return QStringLiteral("shuffleMemory");
    case F::ResumeMemory: return QStringLiteral("resumeMemory");
    case F::GridMapping: return QStringLiteral("gridMapping");
    case F::ClipAuthoredDuration: return QStringLiteral("clipAuthoredDuration");
    case F::ClipTargetTrack: return QStringLiteral("clipTargetTrack");
    case F::EntryPosition: return QStringLiteral("entryPosition");
    case F::NestedPlaceholder: return QStringLiteral("nestedPlaceholder");
    case F::ResourceMapping: return QStringLiteral("resourceMapping");
    case F::NonBusMuteSolo: return QStringLiteral("nonBusMuteSolo");
    case F::AttenuationUnregister: return QStringLiteral("attenuationUnregister");
    case F::RuntimeLanguageSwitch: return QStringLiteral("runtimeLanguageSwitch");
    }
    return {};
}

QString stateName(RuntimeCapabilityState state)
{
    switch (state) {
    case S::Ready: return QStringLiteral("Ready");
    case S::CompiledPreviewOnly: return QStringLiteral("CompiledPreviewOnly");
    case S::PackOnly: return QStringLiteral("PackOnly");
    case S::UnavailableEngineContract: return QStringLiteral("UnavailableEngineContract");
    case S::InvalidAuthoring: return QStringLiteral("InvalidAuthoring");
    }
    return {};
}

QString operationName(RuntimeCapabilityOperation operation)
{
    return legacyOperationName(operation);
}

QString backendName(RuntimeCapabilityBackend backend)
{
    switch (backend) {
    case B::None: return QStringLiteral("None");
    case B::Native: return QStringLiteral("Native");
    case B::CompiledPreview: return QStringLiteral("CompiledPreview");
    case B::Pack: return QStringLiteral("Pack");
    case B::CurrentWorld: return QStringLiteral("CurrentWorld");
    }
    return {};
}

const std::vector<RuntimeCapabilityEntry>& entries()
{
    static const std::vector<RuntimeCapabilityEntry> value = [] {
        std::vector<RuntimeCapabilityEntry> rows;
        const auto add = [&](T type, O operation, F feature, S state = S::Ready,
                             const QString& diagnostic = QString()) {
            rows.push_back(row(type, operation, feature, state, diagnostic));
        };
        for (const O operation : {O::PackBuild, O::LiveSync, O::ActionRegistration,
                                  O::ProjectInputCompile, O::CompiledPreview,
                                  O::AuditionPlay, O::Delete}) {
            for (const T type : {T::Sound, T::Layer, T::Container, T::Track,
                                 T::Piece, T::Variable, T::Action, T::Bus, T::Attenuation}) {
                const bool supported = operationSupports(type, operation);
                add(type, operation, F::ContentClosure,
                    supported ? S::Ready : S::UnavailableEngineContract,
                    supported ? QString() : QStringLiteral("%1 does not support %2")
                        .arg(ItemTypeCatalog::canonicalName(type), operationName(operation)));
                add(type, operation, F::StableIdentity);
                if (!supported) continue;
                if (type == T::Track) add(type, operation, F::TrackProgram, S::Ready,
                    operation == O::Delete ? QString() : scheduleAdmissionDiagnostic());
                if (type == T::Piece) add(type, operation, F::PieceProgram, S::Ready,
                    operation == O::Delete ? QString() : scheduleAdmissionDiagnostic());
                if (operation == O::Delete) continue;
                if (type == T::Sound || type == T::Layer) add(type, operation, F::SoundPlayback);
                if (type == T::Variable) add(type, operation, F::VariableDeclaration);
                if (type == T::Container)
                    for (const F feature : {F::FlatContainer, F::Transition, F::ContinuousGrid,
                                            F::MultiAxisGrid, F::QueueMemory, F::ShuffleMemory,
                                            F::ResumeMemory, F::GridMapping})
                        add(type, operation, feature, S::Ready,
                            QStringLiteral("Current World validates the Container mode, targets and dependencies"));
                if (type == T::Action)
                    for (const F feature : {F::ActionPlay, F::ActionStop, F::ActionPost,
                                            F::ActionSetVariable, F::ActionReset, F::ActionTarget,
                                            F::Quantize, F::IfSilent})
                        add(type, operation, feature, S::Ready,
                            QStringLiteral("Current World validates the Action target, Marker timing and reference"));
                if (type == T::Sound || type == T::Layer || type == T::Container
                    || type == T::Track || type == T::Piece || type == T::Bus) {
                    add(type, operation, F::TypedEffect, S::Ready,
                        QStringLiteral("Effect types and parameters must satisfy the selected compile target"));
                    add(type, operation, F::TypedBinding,
                        operation == O::ProjectInputCompile ? S::UnavailableEngineContract : S::Ready,
                        operation == O::ProjectInputCompile
                            ? QStringLiteral("Current World input does not carry typed bindings; sync and Pack use the binding bridge")
                            : QStringLiteral("Binding targets, Variables and curves require supported runtime lowering"));
                }
                if (type == T::Piece) add(type, operation, F::NestedPlaceholder);
                if (type == T::Track) {
                    add(type, operation, F::ClipAuthoredDuration, S::Ready,
                        QStringLiteral("Clips schedule their start and use authored duration as the playback boundary"));
                    add(type, operation, F::ClipTargetTrack, S::UnavailableEngineContract,
                        QStringLiteral("Track clips accept Sound, Container and Piece targets"));
                }
            }
        }
        add(T::Sound, O::LiveSync, F::NonBusMuteSolo, S::UnavailableEngineContract,
            QStringLiteral("Runtime has no non-Bus object mute or solo API"));
        add(T::Attenuation, O::Delete, F::AttenuationUnregister, S::UnavailableEngineContract,
            QStringLiteral("Runtime has no Attenuation unregister API"));
        add(T::Sound, O::AuditionPlay, F::RuntimeLanguageSwitch, S::Ready,
            QStringLiteral("Changing the Studio language rebuilds Current World for that language"));
        add(T::Sound, O::PackBuild, F::RuntimeLanguageSwitch, S::UnavailableEngineContract,
            QStringLiteral("Pack language selection is a build and load variant boundary"));
        return rows;
    }();
    return value;
}

std::optional<RuntimeCapabilityEntry> query(ProjectItem::Type objectType,
                                            RuntimeCapabilityOperation operation,
                                            RuntimeCapabilityFeature feature)
{
    for (const auto& entry : entries())
        if (entry.objectType == objectType && entry.operation == operation
            && entry.feature == feature)
            return entry;
    return std::nullopt;
}

std::optional<RuntimeCapabilityEntry> query(ProjectItem::Type objectType,
                                            const QString& operation,
                                            RuntimeCapabilityFeature feature)
{
    auto parsed = parseOperation(operation);
    if (!parsed && operation == QStringLiteral("execute"))
        parsed = objectType == T::Action ? O::ActionRegistration : O::AuditionPlay;
    return parsed ? query(objectType, *parsed, feature) : std::nullopt;
}

QJsonObject toJson()
{
    QJsonArray values;
    for (const auto& entry : entries())
        values.append(entry.toJson());
    return {{QStringLiteral("entries"), values}};
}

}
}
