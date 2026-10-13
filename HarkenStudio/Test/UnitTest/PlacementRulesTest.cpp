#include "ActionManager.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AudioImportGate.h"
#include "AssetPipeline/AudioFormatCatalog.h"
#include "AssetPipeline/ImportAssetsCommand.h"
#include "Core/ItemTypeCatalog.h"
#include "IPC/IPCServer.h"
#include "ProjectIndex.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "TemplateApplyService.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>

using namespace VNX;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void appendU16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
}

void appendU32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
    out.append(static_cast<char>((value >> 16) & 0xFFu));
    out.append(static_cast<char>((value >> 24) & 0xFFu));
}

void writeWavF32(const QString& path, const std::vector<float>& samples)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));

    constexpr uint32_t sampleRate = 48000;
    constexpr uint16_t channels = 1;
    const uint32_t frames = static_cast<uint32_t>(samples.size());
    const uint32_t dataBytes = frames * static_cast<uint32_t>(sizeof(float));
    const uint16_t blockAlign = channels * sizeof(float);
    const uint32_t fmtSize = 18;
    const uint32_t factSize = 4;
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);

    QByteArray bytes;
    bytes.append("RIFF", 4);
    appendU32(bytes, riffSize);
    bytes.append("WAVE", 4);
    bytes.append("fmt ", 4);
    appendU32(bytes, fmtSize);
    appendU16(bytes, 3);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * blockAlign);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 32);
    appendU16(bytes, 0);
    bytes.append("fact", 4);
    appendU32(bytes, factSize);
    appendU32(bytes, frames);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    bytes.append(reinterpret_cast<const char*>(samples.data()),
                 static_cast<qsizetype>(dataBytes));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

ProjectFileBinding makeMasterBusBinding(ProjectData& project)
{
    auto master = std::make_shared<ProjectItem>(
        QStringLiteral("Master Bus"), ProjectItem::Type::Bus);
    master->attachDefaultComponents();
    master->objectID = project.generateObjectID();
    master->guid = QUuid::createUuid();
    master->guid = QUuid::createUuid();
    ProjectFileBinding binding;
    binding.parentFolder = QStringLiteral("Busses");
    binding.rootItem = std::move(master);
    return binding;
}

// First negative placement coverage (who may NOT go where): D4-4 routed the
// creation funnels through validatePlacement, D4-1 narrowed audio-import
// parents to {Layer, Sound}, and D4-2 made a null import context land on
// the first Layers root.
class PlacementRulesTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ActionManager::instance().clearAllForTesting();
        registerAssetActions(ActionManager::instance(), nullptr,
                             &ProjectManager::instance());
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("PlacementRulesProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
        errorCount_ = 0;
        errorConn_ = QObject::connect(
            &ProjectManager::instance(), &ProjectManager::errorOccurred,
            [this](const QString&) { ++errorCount_; });
    }

    void TearDown() override
    {
        QObject::disconnect(errorConn_);
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir_;
    int errorCount_ = 0;
    QMetaObject::Connection errorConn_;
};

} // namespace

TEST_F(PlacementRulesTest, CreateSoundUnderBusIsRejectedWithError)
{
    ProjectItem* masterBus = ProjectManager::instance().getMasterBusItem();
    ASSERT_NE(masterBus, nullptr);
    const size_t childrenBefore = masterBus->children.size();

    ProjectItem* item = ProjectManager::instance().createItem(
        masterBus, QStringLiteral("Bad_Sound"), ProjectItem::Type::Sound);

    EXPECT_EQ(item, nullptr);
    EXPECT_EQ(errorCount_, 1);
    EXPECT_EQ(masterBus->children.size(), childrenBefore);
}

TEST_F(PlacementRulesTest, CreateTopLevelContentRootsLandInLayersBucket)
{
    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);

    const struct { const char* name; ProjectItem::Type type; } roots[] = {
        {"Root_Shot", ProjectItem::Type::Sound},
        {"Root_Grp", ProjectItem::Type::Container},
        {"Root_Trk", ProjectItem::Type::Track},
        {"Root_Pc", ProjectItem::Type::Piece},
    };
    for (const auto& entry : roots) {
        ProjectItem* item = ProjectManager::instance().createItem(
            nullptr, QLatin1String(entry.name), entry.type);
        ASSERT_NE(item, nullptr) << entry.name;
        EXPECT_EQ(item->parent, nullptr);
        EXPECT_EQ(item->parentRef.kind, ParentRef::Kind::Root);
        ProjectFileBinding* binding = findFileBindingForItem(*project, item);
        ASSERT_NE(binding, nullptr) << entry.name;
        EXPECT_EQ(binding->parentFolder, QStringLiteral("Layers"));
    }
    EXPECT_EQ(errorCount_, 0);
}

