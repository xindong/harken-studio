#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ProjectManager.h"
#include "ProjectSerializer.h"
#include "ProjectItemSaveState.h"
#include "ItemTypeCatalog.h"
#include "Components/VAction.h"
#include "Components/VVariable.h"
#include "ProjectTypes.h"
#include "PlayablePropertyAuthoringV1.h"
#include "Components/VSelection.h"
#include "TransitionSemantics.h"
#include "Support/ProjectDocumentTestUtils.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <limits>

using VNX::ProjectData;
using VNX::ProjectItem;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

std::shared_ptr<ProjectItem> addChild(ProjectItem& parent,
                                      const char* name,
                                      ProjectItem::Type type)
{
    auto child = std::make_shared<ProjectItem>(QLatin1String(name), type);
    child->attachDefaultComponents();
    child->guid = QUuid::createUuid();
    child->parent = &parent;
    parent.children.push_back(child);
    if (parent.type == ProjectItem::Type::Container) {
        EXPECT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(
            parent, child->guid));
    }
    return child;
}

QMap<QString, QByteArray> projectFileBytes(const QString& root)
{
    QMap<QString, QByteArray> out;
    const QDir rootDir(root);
    QDirIterator it(root,
                    QStringList() << QStringLiteral("*.hsp") << QStringLiteral("*.vp"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString abs = it.next();
        QFile f(abs);
        if (f.open(QIODevice::ReadOnly))
            out.insert(rootDir.relativeFilePath(abs), f.readAll());
    }
    return out;
}

QJsonObject canonicalPlayableProperties()
{
    return QJsonObject{{QStringLiteral("presence"),
                        QStringLiteral("0000000000000000")}};
}

}

TEST(ProjectSerialization, NewProjectIdentityRemainsStableAcrossSaves)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    ProjectData project = HarkenTest::projectWithPlatforms();
    project.name = QStringLiteral("Identity");
    project.projectPath = QDir(tempDir.path()).filePath(QStringLiteral("Identity.hsp"));
    auto master = std::make_shared<ProjectItem>(QStringLiteral("Master"), ProjectItem::Type::Bus);
    master->objectID = 1;
    master->guid = VNX::authoringUuid(harkentest::StudioObjectId(1));
    master->attachDefaultComponents();
    project.busses.push_back({master, QStringLiteral("Busses")});

    VNX::ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareProjectSave(project.projectPath, project))
        << serializer.lastError().toStdString();
    ASSERT_TRUE(serializer.commitProjectSave());

    QFile firstFile(project.projectPath);
    ASSERT_TRUE(firstFile.open(QIODevice::ReadOnly));
    const QString firstId =
        HarkenTest::projectHeader(
            QJsonDocument::fromJson(firstFile.readAll()).object())
            .value(QStringLiteral("id")).toString();
    firstFile.close();
    ASSERT_FALSE(firstId.isEmpty());
    EXPECT_EQ(project.projectId, firstId);

    ASSERT_TRUE(serializer.prepareProjectSave(project.projectPath, project));
    ASSERT_TRUE(serializer.commitProjectSave());

    QFile secondFile(project.projectPath);
    ASSERT_TRUE(secondFile.open(QIODevice::ReadOnly));
    const QString secondId =
        HarkenTest::projectHeader(
            QJsonDocument::fromJson(secondFile.readAll()).object())
            .value(QStringLiteral("id")).toString();
    EXPECT_EQ(secondId, firstId);
    EXPECT_EQ(project.projectId, firstId);
}

