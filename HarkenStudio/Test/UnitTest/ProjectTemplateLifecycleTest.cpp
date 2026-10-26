#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/FieldGroupTestUtils.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "FieldGroupProjectAdapter.h"
#include "AuthoringPolicy.h"
#include "Components/VAction.h"
#include "Components/VAttenuation.h"
#include "Components/VMixNode.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "Components/VSpatial.h"
#include "Components/VVariable.h"
#include "ProjectSerializer.h"
#include "ProjectTemplateLoader.h"
#include "TemplateApplyService.h"

#include <QJsonDocument>
#include <gtest/gtest.h>

#include <algorithm>

using namespace VNX;

namespace {

QJsonObject minimalTemplate()
{
    return QJsonObject{
        {QStringLiteral("displayName"), QStringLiteral("Template")},
        {QStringLiteral("description"), QString()},
        {QStringLiteral("bus"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("Master Bus")},
            {QStringLiteral("children"), QJsonArray{}}}},
        {QStringLiteral("attenuation"), QJsonArray{}}};
}

std::shared_ptr<ProjectItem> makeItem(ProjectData& project, const QString& name,
                                      ProjectItem::Type type)
{
    auto item = std::make_shared<ProjectItem>(name, type);
    item->attachDefaultComponents();
    item->objectID = project.generateObjectID();
    item->guid = QUuid::createUuid();
    item->guid = QUuid::createUuid();
    item->objectPath = QStringLiteral("\\") + name;
    return item;
}

ProjectFileBinding binding(std::shared_ptr<ProjectItem> item, const QString& folder)
{
    ProjectFileBinding result;
    result.parentFolder = folder;
    result.rootItem = std::move(item);
    return result;
}

void addChild(const std::shared_ptr<ProjectItem>& parent,
              const std::shared_ptr<ProjectItem>& child)
{
    child->parent = parent.get();
    child->parentRef.kind = ParentRef::Kind::Item;
    child->parentRef.guid = parent->guid;
    child->objectPath = parent->objectPath + QStringLiteral("\\") + child->name;
    parent->children.push_back(child);
}

ProjectData makeProjectWithMaster()
{
    ProjectData project;
    project.name = QStringLiteral("Lifecycle Template");
    project.description = QStringLiteral("Reusable authoring data");
    project.busses.push_back(binding(
        makeItem(project, QStringLiteral("Master Bus"), ProjectItem::Type::Bus),
        QStringLiteral("Busses")));
    return project;
}

bool hasErrorPath(const ValidationResult& result, const QString& path)
{
    return std::any_of(result.errors.cbegin(), result.errors.cend(),
                       [&](const ValidationError& error) {
                           return error.jsonPath == path;
                       });
}

}

TEST(ProjectTemplateLifecycleTest, LegacyCurrentAndFutureVersionsAreGated)
{
    QJsonObject legacy = minimalTemplate();
    ValidationResult validation;
    auto parsed = ProjectTemplateLoader::parse(legacy, &validation);
    ASSERT_TRUE(parsed) << validation.toUserText().toStdString();
    EXPECT_EQ(parsed->templateFormatVersion, ProjectTemplateData::kLegacyFormatVersion);
    EXPECT_EQ(parsed->toJson().value(QStringLiteral("templateFormatVersion")).toInt(),
              ProjectTemplateData::kCurrentFormatVersion);

    QJsonObject current = parsed->toJson();
    parsed = ProjectTemplateLoader::parse(current, &validation);
    ASSERT_TRUE(parsed) << validation.toUserText().toStdString();
    EXPECT_EQ(parsed->templateFormatVersion, ProjectTemplateData::kCurrentFormatVersion);

    current[QStringLiteral("templateFormatVersion")] =
        ProjectTemplateData::kCurrentFormatVersion + 1;
    EXPECT_FALSE(ProjectTemplateLoader::parse(current, &validation));
    EXPECT_TRUE(hasErrorPath(validation, QStringLiteral("templateFormatVersion")));

    current[QStringLiteral("templateFormatVersion")] = 1.5;
    EXPECT_FALSE(ProjectTemplateLoader::parse(current, &validation));
    EXPECT_TRUE(hasErrorPath(validation, QStringLiteral("templateFormatVersion")));
}

