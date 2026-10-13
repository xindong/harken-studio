#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Components/VAction.h"
#include "Components/VAttenuation.h"
#include "Components/VVariable.h"
#include "FieldGroupProjectAdapter.h"
#include "ProjectManager.h"
#include "ProjectPackExporter.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>
#include <stdexcept>

using namespace VNX;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t makeTone(const QString& root, const QString& name, int period) {
    const QString path = QDir(PathUtils::getAudioSourcesDir(root)).filePath(name);
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "audio directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "wave file");
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 frames = 48000 * 4;
    out.writeRawData("RIFF", 4);
    out << quint32(36 + frames * 2);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(48000)
        << quint32(96000) << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << quint32(frames * 2);
    for (quint32 i = 0; i < frames; ++i) {
        const int phase = int(i % quint32(period));
        out << qint16((phase < period / 2 ? phase : period - phase) * 100 - period * 25);
    }
    file.close();
    AssetInfo info;
    info.relativePath = name;
    info.absolutePath = path;
    info.sourceID = AssetRegistry::instance().allocateStableID();
    info.metadata.sampleRate = 48000;
    info.metadata.channels = 1;
    info.metadata.bitDepth = 16;
    info.metadata.totalSamples = frames;
    info.metadata.fileSize = std::uint64_t(QFileInfo(path).size());
    require(AssetRegistry::instance().registerAsset(info), "register wave");
    AudioSourceData source;
    source.sourceID = info.sourceID;
    source.relativePath = name;
    ProjectManager::instance().currentProject()->audioSources.push_back(source);
    return source.sourceID;
}

ProjectItem* addAction(ProjectItem* layer, const QString& name, std::uint32_t sourceId) {
    auto& manager = ProjectManager::instance();
    auto* sound = manager.createItem(layer, name + QStringLiteral("_Sound"), ProjectItem::Type::Sound);
    require(sound != nullptr, "create sound");
    HarkenTest::setSharedSoundSource(*sound, sourceId);
    sound->markDirty();
    auto* action = manager.createItem(nullptr, name, ProjectItem::Type::Action);
    require(action && action->comp<VAction>(), "create action");
    VActionStep play;
    play.type = VActionStep::Type::Play;
    play.targetObjectID = sound->objectID;
    action->comp<VAction>()->steps = {play};
    action->markDirty();
    return action;
}