TEST(ProjectSerialization, DuplicateTransitionEndpointsRejectItemLoad)
{
    ProjectItem container(QStringLiteral("Container"),
                          ProjectItem::Type::Container);
    container.guid = QUuid::createUuid();
    container.attachDefaultComponents();
    VNX::TransitionRuleDef override = VNX::canonicalTransitionRule();
    override.source = QUuid::createUuid();
    override.target = QUuid::createUuid();
    ASSERT_TRUE(container.comp<VNX::VSelection>()->transitions.addRule(override));

    VNX::ProjectSerializer serializer;
    QJsonObject saved = serializer.projectItemToJson(container);
    ProjectItem original;
    ASSERT_TRUE(serializer.projectItemFromJson(saved, original));
    QJsonObject components = saved[QStringLiteral("components")].toObject();
    QJsonObject selection = components[QStringLiteral("selection")].toObject();
    QJsonObject transitions = selection[QStringLiteral("transitions")].toObject();
    QJsonArray rules = transitions[QStringLiteral("rules")].toArray();
    rules.append(rules.last());
    transitions[QStringLiteral("rules")] = rules;
    selection[QStringLiteral("transitions")] = transitions;
    components[QStringLiteral("selection")] = selection;
    saved[QStringLiteral("components")] = components;

    ProjectItem rejected;
    EXPECT_FALSE(serializer.projectItemFromJson(saved, rejected));
}

TEST(ProjectSerialization, NestedTreeDeserializationRestoresParentLinks)
{
    ProjectItem layer(QStringLiteral("Amb_Field"), ProjectItem::Type::Layer);
    layer.attachDefaultComponents();
    layer.guid = QUuid::createUuid();

    addChild(layer, "Amb_Rain", ProjectItem::Type::Sound);
    auto birds = addChild(layer, "Birds", ProjectItem::Type::Container);
    addChild(*birds, "Chirp_A", ProjectItem::Type::Sound);
    auto seq   = addChild(*birds, "Seq", ProjectItem::Type::Track);
    auto intro = addChild(*seq, "Intro", ProjectItem::Type::Piece);
    auto tail  = addChild(*intro, "Tail", ProjectItem::Type::Track);
    addChild(*tail, "Tail_Hit", ProjectItem::Type::Sound);

    VNX::ProjectSerializer serializer;

    std::function<QJsonObject(const ProjectItem&)> inlineWindowForm =
        [&](const ProjectItem& node) {
            QJsonObject json = serializer.audioItemToJson(node);
            QJsonArray children;
            for (const auto& child : node.children)
                children.append(inlineWindowForm(*child));
            json[QStringLiteral("children")] = children;
            return json;
        };
    const QJsonObject json = inlineWindowForm(layer);
    ASSERT_FALSE(json.isEmpty());

    ProjectItem reloaded(QString(), ProjectItem::Type::Layer);
    ASSERT_TRUE(serializer.audioItemFromJson(json, reloaded));

    ASSERT_EQ(reloaded.children.size(), 2u);
    const auto& birds2 = reloaded.children[1];
    EXPECT_EQ(birds2->parent, &reloaded);

    ASSERT_EQ(birds2->children.size(), 2u);
    const auto& seq2 = birds2->children[1];

    ASSERT_EQ(seq2->children.size(), 1u);
    const auto& intro2 = seq2->children[0];
    EXPECT_EQ(intro2->parent, seq2.get());

    ASSERT_EQ(intro2->children.size(), 1u);
    const auto& tail2 = intro2->children[0];
    ASSERT_EQ(tail2->children.size(), 1u);
    EXPECT_EQ(tail2->children[0]->parent, tail2.get());
}