TEST(ProjectTemplateLifecycleTest, RejectsMalformedAndRetiredAuthoringSections)
{
    struct Case {
        QString key;
        QJsonValue value;
        QString errorPath;
    };
    const std::vector<Case> cases{
        {QStringLiteral("content"), QJsonObject{}, QStringLiteral("content")},
        {QStringLiteral("variables"), QJsonArray{QStringLiteral("bad")},
         QStringLiteral("variables[0]")},
        {QStringLiteral("actions"),
         QJsonArray{QJsonObject{{QStringLiteral("type"),
                                 static_cast<int>(ProjectItem::Type::Variable)}}},
         QStringLiteral("actions[0].type")},
        {QStringLiteral("switch"), QJsonArray{}, QStringLiteral("switch")},
        {QStringLiteral("state"), QJsonArray{}, QStringLiteral("state")},
        {QStringLiteral("legacyVariables"), QJsonArray{},
         QStringLiteral("legacyVariables")}};
    for (const Case& testCase : cases) {
        QJsonObject object = minimalTemplate();
        object[testCase.key] = testCase.value;
        const ValidationResult result = ProjectTemplateLoader::validate(
            QJsonDocument(object), testCase.key);
        EXPECT_FALSE(result.valid) << testCase.key.toStdString();
        EXPECT_TRUE(hasErrorPath(result, testCase.errorPath))
            << testCase.key.toStdString();
    }
}

TEST(ProjectTemplateLifecycleTest, RejectsPlatformReferencesBeforeApplyingTemplate)
{
    ProjectData source = makeProjectWithMaster();
    auto sound = makeItem(source, QStringLiteral("Scoped"), ProjectItem::Type::Sound);
    source.layers.push_back(binding(sound, QStringLiteral("Layers")));
    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareReferenceWrite(source));
    const auto serialized = serializer.projectItemToJson(*sound);
    const auto platformId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    for (bool nested : {false, true}) {
        auto content = serialized;
        const QJsonObject overrides{{QStringLiteral("platformOverrides"), QJsonArray{
            QJsonObject{{QStringLiteral("platformId"), platformId},
                        {QStringLiteral("presence"), QStringLiteral("0000000000000001")},
                        {QStringLiteral("volume"), -6.0}}}}};
        content[QStringLiteral("playableProperties")] = nested
            ? QJsonObject{{QStringLiteral("rows"), QJsonArray{overrides}}} : overrides;
        auto json = minimalTemplate();
        json[QStringLiteral("content")] = QJsonArray{content};
        EXPECT_FALSE(ProjectTemplateLoader::parse(json));
        ProjectTemplateData typed;
        typed.displayName = QStringLiteral("Scoped");
        typed.bus.name = QStringLiteral("Master Bus");
        typed.content = QJsonArray{content};
        auto target = makeProjectWithMaster();
        const auto beforeId = target.nextObjectID;
        QString error;
        EXPECT_FALSE(TemplateApplyService::seedTemplateInto(target, typed, &error));
        EXPECT_TRUE(error.contains(QStringLiteral("platform")));
        EXPECT_TRUE(target.layers.empty());
        EXPECT_EQ(target.nextObjectID, beforeId);
    }
    auto content = serialized;
    VSound scopedSound;
    scopedSound.sources.plain.platforms[platformId].active = QUuid{};
    content[QStringLiteral("components")] = QJsonObject{{QStringLiteral("sound"), scopedSound.toJson()}};
    auto json = minimalTemplate();
    json[QStringLiteral("content")] = QJsonArray{content};
    EXPECT_FALSE(ProjectTemplateLoader::parse(json));
}

