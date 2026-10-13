#include "UnitTest/StudioObjectIdTestSupport.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Components/VAction.h"
#include "Components/VMixNode.h"
#include "ProjectPackExporter.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "Support/ProjectPlatformTestUtils.h"

#ifndef HARKEN_PACKC_PATH
#error "HARKEN_PACKC_PATH must point at the packc executable"
#endif

namespace {

constexpr uint64_t kStableIdBias = 100000;
constexpr uint32_t kMasterBusObjectId = 200;
constexpr uint32_t kRootLayerObjectId = 10;
constexpr uint32_t kSoundObjectId = 1;
constexpr uint32_t kActionFolderObjectId = 20;
constexpr uint32_t kHeadActionObjectId = 21;
constexpr uint32_t kTailActionObjectId = 22;
constexpr uint32_t kToneSourceId = 9001;
constexpr uint32_t kToneSampleRate = 48000;
constexpr uint32_t kToneFrames = 96;
constexpr uint16_t kToneChannels = 1;

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

void writeTone(const QString& path, float amplitude)
{
    std::vector<float> samples(kToneFrames);
    for (uint32_t i = 0; i < kToneFrames; ++i)
        samples[i] = amplitude * static_cast<float>(i % 13) / 16.0f;

    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    const uint16_t blockAlign = kToneChannels * sizeof(float);
    constexpr uint32_t fmtSize = 18;
    constexpr uint32_t factSize = 4;
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);

    QByteArray bytes;
    bytes.append("RIFF", 4);
    appendU32(bytes, riffSize);
    bytes.append("WAVE", 4);
    bytes.append("fmt ", 4);
    appendU32(bytes, fmtSize);
    appendU16(bytes, 3);
    appendU16(bytes, kToneChannels);
    appendU32(bytes, kToneSampleRate);
    appendU32(bytes, kToneSampleRate * blockAlign);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 32);
    appendU16(bytes, 0);
    bytes.append("fact", 4);
    appendU32(bytes, factSize);
    appendU32(bytes, kToneFrames);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    bytes.append(reinterpret_cast<const char*>(samples.data()), dataBytes);

    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

QUuid stableGuid(uint32_t objectId)
{
    return VNX::authoringUuid(harkentest::StudioObjectId(objectId + kStableIdBias));
}

std::shared_ptr<VNX::ProjectItem> makeItem(const QString& name,
                                           VNX::ProjectItem::Type type,
                                           uint32_t objectId)
{
    auto item = std::make_shared<VNX::ProjectItem>();
    item->name = name;
    item->type = type;
    item->objectID = objectId;
    item->guid = VNX::authoringUuid(harkentest::StudioObjectId(objectId + kStableIdBias));
    item->guid = stableGuid(objectId);
    item->attachDefaultComponents();
    return item;
}

void attach(const std::shared_ptr<VNX::ProjectItem>& parent,
            const std::shared_ptr<VNX::ProjectItem>& child)
{
    child->parent = parent.get();
    child->parentRef = {VNX::ParentRef::Kind::Item, parent->guid};
    parent->children.push_back(child);
}

void addBinding(std::vector<VNX::ProjectFileBinding>& bindings,
                const std::shared_ptr<VNX::ProjectItem>& root)
{
    VNX::ProjectFileBinding binding;
    binding.rootItem = root;
    bindings.push_back(std::move(binding));
}

VNX::VActionStep playStep(const VNX::ProjectItem& target)
{
    VNX::VActionStep result;
    result.type = VNX::VActionStep::Type::Play;
    result.targetObjectID = target.objectID;
    result.targetName = target.name;
    return result;
}

VNX::VActionStep postActionStep(const VNX::ProjectItem& target)
{
    VNX::VActionStep result;
    result.type = VNX::VActionStep::Type::PostAction;
    result.targetObjectID = target.objectID;
    result.targetName = target.name;
    return result;
}

uint32_t registerFixedTone(const QString& root)
{
    const QString relativePath = QStringLiteral("shared.wav");
    const QString absolutePath =
        QDir(VNX::PathUtils::getAudioSourcesDir(root)).filePath(relativePath);
    writeTone(absolutePath, 1.0f);

    VNX::AssetInfo info;
    info.relativePath = relativePath;
    info.absolutePath = absolutePath;
    info.sourceID = kToneSourceId;
    info.metadata.sampleRate = kToneSampleRate;
    info.metadata.channels = kToneChannels;
    info.metadata.bitDepth = 32;
    info.metadata.totalSamples = kToneFrames;
    info.metadata.fileSize = static_cast<uint64_t>(QFileInfo(absolutePath).size());
    info.lastModified = 1;
    EXPECT_TRUE(VNX::AssetRegistry::instance().registerAsset(info));
    return info.sourceID;
}