TEST(ProjectSerialization, RejectsOrphanContentPackWithoutChangingDisk)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.name = QStringLiteral("V6Orphan");
    config.location = tempDir.path();
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_TRUE(pm.createProject(config));
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);
    const QString projectRoot = QDir(tempDir.path()).filePath(config.name);
    const QString projectPath = project->projectPath;
    ASSERT_TRUE(pm.saveProject());
    pm.closeProject();
    const QJsonObject rootDocument = QJsonDocument::fromJson(
        projectFileBytes(projectRoot).value(QFileInfo(projectPath).fileName())).object();
    const QString projectId = HarkenTest::projectHeader(rootDocument)
                                  .value(QStringLiteral("id")).toString();

    const QString shotGuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString missingGuid =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject missingRef{{QStringLiteral("kind"), QStringLiteral("item")},
                                 {QStringLiteral("id"), missingGuid}};
    {
        QFile f(QDir(projectRoot).filePath(
            QStringLiteral("Layers/Orph_Shot.vp")));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(HarkenTest::projectDocument(
                    QStringLiteral("project.layer"), shotGuid, projectId,
                    QJsonObject{
                         {QStringLiteral("name"), QStringLiteral("Orph_Shot")},
                         {QStringLiteral("type"), QStringLiteral("Sound")},
                         {QStringLiteral("guid"), shotGuid},
                         {QStringLiteral("parentRef"), missingRef},
                         {QStringLiteral("playableProperties"),
                          canonicalPlayableProperties()},
                         {QStringLiteral("children"), QJsonArray{}},
                         {QStringLiteral("order"), 0},
                    }))
                    .toJson(QJsonDocument::Indented));
    }

    const auto before = projectFileBytes(projectRoot);
    EXPECT_FALSE(pm.loadProject(projectPath));
    EXPECT_EQ(pm.currentProject(), nullptr);
    EXPECT_EQ(projectFileBytes(projectRoot), before);

    pm.closeProject();
}

namespace {

std::shared_ptr<ProjectItem> addSaveStateRoot(
    ProjectData& project, const QString& name, ProjectItem::Type type, uint32_t objectId)
{
    auto item = std::make_shared<ProjectItem>(name, type);
    item->objectID = objectId;
    item->guid = QUuid::createUuid();
    item->attachDefaultComponents();
    auto* bucket = project.getBucketForType(type);
    bucket->push_back({item, project.folderNameForBucket(bucket)});
    return item;
}

class ProjectSerializationSavedState : public ::testing::TestWithParam<bool>
{
protected:
    bool markSaved(VNX::ProjectItemSaveState& state, const ProjectData& project)
    {
        if (!GetParam()) return state.markSaved(project);
        VNX::ProjectSerializer serializer;
        if (!serializer.prepareReferenceWrite(project)) return false;
        QHash<QUuid, QJsonObject> serializedItems;
        const auto capture = [&](const auto& self, const ProjectItem& item) -> bool {
            if (VNX::ItemTypeCatalog::bucketOf(item.type)
                == VNX::ItemTypeCatalog::Bucket::Layers) {
                auto json = serializer.audioItemToJson(item);
                if (json.isEmpty()) return false;
                serializedItems.insert(item.guid, std::move(json));
            }
            for (const auto& child : item.children)
                if (!self(self, *child)) return false;
            return true;
        };
        for (const auto* binding : project.allFileBindings())
            if (!capture(capture, *binding->rootItem)) return false;
        return state.markSaved(project, serializer, serializedItems);
    }
};

}

TEST_P(ProjectSerializationSavedState, SavedContentTracksValuesAcrossObjectKindsAndSavePoints)
{
    ProjectData project = HarkenTest::projectWithPlatforms();
    std::vector<std::shared_ptr<ProjectItem>> items;
    uint32_t objectId = 1;
    for (const auto type : {ProjectItem::Type::Layer, ProjectItem::Type::Sound,
                           ProjectItem::Type::Container, ProjectItem::Type::Track,
                           ProjectItem::Type::Piece, ProjectItem::Type::Bus,
                           ProjectItem::Type::Action, ProjectItem::Type::Variable,
                           ProjectItem::Type::Attenuation}) {
        items.push_back(addSaveStateRoot(project, QStringLiteral("Object%1").arg(objectId),
                                        type, objectId));
        ++objectId;
    }
    VNX::ProjectItemSaveState saved;
    ASSERT_TRUE(markSaved(saved, project));
    EXPECT_TRUE(saved.refresh(project).isEmpty());
    for (const auto& item : items) {
        item->expanded = true;
        item->objectPath = QStringLiteral("Changed/Derived/Path");
        ++item->nameBindingRevision;
    }
    EXPECT_TRUE(saved.refresh(project).isEmpty());

    for (const auto& item : items) {
        item->notes = QStringLiteral("Authoring change");
        item->extensions = {{QStringLiteral("external"), QJsonArray{1, 2, 3}}};
        item->color = 0x123456;
        item->canvasPos = QPointF(25.0, -10.0);
    }
    saved.refresh(project);
    for (const auto& item : items) EXPECT_TRUE(item->isDirtyObject());
    EXPECT_TRUE(saved.refresh(project).isEmpty());
    for (const auto& item : items) {
        item->notes.clear();
        item->extensions = {};
        item->color = 0;
        item->canvasPos = {};
    }
    saved.refresh(project);
    for (const auto& item : items) EXPECT_FALSE(item->isDirtyObject());

    const auto variable = items[7];
    auto* declaration = variable->comp<VNX::VVariable>();
    ASSERT_NE(declaration, nullptr);
    const float initialValue = declaration->defaultValue;
    declaration->defaultValue = 2.5f;
    saved.refresh(project);
    EXPECT_TRUE(variable->isDirtyObject());
    ASSERT_TRUE(markSaved(saved, project));
    ASSERT_TRUE(markSaved(saved, project));
    saved.refresh(project);
    EXPECT_FALSE(variable->isDirtyObject());
    declaration->defaultValue = initialValue;
    saved.refresh(project);
    EXPECT_TRUE(variable->isDirtyObject());
    declaration->defaultValue = 2.5f;
    saved.refresh(project);
    EXPECT_FALSE(variable->isDirtyObject());
    EXPECT_TRUE(saved.refresh(project).isEmpty());
}