TEST_F(PlacementRulesTest, CreateTopLevelFolderInBussesIsRejected)
{
    auto* item = ProjectManager::instance().createFolder(nullptr, QStringLiteral("Orphan_Folder"), QStringLiteral("Busses"));

    EXPECT_EQ(item, nullptr);
    EXPECT_EQ(errorCount_, 1);
}

TEST_F(PlacementRulesTest, OnlyAudioParentsAcceptImportedAudio)
{
    ProjectItem layer(QStringLiteral("L"), ProjectItem::Type::Layer);
    ProjectItem sound(QStringLiteral("S"), ProjectItem::Type::Sound);
    ProjectItem bus(QStringLiteral("B"), ProjectItem::Type::Bus);

    EXPECT_TRUE(ProjectManager::canAcceptImportedAudioParent(&layer));
    EXPECT_TRUE(ProjectManager::canAcceptImportedAudioParent(&sound));
    EXPECT_FALSE(ProjectManager::canAcceptImportedAudioParent(&bus));
    EXPECT_FALSE(ProjectManager::canAcceptImportedAudioParent(nullptr));
}

TEST_F(PlacementRulesTest, NullContextImportLandsAtRootLevel)
{
    const ImportPlacement placement =
        AudioImportGate::resolveImportParent(nullptr);
    EXPECT_EQ(placement.kind, ImportPlacementKind::RootSound);
    EXPECT_EQ(placement.parent, nullptr);
}

TEST_F(PlacementRulesTest, NullContextImportNeverReusesExistingRoots)
{
    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_TRUE(project->layers.empty());

    ProjectItem* soundRoot = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Root_Impact"), ProjectItem::Type::Sound);
    ASSERT_NE(soundRoot, nullptr);
    ProjectItem* pieceRoot = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Root_Marker"), ProjectItem::Type::Piece);
    ASSERT_NE(pieceRoot, nullptr);
    ProjectItem* containerRoot = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Root_Grp"), ProjectItem::Type::Container);
    ASSERT_NE(containerRoot, nullptr);

    const ImportPlacement placement =
        AudioImportGate::resolveImportParent(nullptr);
    EXPECT_EQ(placement.kind, ImportPlacementKind::RootSound);
    EXPECT_EQ(placement.parent, nullptr);
}

TEST_F(PlacementRulesTest, LayerContainerTrackContextsAcceptImportBesideThemselves)
{
    ProjectItem* layer = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Layer_X"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* container = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Container_X"), ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    ProjectItem* track = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Track_X"), ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);

    for (ProjectItem* context : {layer, container, track}) {
        const ImportPlacement placement =
            AudioImportGate::resolveImportParent(context);
        EXPECT_EQ(placement.kind, ImportPlacementKind::Parent);
        EXPECT_EQ(placement.parent, context);
    }
}

TEST_F(PlacementRulesTest, BusAndPieceContextsAreRejected)
{
    ProjectItem* masterBus = ProjectManager::instance().getMasterBusItem();
    ASSERT_NE(masterBus, nullptr);
    ProjectItem* piece = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Piece_X"), ProjectItem::Type::Piece);
    ASSERT_NE(piece, nullptr);

    for (ProjectItem* context : {masterBus, piece}) {
        const ImportPlacement placement =
            AudioImportGate::resolveImportParent(context);
        EXPECT_EQ(placement.kind, ImportPlacementKind::Rejected);
        EXPECT_EQ(placement.parent, nullptr);
    }
}