TEST(ProjectTemplateLifecycleTest, DuplicateDisplayNamesSurviveTemplateInstantiation)
{
    const QString name = QStringLiteral("Cafe\u0301");
    const QJsonObject bus{{QStringLiteral("name"), name}};
    const QJsonObject attenuation{
        {QStringLiteral("name"), name},
        {QStringLiteral("max"), 10.0},
        {QStringLiteral("volume"), QJsonObject{
            {QStringLiteral("curve"), QStringLiteral("linear")},
            {QStringLiteral("points"), QJsonArray{QJsonArray{0.0, 0.0}, QJsonArray{10.0, -96.0}}}}}};
    auto json = minimalTemplate();
    json[QStringLiteral("bus")] = QJsonObject{
        {QStringLiteral("name"), QStringLiteral("Master Bus")},
        {QStringLiteral("children"), QJsonArray{bus, bus}}};
    json[QStringLiteral("attenuation")] = QJsonArray{attenuation, attenuation};
    ProjectData target = makeProjectWithMaster();
    QString error;
    ASSERT_TRUE(TemplateApplyService::seedTemplateInto(target, json, &error)) << error.toStdString();
    const auto& busses = target.busses.front().rootItem->children;
    ASSERT_EQ(busses.size(), 2u);
    ASSERT_EQ(target.attenuationItems.size(), 2u);
    EXPECT_EQ(busses[0]->name, QStringLiteral("Caf\u00e9"));
    EXPECT_EQ(busses[0]->name, busses[1]->name);
    EXPECT_NE(busses[0]->guid, busses[1]->guid);
    EXPECT_EQ(target.attenuationItems[0].rootItem->name, busses[0]->name);
    EXPECT_EQ(target.attenuationItems[0].rootItem->name, target.attenuationItems[1].rootItem->name);
    EXPECT_NE(target.attenuationItems[0].rootItem->guid, target.attenuationItems[1].rootItem->guid);
    ProjectSerializer serializer;
    EXPECT_TRUE(serializer.prepareReferenceWrite(target)) << serializer.lastError().toStdString();
}

TEST(ProjectTemplateLifecycleTest, PostActionCyclesRejectBeforeAnyTemplateSectionIsApplied)
{
    ProjectData source = makeProjectWithMaster();
    auto first = makeItem(source, QStringLiteral("First"), ProjectItem::Type::Action);
    auto second = makeItem(source, QStringLiteral("Second"), ProjectItem::Type::Action);
    source.actions.push_back(binding(first, QStringLiteral("Actions")));
    source.actions.push_back(binding(second, QStringLiteral("Actions")));
    VActionStep firstStep;
    firstStep.type = VActionStep::Type::PostAction;
    firstStep.targetObjectID = second->objectID;
    firstStep.targetName = second->name;
    firstStep.delay = 500.0f;
    first->comp<VAction>()->steps = {firstStep};
    VActionStep secondStep = firstStep;
    secondStep.targetObjectID = first->objectID;
    secondStep.targetName = first->name;
    second->comp<VAction>()->steps = {secondStep};

    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareReferenceWrite(source));
    QJsonObject json = minimalTemplate();
    json[QStringLiteral("bus")] = QJsonObject{
        {QStringLiteral("name"), QStringLiteral("Master Bus")},
        {QStringLiteral("children"), QJsonArray{QJsonObject{
            {QStringLiteral("name"), QStringLiteral("Pending Bus")}}}}};
    json[QStringLiteral("actions")] = QJsonArray{
        serializer.projectItemToJson(*first), serializer.projectItemToJson(*second)};
    ProjectData target = makeProjectWithMaster();
    const auto nextObjectId = target.nextObjectID;
    QString error;
    EXPECT_FALSE(TemplateApplyService::seedTemplateInto(target, json, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("cycle"), Qt::CaseInsensitive));
    EXPECT_TRUE(target.actions.empty());
    EXPECT_TRUE(target.busses.front().rootItem->children.empty());
    EXPECT_TRUE(target.actionPublishedNameHistory.empty());
    EXPECT_EQ(target.nextObjectID, nextObjectId);

    second->comp<VAction>()->steps.clear();
    json[QStringLiteral("actions")] = QJsonArray{
        serializer.projectItemToJson(*first), serializer.projectItemToJson(*second)};
    ASSERT_TRUE(TemplateApplyService::seedTemplateInto(target, json, &error))
        << error.toStdString();
    EXPECT_EQ(target.actions.size(), 2u);
    EXPECT_EQ(target.busses.front().rootItem->children.size(), 1u);
}