TEST_P(ProjectSerializationSavedState, SavedContentFollowsPersistentIdentityAndHierarchy)
{
    ProjectData project = HarkenTest::projectWithPlatforms();
    const auto left = addSaveStateRoot(project, QStringLiteral("Left"), ProjectItem::Type::Layer, 1);
    const auto right = addSaveStateRoot(project, QStringLiteral("Right"), ProjectItem::Type::Layer, 2);
    auto sound = addChild(*left, "Tone", ProjectItem::Type::Sound);
    sound->objectID = 3;
    sound->parentRef.kind = VNX::ParentRef::Kind::Item;
    const auto sibling = addChild(*left, "Tail", ProjectItem::Type::Sound);
    sibling->objectID = 4;
    sibling->parentRef.kind = VNX::ParentRef::Kind::Item;
    const auto action = addSaveStateRoot(project, QStringLiteral("PlayTone"), ProjectItem::Type::Action, 5);
    VNX::VActionStep step;
    step.targetObjectID = sound->objectID;
    step.targetName = sound->name;
    action->comp<VNX::VAction>()->steps.push_back(step);
    auto& target = action->comp<VNX::VAction>()->steps.front();
    VNX::ProjectItemSaveState saved;
    ASSERT_TRUE(markSaved(saved, project));

    sound->name = QStringLiteral("RenamedTone");
    target.targetName = sound->name;
    saved.refresh(project);
    EXPECT_TRUE(sound->isDirtyObject());
    EXPECT_FALSE(left->isDirtyObject());
    EXPECT_FALSE(action->isDirtyObject());
    sound->name = QStringLiteral("Tone");
    target.targetName = sound->name;
    sound->objectID = 103;
    target.targetObjectID = sound->objectID;
    saved.refresh(project);
    EXPECT_FALSE(sound->isDirtyObject());
    EXPECT_FALSE(action->isDirtyObject());

    VNX::ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareReferenceWrite(project));
    const QJsonObject content = serializer.projectItemToJson(*sound, false);
    ASSERT_FALSE(content.isEmpty());
    auto restored = std::make_shared<ProjectItem>();
    ASSERT_TRUE(serializer.projectItemFromJson(content, *restored));
    restored->parent = left.get();
    restored->markDirty();
    left->children[0] = restored;
    sound = restored;
    saved.refresh(project);
    EXPECT_FALSE(sound->isDirtyObject());
    EXPECT_FALSE(left->isDirtyObject());
    EXPECT_FALSE(action->isDirtyObject());

    std::swap(left->children[0], left->children[1]);
    saved.refresh(project);
    EXPECT_TRUE(left->isDirtyObject());
    EXPECT_FALSE(sound->isDirtyObject());
    EXPECT_FALSE(sibling->isDirtyObject());
    std::swap(left->children[0], left->children[1]);
    saved.refresh(project);
    EXPECT_FALSE(left->isDirtyObject());

    left->children.erase(left->children.begin());
    right->children.push_back(sound);
    sound->parent = right.get();
    saved.refresh(project);
    EXPECT_TRUE(left->isDirtyObject());
    EXPECT_TRUE(right->isDirtyObject());
    EXPECT_TRUE(sound->isDirtyObject());
    EXPECT_FALSE(action->isDirtyObject());
    right->children.clear();
    left->children.insert(left->children.begin(), sound);
    sound->parent = left.get();
    saved.refresh(project);
    EXPECT_FALSE(left->isDirtyObject());
    EXPECT_FALSE(right->isDirtyObject());
    EXPECT_FALSE(sound->isDirtyObject());

    target.targetObjectID = sibling->objectID;
    target.targetName = sibling->name;
    saved.refresh(project);
    EXPECT_TRUE(action->isDirtyObject());
    target.targetObjectID = sound->objectID;
    target.targetName = sound->name;
    saved.refresh(project);
    EXPECT_FALSE(action->isDirtyObject());

    left->children.pop_back();
    saved.refresh(project);
    EXPECT_TRUE(left->isDirtyObject());
    left->children.push_back(sibling);
    saved.refresh(project);
    EXPECT_FALSE(left->isDirtyObject());
    EXPECT_FALSE(sibling->isDirtyObject());
}