TEST_F(PlacementRulesTest, RootLevelImportCreatesIndependentRootSoundsWithoutShellObjects)
{
    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_TRUE(project->layers.empty());

    const QString wavPathA = tempDir_.path() + QStringLiteral("/Root_Import_A.wav");
    const QString wavPathB = tempDir_.path() + QStringLiteral("/Root_Import_B.wav");
    writeWavF32(wavPathA, {0.0f, 0.5f, -0.5f, 0.0f});
    writeWavF32(wavPathB, {0.0f, 0.1f, -0.1f, 0.0f});

    auto* cmd = ImportAssetsCommand::pushImportAtRoot({wavPathA, wavPathB});

    EXPECT_EQ(cmd->successCount(), 2);
    EXPECT_EQ(cmd->failCount(), 0);
    ASSERT_EQ(project->layers.size(), 2u);
    for (const ProjectFileBinding& binding : project->layers) {
        ASSERT_NE(binding.rootItem, nullptr);
        EXPECT_EQ(binding.rootItem->type, ProjectItem::Type::Sound);
        EXPECT_EQ(binding.rootItem->parent, nullptr);
        EXPECT_EQ(binding.rootItem->parentRef.kind, ParentRef::Kind::Root);
    }
}

TEST_F(PlacementRulesTest, RootLevelImportDoesNotReuseExistingRootObjects)
{
    ProjectItem* existingLayer = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Existing_Layer"), ProjectItem::Type::Layer);
    ASSERT_NE(existingLayer, nullptr);
    ProjectItem* existingContainer = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Existing_Container"), ProjectItem::Type::Container);
    ASSERT_NE(existingContainer, nullptr);

    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    const size_t rootsBefore = project->layers.size();

    const QString wavPath = tempDir_.path() + QStringLiteral("/Root_Import_Solo.wav");
    writeWavF32(wavPath, {0.0f, 0.5f, -0.5f, 0.0f});

    auto* cmd = ImportAssetsCommand::pushImportAtRoot({wavPath});

    EXPECT_EQ(cmd->successCount(), 1);
    EXPECT_TRUE(existingLayer->children.empty());
    EXPECT_TRUE(existingContainer->children.empty());
    ASSERT_EQ(project->layers.size(), rootsBefore + 1);
    EXPECT_EQ(project->layers.back().rootItem->type, ProjectItem::Type::Sound);
}

TEST_F(PlacementRulesTest, RootLevelImportUndoRedoRestoresRootObjectAndAssetOwner)
{
    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_TRUE(project->layers.empty());

    const QString wavPath = tempDir_.path() + QStringLiteral("/Root_Import_UndoRedo.wav");
    writeWavF32(wavPath, {0.0f, 0.5f, -0.5f, 0.0f});

    auto* cmd = ImportAssetsCommand::pushImportAtRoot({wavPath});
    ASSERT_EQ(cmd->successCount(), 1);
    ASSERT_EQ(project->layers.size(), 1u);
    const QUuid rootGuid = project->layers.front().rootItem->guid;

    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(project->layers.empty());

    UndoRedoManager::instance().undoStack()->redo();
    ASSERT_EQ(project->layers.size(), 1u);
    ProjectItem* restoredRoot = project->layers.front().rootItem.get();
    ASSERT_NE(restoredRoot, nullptr);
    EXPECT_EQ(restoredRoot->guid, rootGuid);
    EXPECT_EQ(restoredRoot->type, ProjectItem::Type::Sound);

    ProjectFileBinding* bindingByGuid =
        findFileBindingByRootGuid(*project, restoredRoot->guid.toString(QUuid::WithoutBraces));
    ASSERT_EQ(bindingByGuid, &project->layers.front());

    QList<uint32_t> ids = cmd->importedIDs();
    ASSERT_EQ(ids.size(), 1);
    AssetInfo* info = AssetRegistry::instance().getAsset(ids.first());
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->ownerObjectGuid, restoredRoot->guid.toString(QUuid::WithoutBraces));
    ProjectFileBinding* bindingByOwner = findFileBindingByRootGuid(*project, info->ownerObjectGuid);
    EXPECT_EQ(bindingByOwner, &project->layers.front());
}