VNX::ProjectData makeDualPlatformProject(const QString& root, uint32_t sourceId)
{
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    project.name = QStringLiteral("s7-dual-platform-digest");
    project.projectPath = QDir(root).filePath(QStringLiteral("scenario.vproj"));
    project.conversionFailurePolicy = VNX::ConversionFailurePolicy::AbortBuild;
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Linux")});

    VNX::AudioSourceData source;
    source.sourceID = sourceId;
    source.relativePath = QStringLiteral("shared.wav");
    source.language = QStringLiteral("SFX");
    project.audioSources.push_back(source);

    auto master = makeItem(QStringLiteral("MasterBus"),
                           VNX::ProjectItem::Type::Bus, kMasterBusObjectId);
    master->guid = VNX::authoringUuid(harkentest::StudioObjectId(0x4d4153544552));
    addBinding(project.busses, master);

    auto layer = makeItem(QStringLiteral("Root"),
                          VNX::ProjectItem::Type::Layer, kRootLayerObjectId);
    layer->fieldGroups = VNX::MakeRootFieldGroups(VNX::toFieldGroupGuid(master->guid));
    auto sound = makeItem(QStringLiteral("SharedTone"),
                          VNX::ProjectItem::Type::Sound, kSoundObjectId);
    HarkenTest::setSharedSoundSource(*sound, sourceId);
    sound->comp<VNX::VMixNode>()->outputBusId = master->objectID;
    attach(layer, sound);
    addBinding(project.layers, layer);

    auto actions = makeItem(QStringLiteral("Actions"),
                            VNX::ProjectItem::Type::None, kActionFolderObjectId);
    auto head = makeItem(QStringLiteral("Head"),
                         VNX::ProjectItem::Type::Action, kHeadActionObjectId);
    auto tail = makeItem(QStringLiteral("Tail"),
                         VNX::ProjectItem::Type::Action, kTailActionObjectId);
    head->comp<VNX::VAction>()->steps = {
        playStep(*sound),
        postActionStep(*tail)};
    tail->comp<VNX::VAction>()->steps = {playStep(*sound)};
    attach(actions, head);
    attach(actions, tail);
    addBinding(project.actions, actions);

    project.nextObjectID = kMasterBusObjectId + 1;

    return project;
}

std::vector<std::pair<QString, QByteArray>> collectRelativeDigests(
    const QString& exportRoot)
{
    std::vector<std::pair<QString, QByteArray>> files;
    QDirIterator it(exportRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString absolutePath = it.filePath();
        QFile file(absolutePath);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << absolutePath.toStdString();
        const QByteArray digest =
            QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
        files.emplace_back(QDir(exportRoot).relativeFilePath(absolutePath), digest);
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return files;
}

void printDigestLines(const QString& platform,
                      const std::vector<std::pair<QString, QByteArray>>& files)
{
    for (const auto& file : files) {
        std::cout << "[S7-DIGEST] "
                  << platform.toStdString() << '/'
                  << QDir::fromNativeSeparators(file.first).toStdString()
                  << ' ' << file.second.toHex().constData() << '\n';
    }
}

} // namespace

TEST(DualPlatformExportDigest, PrintsStableSha256ForWindowsAndLinux)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sourceId = registerFixedTone(dir.path());
    VNX::ProjectData project = makeDualPlatformProject(dir.path(), sourceId);

    const QStringList platforms = {
        QStringLiteral("Windows"), QStringLiteral("Linux")};
    VNX::ProjectPackExporter exporter;
    for (const QString& platform : platforms) {
        const QString exportRoot =
            QDir(dir.path()).filePath(platform);
        ASSERT_TRUE(QDir().mkpath(exportRoot));

        VNX::ProjectPackExporter::ExportOptions options;
        options.platform = HarkenTest::platformIdString(platform);
        const auto result = exporter.ExportAllPacks(project, exportRoot, options);
        ASSERT_TRUE(result.success) << platform.toStdString() << ": "
                                    << result.error.toStdString();

        const auto files = collectRelativeDigests(exportRoot);
        ASSERT_GT(files.size(), 0u) << platform.toStdString();
        printDigestLines(platform, files);
    }

    VNX::AssetRegistry::instance().clear();
}
