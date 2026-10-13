#include "IPC/IPCServer.h"
#include "ActionManager.h"
#include "ComponentCatalog.h"
#include "ItemTypeCatalog.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "BindingSemantics.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "Components/VAudio.h"
#include "Components/VSound.h"
#include "AssetPipeline/AssetRegistry.h"
#include "ContainerBehaviorSemantics.h"
#include "PlayablePropertyAuthoringV1.h"
#include "SoundSourceSemantics.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "harken_playable_properties.g.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>

namespace VNX {
uint32_t itemShortID(const ProjectItem* item);
}

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

} // namespace

TEST(IpcSchemaCoverage, NineTypesReturnSchemaWithCatalogWordFaces)
{
    ensureApp();
    InspectorPanel panel;

    const struct { ProjectItem::Type type; ComponentType component; bool hasRows; } rows[] = {
        { ProjectItem::Type::Sound,       ComponentType::Audio,       true },
        { ProjectItem::Type::Container,   ComponentType::Audio,       true },
        { ProjectItem::Type::Track,       ComponentType::Audio,       true },
        { ProjectItem::Type::Piece,       ComponentType::Audio,       true },
        { ProjectItem::Type::Layer,       ComponentType::MixNode,     true },
        { ProjectItem::Type::Bus,         ComponentType::Bus,         true },
        { ProjectItem::Type::Action,      ComponentType::Action,      true },
        { ProjectItem::Type::Variable,    ComponentType::Variable,    true },
        { ProjectItem::Type::Attenuation, ComponentType::Attenuation, true },
    };

    for (const auto& row : rows)
    {
        const QString schemaId = ItemTypeCatalog::ipcName(row.type);
        const QJsonObject schema = panel.getInspectorSchema(schemaId);
        ASSERT_FALSE(schema.isEmpty()) << schemaId.toStdString();

        const QJsonArray itemTypes = schema.value(QStringLiteral("itemTypes")).toArray();
        ASSERT_EQ(itemTypes.size(), 1) << schemaId.toStdString();
        EXPECT_EQ(itemTypes.first().toString(), schemaId);

        EXPECT_EQ(schema.value(QStringLiteral("component")).toString(),
                  ComponentCatalog::componentName(row.component))
            << schemaId.toStdString();

        EXPECT_EQ(!schema.value(QStringLiteral("sections")).toArray().isEmpty(), row.hasRows)
            << schemaId.toStdString();

        QSet<QString> fields;
        for (const auto& entry : schema.value(QStringLiteral("sections")).toArray()) {
            const auto section = entry.toObject();
            EXPECT_FALSE(section.value(QStringLiteral("category")).toString().isEmpty())
                << schemaId.toStdString();
            for (const auto& child : section.value(QStringLiteral("children")).toArray()) {
                const auto key = child.toObject().value(QStringLiteral("field")).toString();
                EXPECT_FALSE(key.isEmpty()) << schemaId.toStdString();
                EXPECT_FALSE(fields.contains(key)) << schemaId.toStdString();
                fields.insert(key);
            }
        }
    }

    EXPECT_TRUE(panel.getInspectorSchema(QStringLiteral("Folder")).isEmpty());
    EXPECT_TRUE(panel.getInspectorSchema(QStringLiteral("sound")).isEmpty());
    EXPECT_TRUE(panel.getInspectorSchema(QStringLiteral("Nope")).isEmpty());
}

TEST(IpcSchemaCoverage, SoundSfxAliasIsRetired)
{
    EXPECT_EQ(ItemTypeCatalog::parseTypeName(QStringLiteral("SoundSFX"),
                                             Qt::CaseInsensitive, true),
              std::nullopt);
    EXPECT_EQ(ItemTypeCatalog::parseTypeName(QStringLiteral("SoundSFX"),
                                             Qt::CaseSensitive, true),
              std::nullopt);
}