// Root-level import_audio used to look the created Sound up through
// AssetInfo.ownerObjectGuid. Dedup reuses that record, so the response
// named the original Sound instead of the new one.
TEST_F(PlacementRulesTest, RootLevelIpcImportDedupReportsNewlyCreatedSound)
{
    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_TRUE(project->layers.empty());

    const QString wavPathA = tempDir_.path() + QStringLiteral("/Root_Import_Dedup_A.wav");
    const QString wavPathB = tempDir_.path() + QStringLiteral("/Root_Import_Dedup_B.wav");
    const std::vector<float> samples{0.0f, 0.5f, -0.5f, 0.0f};
    writeWavF32(wavPathA, samples);
    writeWavF32(wavPathB, samples);

    auto* first = ImportAssetsCommand::pushImportAtRoot({wavPathA});
    ASSERT_EQ(first->successCount(), 1);
    ASSERT_EQ(first->duplicateCount(), 0);
    ASSERT_EQ(project->layers.size(), 1u);
    ProjectItem* originalRoot = project->layers.front().rootItem.get();
    ASSERT_NE(originalRoot, nullptr);
    const QString originalName = originalRoot->name;
    const uint32_t originalObjectID = originalRoot->objectID;
    QList<uint32_t> firstIds = first->importedIDs();
    ASSERT_EQ(firstIds.size(), 1);
    const uint32_t sharedSourceID = firstIds.first();

    IPCServer server(nullptr);
    server.setProjectManager(&ProjectManager::instance());
    const QJsonObject response = server.dispatchCommand(
        QStringLiteral("import_audio"),
        {{QStringLiteral("path"), wavPathB}});
    ASSERT_EQ(response.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));

    ASSERT_EQ(project->layers.size(), 2u);
    ProjectItem* createdRoot = project->layers.back().rootItem.get();
    ASSERT_NE(createdRoot, nullptr);
    EXPECT_NE(createdRoot, originalRoot);

    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    EXPECT_EQ(result.value(QStringLiteral("source_id")).toInteger(),
              static_cast<qint64>(sharedSourceID));
    EXPECT_EQ(result.value(QStringLiteral("target_parent_authoring_handle")).toInteger(),
              static_cast<qint64>(createdRoot->objectID));
    EXPECT_EQ(result.value(QStringLiteral("target_parent_name")).toString(),
              createdRoot->name);
    EXPECT_NE(result.value(QStringLiteral("target_parent_authoring_handle")).toInteger(),
              static_cast<qint64>(originalObjectID));
    EXPECT_NE(result.value(QStringLiteral("target_parent_name")).toString(),
              originalName);
}

TEST_F(PlacementRulesTest, DeleteRootLevelBindingUndoRestoresIndexAndFilePath)
{
    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    ASSERT_TRUE(project->layers.empty());

    ProjectItem* first = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Root_First"), ProjectItem::Type::Sound);
    ProjectItem* middle = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Root_Middle"), ProjectItem::Type::Sound);
    ProjectItem* last = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Root_Last"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(middle, nullptr);
    ASSERT_NE(last, nullptr);
    ASSERT_EQ(project->layers.size(), 3u);

    project->setObjectFileLocation(first, nullptr, QStringLiteral("Root_First.vp"));
    project->setObjectFileLocation(middle, nullptr, QStringLiteral("Root_Middle.vp"));
    project->setObjectFileLocation(last, nullptr, QStringLiteral("Root_Last.vp"));

    ASSERT_TRUE(ProjectManager::instance().deleteItem(middle));
    ASSERT_EQ(project->layers.size(), 2u);

    UndoRedoManager::instance().undoStack()->undo();

    ASSERT_EQ(project->layers.size(), 3u);
    EXPECT_EQ(project->layers[0].rootItem.get(), first);
    EXPECT_EQ(project->layers[1].rootItem.get(), middle);
    EXPECT_EQ(project->layers[2].rootItem.get(), last);
    EXPECT_EQ(project->objectFilePath(middle), QStringLiteral("Root_Middle.vp"));
}

TEST(CreateItemWhitelistTest, DerivesFromCreationSpecsMinusBus)
{
    QSet<ProjectItem::Type> types;
    for (const ItemTypeCatalog::CreationSpec& spec : ItemTypeCatalog::creationSpecs())
        types.insert(spec.type);
    types.remove(ProjectItem::Type::Bus);

    const QSet<ProjectItem::Type> expected = {
        ProjectItem::Type::Sound,
        ProjectItem::Type::Container,
        ProjectItem::Type::Track,
        ProjectItem::Type::Piece,
        ProjectItem::Type::Layer,
        ProjectItem::Type::Variable,
        ProjectItem::Type::Attenuation,
        ProjectItem::Type::Action,
    };
    EXPECT_EQ(types, expected);
}