TEST_P(ProjectSerializationSavedState, FailedContentCapturePreservesTheLastSuccessfulSavePoint)
{
    ProjectData project = HarkenTest::projectWithPlatforms();
    const auto layer = addSaveStateRoot(project, QStringLiteral("Layer"), ProjectItem::Type::Layer, 1);
    const auto sound = addChild(*layer, "Tone", ProjectItem::Type::Sound);
    sound->objectID = 2;
    sound->parentRef.kind = VNX::ParentRef::Kind::Item;
    VNX::ProjectItemSaveState saved;
    ASSERT_TRUE(markSaved(saved, project));
    sound->notes = QStringLiteral("Pending content");
    saved.refresh(project);
    EXPECT_TRUE(sound->isDirtyObject());

    const QUuid identity = sound->guid;
    sound->guid = layer->guid;
    layer->notes = QStringLiteral("Pending parent content");
    EXPECT_TRUE(saved.refresh(project).isEmpty());
    EXPECT_FALSE(layer->isDirtyObject());
    EXPECT_TRUE(sound->isDirtyObject());
    EXPECT_FALSE(markSaved(saved, project));
    sound->guid = identity;
    layer->notes.clear();
    sound->notes.clear();
    saved.refresh(project);
    EXPECT_FALSE(layer->isDirtyObject());
    EXPECT_FALSE(sound->isDirtyObject());

    sound->notes = QStringLiteral("Pending content");
    saved.refresh(project);
    EXPECT_TRUE(sound->isDirtyObject());
    const auto groups = sound->fieldGroups;
    sound->fieldGroups.spatialRating.shared.localHistory = VNX::SpatialRatingFieldGroup{};
    sound->fieldGroups.spatialRating.shared.localHistory->spatialPanning =
        std::numeric_limits<float>::quiet_NaN();
    sound->notes.clear();
    layer->notes = QStringLiteral("Pending parent content");
    EXPECT_FALSE(markSaved(saved, project));
    saved.refresh(project);
    EXPECT_TRUE(sound->isDirtyObject());
    EXPECT_TRUE(layer->isDirtyObject());
    sound->fieldGroups = groups;
    layer->notes.clear();
    saved.refresh(project);
    EXPECT_FALSE(sound->isDirtyObject());
    EXPECT_FALSE(layer->isDirtyObject());
    EXPECT_TRUE(saved.refresh(project).isEmpty());
}