TEST(ProjectTemplateLifecycleTest, ParseApplyRemintsIdentityAndReferences)
{
    ProjectData source = makeProjectWithMaster();
    auto musicBus = makeItem(source, QStringLiteral("Music"), ProjectItem::Type::Bus);
    const uint32_t oldBusObjectId = musicBus->objectID;
    const harkenstudio::ObjectId oldBusStableId = musicBus->runtimeId();
    addChild(source.busses.front().rootItem, musicBus);

    auto attenuation = makeItem(source, QStringLiteral("Near"),
                                ProjectItem::Type::Attenuation);
    auto* attenuationComponent = attenuation->comp<VAttenuation>();
    ASSERT_NE(attenuationComponent, nullptr);
    attenuationComponent->maxDistance = 25.0f;
    auto& volume = attenuationComponent->dimensions.emplaceBack(VNX::AttenuationDimensionDef{AttenuationDimension::Volume});
    volume.curve = {{0.0f, 0.0f}, {25.0f, -96.0f}};
    attenuationNormalizeCurve(AttenuationDimension::Volume, volume.curve, volume.handles);
    const QUuid oldAttenuationGuid = attenuation->guid;
    const harkenstudio::ObjectId oldAttenuationStableId = attenuation->runtimeId();
    source.attenuationItems.push_back(binding(attenuation, QStringLiteral("Attenuations")));

    auto variable = makeItem(source, QStringLiteral("Intensity"),
                             ProjectItem::Type::Variable);
    variable->comp<VVariable>()->defaultValue = 40.0f;
    const QUuid oldVariableGuid = variable->guid;
    source.variables.push_back(binding(variable, QStringLiteral("Variables")));

    auto layer = makeItem(source, QStringLiteral("Music Layer"), ProjectItem::Type::Layer);
    layer->fieldGroups = MakeRootFieldGroups(
        toFieldGroupGuid(source.busses.front().rootItem->guid));
    auto sound = makeItem(source, QStringLiteral("Theme"), ProjectItem::Type::Sound);
    auto* soundComponent = sound->comp<VSound>();
    ASSERT_NE(soundComponent, nullptr);
    SoundSourceDef sourceRow;
    sourceRow.entryId = QUuid::createUuid();
    sourceRow.sourceId = 17;
    soundComponent->sources.plain.entries.push_back(sourceRow);
    soundComponent->sources.plain.active = sourceRow.entryId;
    auto* mix = sound->comp<VMixNode>();
    ASSERT_NE(mix, nullptr);
    sound->fieldGroups.mainOutput.shared = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(musicBus->guid), 0.0f}};
    ParameterBindingDef parameterBinding;
    parameterBinding.guid = QUuid::createUuid();
    const harkenstudio::ObjectId oldBindingStableId = parameterBinding.runtimeId();
    parameterBinding.variable = oldVariableGuid;
    parameterBinding.address = BindingSemantics::volumeAddress();
    parameterBinding.curve = {{0.0f, -96.0f}, {100.0f, 0.0f}};
    ASSERT_TRUE(mix->bindings.replace({parameterBinding}));
    auto* spatial = sound->comp<VSpatial>();
    ASSERT_NE(spatial, nullptr);
    SpatialRatingFieldGroup rating;
    rating.spatialAttenuationRef = toFieldGroupGuid(oldAttenuationGuid);
    sound->fieldGroups.spatialRating.shared = {FieldGroupMode::Local, rating};
    sound->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {FieldGroupMode::Local, rating};
    sound->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {FieldGroupMode::Inherit, rating};
    const uint32_t oldSoundObjectId = sound->objectID;
    const QUuid oldSoundGuid = sound->guid;
    const QUuid oldSourceEntryId = sourceRow.entryId;
    addChild(layer, sound);
    source.layers.push_back(binding(layer, QStringLiteral("Layers")));

    auto action = makeItem(source, QStringLiteral("Play_Theme"), ProjectItem::Type::Action);
    VActionStep step;
    step.type = VActionStep::Type::Play;
    step.targetObjectID = oldSoundObjectId;
    step.targetName = sound->name;
    step.quantize.referenceObjectID = oldSoundObjectId;
    step.quantize.referenceName = sound->name;
    action->comp<VAction>()->steps.push_back(step);
    source.actions.push_back(binding(action, QStringLiteral("Actions")));

    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareReferenceWrite(source))
        << serializer.lastError().toStdString();
    ProjectTemplateData templateData;
    templateData.displayName = source.name;
    templateData.description = source.description;
    templateData.bus.name = source.busses.front().rootItem->name;
    templateData.bus.authoring =
        serializer.projectItemToJson(*source.busses.front().rootItem);
    templateData.bus.authoring.remove(QStringLiteral("children"));
    TemplateBusNode musicBusTemplate;
    musicBusTemplate.name = musicBus->name;
    musicBusTemplate.authoring = serializer.projectItemToJson(*musicBus);
    musicBusTemplate.authoring.remove(QStringLiteral("children"));
    templateData.bus.children.push_back(std::move(musicBusTemplate));

    TemplateAttenuationDef attenuationTemplate;
    attenuationTemplate.name = attenuation->name;
    attenuationTemplate.maxDistance = attenuationComponent->maxDistance;
    attenuationTemplate.volumeCurve = QStringLiteral("linear");
    attenuationTemplate.authoring = serializer.projectItemToJson(*attenuation);
    for (const auto& point : volume.curve)
        attenuationTemplate.volumePoints.append(QPointF(point.x, point.value));
    templateData.attenuations.push_back(std::move(attenuationTemplate));
    templateData.variables.append(serializer.projectItemToJson(*variable));
    templateData.content.append(serializer.projectItemToJson(*layer));
    templateData.actions.append(serializer.projectItemToJson(*action));

    ValidationResult validation;
    EXPECT_FALSE(ProjectTemplateLoader::parse(templateData.toJson(), &validation));
    EXPECT_TRUE(validation.toUserText().contains(QStringLiteral("platform field groups")));
    sound->fieldGroups.spatialRating.platforms.clear();
    templateData.content = QJsonArray{serializer.projectItemToJson(*layer)};
    const auto parsed = ProjectTemplateLoader::parse(templateData.toJson(), &validation);
    ASSERT_TRUE(parsed) << validation.toUserText().toStdString();
    ASSERT_EQ(parsed->content.size(), 1);
    ASSERT_TRUE(parsed->content.at(0).isObject());
    EXPECT_EQ(parsed->content.at(0).toObject().value(QStringLiteral("type")).toString(),
              QStringLiteral("Layer"));

    ProjectData target = makeProjectWithMaster();
    target.nextObjectID = 100;
    QString error;
    ASSERT_TRUE(TemplateApplyService::seedTemplateInto(target, *parsed, &error))
        << error.toStdString();
    ASSERT_EQ(target.busses.front().rootItem->children.size(), 1u);
    ASSERT_EQ(target.attenuationItems.size(), 1u);
    ASSERT_EQ(target.variables.size(), 1u);
    ASSERT_EQ(target.layers.size(), 1u);
    ASSERT_EQ(target.actions.size(), 1u);

    const auto& newBus = target.busses.front().rootItem->children.front();
    const auto& newAttenuation = target.attenuationItems.front().rootItem;
    const auto& newVariable = target.variables.front().rootItem;
    const auto& newLayer = target.layers.front().rootItem;
    ASSERT_EQ(newLayer->children.size(), 1u);
    const auto& newSound = newLayer->children.front();
    const auto& newAction = target.actions.front().rootItem;
    EXPECT_NE(newBus->objectID, oldBusObjectId);
    EXPECT_NE(newBus->runtimeId(), oldBusStableId);
    EXPECT_NE(newAttenuation->guid, oldAttenuationGuid);
    EXPECT_NE(newVariable->guid, oldVariableGuid);
    EXPECT_NE(newSound->objectID, oldSoundObjectId);
    EXPECT_NE(newSound->guid, oldSoundGuid);
    EXPECT_EQ(newSound->parentRef.guid, newLayer->guid);
    EXPECT_EQ(newSound->objectPath, QStringLiteral("\\Music Layer\\Theme"));

    const auto* newMix = newSound->comp<VMixNode>();
    ASSERT_NE(newMix, nullptr);
    EXPECT_EQ(newMix->outputBusId, newBus->objectID);
    ASSERT_EQ(newMix->bindings.size(), 1u);
    EXPECT_NE(newMix->bindings.entries().front().runtimeId(), oldBindingStableId);
    EXPECT_EQ(newMix->bindings.entries().front().variable, newVariable->guid);
    const auto* newSpatial = newSound->comp<VSpatial>();
    ASSERT_NE(newSpatial, nullptr);
    EXPECT_EQ(newSpatial->attenuationRef, newAttenuation->runtimeId());
    EXPECT_EQ(newSound->playablePropertyPresence.bits & kFieldGroupPropertyMask, 0u);
    EXPECT_EQ(newSound->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef,
              toFieldGroupGuid(newBus->guid));
    EXPECT_EQ(newSound->fieldGroups.spatialRating.shared.localHistory->spatialAttenuationRef,
              toFieldGroupGuid(newAttenuation->guid));
    EXPECT_TRUE(newSound->fieldGroups.spatialRating.platforms.empty());

    const auto* newSoundComponent = newSound->comp<VSound>();
    ASSERT_NE(newSoundComponent, nullptr);
    ASSERT_EQ(newSoundComponent->sources.plain.entries.size(), 1u);
    ASSERT_FALSE(newSoundComponent->sources.plain.active.isNull());
    EXPECT_NE(newSoundComponent->sources.plain.entries.front().entryId, oldSourceEntryId);
    EXPECT_EQ(newSoundComponent->sources.plain.active,
              newSoundComponent->sources.plain.entries.front().entryId);

    const auto* newActionComponent = newAction->comp<VAction>();
    ASSERT_NE(newActionComponent, nullptr);
    ASSERT_EQ(newActionComponent->steps.size(), 1u);
    EXPECT_EQ(newActionComponent->steps.front().targetObjectID, newSound->objectID);
    EXPECT_EQ(newActionComponent->steps.front().quantize.referenceObjectID,
              newSound->objectID);
}