namespace {

class IpcSetItemPropertyDispatchTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(),
                               &ProjectManager::instance(), nullptr, {});
        registerBindingActions(ActionManager::instance());

        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("SchemaCoverageProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
        UndoRedoManager::instance().clear();

        auto& pm = ProjectManager::instance();
        server_.setProjectManager(&pm);
        layer_ = pm.createItem(nullptr, QStringLiteral("Cov_Layer"), ProjectItem::Type::Layer);
        ASSERT_NE(layer_, nullptr);
        sound_ = pm.createItem(layer_, QStringLiteral("Cov_Sound"), ProjectItem::Type::Sound);
        ASSERT_NE(sound_, nullptr);
        container_ = pm.createItem(layer_, QStringLiteral("Cov_Container"), ProjectItem::Type::Container);
        variable_ = pm.createItem(nullptr, QStringLiteral("Cov_Variable"), ProjectItem::Type::Variable);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(variable_, nullptr);
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QJsonObject dispatchSet(const ProjectItem* item, const QString& property,
                            const QJsonValue& value)
    {
        return server_.dispatchCommand(QStringLiteral("set_item_property"), QJsonObject{
            {QStringLiteral("object_id"), static_cast<qint64>(itemShortID(item))},
            {QStringLiteral("property"), property},
            {QStringLiteral("value"), value}
        });
    }

    QJsonObject dispatchGet(const ProjectItem* item)
    {
        return server_.dispatchCommand(QStringLiteral("get_item_info"), QJsonObject{
            {QStringLiteral("object_id"), static_cast<qint64>(itemShortID(item))}
        });
    }

    QJsonObject dispatchBinding(const ProjectItem* item, const QJsonValue& binding)
    {
        return server_.dispatchCommand(QStringLiteral("set_binding"), QJsonObject{
            {QStringLiteral("object_id"), static_cast<qint64>(itemShortID(item))},
            {QStringLiteral("binding"), binding}
        });
    }

    IPCServer server_{nullptr};
    QTemporaryDir tempDir_;
    ProjectItem* layer_ = nullptr;
    ProjectItem* sound_ = nullptr;
    ProjectItem* container_ = nullptr;
    ProjectItem* variable_ = nullptr;
};

} // namespace

