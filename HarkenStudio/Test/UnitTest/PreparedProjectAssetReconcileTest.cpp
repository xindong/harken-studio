#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSound.h"
#include "Core/IntegrityScanner.h"
#include "Core/ProjectTypes.h"

#include <gtest/gtest.h>

#include <future>

TEST(PreparedProjectAssetReconcile, WorkerIntegrityScanUsesOnlyTheSuppliedProjectAndAssets)
{
    using namespace VNX;
    const auto item = [](ProjectItem::Type type, uint32_t id) {
        auto result = std::make_shared<ProjectItem>(QString::number(id), type);
        result->objectID = id;
        result->guid = QUuid::createUuid();
        result->attachDefaultComponents();
        return result;
    };
    auto master = item(ProjectItem::Type::Bus, 700);
    auto output = item(ProjectItem::Type::Bus, 701);
    auto send = item(ProjectItem::Type::Bus, 702);
    for (const auto& bus : {output, send}) {
        bus->parent = master.get();
        master->children.push_back(bus);
    }
    output->comp<VMixNode>()->sends = {{send->objectID, 1.0f}};
    auto sound = item(ProjectItem::Type::Sound, 800);
    sound->comp<VMixNode>()->outputBusId = output->objectID;
    sound->comp<VSound>()->sources.plain.entries = {{QUuid::createUuid(), 1701, {}}};
    ProjectData project;
    project.busses.push_back({master, {}});
    project.layers.push_back({sound, {}});
    AssetInfo asset;
    asset.sourceID = 1701;
    const QList<AssetInfo> assets{asset};
    const auto report = std::async(std::launch::async, [&] {
        return IntegrityScanner().scan(project, assets);
    }).get();
    EXPECT_TRUE(report.issues.empty());
    const auto missing = IntegrityScanner().scan(project, {});
    EXPECT_FALSE(missing.issues.empty());
}