TEST(ProjectTemplateLifecycleTest, MusicalSubtreeRejectsBeforeAnyTemplateSectionIsApplied)
{
    ProjectData source = makeProjectWithMaster();
    addChild(source.busses.front().rootItem,
             makeItem(source, QStringLiteral("New Bus"), ProjectItem::Type::Bus));
    auto layer = makeItem(source, QStringLiteral("Nested Layer"), ProjectItem::Type::Layer);
    layer->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(source.busses.front().rootItem->guid));
    auto piece = makeItem(source, QStringLiteral("Legacy Piece"), ProjectItem::Type::Piece);
    piece->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    addChild(layer, piece);
    source.layers.push_back(binding(layer, QStringLiteral("Layers")));
    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareReferenceWrite(source)) << serializer.lastError().toStdString();
    auto json = minimalTemplate();
    json[QStringLiteral("bus")] = serializer.projectItemToJson(*source.busses.front().rootItem);
    json[QStringLiteral("content")] = QJsonArray{serializer.projectItemToJson(*layer)};

    ProjectData target = makeProjectWithMaster();
    const auto nextObject = target.nextObjectID;
    QString error;
    EXPECT_FALSE(TemplateApplyService::seedTemplateInto(target, json, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("absolute-time")));
    EXPECT_TRUE(target.layers.empty());
    EXPECT_TRUE(target.busses.front().rootItem->children.empty());
    EXPECT_EQ(target.nextObjectID, nextObject);
}