void publish(const QString& directory) {
    auto& manager = ProjectManager::instance();
    require(manager.saveProject(), "save project");
    ProjectPackExporter exporter;
    ProjectPackExporter::ExportOptions options;
    options.platform = manager.currentProject()->packExportCurrentPlatformId;
    const auto result = exporter.ExportAllPacks(*manager.currentProject(), directory, options);
    if (!result.success) throw std::runtime_error(result.error.toStdString());
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc != 3 && argc != 4 && argc != 5) return 2;
    const bool spatial = argc == 4 && QByteArray(argv[3]) == "--spatial";
    const bool variableFixture = argc == 4 && QByteArray(argv[3]) == "--variable";
    const bool reexport = argc == 5 && QByteArray(argv[3]) == "--project";
    if ((argc == 4 && !spatial && !variableFixture) || (argc == 5 && !reexport)) return 2;
    const QString root = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    if (QFileInfo::exists(root)) return 3;
    qputenv("HARKEN_PACKC_PATH", QByteArray(argv[2]));
    try {
        require(QDir().mkpath(root), "output directory");
        if (reexport) {
            auto& manager = ProjectManager::instance();
            auto prepared = ProjectManager::prepareProject(QString::fromLocal8Bit(argv[4]), false);
            if (!prepared.project) throw std::runtime_error(prepared.error.toStdString());
            require(manager.commitPreparedProject(std::move(prepared)), "open copied author project");
            auto* project = manager.currentProject();
            project->conversionFailurePolicy = ConversionFailurePolicy::AbortBuild;
            require(project->platforms.size() == 1, "reexport requires exactly one author platform");
            require(project->packExportCurrentPlatformId ==
                ProjectPlatformSemantics::idString(project->platforms.front().id),
                "reexport requires the selected author platform");
            publish(ProjectPackExporter::platformOutputDir(root, project->platforms.front()));
            QString error;
            require(ProjectPackExporter::WritePackIndex(root, project->platforms, &error), "platform index");
            manager.closeProject();
            return 0;
        }
        ProjectConfig config;
        config.name = spatial ? QStringLiteral("UnitySpatialFixture") : QStringLiteral("UnityPlaybackFixture");
        config.location = root;
        auto& manager = ProjectManager::instance();
        require(manager.createProject(config), "create project");
        auto* project = manager.currentProject();
        const QString projectRoot = QFileInfo(project->projectPath).absolutePath();
        AssetRegistry::instance().clear();
        AssetRegistry::instance().setProjectRoot(projectRoot);
        project->conversionFailurePolicy = ConversionFailurePolicy::AbortBuild;
        if (variableFixture) {
            auto* variable = manager.createItem(nullptr, QStringLiteral("Sdk_Value"), ProjectItem::Type::Variable);
            require(variable && variable->comp<VVariable>(), "create variable");
            auto* declaration = variable->comp<VVariable>();
            declaration->kind = VariableKind::Smooth;
            declaration->rangeMin = 0.0f;
            declaration->rangeMax = 1.0f;
            declaration->defaultValue = 0.0f;
            declaration->normalizeDeclaration();
            variable->markDirty();
        }
        auto* layer = manager.createItem(nullptr, QStringLiteral("Sounds"), ProjectItem::Type::Layer);
        require(layer != nullptr && !project->busses.empty(), "create layer");
        layer->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(project->busses.front().rootItem->guid));
        if (spatial) {
            auto* attenuation = manager.createItem(nullptr, QStringLiteral("Distance"), ProjectItem::Type::Attenuation);
            require(attenuation && attenuation->comp<VAttenuation>(), "create attenuation");
            auto* profile = attenuation->comp<VAttenuation>();
            profile->maxDistance = 100.0f;
            AttenuationDimensionDef volume;
            volume.curve = {{0.0f, 0.0f}, {100.0f, -60.0f}};
            profile->dimensions = {volume};
            attenuation->markDirty();
            layer->fieldGroups.spatialRating.shared = {FieldGroupMode::Local,
                SpatialRatingFieldGroup{0.0f, true, toFieldGroupGuid(attenuation->guid), 1.0f}};
        }
        auto* first = addAction(layer, QStringLiteral("Play_U4_A"), makeTone(projectRoot, QStringLiteral("a.wav"), 100));
        auto* second = addAction(layer, QStringLiteral("Play_U4_B"), makeTone(projectRoot, QStringLiteral("b.wav"), 160));
        if (spatial) {
            auto* delayed = manager.createItem(nullptr, QStringLiteral("Play_Spatial_Delayed"), ProjectItem::Type::Action);
            require(delayed && delayed->comp<VAction>(), "create delayed action");
            auto step = first->comp<VAction>()->steps.front();
            step.delay = 100.0f;
            delayed->comp<VAction>()->steps = {step};
            delayed->markDirty();
        }
        auto* caller = manager.createItem(nullptr, QStringLiteral("Play_U4_Caller"), ProjectItem::Type::Action);
        require(caller && caller->comp<VAction>(), "create caller");
        VActionStep call;
        call.type = VActionStep::Type::PostAction;
        call.targetObjectID = second->objectID;
        caller->comp<VAction>()->steps = {call};
        caller->markDirty();
        publish(QDir(root).filePath(QStringLiteral("original")));
        require(manager.renameItem(first, QStringLiteral("Play_U4_Renamed")), "rename action");
        publish(QDir(root).filePath(QStringLiteral("renamed")));
        manager.closeProject();
        AssetRegistry::instance().clear();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
