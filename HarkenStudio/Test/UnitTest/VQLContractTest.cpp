#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectTypes.h"
#include "Query/LMDBIndexer.h"
#include "Query/MemoryDataSource.h"
#include "Query/VQLEngine.h"
#include "Query/VQLEvaluator.h"

#include <gtest/gtest.h>

namespace {

using VNX::ProjectItem;

std::shared_ptr<ProjectItem> makeItem(ProjectItem::Type type, uint32_t objectId)
{
    auto item = std::make_shared<ProjectItem>(
        VNX::ItemTypeCatalog::canonicalName(type), type);
    item->objectID = objectId;
    item->attachDefaultComponents();
    return item;
}

VNX::ProjectData projectWithAllTypes()
{
    VNX::ProjectData project;
    uint32_t objectId = 1;
    for (ProjectItem::Type type : VNX::ItemTypeCatalog::allTypes()) {
        VNX::ProjectFileBinding binding;
        binding.rootItem = makeItem(type, objectId++);
        project.layers.push_back(std::move(binding));
    }
    return project;
}

bool boolProperty(VNX::MemoryDataSource& source, const ProjectItem& item,
                  const QString& field)
{
    const VNX::VQLValue value = source.getProperty(&item, field);
    EXPECT_TRUE(std::holds_alternative<bool>(value));
    return std::holds_alternative<bool>(value) && std::get<bool>(value);
}

}

TEST(VQLContract, CapabilityFieldsUseItemTypeCatalogInMemoryAndProjection)
{
    VNX::MemoryDataSource source;
    uint32_t objectId = 1;

    for (ProjectItem::Type type : VNX::ItemTypeCatalog::allTypes()) {
        const auto item = makeItem(type, objectId++);
        struct Capability {
            const char* field;
            bool expected;
        };
        const Capability capabilities[] = {
            {"isContentObject", VNX::ItemTypeCatalog::isContentObject(type)},
            {"hasMixingCapability", VNX::ItemTypeCatalog::hasMixingCapability(type)},
            {"hasSpatialCapability", VNX::ItemTypeCatalog::hasSpatialCapability(type)},
            {"isTriggerable", VNX::ItemTypeCatalog::isTriggerable(type)},
        };

        for (const Capability& capability : capabilities) {
            const QString field = QLatin1String(capability.field);
            EXPECT_EQ(boolProperty(source, *item, field), capability.expected)
                << capability.field << " type=" << static_cast<int>(type);

            const QJsonObject json = VNX::VQLEvaluator::itemToJson(item.get(), {field});
            ASSERT_EQ(json.size(), 1);
            EXPECT_EQ(json.constBegin().value().toBool(), capability.expected)
                << capability.field << " type=" << static_cast<int>(type);
        }
    }
}

TEST(VQLContract, RetiredAndUnknownFieldsFailCompilationAndReturnProjection)
{
    VNX::VQLEngine engine;
    for (const QString& field : {QStringLiteral("iscontainer"),
                                 QStringLiteral("issoundobject"),
                                 QStringLiteral("ismusicobject"),
                                 QStringLiteral("notAField")}) {
        VNX::VQLError error;
        EXPECT_FALSE(engine.compile(
            QStringLiteral("$ where %1 = true").arg(field), &error));
        EXPECT_TRUE(error.isError());
        EXPECT_TRUE(error.message.contains(QStringLiteral("Unknown VQL field")));
        EXPECT_TRUE(error.message.contains(field));
    }

    VNX::ProjectData project = projectWithAllTypes();
    engine.setProject(&project);
    for (const QString& field : {QStringLiteral("iscontainer"),
                                 QStringLiteral("issoundobject"),
                                 QStringLiteral("ismusicobject")}) {
        VNX::VQLError error;
        EXPECT_TRUE(engine.queryJson(QStringLiteral("$"), {field}, &error).isEmpty());
        EXPECT_TRUE(error.message.contains(QStringLiteral("Unknown VQL field")));
        EXPECT_TRUE(error.message.contains(field));
    }
}