TEST_P(ProjectSerializationSavedState, ScopedSavedContentMatchesFullComparisonAcrossAuthoringChanges)
{
    ProjectData project = HarkenTest::projectWithPlatforms();
    const auto layer = addSaveStateRoot(project, QStringLiteral("Layer"), ProjectItem::Type::Layer, 1);
    const auto sound = addChild(*layer, "Tone", ProjectItem::Type::Sound);
    sound->objectID = 2;
    sound->parentRef.kind = VNX::ParentRef::Kind::Item;
    const auto other = addSaveStateRoot(project, QStringLiteral("Other"), ProjectItem::Type::Sound, 3);
    const QList<ProjectItem*> items{layer.get(), sound.get(), other.get()};
    VNX::ProjectItemSaveState scoped;
    VNX::ProjectItemSaveState full;
    ASSERT_TRUE(markSaved(scoped, project));
    ASSERT_TRUE(full.markSaved(project));
    const QSet<QUuid> writes{sound->guid};
    const auto compare = [&](bool exact = true) {
        QList<bool> before;
        for (auto* item : items) before.append(item->dirtyBit);
        const auto scopedChanged = scoped.refresh(project, exact ? &writes : nullptr);
        QList<bool> actual;
        for (auto* item : items) actual.append(item->dirtyBit);
        for (qsizetype i = 0; i < items.size(); ++i) items[i]->dirtyBit = before[i];
        EXPECT_EQ(scopedChanged, full.refresh(project));
        for (qsizetype i = 0; i < items.size(); ++i) EXPECT_EQ(actual[i], items[i]->dirtyBit);
    };
    using Authoring = VNX::PlayablePropertyAuthoringV1;
    const auto locator = Authoring::definitionLocator(*sound);
    ASSERT_TRUE(locator);
    for (const auto* name : {"volume", "pitch", "loopCount"}) {
        const auto* field = Authoring::fieldByName(QLatin1String(name));
        ASSERT_NE(field, nullptr);
        for (const auto& scope : {VNX::PlatformScope{}, VNX::PlatformScope{HarkenTest::PlatformIds::Windows}}) {
            const auto original = scope
                ? Authoring::readPlatformOverlay(*sound, *locator, field->id, *scope)
                : Authoring::readProperty(*sound, *locator, field->id, scope);
            ASSERT_TRUE(original);
            ASSERT_TRUE(Authoring::setOverride(*sound, *locator, field->id, QJsonValue(2), scope));
            sound->markDirty();
            compare();
            EXPECT_TRUE(sound->dirtyBit);
            ASSERT_TRUE(Authoring::restoreProperty(*sound, *locator, field->id, *original, scope));
            sound->markDirty();
            compare();
            EXPECT_FALSE(sound->dirtyBit);
        }
    }
    other->notes = QStringLiteral("Pending write outside the current command");
    other->markDirty();
    compare();
    EXPECT_TRUE(other->dirtyBit);
    other->notes.clear();
    compare();
    EXPECT_FALSE(other->dirtyBit);
    other->notes = QStringLiteral("Temporary authoring preview");
    scoped.recordPreview(other->guid);
    compare();
    EXPECT_TRUE(other->dirtyBit);
    other->notes.clear();
    scoped.recordPreview(other->guid);
    compare();
    EXPECT_FALSE(other->dirtyBit);
    sound->notes = QStringLiteral("Previously dirty owner");
    sound->markDirty();
    compare();
    const auto* volume = Authoring::fieldByName(QStringLiteral("volume"));
    const auto original = Authoring::readProperty(*sound, *locator, volume->id, std::nullopt);
    ASSERT_TRUE(original);
    ASSERT_TRUE(Authoring::setOverride(*sound, *locator, volume->id, QJsonValue(-3), std::nullopt));
    compare();
    ASSERT_TRUE(Authoring::restoreProperty(*sound, *locator, volume->id, *original, std::nullopt));
    compare();
    EXPECT_TRUE(sound->dirtyBit);
    sound->notes.clear();
    compare();
    EXPECT_FALSE(sound->dirtyBit);
    other->name = QStringLiteral("Renamed");
    scoped.invalidate();
    compare();
    other->name = QStringLiteral("Other");
    compare(false);
    layer->canvasPos = QPointF(10, 20);
    compare(false);
    layer->canvasPos = {};
    compare();
    const auto otherGuid = other->guid;
    other->guid = sound->guid;
    layer->canvasPos = QPointF(20, 30);
    compare(false);
    other->guid = otherGuid;
    compare();
}