TEST(ProjectTemplateLifecycleTest, StoredImplicitMusicalTransitionDefaultRequiresExplicitSupportedAuthoring)
{
    ProjectData source = makeProjectWithMaster();
    auto container = makeItem(source, QStringLiteral("Stored Container"), ProjectItem::Type::Container);
    container->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(source.busses.front().rootItem->guid));
    source.layers.push_back(binding(container, QStringLiteral("Layers")));
    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareReferenceWrite(source));
    auto json = minimalTemplate();
    json[QStringLiteral("bus")] = serializer.projectItemToJson(*source.busses.front().rootItem);
    json[QStringLiteral("content")] = QJsonArray{serializer.projectItemToJson(*container)};
    EXPECT_FALSE(ProjectTemplateLoader::parse(json));
    EXPECT_EQ(container->comp<VSelection>()->transitions, canonicalTransitionTable());

    container->comp<VSelection>()->transitions = AuthoringPolicy::defaultTransitionTable();
    json[QStringLiteral("content")] = QJsonArray{serializer.projectItemToJson(*container)};
    ProjectData target = makeProjectWithMaster();
    QString error;
    ASSERT_TRUE(TemplateApplyService::seedTemplateInto(target, json, &error)) << error.toStdString();
    ASSERT_EQ(target.layers.size(), 1u);
    EXPECT_EQ(target.layers.front().rootItem->comp<VSelection>()->transitions,
              AuthoringPolicy::defaultTransitionTable());
}