TEST(AudioFormatCatalogTest, ExposesExactlyTheSixSupportedFormats)
{
    const QStringList expected{
        QStringLiteral("wav"), QStringLiteral("mp3"),
        QStringLiteral("flac"), QStringLiteral("ogg"),
        QStringLiteral("aiff"), QStringLiteral("aif")};
    EXPECT_EQ(AudioFormatCatalog::extensions(), expected);
    EXPECT_EQ(AudioImportGate::acceptedExtensions(), expected);
    EXPECT_FALSE(AudioFormatCatalog::containsExtension(QStringLiteral("wma")));
    EXPECT_FALSE(AudioFormatCatalog::containsExtension(QStringLiteral("m4a")));
    EXPECT_TRUE(AudioFormatCatalog::containsExtension(QStringLiteral("AIFF")));
}

// The IPC/AI import path used to resolve its parent through a second,
// fork-copied resolver that neither walked up nor excluded Sound, so a Sound
// target silently produced zero imports. The command now reuses the
// AudioImportGate semantics: a Sound context imports beside it, under its
// parent layer.
TEST_F(PlacementRulesTest, ImportWithSoundTargetLandsBesideIt)
{
    ProjectItem* layer = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Layer_Import"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* sound = ProjectManager::instance().createItem(
        layer, QStringLiteral("Existing_Sound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);

    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    ProjectFileBinding* binding = findFileBindingForItem(*project, layer);
    ASSERT_NE(binding, nullptr);

    const QString wavPath = tempDir_.path() + QStringLiteral("/Imported_Clip.wav");
    writeWavF32(wavPath, {0.0f, 0.25f, -0.25f, 0.0f});

    auto* cmd = ImportAssetsCommand::pushImport({wavPath}, binding, sound);

    EXPECT_EQ(cmd->successCount(), 1);
    EXPECT_EQ(cmd->failCount(), 0);
    EXPECT_TRUE(sound->children.empty());
    ASSERT_EQ(layer->children.size(), 2u);
    EXPECT_EQ(layer->children.back()->type, ProjectItem::Type::Sound);
    EXPECT_EQ(layer->children.back()->name, QStringLiteral("Imported_Clip"));
}

// The wasTopLevelBinding_ branch of MoveItemCommand::redo used to mutate the
// tree with zero validation while the regular branch went through
// validateMoveConstraints. An invalid move must now leave the tree and the
// undo stack untouched (the command marks itself obsolete) and report the
// rejection through errorOccurred.
TEST_F(PlacementRulesTest, TopLevelRootMoveValidatesPlacement)
{
    ProjectItem* att = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Att_A"), ProjectItem::Type::Attenuation);
    ASSERT_NE(att, nullptr);
    ProjectItem* layer = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Layer_A"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);

    const int undoEntriesBefore = UndoRedoManager::instance().undoStack()->count();
    const int errorsBefore = errorCount_;

    UndoRedoManager::instance().push(new MoveItemCommand(att, layer));

    EXPECT_EQ(att->parent, nullptr);
    EXPECT_TRUE(layer->children.empty());
    EXPECT_EQ(errorCount_, errorsBefore + 1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoEntriesBefore);
}

// Bus packs write a string "type" ("Bus") that the scan parses through the
// catalog; the Busses folder must land in the index.
TEST_F(PlacementRulesTest, ProjectScanIndexesBusPacks)
{
    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    const QString projectRoot = QFileInfo(project->projectPath).absolutePath();

    ProjectIndex index;
    ASSERT_EQ(index.scanProjectRoot(projectRoot), ProjectIndex::ScanResult::Ok);

    const ProjectIndex::Entry* master =
        index.findByName(ProjectItem::Type::Bus, QStringLiteral("Master Bus"));
    ASSERT_NE(master, nullptr);
    EXPECT_EQ(master->type, ProjectItem::Type::Bus);
    EXPECT_FALSE(index.findByGuid(master->guid) == nullptr);
}

TEST(TemplateSeedValidationTest, ValidTemplateSeedsBusTree)
{
    ensureApp();
    ProjectData project;
    project.busses.push_back(makeMasterBusBinding(project));
    ProjectItem* master = project.busses[0].rootItem.get();

    const QJsonObject tpl{
        {"displayName", "Test"},
        {"description", ""},
        {"bus", QJsonObject{{"name", "Master Bus"}, {"children", QJsonArray{
            QJsonObject{{"name", "Music"},
                        {"children", QJsonArray{QJsonObject{{"name", "Ambience"}}}}}}}}}};

    QString error;
    ASSERT_TRUE(TemplateApplyService::seedTemplateInto(project, tpl, &error))
        << error.toStdString();
    ASSERT_EQ(master->children.size(), 1u);
    EXPECT_EQ(master->children[0]->name, QStringLiteral("Music"));
    ASSERT_EQ(master->children[0]->children.size(), 1u);
    EXPECT_EQ(master->children[0]->children[0]->name, QStringLiteral("Ambience"));
}

TEST(TemplateSeedValidationTest, DuplicateBusNamesArePreservedWithDistinctIdentities)
{
    ensureApp();
    ProjectData project;
    project.busses.push_back(makeMasterBusBinding(project));
    ProjectItem* master = project.busses[0].rootItem.get();

    const QJsonObject tpl{
        {"displayName", "Test"},
        {"description", ""},
        {"bus", QJsonObject{{"name", "Master Bus"}, {"children", QJsonArray{
            QJsonObject{{"name", "Music"}},
            QJsonObject{{"name", "Music"}}}}}}};

    QString error;
    ASSERT_TRUE(TemplateApplyService::seedTemplateInto(project, tpl, &error)) << error.toStdString();
    ASSERT_EQ(master->children.size(), 2u);
    EXPECT_EQ(master->children[0]->name, QStringLiteral("Music"));
    EXPECT_EQ(master->children[1]->name, QStringLiteral("Music"));
    EXPECT_NE(master->children[0]->guid, master->children[1]->guid);
}

TEST(TemplateSeedValidationTest, BusFanoutLimitRollsBackSeeding)
{
    ensureApp();
    ProjectData project;
    project.busses.push_back(makeMasterBusBinding(project));
    ProjectItem* master = project.busses[0].rootItem.get();

    QJsonArray children;
    for (int i = 0; i < 17; ++i)
        children.append(QJsonObject{{"name", QStringLiteral("Bus_%1").arg(i)}});
    const QJsonObject tpl{{"displayName", "Test"},
                          {"description", ""},
                          {"bus", QJsonObject{{"name", "Master Bus"},
                                               {"children", children}}}};

    QString error;
    EXPECT_FALSE(TemplateApplyService::seedTemplateInto(project, tpl, &error));
    EXPECT_TRUE(master->children.empty());
    EXPECT_FALSE(error.isEmpty());
}

// create_attenuation (AI path) used to accept any non-empty name; it now runs
// the same NameValidator + namespace-uniqueness gauntlet as createItem.
class SpatialActionValidationTest : public PlacementRulesTest
{
protected:
    void SetUp() override
    {
        PlacementRulesTest::SetUp();
        ActionManager::instance().clearAllForTesting();
        registerSpatialActions(ActionManager::instance(), nullptr,
                               &ProjectManager::instance());
    }
};

TEST_F(SpatialActionValidationTest, CreateAttenuationRejectsIllegalName)
{
    const QString illegal = QStringLiteral("Invalid/Name");
    const ActionResult result = ActionManager::instance().execute(
        QStringLiteral("create_attenuation"), QJsonObject{{"name", illegal}});
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(ProjectManager::instance().currentProject()->attenuationItems.empty());
}

TEST_F(SpatialActionValidationTest, AttenuationActionsReturnResolvedNames)
{
    const ActionResult first = ActionManager::instance().execute(
        QStringLiteral("create_attenuation"), QJsonObject{{"name", "Att_Dup"}});
    ASSERT_TRUE(first.success);

    const ActionResult second = ActionManager::instance().execute(
        QStringLiteral("create_attenuation"), QJsonObject{{"name", "Att_Dup"}});
    ASSERT_TRUE(second.success);
    EXPECT_EQ(second.data.value("name").toString(), QStringLiteral("Att_Dup_01"));
    const ActionResult renamed = ActionManager::instance().execute(
        QStringLiteral("rename_attenuation"),
        QJsonObject{{"old_name", "Att_Dup_01"}, {"new_name", "att_dup"}});
    ASSERT_TRUE(renamed.success);
    EXPECT_EQ(renamed.data.value("new_name").toString(), QStringLiteral("att_dup"));
}