INSTANTIATE_TEST_SUITE_P(SerializationSources, ProjectSerializationSavedState,
    ::testing::Bool(), [](const ::testing::TestParamInfo<bool>& info) {
        return info.param ? "Captured" : "Independent";
    });

TEST(ProjectSerialization, ReusedSerializerObservesAuthoringChangesAndOwnsReturnedDocuments)
{
    ProjectItem item(QStringLiteral("Layer"), ProjectItem::Type::Layer);
    item.guid = QUuid::createUuid();
    item.attachDefaultComponents();
    VNX::ProjectSerializer reused;
    const auto compare = [&](bool valid) {
        VNX::ProjectSerializer fresh;
        const auto expected = fresh.projectItemToJson(item, false);
        auto actual = reused.projectItemToJson(item, false);
        EXPECT_EQ(actual.isEmpty(), !valid);
        EXPECT_EQ(actual, expected);
        EXPECT_EQ(QJsonDocument(actual).toJson(QJsonDocument::Compact),
                  QJsonDocument(expected).toJson(QJsonDocument::Compact));
        if (!valid) {
            EXPECT_EQ(reused.lastError(), fresh.lastError());
            EXPECT_FALSE(reused.lastError().isEmpty());
            return;
        }
        auto groups = actual.value(QStringLiteral("fieldGroups")).toObject();
        groups.insert(QStringLiteral("externalMutation"), true);
        actual.insert(QStringLiteral("fieldGroups"), groups);
        EXPECT_EQ(reused.projectItemToJson(item, false), expected);
        EXPECT_EQ(reused.audioBranchToJson(item).value(QStringLiteral("fieldGroups")),
                  expected.value(QStringLiteral("fieldGroups")));
    };
    compare(true);
    item.fieldGroups = VNX::MakeRootFieldGroups({1, 2});
    item.fieldGroups.send.shared.localHistory->sends = {{{3, 4}, -6.0f}, {{5, 6}, -12.0f}};
    compare(true);
    item.fieldGroups.mainOutput.shared.localHistory->outputBusVolume = -0.0f;
    compare(true);
    item.fieldGroups.mainOutput.shared.localHistory->outputBusVolume = 0.0f;
    compare(true);
    std::reverse(item.fieldGroups.send.shared.localHistory->sends.begin(),
                 item.fieldGroups.send.shared.localHistory->sends.end());
    compare(true);
    auto overlay = item.fieldGroups.mainOutput.shared;
    overlay.mode = VNX::FieldGroupMode::UseShared;
    overlay.localFields = VNX::FieldGroupFieldMask(
        harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
    overlay.localHistory->outputBusVolume = -3.0f;
    item.fieldGroups.mainOutput.platforms[{7, 8}] = overlay;
    compare(true);
    item.fieldGroups.mainOutput.platforms[{7, 8}].localFields =
        VNX::FieldGroupPropertyMask(VNX::FieldGroupId::MainOutput);
    compare(true);
    item.fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef = {9, 10};
    compare(true);
    item.fieldGroups.mainOutput.platforms[{7, 8}].mode = VNX::FieldGroupMode::Inherit;
    compare(true);
    item.fieldGroups.mainOutput.platforms.clear();
    compare(true);
    item.fieldGroups.mainOutput.shared.localHistory->outputBusVolume =
        std::numeric_limits<float>::quiet_NaN();
    compare(false);
    item.fieldGroups.mainOutput.shared.localHistory->outputBusVolume = 0.0f;
    compare(true);
    item.type = ProjectItem::Type::Action;
    compare(false);
    item.type = ProjectItem::Type::Layer;
    compare(true);
}