TEST_F(IpcSetItemPropertyDispatchTest, DispatchChainReachesActionManagerRoutes)
{
    QJsonObject r = dispatchSet(container_, QStringLiteral("notes"),
                                QStringLiteral("dispatched notes"));
    EXPECT_EQ(r.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(container_->notes, QStringLiteral("dispatched notes"));

    auto* var = variable_->comp<VVariable>();
    ASSERT_NE(var, nullptr);

    r = dispatchSet(variable_, QStringLiteral("defaultValue"), 42.5);
    EXPECT_EQ(r.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_FLOAT_EQ(var->defaultValue, 42.5f);

    r = dispatchSet(variable_, QStringLiteral("kind"),
                    static_cast<int>(VariableKind::Step));
    EXPECT_EQ(r.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(var->kind, VariableKind::Step);

    auto* sel = container_->comp<VNX::VSelection>();
    ASSERT_NE(sel, nullptr);

    r = dispatchSet(container_, QStringLiteral("mode"),
                    static_cast<int>(SelectionMode::Sequence));
    EXPECT_EQ(r.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(sel->mode, SelectionMode::Sequence);
}

TEST_F(IpcSetItemPropertyDispatchTest, ColorWritesAreRejectedWithoutMutation)
{
    const auto* action = ActionManager::instance().findAction(QStringLiteral("set_item_property"));
    ASSERT_NE(action, nullptr);
    const auto properties = action->paramSchema.value(QStringLiteral("properties")).toObject()
        .value(QStringLiteral("property")).toObject().value(QStringLiteral("enum")).toArray();
    EXPECT_FALSE(properties.contains(QStringLiteral("color")));

    const int undoCount = UndoRedoManager::instance().undoStack()->count();
    const int undoIndex = UndoRedoManager::instance().undoStack()->index();
    for (ProjectItem* item : {layer_, ProjectManager::instance().getMasterBusItem()}) {
        ASSERT_NE(item, nullptr);
        item->color = 0xFF112233u;
        item->dirtyBit = false;
        const auto rejected = dispatchSet(item, QStringLiteral("color"),
                                          static_cast<qint64>(0xFF336699u));
        EXPECT_EQ(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        EXPECT_EQ(rejected.value(QStringLiteral("code")).toString(),
                  QStringLiteral("COLOR_EDITING_UNAVAILABLE"));
        EXPECT_EQ(item->color, 0xFF112233u);
        EXPECT_FALSE(item->dirtyBit);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->index(), undoIndex);
    }
}

TEST_F(IpcSetItemPropertyDispatchTest, PhaseFiveCanonicalPropertiesAreAtomicAndUndoable)
{
    ProjectItem* child = ProjectManager::instance().createItem(
        container_, QStringLiteral("Cov_Child"), ProjectItem::Type::Sound);
    ASSERT_NE(child, nullptr);
    UndoRedoManager::instance().clear();

    const QJsonObject behavior{
        {QStringLiteral("discipline"), QStringLiteral("shuffle")},
        {QStringLiteral("avoidRepeat"), 1}
    };
    QJsonObject result = dispatchSet(container_, QStringLiteral("behavior"), behavior);
    ASSERT_EQ(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(container_->comp<VNX::VSelection>()->behavior.discipline, RandomDiscipline::Shuffle);
    const auto childLocator = PlayablePropertyAuthoringV1::containerChildLocator(
        *container_, child->guid);
    ASSERT_TRUE(childLocator.has_value());
    auto selections = container_->comp<VNX::VSelection>()->selections;
    selections.shared.find(childLocator->stableOccurrenceOrdinal)->weight = 75;
    result = dispatchSet(container_, QStringLiteral("selections"), selections.toJson());
    ASSERT_EQ(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(container_->comp<VNX::VSelection>()->selections.toJson(), selections.toJson());
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(container_->comp<VNX::VSelection>()->selections.shared.find(childLocator->stableOccurrenceOrdinal)->weight, 50);
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(container_->comp<VNX::VSelection>()->selections.toJson(), selections.toJson());

    result = dispatchSet(sound_, QStringLiteral("loopCount"), 4);
    ASSERT_EQ(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(sound_->comp<VAudio>()->loopCount, 4u);

    result = dispatchSet(sound_, QStringLiteral("scheduleOffsetSec"), 0.75);
    ASSERT_EQ(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_DOUBLE_EQ(sound_->comp<VSound>()->scheduleOffsetSec, 0.75);

    AssetInfo asset;
    asset.sourceID = 7001;
    asset.relativePath = QStringLiteral("source.wav");
    asset.absolutePath = tempDir_.path() + QStringLiteral("/source.wav");
    asset.metadata.durationSec = 3.0;
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));
    SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = asset.sourceID;
    SoundSources sources;
    sources.plain.entries = {source};
    const QJsonObject ledger = SoundSourceSemantics::toJson(sources);
    result = dispatchSet(sound_, QStringLiteral("sources"), ledger);
    ASSERT_EQ(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    ASSERT_EQ(sound_->comp<VSound>()->sources.plain.entries.size(), 1u);
    EXPECT_EQ(sound_->comp<VSound>()->sources.plain.entries.front().sourceId, asset.sourceID);
    EXPECT_TRUE(sound_->comp<VSound>()->sources.localized.isEmpty());
    EXPECT_EQ(result.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("value")).toObject(), ledger);

    const int undoCount = UndoRedoManager::instance().undoStack()->count();
    QJsonObject invalid = ledger;
    invalid[QStringLiteral("activeSourceId")] = source.entryId.toString(QUuid::WithoutBraces);
    result = dispatchSet(sound_, QStringLiteral("sources"), invalid);
    EXPECT_EQ(result.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(sound_->comp<VSound>()->sources.plain.entries.front().sourceId, asset.sourceID);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);

    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(sound_->comp<VSound>()->sources.plain.entries.empty());
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(sound_->comp<VSound>()->sources.plain.entries.front().sourceId, asset.sourceID);
}

TEST_F(IpcSetItemPropertyDispatchTest, BindingCommandsSerializeRelationsAndSupportMultipleBindings)
{
    const ParameterBindingDef binding = BindingSemantics::defaultBinding(
        BindingSemantics::pitchAddress(), variable_->guid);
    ASSERT_EQ(dispatchBinding(layer_, binding.toJson())
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonArray canonical = layer_->comp<VMixNode>()->bindings.toJson();
    const int undoCount = UndoRedoManager::instance().undoStack()->count();

    const QJsonObject layerInfo = dispatchGet(layer_);
    ASSERT_EQ(layerInfo.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    const QJsonArray serializedBindings = layerInfo.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("components")).toObject()
        .value(QStringLiteral("mixnode")).toObject()
        .value(QStringLiteral("bindings")).toArray();
    ASSERT_EQ(serializedBindings.size(), 1);
    EXPECT_FALSE(serializedBindings.at(0).toObject()
                     .value(QStringLiteral("runtimeStableId")).isNull());

    const QJsonObject duplicated = dispatchBinding(layer_, binding.toJson());
    EXPECT_EQ(duplicated.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    const auto bindings = layer_->comp<VMixNode>()->bindings.findAll(
        BindingSemantics::pitchAddress());
    ASSERT_EQ(bindings.size(), 2u);
    EXPECT_NE(bindings[0]->runtimeId(), bindings[1]->runtimeId());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount + 1);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(layer_->comp<VMixNode>()->bindings.toJson(), canonical);

    const int undoIndex = UndoRedoManager::instance().undoStack()->index();
    const QJsonObject malformed = dispatchBinding(layer_, 7);
    EXPECT_EQ(malformed.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(layer_->comp<VMixNode>()->bindings.toJson(), canonical);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->index(), undoIndex);

    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(layer_->comp<VMixNode>()->bindings.isEmpty());
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(layer_->comp<VMixNode>()->bindings.toJson(), canonical);
}

TEST_F(IpcSetItemPropertyDispatchTest, BindingRelationsCannotBeWrittenAsAnObjectProperty)
{
    const ParameterBindingDef binding = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), variable_->guid);
    ASSERT_EQ(dispatchBinding(layer_, binding.toJson())
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonArray canonical = layer_->comp<VMixNode>()->bindings.toJson();
    const auto presence = layer_->playablePropertyPresence.bits;
    const int undoCount = UndoRedoManager::instance().undoStack()->count();
    const int undoIndex = UndoRedoManager::instance().undoStack()->index();
    layer_->dirtyBit = false;
    for (const QJsonArray& value : {QJsonArray{}, QJsonArray{binding.toJson()}, QJsonArray{7}}) {
        const QJsonObject rejected = dispatchSet(layer_, QStringLiteral("bindings"), value);
        EXPECT_EQ(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        EXPECT_EQ(rejected.value(QStringLiteral("code")).toString(),
                  QStringLiteral("UNKNOWN_PROPERTY"));
        EXPECT_EQ(layer_->comp<VMixNode>()->bindings.toJson(), canonical);
        EXPECT_EQ(layer_->playablePropertyPresence.bits, presence);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->index(), undoIndex);
        EXPECT_FALSE(layer_->dirtyBit);
    }
}

TEST_F(IpcSetItemPropertyDispatchTest, BindingCommandRejectsGatedNoneAddress)
{
    ParameterBindingDef gated = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), variable_->guid);
    gated.address.slotId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain);
    const QJsonObject rejected = dispatchBinding(layer_, gated.toJson());
    EXPECT_EQ(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_TRUE(layer_->comp<VMixNode>()->bindings.isEmpty());
}