TEST(VQLContract, TypeFiltersAcceptOnlyBusinessObjectTypes)
{
    VNX::ProjectData project = projectWithAllTypes();
    VNX::VQLEngine engine;
    engine.setProject(&project);

    for (ProjectItem::Type type : VNX::ItemTypeCatalog::allTypes()) {
        const QString canonical = VNX::ItemTypeCatalog::canonicalName(type);
        VNX::VQLError error;
        const VNX::VQLResultSet result = engine.query(
            QStringLiteral("$ type:%1").arg(canonical), &error);
        EXPECT_FALSE(error.isError()) << canonical.toStdString();
        ASSERT_EQ(result.count(), 1u) << canonical.toStdString();
        EXPECT_EQ(result.items().front()->type, type);

        const QString searchText = VNX::LMDBIndexer::buildSearchText(result.items().front());
        EXPECT_TRUE(searchText.split(QLatin1Char(' ')).contains(canonical));
    }

    for (const QString& retired : {QStringLiteral("AudioLayer"),
                                   QStringLiteral("SoundSFX"),
                                   QStringLiteral("AudioBus"),
                                   QStringLiteral("Unknown")}) {
        VNX::VQLError error;
        EXPECT_FALSE(engine.compile(QStringLiteral("$ type:%1").arg(retired), &error));
        EXPECT_TRUE(error.message.contains(QStringLiteral("Unknown VQL type")));
        EXPECT_TRUE(error.message.contains(retired));
    }
}

TEST(VQLContract, PathsFollowFolderMembershipWithoutReplacingSoundSourcePaths)
{
    VNX::ProjectData project;
    auto* layerFolder = project.ensureFolderPath(
        QStringLiteral("Layers"), QStringLiteral("Layout/Nested"));
    auto* actionFolder = project.ensureFolderPath(
        QStringLiteral("Actions"), QStringLiteral("Layout/Nested"));
    ASSERT_NE(layerFolder, nullptr);
    ASSERT_NE(actionFolder, nullptr);
    auto layer = makeItem(ProjectItem::Type::Layer, 101);
    auto sound = makeItem(ProjectItem::Type::Sound, 102);
    auto child = makeItem(ProjectItem::Type::Container, 103);
    auto action = makeItem(ProjectItem::Type::Action, 104);
    for (const auto& item : {layer, sound, child, action}) item->guid = QUuid::createUuid();
    child->parent = layer.get();
    layer->children.push_back(child);
    sound->sourcePath = QStringLiteral("Recordings/Shot.wav");
    project.layers.push_back({layer, QStringLiteral("Layers")});
    project.layers.push_back({sound, QStringLiteral("Layers")});
    project.actions.push_back({action, QStringLiteral("Actions")});
    project.setObjectFileLocation(layer.get(), layerFolder, {});
    project.setObjectFileLocation(sound.get(), layerFolder, {});
    project.setObjectFileLocation(action.get(), actionFolder, {});

    VNX::VQLEngine engine;
    engine.setProject(&project);
    VNX::VQLError error;
    const auto before = engine.queryJson(
        QStringLiteral("$ where path = \"Layout/Nested\""),
        {QStringLiteral("name"), QStringLiteral("path")}, &error);
    ASSERT_FALSE(error.isError()) << error.message.toStdString();
    ASSERT_EQ(before.size(), 2);
    for (const auto& row : before)
        EXPECT_EQ(row.toObject().value(QStringLiteral("path")).toString(), QStringLiteral("Layout/Nested"));

    layerFolder->name = QStringLiteral("Renamed");
    const auto renamed = engine.queryJson(
        QStringLiteral("$ where path = \"Layout/Renamed\""),
        {QStringLiteral("name"), QStringLiteral("path")}, &error);
    ASSERT_FALSE(error.isError()) << error.message.toStdString();
    ASSERT_EQ(renamed.size(), 1);
    EXPECT_EQ(renamed.first().toObject().value(QStringLiteral("name")).toString(), layer->name);
    EXPECT_EQ(renamed.first().toObject().value(QStringLiteral("path")).toString(), QStringLiteral("Layout/Renamed"));
    EXPECT_TRUE(VNX::LMDBIndexer::buildSearchText(layer.get(), &project).contains(QStringLiteral("Layout/Renamed")));
    EXPECT_FALSE(VNX::LMDBIndexer::buildSearchText(layer.get(), &project).contains(QStringLiteral("Layout/Nested")));

    const auto sounds = engine.queryJson(QStringLiteral("$ type:Sound"), {QStringLiteral("path")}, &error);
    ASSERT_EQ(sounds.size(), 1);
    EXPECT_EQ(sounds.first().toObject().value(QStringLiteral("path")).toString(), sound->sourcePath);
    EXPECT_TRUE(VNX::LMDBIndexer::buildSearchText(sound.get(), &project).contains(sound->sourcePath));
    EXPECT_FALSE(VNX::LMDBIndexer::buildSearchText(sound.get(), &project).contains(QStringLiteral("Layout/Renamed")));
    const auto children = engine.queryJson(QStringLiteral("$ type:Container"), {QStringLiteral("path")}, &error);
    ASSERT_EQ(children.size(), 1);
    EXPECT_TRUE(children.first().toObject().value(QStringLiteral("path")).toString().isEmpty());
}
