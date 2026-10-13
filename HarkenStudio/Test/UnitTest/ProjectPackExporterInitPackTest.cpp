#include "UnitTest/StudioObjectIdTestSupport.h"
#include "BindingSemantics.h"
#include "Components/VAttenuation.h"
#include "Components/VBus.h"
#include "Components/VMixNode.h"
#include "Components/VVariable.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectManager.h"
#include "ProjectObjectIndex.h"
#include "ProjectPackExporter.h"
#include "CanonicalPackSetExport.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "HarkenMath.h"

#include "harken/Harken.h"
#include <harken/param/ParamTargetTable.h>
#include "harken/media/PackFormatV2.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QTemporaryDir>
#include <QUuid>

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>
#include "Support/ProjectPlatformTestUtils.h"

#ifndef HARKEN_PACKC_PATH
#error "HARKEN_PACKC_PATH must point at the packc executable"
#endif

namespace {

using namespace HarkenEditor;

struct PackImage
{
    QByteArray bytes;
    harken::PackV2Header header;
    std::vector<harken::PackV2DirectoryEntry> entries;

    const uint8_t* data() const
    {
        return reinterpret_cast<const uint8_t*>(bytes.constData());
    }

    int countKind(uint32_t kind) const
    {
        int count = 0;
        for (const auto& entry : entries)
            if (entry.kind == kind)
                ++count;
        return count;
    }

    const harken::PackV2DirectoryEntry* findKind(uint32_t kind) const
    {
        for (const auto& entry : entries)
            if (entry.kind == kind)
                return &entry;
        return nullptr;
    }
};

void readPack(const QString& path, PackImage& out)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    out.bytes = file.readAll();
    ASSERT_GE(out.bytes.size(), static_cast<qsizetype>(harken::kPackV2HeaderBytes));
    ASSERT_TRUE(harken::ParsePackV2Header(out.data(), &out.header));
    out.entries.clear();
    for (uint32_t i = 0; i < out.header.entryCount; ++i)
        out.entries.push_back(harken::ParsePackV2DirectoryEntry(
            out.data() + out.header.directoryOffset +
            i * harken::kPackV2DirectoryEntryBytes));
}

VnResult loadPack(const QString& path, VnPackHandle* outHandle)
{
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    const QByteArray utf8 = QFileInfo(path).absoluteFilePath().toUtf8();
    desc.utf8Path = utf8.constData();
    return Vn_LoadPack(&desc, outHandle);
}

uint32_t fnvOf(const char* name)
{
    return HarkenEditor::hashString(name);
}

std::shared_ptr<VNX::ProjectItem> makeItem(const QString& name,
                                           VNX::ProjectItem::Type type,
                                           uint32_t objectID)
{
    auto item = std::make_shared<VNX::ProjectItem>();
    item->name = name;
    item->type = type;
    item->objectID = objectID;
    item->guid = QUuid::createUuid();
    item->guid = VNX::authoringUuid(harkentest::StudioObjectId(fnvOf(name.toUtf8().constData())));
    item->attachDefaultComponents();
    return item;
}

void addChild(const std::shared_ptr<VNX::ProjectItem>& parent,
              const std::shared_ptr<VNX::ProjectItem>& child)
{
    child->parent = parent.get();
    parent->children.push_back(child);
}

constexpr harkenstudio::ObjectId kBusId = harkentest::StudioObjectId(501);
constexpr harkenstudio::ObjectId kVariableId = harkentest::StudioObjectId(901);
constexpr harkenstudio::ObjectId kBindingId = harkentest::StudioObjectId(801);
constexpr harkenstudio::ObjectId kVoiceBindingId = harkentest::StudioObjectId(802);
constexpr harkenstudio::ObjectId kVoiceObjectId = harkentest::StudioObjectId(7101);

} // namespace

TEST(EditorProjectPackExporterInitPack, GeneratedPackBinaryStaysFormatVersion2)
{
    static_assert(harken::kPackFormatVersion2 == 4);
    EXPECT_EQ(harken::kPackFormatVersion2, 4u);
}

TEST(EditorProjectPackExporterInitPack, DefaultMasterExportsItsUuidAndEffectOwner)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("MasterIdentity");
    config.location = dir.path();
    auto prepared = VNX::ProjectManager::prepareProject(config);
    ASSERT_NE(prepared.project, nullptr) << prepared.error.toStdString();
    auto& project = *prepared.project;
    ASSERT_EQ(project.busses.size(), 1u);
    auto& master = *project.busses.front().rootItem;
    const auto masterId = master.runtimeId();
    ASSERT_FALSE(harkenstudio::IsNull(masterId));
    ASSERT_NE(masterId, harkenstudio::kMasterBusObjectId);

    VNX::EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.type = VNX::EffectType::Gain;
    effect.parameters = {-3.0f};
    ASSERT_TRUE(master.comp<VNX::VMixNode>()->effects.replace({effect}));
    const auto effectId = effect.runtimeId();
    const auto index = VNX::ProjectObjectIndex::fromProject(&project);
    const auto* location = index.find(effectId);
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->kind, VNX::ProjectObjectLocation::Kind::Effect);
    EXPECT_EQ(location->ownerGuid, master.guid);
    EXPECT_EQ(location->ownerObjectId, master.objectID);
    EXPECT_EQ(location->name, master.name + QStringLiteral(" / gain"));
    EXPECT_EQ(index.find(harkenstudio::kMasterBusObjectId), nullptr);

    VNX::ProjectPackExporter exporter;
    const auto packPath = QDir(dir.path()).filePath(QStringLiteral("Init.pack"));
    auto result = exporter.ExportInitPack(project, packPath,
        {.platform = project.platformIds().first()});
    ASSERT_TRUE(result.success) << result.error.toStdString();
    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
    ASSERT_EQ(image.countKind(harken::kPackEntryKindBus), 1);
    const auto* busEntry = image.findKind(harken::kPackEntryKindBus);
    harken::PackBusEntry bus{};
    ASSERT_TRUE(harken::ParsePackV6Bus(image.data() + busEntry->offset,
        busEntry->size, &bus));
    EXPECT_EQ(bus.busId, masterId);
    EXPECT_EQ(bus.parentBusId, harkenstudio::kMasterBusObjectId);
    ASSERT_EQ(image.countKind(harken::kPackEntryKindBusEffect), 1);
    const auto* effectEntry = image.findKind(harken::kPackEntryKindBusEffect);
    harken::PackBusEffectEntry packedEffect{};
    ASSERT_TRUE(harken::ParsePackV2BusEffect(image.data() + effectEntry->offset,
        effectEntry->size, &packedEffect));
    EXPECT_EQ(packedEffect.effectId, effectId);
    EXPECT_EQ(packedEffect.busId, masterId);
}

TEST(EditorPackBuildSessionInitSections, ResidentPackCarriesSupportedLogicSections)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("init.pack"));

    Bridge::PackBuildSession builder;
    builder.setInitPack(true);

    Bridge::PackBus bus;
    bus.id = harkentest::StudioObjectId(kBusId);
    bus.parentId = harkentest::StudioObjectId(kBusId);
    bus.name = QStringLiteral("RootBus");
    const harkenstudio::ObjectId effectId{0xa1096084e74a4478ULL, 0x96bdc6d7cf119c35ULL};
    bus.effects.push_back({effectId, VN_EFFECT_GAIN, true, {-3.0f}});
    builder.addBus(bus);

    Bridge::PackParameterBinding binding;
    binding.id = kBindingId;
    binding.variableId = harkentest::StudioObjectId(kVariableId);
    binding.scope = 1;
    binding.target = 0;
    binding.transitionMs = 50;
    binding.points.push_back({0.0f, 0.0f});
    binding.points.push_back({1.0f, -12.0f});
    binding.handles.push_back({0.0f, 0.0f, 1.0f, -12.0f});
    builder.addBinding(binding);

    Bridge::PackBindingAttachment attachment;
    attachment.bindingId = kBindingId;
    attachment.scope = 1;
    attachment.objectId = harkentest::StudioObjectId(kBusId);
    builder.addBindingAttachment(attachment);

    ASSERT_TRUE(builder.buildToFile(packPath)) << builder.lastError().toStdString();
    EXPECT_TRUE(builder.warnings().isEmpty())
        << builder.warnings().join('\n').toStdString();

    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
    EXPECT_EQ(image.header.packKind, harken::kPackKindResident);

    ASSERT_EQ(image.countKind(harken::kPackEntryKindBusEffect), 1);
    const auto* effectEntry = image.findKind(harken::kPackEntryKindBusEffect);
    harken::PackBusEffectEntry effectWire{};
    ASSERT_TRUE(harken::ParsePackV2BusEffect(image.data() + effectEntry->offset,
        effectEntry->size, &effectWire));
    EXPECT_EQ(effectWire.effectId, effectId);
    EXPECT_EQ(effectWire.busId, kBusId);
    EXPECT_EQ(effectWire.effectType, VN_EFFECT_GAIN);
    EXPECT_EQ(effectWire.bypass, 1u);
    EXPECT_EQ(effectWire.descriptorVintage, 2u);
    EXPECT_FLOAT_EQ(effectWire.params[0], -3.0f);

    EXPECT_EQ(image.countKind(harken::kPackEntryKindDiscreteVariable), 0);
    EXPECT_EQ(image.countKind(harken::kPackEntryKindMeterWiring), 0);

    ASSERT_EQ(image.countKind(harken::kPackEntryKindBindingDefinition), 1);
    const auto* bindingEntry = image.findKind(harken::kPackEntryKindBindingDefinition);
    VnBindingId bindingId{};
    VnBindingDesc bindingWire{};
    ASSERT_TRUE(harken::ParsePackV2BindingDefinition(
        image.data() + bindingEntry->offset, bindingEntry->size,
        &bindingId, &bindingWire));
    EXPECT_EQ(bindingId, kBindingId);
    EXPECT_EQ(bindingWire.variableId, kVariableId);
    EXPECT_EQ(bindingWire.targetScopeKind, 1u);
    EXPECT_EQ(bindingWire.targetId, 0u);
    EXPECT_EQ(bindingWire.transitionMs, 50u);
    ASSERT_EQ(bindingWire.curve.pointCount, 2u);
    EXPECT_FLOAT_EQ(bindingWire.curve.points[0].x, 0.0f);
    EXPECT_FLOAT_EQ(bindingWire.curve.points[1].x, 1.0f);
    EXPECT_FLOAT_EQ(bindingWire.curve.points[1].y, -12.0f);

    ASSERT_EQ(image.countKind(harken::kPackEntryKindBindingAttachment), 1);
    const auto* attachmentEntry = image.findKind(harken::kPackEntryKindBindingAttachment);
    harken::PackBindingAttachmentEntry attachmentWire{};
    ASSERT_TRUE(harken::ParsePackV2BindingAttachment(
        image.data() + attachmentEntry->offset, attachmentEntry->size,
        &attachmentWire));
    EXPECT_EQ(attachmentWire.bindingId, kBindingId);
    EXPECT_EQ(attachmentWire.scopeKind, 1u);
    EXPECT_EQ(attachmentWire.objectId, kBusId);

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    config.gainEffectPoolCapacity = 1;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    VnPackHandle handle = 0;
    EXPECT_EQ(VN_OK, loadPack(packPath, &handle));
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(EditorPackBuildSessionInitSections, VolumeCurveYValuesPassThroughUnchanged)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("attenuation.pack"));

    Bridge::PackBuildSession builder;
    builder.setInitPack(true);

    Bridge::PackAttenuation att;
    att.id = harkentest::StudioObjectId(1001);
    att.maxDistance = 100.0f;
    Bridge::PackAttenuation::Slot volume;
    volume.slot = 0;
    volume.points.push_back({0.0f, 0.0f});
    volume.points.push_back({40.0f, -12.0f});
    volume.points.push_back({100.0f, -96.0f});
    volume.handles.push_back({0.0f, 0.0f, 40.0f, -12.0f});
    volume.handles.push_back({40.0f, -12.0f, 100.0f, -96.0f});
    att.curveSlots.push_back(volume);
    builder.addAttenuation(att);

    ASSERT_TRUE(builder.buildToFile(packPath)) << builder.lastError().toStdString();

    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
    ASSERT_EQ(image.countKind(harken::kPackEntryKindAttenuation), 1);

    VnAttenuationDesc wire{};
    for (const auto& entry : image.entries) {
        if (entry.kind != harken::kPackEntryKindAttenuation)
            continue;
        ASSERT_TRUE(harken::ParsePackV3Attenuation(
            image.data() + entry.offset, entry.size, &wire));
    }

    ASSERT_EQ(wire.volumeCurve.pointCount, 3u);
    EXPECT_FLOAT_EQ(wire.volumeCurve.points[0].y, 0.0f);
    EXPECT_FLOAT_EQ(wire.volumeCurve.points[1].y, -12.0f);
    EXPECT_FLOAT_EQ(wire.volumeCurve.points[2].y, -96.0f);
}

// A seven-point native DTO curve exercises all handle slots and verifies
// absolute coordinate ordering through the packc boundary.
TEST(EditorPackBuildSessionInitSections, CurveHandlesRoundtripThroughPackc)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("handles.pack"));

    Bridge::PackBuildSession builder;
    builder.setInitPack(true);

    Bridge::PackAttenuation att;
    att.id = harkentest::StudioObjectId(1002);
    att.maxDistance = 60.0f;
    Bridge::PackAttenuation::Slot volume;
    volume.slot = 0;
    for (uint8_t i = 0; i < 7; ++i)
        volume.points.push_back({i * 10.0f, i * -2.0f});
    for (uint8_t i = 0; i < 6; ++i)
        volume.handles.push_back({i * 10.0f, i * -2.0f,
                                  (i + 1) * 10.0f, (i + 1) * -2.0f});
    att.curveSlots.push_back(volume);
    builder.addAttenuation(att);

    ASSERT_TRUE(builder.buildToFile(packPath)) << builder.lastError().toStdString();
    EXPECT_TRUE(builder.warnings().isEmpty()) << builder.warnings().join('\n').toStdString();

    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
    const auto* entry = image.findKind(harken::kPackEntryKindAttenuation);
    ASSERT_NE(entry, nullptr);
    VnAttenuationDesc wire{};
    ASSERT_TRUE(harken::ParsePackV3Attenuation(
        image.data() + entry->offset, entry->size, &wire));

    ASSERT_EQ(wire.volumeCurve.pointCount, 7u);
    for (int i = 0; i < 6; ++i)
    {
        EXPECT_FLOAT_EQ(wire.volumeCurve.handles[i].h1x, i * 10.0f);
        EXPECT_FLOAT_EQ(wire.volumeCurve.handles[i].h1y, i * -2.0f);
        EXPECT_FLOAT_EQ(wire.volumeCurve.handles[i].h2x, (i + 1) * 10.0f);
        EXPECT_FLOAT_EQ(wire.volumeCurve.handles[i].h2y, (i + 1) * -2.0f);
    }
}

TEST(EditorPackBuildSessionInitSections, BusMixFieldsReachWire)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("bus.pack"));

    Bridge::PackBuildSession builder;
    builder.setInitPack(true);

    Bridge::PackBus bus;
    bus.id = harkentest::StudioObjectId(kBusId);
    bus.parentId = harkentest::StudioObjectId(kBusId);
    bus.name = QStringLiteral("Music");
    bus.volume = -6.0f;
    bus.outputBusVolume = -3.5f;
    bus.makeUpGain = 2.0f;
    builder.addBus(bus);

    ASSERT_TRUE(builder.buildToFile(packPath)) << builder.lastError().toStdString();
    const QString warnings = builder.warnings().join(QStringLiteral("\n"));
    EXPECT_FALSE(warnings.contains(QStringLiteral("no pack representation")))
        << warnings.toStdString();

    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
    const auto* entry = image.findKind(harken::kPackEntryKindBus);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->vintage, harken::kPackV2BusMixSchemaVintage);
    EXPECT_EQ(entry->size, harken::kPackV6BusPayloadBytes);
    harken::PackBusEntry wire{};
    ASSERT_TRUE(harken::ParsePackV6Bus(image.data() + entry->offset, entry->size, &wire));
    EXPECT_EQ(wire.busId, kBusId);
    EXPECT_EQ(wire.parentBusId, harkenstudio::kMasterBusObjectId);
    EXPECT_FLOAT_EQ(wire.volume, -6.0f);
    EXPECT_FLOAT_EQ(wire.outputBusVolume, -3.5f);
    EXPECT_FLOAT_EQ(wire.makeUpGain, 2.0f);
}

TEST(EditorPackBuildSessionInitSections, KindDisciplineForNewSections)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    {
        const QString contentPath =
            QDir(dir.path()).filePath(QStringLiteral("content.pack"));
        Bridge::PackBuildSession builder;

        builder.addDiscreteVariable(kVariableId);
        Bridge::PackMeterWiring wiring;
        wiring.busId = harkentest::StudioObjectId(kBusId);
        wiring.variableId = harkentest::StudioObjectId(kVariableId);
        builder.addMeterWiring(wiring);

        Bridge::PackParameterBinding voiceBinding;
        voiceBinding.id = kVoiceBindingId;
        voiceBinding.variableId = harkentest::StudioObjectId(kVariableId);
        voiceBinding.scope = 0;
        voiceBinding.target = 1;
        voiceBinding.points.push_back({0.0f, 0.5f});
        voiceBinding.points.push_back({1.0f, 2.0f});
        voiceBinding.handles.push_back({0.0f, 0.5f, 1.0f, 2.0f});
        builder.addBinding(voiceBinding);

        Bridge::PackBindingAttachment voiceAttachment;
        voiceAttachment.bindingId = kVoiceBindingId;
        voiceAttachment.scope = 0;
        voiceAttachment.objectId = harkentest::StudioObjectId(kVoiceObjectId);
        builder.addBindingAttachment(voiceAttachment);

        ASSERT_TRUE(builder.buildToFile(contentPath))
            << builder.lastError().toStdString();

        bool refusedResidentSections = false;
        for (const QString& warning : builder.warnings())
            refusedResidentSections = refusedResidentSections ||
                warning.contains(QStringLiteral("discrete-variable/meter-wiring"));
        EXPECT_TRUE(refusedResidentSections)
            << builder.warnings().join('\n').toStdString();

        PackImage image;
        ASSERT_NO_FATAL_FAILURE(readPack(contentPath, image));
        EXPECT_EQ(image.header.packKind, harken::kPackKindContent);
        EXPECT_EQ(image.countKind(harken::kPackEntryKindDiscreteVariable), 0);
        EXPECT_EQ(image.countKind(harken::kPackEntryKindMeterWiring), 0);
        ASSERT_EQ(image.countKind(harken::kPackEntryKindBindingDefinition), 1);
        ASSERT_EQ(image.countKind(harken::kPackEntryKindBindingAttachment), 1);

        const auto* bindingEntry =
            image.findKind(harken::kPackEntryKindBindingDefinition);
        VnBindingId bindingId{};
        VnBindingDesc bindingWire{};
        ASSERT_TRUE(harken::ParsePackV2BindingDefinition(
            image.data() + bindingEntry->offset, bindingEntry->size,
            &bindingId, &bindingWire));
        EXPECT_EQ(bindingId, kVoiceBindingId);
        EXPECT_EQ(bindingWire.targetScopeKind, 0u);
        EXPECT_EQ(bindingWire.targetId, 1u);
    }

    {
        const QString residentPath =
            QDir(dir.path()).filePath(QStringLiteral("resident.pack"));
        Bridge::PackBuildSession builder;
        builder.setInitPack(true);

        Bridge::PackBus bus;
        bus.id = harkentest::StudioObjectId(kBusId);
        bus.parentId = harkentest::StudioObjectId(kBusId);
        bus.name = QStringLiteral("RootBus");
        builder.addBus(bus);

        Bridge::PackParameterBinding voiceBinding;
        voiceBinding.id = kVoiceBindingId;
        voiceBinding.variableId = harkentest::StudioObjectId(kVariableId);
        voiceBinding.scope = 0;
        voiceBinding.target = 1;
        voiceBinding.points.push_back({0.0f, 0.5f});
        voiceBinding.points.push_back({1.0f, 2.0f});
        voiceBinding.handles.push_back({0.0f, 0.5f, 1.0f, 2.0f});
        builder.addBinding(voiceBinding);

        ASSERT_TRUE(builder.buildToFile(residentPath))
            << builder.lastError().toStdString();

        bool droppedVoiceBinding = false;
        for (const QString& warning : builder.warnings())
            droppedVoiceBinding = droppedVoiceBinding ||
                warning.contains(QStringLiteral("voice-scope"));
        EXPECT_TRUE(droppedVoiceBinding)
            << builder.warnings().join('\n').toStdString();

        PackImage image;
        ASSERT_NO_FATAL_FAILURE(readPack(residentPath, image));
        EXPECT_EQ(image.countKind(harken::kPackEntryKindBindingDefinition), 0);
    }
}

TEST(EditorProjectPackExporterInitPack, CollectsInitInfrastructureFromProject)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    project.name = QStringLiteral("init-e2e");
    project.projectPath = QDir(dir.path()).filePath(QStringLiteral("init-e2e.vproj"));

    auto master = makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 201);
    auto aux = makeItem(QStringLiteral("AuxBus"), VNX::ProjectItem::Type::Bus, 202);
    addChild(master, aux);
    {
        const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*aux);
        ASSERT_TRUE(locator.has_value());
        ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
            *aux, *locator,
            static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Sends),
            QJsonArray{QJsonObject{
                {QStringLiteral("targetBusID"), 201},
                {QStringLiteral("sendLevel"),
                 static_cast<double>(VNX::VMixNode::linearToDb(0.5f))}}}, std::nullopt));
    }

    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    auto layer = makeItem(QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer, 10);
    auto sound = makeItem(QStringLiteral("Tone"), VNX::ProjectItem::Type::Sound, 11);
    HarkenTest::setSharedSoundSource(*sound, 77);
    sound->fieldGroups.send.shared = {
        VNX::FieldGroupMode::Local,
        VNX::SendFieldGroup{{{VNX::toFieldGroupGuid(aux->guid),
                                VNX::VMixNode::linearToDb(0.7f)}}}};
    addChild(layer, sound);
    VNX::ProjectFileBinding layerBinding;
    layerBinding.rootItem = layer;
    project.layers.push_back(std::move(layerBinding));

    auto attenuationItem =
        makeItem(QStringLiteral("Fade"), VNX::ProjectItem::Type::Attenuation, 301);
    auto* attenuation = attenuationItem->comp<VNX::VAttenuation>();
    attenuation->maxDistance = 50.0f;
    auto& fadeVolume = attenuation->dimensions.emplaceBack(VNX::AttenuationDimensionDef{VNX::AttenuationDimension::Volume});
    fadeVolume.curve.push_back({0.0f, 0.0f});
    fadeVolume.curve.push_back({50.0f, -60.0f});
    VNX::ProjectFileBinding attenuationBinding;
    attenuationBinding.rootItem = attenuationItem;
    project.attenuationItems.push_back(std::move(attenuationBinding));
    VNX::SpatialRatingFieldGroup rating;
    rating.spatialAttenuationRef = VNX::toFieldGroupGuid(attenuationItem->guid);
    sound->fieldGroups.spatialRating.shared = {VNX::FieldGroupMode::Local, rating};

    VNX::ProjectPackExporter exporter;
    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("Init.pack"));
    auto result = exporter.ExportInitPack(project, packPath, {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const QString initWarnings = result.warnings.join(QStringLiteral("\n"));
    EXPECT_FALSE(initWarnings.contains(QStringLiteral("no pack representation")))
        << initWarnings.toStdString();
    EXPECT_FALSE(initWarnings.contains(QStringLiteral("is not readable")))
        << initWarnings.toStdString();

    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
    EXPECT_EQ(image.header.packKind, harken::kPackKindResident);

    const harkenstudio::ObjectId masterId = harkentest::StudioObjectId(fnvOf("MasterBus"));
    const harkenstudio::ObjectId auxId = harkentest::StudioObjectId(fnvOf("AuxBus"));

    ASSERT_EQ(image.countKind(harken::kPackEntryKindBus), 2);
    bool sawMaster = false;
    bool sawAux = false;
    for (const auto& entry : image.entries)
    {
        if (entry.kind != harken::kPackEntryKindBus)
            continue;
        EXPECT_EQ(entry.vintage, harken::kPackV2BusMixSchemaVintage);
        EXPECT_EQ(entry.size, harken::kPackV6BusPayloadBytes);
        harken::PackBusEntry busWire{};
        ASSERT_TRUE(harken::ParsePackV6Bus(image.data() + entry.offset,
                                          entry.size, &busWire));
        EXPECT_FLOAT_EQ(busWire.volume, 0.0f);
        EXPECT_FLOAT_EQ(busWire.outputBusVolume, 0.0f);
        EXPECT_FLOAT_EQ(busWire.makeUpGain, 0.0f);
        if (busWire.busId == masterId)
        {
            sawMaster = true;
            EXPECT_EQ(busWire.parentBusId, harkenstudio::kMasterBusObjectId);
        }
        if (busWire.busId == auxId)
        {
            sawAux = true;
            EXPECT_EQ(busWire.parentBusId, masterId);
        }
    }
    EXPECT_TRUE(sawMaster);
    EXPECT_TRUE(sawAux);

    ASSERT_EQ(image.countKind(harken::kPackEntryKindBusSend), 1);
    const auto* sendEntry = image.findKind(harken::kPackEntryKindBusSend);
    harken::PackBusSendEntry sendWire{};
    ASSERT_TRUE(harken::ParsePackV2BusSend(image.data() + sendEntry->offset,
                                          sendEntry->size, &sendWire));
    EXPECT_EQ(sendWire.fromBusId, auxId);
    EXPECT_EQ(sendWire.toBusId, masterId);
    EXPECT_FLOAT_EQ(sendWire.levelDb, VNX::VMixNode::linearToDb(0.5f));

    ASSERT_EQ(image.countKind(harken::kPackEntryKindAttenuation), 1);
    const auto* attenuationEntry = image.findKind(harken::kPackEntryKindAttenuation);
    EXPECT_EQ(attenuationEntry->id, harkentest::StudioObjectId(fnvOf("Fade")));
    EXPECT_EQ(attenuationEntry->vintage, harken::kPackV2AuthoredToneSchemaVintage);
    EXPECT_EQ(attenuationEntry->size, harken::kPackV3AttenuationPayloadBytes);
    EXPECT_EQ(image.countKind(harken::kPackEntryKindVoiceRecipe), 0);
    for (const auto& entry : image.entries) {
        if (entry.kind == harken::kPackEntryKindAttenuation) {
            EXPECT_EQ(entry.vintage, harken::kPackV2AuthoredToneSchemaVintage);
            EXPECT_NE(entry.vintage, harken::kPackV2SchemaVintage);
        }
    }

    EXPECT_EQ(image.countKind(harken::kPackEntryKindMedia), 0);
    EXPECT_EQ(image.countKind(harken::kPackEntryKindDiscreteVariable), 0);
    EXPECT_EQ(image.countKind(harken::kPackEntryKindMeterWiring), 0);
    EXPECT_EQ(image.countKind(harken::kPackEntryKindBindingDefinition), 0);
    EXPECT_EQ(image.countKind(harken::kPackEntryKindBindingAttachment), 0);
    EXPECT_EQ(image.countKind(harken::kPackEntryKindContainer), 0);
    EXPECT_EQ(image.countKind(harken::kPackEntryKindAction), 0);

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    VnPackHandle handle = 0;
    EXPECT_EQ(VN_OK, loadPack(packPath, &handle));
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(EditorProjectPackExporterInitPack, BusSendLevelFollowsExportPlatform)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    project.name = QStringLiteral("init-platform-send");
    project.projectPath = QDir(dir.path()).filePath(QStringLiteral("init-platform-send.vproj"));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});

    auto master = makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 201);
    auto aux = makeItem(QStringLiteral("AuxBus"), VNX::ProjectItem::Type::Bus, 202);
    addChild(master, aux);

    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*aux);
    ASSERT_TRUE(locator.has_value());
    const uint8_t sendsId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::Sends);
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *aux, *locator, sendsId,
        QJsonArray{QJsonObject{
            {QStringLiteral("targetBusID"), 201},
            {QStringLiteral("sendLevel"), -6.0}}},
        std::nullopt));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *aux, *locator, sendsId,
        QJsonArray{QJsonObject{
            {QStringLiteral("targetBusID"), 201},
            {QStringLiteral("sendLevel"), -12.0}}},
        HarkenTest::PlatformIds::Android));

    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    const harkenstudio::ObjectId masterId = harkentest::StudioObjectId(fnvOf("MasterBus"));
    const harkenstudio::ObjectId auxId = harkentest::StudioObjectId(fnvOf("AuxBus"));
    VNX::ProjectPackExporter exporter;

    VNX::ProjectPackExporter::ExportOptions windowsOpts;
    windowsOpts.platform = HarkenTest::platformIdString("Windows");
    const QString windowsPath = QDir(dir.path()).filePath(QStringLiteral("Init-Windows.pack"));
    const auto windowsResult = exporter.ExportInitPack(project, windowsPath, windowsOpts);
    ASSERT_TRUE(windowsResult.success) << windowsResult.error.toStdString();

    PackImage windowsImage;
    ASSERT_NO_FATAL_FAILURE(readPack(windowsPath, windowsImage));
    ASSERT_EQ(windowsImage.countKind(harken::kPackEntryKindBusSend), 1);
    const auto* windowsSend = windowsImage.findKind(harken::kPackEntryKindBusSend);
    harken::PackBusSendEntry windowsWire{};
    ASSERT_TRUE(harken::ParsePackV2BusSend(
        windowsImage.data() + windowsSend->offset, windowsSend->size, &windowsWire));
    EXPECT_EQ(windowsWire.fromBusId, auxId);
    EXPECT_EQ(windowsWire.toBusId, masterId);
    EXPECT_FLOAT_EQ(windowsWire.levelDb, -6.0f);

    VNX::ProjectPackExporter::ExportOptions androidOpts;
    androidOpts.platform = HarkenTest::platformIdString("Android");
    const QString androidPath = QDir(dir.path()).filePath(QStringLiteral("Init-Android.pack"));
    const auto androidResult = exporter.ExportInitPack(project, androidPath, androidOpts);
    ASSERT_TRUE(androidResult.success) << androidResult.error.toStdString();

    PackImage androidImage;
    ASSERT_NO_FATAL_FAILURE(readPack(androidPath, androidImage));
    ASSERT_EQ(androidImage.countKind(harken::kPackEntryKindBusSend), 1);
    const auto* androidSend = androidImage.findKind(harken::kPackEntryKindBusSend);
    harken::PackBusSendEntry androidWire{};
    ASSERT_TRUE(harken::ParsePackV2BusSend(
        androidImage.data() + androidSend->offset, androidSend->size, &androidWire));
    EXPECT_EQ(androidWire.fromBusId, auxId);
    EXPECT_EQ(androidWire.toBusId, masterId);
    EXPECT_FLOAT_EQ(androidWire.levelDb, -12.0f);

    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *aux, *locator, sendsId,
        QJsonArray{QJsonObject{
            {QStringLiteral("targetBusID"), 201},
            {QStringLiteral("sendLevel"), 0.0}}},
        HarkenTest::PlatformIds::Android));
    const QString zeroPath = QDir(dir.path()).filePath(QStringLiteral("Init-Android-Zero.pack"));
    const auto zeroResult = exporter.ExportInitPack(project, zeroPath, androidOpts);
    ASSERT_TRUE(zeroResult.success) << zeroResult.error.toStdString();
    PackImage zeroImage;
    ASSERT_NO_FATAL_FAILURE(readPack(zeroPath, zeroImage));
    const auto* zeroSend = zeroImage.findKind(harken::kPackEntryKindBusSend);
    ASSERT_NE(nullptr, zeroSend);
    harken::PackBusSendEntry zeroWire{};
    ASSERT_TRUE(harken::ParsePackV2BusSend(
        zeroImage.data() + zeroSend->offset, zeroSend->size, &zeroWire));
    EXPECT_FLOAT_EQ(0.0f, zeroWire.levelDb);
}

TEST(EditorProjectPackExporterInitPack, UnsupportedBusPropertiesPreserveExistingOutput)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    auto master = makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 201);
    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));
    VNX::ProjectPackExporter exporter;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("existing.pack"));
    const auto exportRejected = [&](const QString& field) {
        QFile existing(path);
        ASSERT_TRUE(existing.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(existing.write("previous-pack"), 13);
        existing.close();
        const auto result = exporter.ExportInitPack(
            project, path, {.platform = HarkenTest::platformIdString("Windows")});
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.error.contains(field)) << result.error.toStdString();
        EXPECT_TRUE(result.error.contains(master->guid.toString(QUuid::WithoutBraces)));
        ASSERT_TRUE(existing.open(QIODevice::ReadOnly));
        EXPECT_EQ(existing.readAll(), QByteArray("previous-pack"));
    };
    master->comp<VNX::VBus>()->pan = 0.25f;
    exportRejected(QStringLiteral("pan"));
    master->comp<VNX::VBus>()->pan = 0.0f;
    master->comp<VNX::VMixNode>()->pitch = 300.0f;
    master->playablePropertyPresence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::Pitch);
    exportRejected(QStringLiteral("pitch"));
    master->comp<VNX::VMixNode>()->pitch = 0.0f;
    master->comp<VNX::VBus>()->listenerRelativeRouting = true;
    exportRejected(QStringLiteral("listenerRelativeRouting"));
}

TEST(EditorProjectPackExporterInitPack, NeutralPitchPlatformOverrideCanPublish)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    auto master = makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 201);
    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));
    master->comp<VNX::VMixNode>()->pitch = 300.0f;
    master->playablePropertyPresence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::Pitch);
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*master);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator,
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Pitch),
        QJsonValue(0.0), HarkenTest::PlatformIds::Windows));
    VNX::ProjectPackExporter exporter;
    const QString windowsPath = QDir(dir.path()).filePath(QStringLiteral("windows.pack"));
    const auto windows = exporter.ExportInitPack(
        project, windowsPath, {.platform = HarkenTest::platformIdString("Windows")});
    EXPECT_TRUE(windows.success) << windows.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(windowsPath));
    const QString androidPath = QDir(dir.path()).filePath(QStringLiteral("android.pack"));
    const auto android = exporter.ExportInitPack(
        project, androidPath, {.platform = HarkenTest::platformIdString("Android")});
    EXPECT_FALSE(android.success);
    EXPECT_TRUE(android.error.contains(QStringLiteral("pitch")));
    EXPECT_FALSE(QFileInfo::exists(androidPath));
}

TEST(EditorProjectPackExporterInitPack, BusMixFieldsFollowExportPlatform)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    project.name = QStringLiteral("init-platform-volume");
    project.projectPath = QDir(dir.path()).filePath(QStringLiteral("init-platform-volume.vproj"));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});

    auto master = makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 201);
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*master);
    ASSERT_TRUE(locator.has_value());
    const uint8_t volumeId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::Volume);
    const uint8_t outputBusVolumeId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
    const uint8_t makeUpGainId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain);
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator, volumeId, QJsonValue(-6.0), std::nullopt));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator, volumeId, QJsonValue(-12.0), HarkenTest::PlatformIds::Android));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator, outputBusVolumeId, QJsonValue(-3.5), std::nullopt));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator, makeUpGainId, QJsonValue(2.0), std::nullopt));

    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    const harkenstudio::ObjectId masterId = harkentest::StudioObjectId(fnvOf("MasterBus"));
    VNX::ProjectPackExporter exporter;

    const auto readBusMix = [&](const QString& path, harken::PackBusEntry* out) {
        PackImage image;
        ASSERT_NO_FATAL_FAILURE(readPack(path, image));
        ASSERT_EQ(image.countKind(harken::kPackEntryKindBus), 1);
        const auto* entry = image.findKind(harken::kPackEntryKindBus);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->vintage, harken::kPackV2BusMixSchemaVintage);
        EXPECT_EQ(entry->size, harken::kPackV6BusPayloadBytes);
        ASSERT_TRUE(harken::ParsePackV6Bus(image.data() + entry->offset, entry->size, out));
        EXPECT_EQ(out->busId, masterId);
    };

    VNX::ProjectPackExporter::ExportOptions windowsOpts;
    windowsOpts.platform = HarkenTest::platformIdString("Windows");
    const QString windowsPath = QDir(dir.path()).filePath(QStringLiteral("Init-Windows.pack"));
    const auto windowsResult = exporter.ExportInitPack(project, windowsPath, windowsOpts);
    ASSERT_TRUE(windowsResult.success) << windowsResult.error.toStdString();
    EXPECT_FALSE(windowsResult.warnings.join(QStringLiteral("\n"))
                     .contains(QStringLiteral("no pack representation")))
        << windowsResult.warnings.join('\n').toStdString();
    harken::PackBusEntry windowsWire{};
    ASSERT_NO_FATAL_FAILURE(readBusMix(windowsPath, &windowsWire));
    EXPECT_FLOAT_EQ(windowsWire.volume, -6.0f);
    EXPECT_FLOAT_EQ(windowsWire.outputBusVolume, -3.5f);
    EXPECT_FLOAT_EQ(windowsWire.makeUpGain, 2.0f);

    VNX::ProjectPackExporter::ExportOptions androidOpts;
    androidOpts.platform = HarkenTest::platformIdString("Android");
    const QString androidPath = QDir(dir.path()).filePath(QStringLiteral("Init-Android.pack"));
    const auto androidResult = exporter.ExportInitPack(project, androidPath, androidOpts);
    ASSERT_TRUE(androidResult.success) << androidResult.error.toStdString();
    EXPECT_FALSE(androidResult.warnings.join(QStringLiteral("\n"))
                     .contains(QStringLiteral("no pack representation")))
        << androidResult.warnings.join('\n').toStdString();
    harken::PackBusEntry androidWire{};
    ASSERT_NO_FATAL_FAILURE(readBusMix(androidPath, &androidWire));
    EXPECT_FLOAT_EQ(androidWire.volume, -12.0f);
    EXPECT_FLOAT_EQ(androidWire.outputBusVolume, -3.5f);
    EXPECT_FLOAT_EQ(androidWire.makeUpGain, 2.0f);
}

TEST(EditorProjectPackExporterInitPack, BusOutputVolumeUsesTargetPlatformOverrideIncludingZero)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    project.name = QStringLiteral("init-output-mixnode");
    project.projectPath = QDir(dir.path()).filePath(QStringLiteral("init-output-mixnode.vproj"));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});

    auto master = makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 301);
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*master);
    ASSERT_TRUE(locator.has_value());
    auto* mix = master->comp<VNX::VMixNode>();
    ASSERT_NE(mix, nullptr);
    mix->outputBusVolume = -6.0f;
    const uint8_t outputBusVolumeId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator, outputBusVolumeId, QJsonValue(-12.0),
        HarkenTest::PlatformIds::Android));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator, outputBusVolumeId, QJsonValue(0.0),
        HarkenTest::PlatformIds::Windows));
    EXPECT_FLOAT_EQ(mix->outputBusVolume, -6.0f);

    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    const harkenstudio::ObjectId masterId = master->runtimeId();
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions androidOpts;
    androidOpts.platform = HarkenTest::platformIdString("Android");
    const QString androidPath = QDir(dir.path()).filePath(QStringLiteral("Init-Android.pack"));
    const auto androidResult = exporter.ExportInitPack(project, androidPath, androidOpts);
    ASSERT_TRUE(androidResult.success) << androidResult.error.toStdString();

    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(androidPath, image));
    ASSERT_EQ(image.countKind(harken::kPackEntryKindBus), 1);
    const auto* entry = image.findKind(harken::kPackEntryKindBus);
    ASSERT_NE(entry, nullptr);
    harken::PackBusEntry wire{};
    ASSERT_TRUE(harken::ParsePackV6Bus(image.data() + entry->offset, entry->size, &wire));
    EXPECT_EQ(wire.busId, masterId);
    EXPECT_FLOAT_EQ(wire.outputBusVolume, -12.0f);

    VNX::ProjectPackExporter::ExportOptions windowsOpts;
    windowsOpts.platform = HarkenTest::platformIdString("Windows");
    const QString windowsPath = QDir(dir.path()).filePath(QStringLiteral("Init-Windows.pack"));
    const auto windowsResult = exporter.ExportInitPack(project, windowsPath, windowsOpts);
    ASSERT_TRUE(windowsResult.success) << windowsResult.error.toStdString();
    PackImage windowsImage;
    ASSERT_NO_FATAL_FAILURE(readPack(windowsPath, windowsImage));
    const auto* windowsEntry = windowsImage.findKind(harken::kPackEntryKindBus);
    ASSERT_NE(windowsEntry, nullptr);
    harken::PackBusEntry windowsWire{};
    ASSERT_TRUE(harken::ParsePackV6Bus(windowsImage.data() + windowsEntry->offset,
        windowsEntry->size, &windowsWire));
    EXPECT_EQ(windowsWire.busId, masterId);
    EXPECT_FLOAT_EQ(windowsWire.outputBusVolume, 0.0f);
}

TEST(EditorProjectPackExporterInitPack, MuteDoesNotChangePackedBusMix)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    project.name = QStringLiteral("init-mute-mix");
    project.projectPath = QDir(dir.path()).filePath(QStringLiteral("init-mute-mix.vproj"));

    auto master = makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 201);
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*master);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator,
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Volume),
        QJsonValue(-6.0), std::nullopt));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator,
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume),
        QJsonValue(-3.5), std::nullopt));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator,
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain),
        QJsonValue(2.0), std::nullopt));

    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    const harkenstudio::ObjectId masterId = harkentest::StudioObjectId(fnvOf("MasterBus"));
    VNX::ProjectPackExporter exporter;
    const auto readBusMix = [&](const QString& path, harken::PackBusEntry* out) {
        PackImage image;
        ASSERT_NO_FATAL_FAILURE(readPack(path, image));
        ASSERT_EQ(image.countKind(harken::kPackEntryKindBus), 1);
        const auto* entry = image.findKind(harken::kPackEntryKindBus);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->vintage, harken::kPackV2BusMixSchemaVintage);
        EXPECT_EQ(entry->size, harken::kPackV6BusPayloadBytes);
        ASSERT_TRUE(harken::ParsePackV6Bus(image.data() + entry->offset, entry->size, out));
        EXPECT_EQ(out->busId, masterId);
    };

    const QString unmutedPath =
        QDir(dir.path()).filePath(QStringLiteral("Init-unmuted.pack"));
    const auto unmuted = exporter.ExportInitPack(project, unmutedPath, {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(unmuted.success) << unmuted.error.toStdString();
    harken::PackBusEntry unmutedWire{};
    ASSERT_NO_FATAL_FAILURE(readBusMix(unmutedPath, &unmutedWire));

    struct OverlayGuard {
        VNX::AuditionOverlay& overlay;
        ~OverlayGuard() { overlay.clearAll(); }
    } guard{VNX::ProjectManager::instance().auditionOverlay()};
    guard.overlay.clearAll();
    guard.overlay.setMuted(harkentest::StudioObjectId(201), true);
    guard.overlay.setSoloed(harkentest::StudioObjectId(201), true);

    const QString mutedPath =
        QDir(dir.path()).filePath(QStringLiteral("Init-muted.pack"));
    const auto muted = exporter.ExportInitPack(project, mutedPath, {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(muted.success) << muted.error.toStdString();
    EXPECT_FALSE(muted.warnings.join(QStringLiteral("\n"))
                     .contains(QStringLiteral("no pack representation")))
        << muted.warnings.join('\n').toStdString();
    harken::PackBusEntry mutedWire{};
    ASSERT_NO_FATAL_FAILURE(readBusMix(mutedPath, &mutedWire));
    EXPECT_FLOAT_EQ(mutedWire.volume, unmutedWire.volume);
    EXPECT_FLOAT_EQ(mutedWire.outputBusVolume, unmutedWire.outputBusVolume);
    EXPECT_FLOAT_EQ(mutedWire.makeUpGain, unmutedWire.makeUpGain);
    EXPECT_FLOAT_EQ(mutedWire.volume, -6.0f);
    EXPECT_FLOAT_EQ(mutedWire.outputBusVolume, -3.5f);
    EXPECT_FLOAT_EQ(mutedWire.makeUpGain, 2.0f);
}

// The frozen cross-side golden table (AttenuationCurveEvalTest.cpp editor
// eval, HarkenEngine/Test/E2E/PackLogicRoundtripTest.cpp runtime eval) goes
// through ExportInitPack and real packc here: the export leg must be an
// identity, handles reach the wire component-wise float-equal with zero
// warnings.
TEST(EditorProjectPackExporterInitPack, GoldenBezierHandlesReachWireVerbatim)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    struct Golden {
        const char* name;
        VNX::CurveHandlePairDef handles;
    };
    const Golden kGoldens[] = {
        {"GoldenEndpointLine",   {0.0f, 0.0f, 100.0f, -96.0f}},
        {"GoldenStandardBend",   {25.0f, 0.0f, 75.0f, -96.0f}},
        {"GoldenFlatDerivative", {100.0f, 0.0f, 0.0f, -96.0f}},
    };

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    project.name = QStringLiteral("bezier-golden-e2e");
    project.projectPath = QDir(dir.path()).filePath(QStringLiteral("golden.vproj"));

    for (size_t i = 0; i < std::size(kGoldens); ++i)
    {
        auto item = makeItem(QString::fromLatin1(kGoldens[i].name),
                             VNX::ProjectItem::Type::Attenuation,
                             static_cast<uint32_t>(302 + i));
        auto* attenuation = item->comp<VNX::VAttenuation>();
        attenuation->maxDistance = 100.0f;
        auto& volume =
            attenuation->dimensions.emplaceBack(VNX::AttenuationDimensionDef{VNX::AttenuationDimension::Volume});
        volume.curve.push_back({0.0f, 0.0f});
        volume.curve.push_back({100.0f, -96.0f});
        volume.handles.push_back(kGoldens[i].handles);
        VNX::ProjectFileBinding binding;
        binding.rootItem = item;
        project.attenuationItems.push_back(std::move(binding));
    }

    // Pitch stays an editor-only dimension: authored but never exported,
    // never warned about.
    auto* first = project.attenuationItems.front().rootItem
                      ->comp<VNX::VAttenuation>();
    auto& pitch = first->dimensions.emplaceBack(VNX::AttenuationDimensionDef{VNX::AttenuationDimension::Pitch});
    pitch.curve.push_back({0.0f, 0.0f});
    pitch.curve.push_back({100.0f, 1200.0f});
    pitch.handles.push_back({25.0f, 0.0f, 75.0f, 1200.0f});

    VNX::ProjectPackExporter exporter;
    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("golden.pack"));
    const auto result = exporter.ExportInitPack(project, packPath, {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_TRUE(result.warnings.isEmpty())
        << result.warnings.join('\n').toStdString();

    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
    ASSERT_EQ(image.countKind(harken::kPackEntryKindAttenuation), 3);

    int matched = 0;
    for (const auto& entry : image.entries)
    {
        if (entry.kind != harken::kPackEntryKindAttenuation)
            continue;
        for (size_t i = 0; i < std::size(kGoldens); ++i)
        {
            if (entry.id != harkentest::StudioObjectId(fnvOf(kGoldens[i].name)))
                continue;
            VnAttenuationDesc wire{};
            ASSERT_TRUE(harken::ParsePackV3Attenuation(
                image.data() + entry.offset, entry.size, &wire));
            ASSERT_EQ(wire.volumeCurve.pointCount, 2u) << kGoldens[i].name;
            EXPECT_FLOAT_EQ(wire.volumeCurve.points[0].x, 0.0f);
            EXPECT_FLOAT_EQ(wire.volumeCurve.points[0].y, 0.0f);
            EXPECT_FLOAT_EQ(wire.volumeCurve.points[1].x, 100.0f);
            EXPECT_FLOAT_EQ(wire.volumeCurve.points[1].y, -96.0f);
            EXPECT_FLOAT_EQ(wire.volumeCurve.handles[0].h1x,
                            kGoldens[i].handles.h1x) << kGoldens[i].name;
            EXPECT_FLOAT_EQ(wire.volumeCurve.handles[0].h1y,
                            kGoldens[i].handles.h1y) << kGoldens[i].name;
            EXPECT_FLOAT_EQ(wire.volumeCurve.handles[0].h2x,
                            kGoldens[i].handles.h2x) << kGoldens[i].name;
            EXPECT_FLOAT_EQ(wire.volumeCurve.handles[0].h2y,
                            kGoldens[i].handles.h2y) << kGoldens[i].name;
            ++matched;
        }
    }
    EXPECT_EQ(matched, 3);
}

TEST(EditorPackBuildSessionInitSections, InvalidInnerCurvePointDropsWholeCurve)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("invalid.pack"));

    Bridge::PackBuildSession builder;
    builder.setInitPack(true);
    Bridge::PackAttenuation attenuation;
    attenuation.id = harkentest::StudioObjectId(1003);
    attenuation.maxDistance = 20.0f;
    Bridge::PackAttenuation::Slot volume;
    volume.slot = 0;
    volume.points.push_back({0.0f, 0.0f});
    volume.points.push_back(
        {10.0f, std::numeric_limits<float>::quiet_NaN()});
    volume.points.push_back({20.0f, -12.0f});
    volume.handles.push_back({0.0f, 0.0f, 10.0f, 0.0f});
    volume.handles.push_back({10.0f, 0.0f, 20.0f, -12.0f});
    attenuation.curveSlots.push_back(volume);
    builder.addAttenuation(attenuation);

    ASSERT_TRUE(builder.buildToFile(packPath)) << builder.lastError().toStdString();
    bool sawInvalidPoint = false;
    for (const QString& warning : builder.warnings())
        sawInvalidPoint = sawInvalidPoint ||
            warning.contains(QStringLiteral("invalid point"));
    EXPECT_TRUE(sawInvalidPoint) << builder.warnings().join('\n').toStdString();

    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
    const auto* entry = image.findKind(harken::kPackEntryKindAttenuation);
    ASSERT_NE(entry, nullptr);
    VnAttenuationDesc wire{};
    ASSERT_TRUE(harken::ParsePackV3Attenuation(
        image.data() + entry->offset, entry->size, &wire));
    EXPECT_EQ(wire.volumeCurve.pointCount, 0u);
}

TEST(PackagingBridge, PitchTransitionMsSurvivesExport)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString packPath =
        QDir(dir.path()).filePath(QStringLiteral("pitch-ramp.pack"));

    Bridge::PackBuildSession builder;
    Bridge::PackParameterBinding volume;
    volume.id = kBindingId;
    volume.variableId = harkentest::StudioObjectId(kVariableId);
    volume.scope = 0;
    volume.target = VN_TARGET_VOICE_VOLUME;
    volume.transitionMs = 50;
    volume.points.push_back({0.0f, 0.0f});
    volume.points.push_back({1.0f, -6.0f});
    volume.handles.push_back({0.0f, 0.0f, 1.0f, -6.0f});
    builder.addBinding(volume);
    Bridge::PackParameterBinding pitch;
    pitch.id = kVoiceBindingId;
    pitch.variableId = harkentest::StudioObjectId(kVariableId);
    pitch.scope = 0;
    pitch.target = VN_TARGET_VOICE_PITCH;
    pitch.transitionMs = 20;
    pitch.points.push_back({0.0f, 0.0f});
    pitch.points.push_back({1.0f, 1200.0f});
    pitch.handles.push_back({0.0f, 0.0f, 1.0f, 1200.0f});
    builder.addBinding(pitch);
    Bridge::PackBindingAttachment volumeAttachment;
    volumeAttachment.bindingId = kBindingId;
    volumeAttachment.scope = 0;
    volumeAttachment.objectId = harkentest::StudioObjectId(kVoiceObjectId);
    builder.addBindingAttachment(volumeAttachment);
    Bridge::PackBindingAttachment pitchAttachment;
    pitchAttachment.bindingId = kVoiceBindingId;
    pitchAttachment.scope = 0;
    pitchAttachment.objectId = harkentest::StudioObjectId(kVoiceObjectId);
    builder.addBindingAttachment(pitchAttachment);

    ASSERT_TRUE(builder.buildToFile(packPath)) << builder.lastError().toStdString();
    for (const QString& warning : builder.warnings()) {
        EXPECT_FALSE(warning.contains(QStringLiteral("transitionMs was dropped")))
            << warning.toStdString();
    }

    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
    ASSERT_EQ(image.countKind(harken::kPackEntryKindBindingDefinition), 2);
    bool sawVolume = false;
    bool sawPitch = false;
    for (const auto& entry : image.entries) {
        if (entry.kind != harken::kPackEntryKindBindingDefinition)
            continue;
        VnBindingId bindingId{};
        VnBindingDesc bindingWire{};
        ASSERT_TRUE(harken::ParsePackV2BindingDefinition(
            image.data() + entry.offset, entry.size, &bindingId, &bindingWire));
        if (bindingId == kBindingId) {
            sawVolume = true;
            EXPECT_EQ(bindingWire.targetId,
                      static_cast<uint32_t>(VN_TARGET_VOICE_VOLUME));
            EXPECT_EQ(bindingWire.transitionMs, 50u);
        }
        if (bindingId == kVoiceBindingId) {
            sawPitch = true;
            EXPECT_EQ(bindingWire.targetId,
                      static_cast<uint32_t>(VN_TARGET_VOICE_PITCH));
            EXPECT_EQ(bindingWire.transitionMs, 20u);
        }
    }
    EXPECT_TRUE(sawVolume);
    EXPECT_TRUE(sawPitch);
}

TEST(PackagingBridge, BusVolumeTransitionMsSurvivesExport)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString packPath =
        QDir(dir.path()).filePath(QStringLiteral("bus-ramp.pack"));

    Bridge::PackBuildSession builder;
    builder.setInitPack(true);
    Bridge::PackBus bus;
    bus.id = harkentest::StudioObjectId(kBusId);
    bus.parentId = harkentest::StudioObjectId(kBusId);
    bus.name = QStringLiteral("Master");
    builder.addBus(bus);
    Bridge::PackParameterBinding volume;
    volume.id = kBindingId;
    volume.variableId = harkentest::StudioObjectId(kVariableId);
    volume.scope = 1;
    volume.target = 0;
    volume.transitionMs = 20;
    volume.points.push_back({0.0f, 0.0f});
    volume.points.push_back({1.0f, -6.0f});
    volume.handles.push_back({0.0f, 0.0f, 1.0f, -6.0f});
    builder.addBinding(volume);
    Bridge::PackBindingAttachment attachment;
    attachment.bindingId = kBindingId;
    attachment.scope = 1;
    attachment.objectId = harkentest::StudioObjectId(kBusId);
    builder.addBindingAttachment(attachment);

    ASSERT_TRUE(builder.buildToFile(packPath)) << builder.lastError().toStdString();
    for (const QString& warning : builder.warnings()) {
        EXPECT_FALSE(warning.contains(QStringLiteral("transitionMs was dropped")))
            << warning.toStdString();
    }

    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
    ASSERT_EQ(image.countKind(harken::kPackEntryKindBindingDefinition), 1);
    const auto* entry = image.findKind(harken::kPackEntryKindBindingDefinition);
    VnBindingId bindingId{};
    VnBindingDesc bindingWire{};
    ASSERT_TRUE(harken::ParsePackV2BindingDefinition(
        image.data() + entry->offset, entry->size, &bindingId, &bindingWire));
    EXPECT_EQ(bindingId, kBindingId);
    EXPECT_EQ(bindingWire.targetScopeKind, 1u);
    EXPECT_EQ(bindingWire.targetId, 0u);
    EXPECT_EQ(bindingWire.transitionMs, 20u);
}

TEST(PackagingBridge, UnsupportedNonzeroTransitionFailsExport)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString packPath =
        QDir(dir.path()).filePath(QStringLiteral("pan-ramp.pack"));

    Bridge::PackBuildSession builder;
    Bridge::PackParameterBinding pan;
    pan.id = kBindingId;
    pan.variableId = harkentest::StudioObjectId(kVariableId);
    pan.scope = 0;
    pan.target = VN_TARGET_VOICE_PAN;
    pan.transitionMs = 20;
    pan.points.push_back({0.0f, -1.0f});
    pan.points.push_back({1.0f, 1.0f});
    pan.handles.push_back({0.0f, -1.0f, 1.0f, 1.0f});
    builder.addBinding(pan);
    Bridge::PackBindingAttachment attachment;
    attachment.bindingId = kBindingId;
    attachment.scope = 0;
    attachment.objectId = harkentest::StudioObjectId(kVoiceObjectId);
    builder.addBindingAttachment(attachment);

    EXPECT_FALSE(builder.buildToFile(packPath));
    EXPECT_TRUE(builder.lastError().contains(QStringLiteral("does not support a ramp")))
        << builder.lastError().toStdString();
    EXPECT_FALSE(QFileInfo::exists(packPath));
}

TEST(ProjectPackExporter, IllegalVoiceVolumeTransitionMsFailsWholeExport)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    const QJsonValue illegals[] = {
        QJsonValue(-1),
        QJsonValue(20.5),
        QJsonValue(QStringLiteral("20")),
        QJsonValue(60001),
    };
    for (const QJsonValue& illegal : illegals) {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        auto project = HarkenTest::projectWithPlatforms({"Windows"});
        project.projectPath = QDir(dir.path()).filePath(QStringLiteral("illegal.vproj"));

        auto master = makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 201);
        VNX::ProjectFileBinding busBinding;
        busBinding.rootItem = master;
        project.busses.push_back(std::move(busBinding));

        auto sound = makeItem(QStringLiteral("Tone"), VNX::ProjectItem::Type::Sound, 11);
        VNX::ProjectFileBinding layerBinding;
        layerBinding.rootItem = sound;
        project.layers.push_back(std::move(layerBinding));

        auto variable = makeItem(QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable, 31);
        auto* variableComp = variable->comp<VNX::VVariable>();
        ASSERT_NE(variableComp, nullptr);
        variableComp->kind = VNX::VariableKind::Smooth;
        variableComp->rangeMin = 0.0f;
        variableComp->rangeMax = 10.0f;
        VNX::ProjectFileBinding variableBinding;
        variableBinding.rootItem = variable;
        project.variables.push_back(std::move(variableBinding));

        auto legal = VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::pitchAddress(), variable->guid);
        legal.guid = VNX::authoringUuid(harkentest::StudioObjectId(802));
        auto volume = VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::volumeAddress(), variable->guid);
        volume.guid = VNX::authoringUuid(harkentest::StudioObjectId(801));
        QJsonObject obj = volume.toJson().toObject();
        obj.insert(QStringLiteral("transitionMs"), illegal);
        const auto loaded = VNX::ParameterBindingDef::fromJson(obj);
        EXPECT_FALSE(loaded.typed);
        ASSERT_TRUE(sound->comp<VNX::VMixNode>()->bindings.replace({loaded, legal}));

        VNX::ProjectPackExporter exporter;
        const QString packPath = QDir(dir.path()).filePath(QStringLiteral("Init.pack"));
        const auto result = exporter.ExportInitPack(
            project, packPath, {.platform = HarkenTest::platformIdString("Windows")});
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.error.contains(QStringLiteral("transitionMs")))
            << result.error.toStdString();
        EXPECT_FALSE(QFileInfo::exists(packPath));
    }
}

TEST(ProjectPackExporter, MissingKeyAndExplicitZeroTransitionMsStillExport)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    project.projectPath = QDir(dir.path()).filePath(QStringLiteral("legal.vproj"));

    auto master = makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 201);
    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    auto sound = makeItem(QStringLiteral("Tone"), VNX::ProjectItem::Type::Sound, 11);
    VNX::ProjectFileBinding layerBinding;
    layerBinding.rootItem = sound;
    project.layers.push_back(std::move(layerBinding));

    auto variable = makeItem(QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable, 31);
    auto* variableComp = variable->comp<VNX::VVariable>();
    ASSERT_NE(variableComp, nullptr);
    variableComp->kind = VNX::VariableKind::Smooth;
    variableComp->rangeMin = 0.0f;
    variableComp->rangeMax = 10.0f;
    VNX::ProjectFileBinding variableBinding;
    variableBinding.rootItem = variable;
    project.variables.push_back(std::move(variableBinding));

    auto missing = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), variable->guid);
    missing.guid = VNX::authoringUuid(harkentest::StudioObjectId(801));
    QJsonObject missingObj = missing.toJson().toObject();
    missingObj.remove(QStringLiteral("transitionMs"));
    const auto missingLoaded = VNX::ParameterBindingDef::fromJson(missingObj);
    EXPECT_TRUE(missingLoaded.typed);
    EXPECT_EQ(missingLoaded.transitionMs, 20u);
    auto zero = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), variable->guid);
    zero.guid = VNX::authoringUuid(harkentest::StudioObjectId(802));
    zero.transitionMs = 0;
    ASSERT_TRUE(sound->comp<VNX::VMixNode>()->bindings.replace({missingLoaded, zero}));

    VNX::ProjectPackExporter exporter;
    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("Init.pack"));
    const auto result = exporter.ExportInitPack(
        project, packPath, {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(packPath));
}

TEST(PackagingBridge, TypedTransitionRangeRejectsBeforeCurveSkipWithoutChangingOutput)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto path = QDir(dir.path()).filePath(QStringLiteral("binding.pack"));
    for (uint32_t ms : {60001u, UINT32_MAX}) {
        for (bool incomplete : {false, true}) {
            QFile existing(path);
            ASSERT_TRUE(existing.open(QIODevice::WriteOnly));
            ASSERT_EQ(existing.write("previous-pack"), 13);
            existing.close();
            Bridge::PackBuildSession builder;
            Bridge::PackParameterBinding binding;
            binding.id = kBindingId;
            binding.variableId = harkentest::StudioObjectId(kVariableId);
            binding.scope = 0;
            binding.target = VN_TARGET_VOICE_VOLUME;
            binding.transitionMs = ms;
            if (!incomplete) {
                binding.points = {{0.0f, 0.0f}, {1.0f, -6.0f}};
                binding.handles = {{0.0f, 0.0f, 1.0f, -6.0f}};
            }
            builder.addBinding(binding);
            EXPECT_FALSE(builder.buildToFile(path));
            EXPECT_TRUE(builder.lastError().contains(QStringLiteral("transitionMs")));
            ASSERT_TRUE(existing.open(QIODevice::ReadOnly));
            EXPECT_EQ(existing.readAll(), QByteArray("previous-pack"));
        }
    }
}

TEST(PackagingBridge, ScopedTargetMetadataPreservesAllSupportedTransitions)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (const auto& meta : harken::kTargetMetaTable) {
        const auto path = QDir(dir.path()).filePath(
            QStringLiteral("binding-%1-%2.pack").arg(meta.scopeKind).arg(meta.targetId));
        Bridge::PackBuildSession builder;
        builder.setInitPack(meta.scopeKind == harken::kTargetScopeBus);
        Bridge::PackParameterBinding binding;
        binding.id = kBindingId;
        binding.variableId = harkentest::StudioObjectId(kVariableId);
        binding.scope = static_cast<uint8_t>(meta.scopeKind);
        binding.target = meta.targetId;
        binding.transitionMs = 60000;
        binding.points = {{0.0f, 0.0f}, {1.0f, 0.0f}};
        binding.handles = {{0.0f, 0.0f, 1.0f, 0.0f}};
        builder.addBinding(binding);
        if (!meta.supportsTransition) {
            EXPECT_FALSE(builder.buildToFile(path));
            EXPECT_TRUE(builder.lastError().contains(QStringLiteral("does not support a ramp")));
            continue;
        }
        ASSERT_TRUE(builder.buildToFile(path)) << builder.lastError().toStdString();
        PackImage image;
        ASSERT_NO_FATAL_FAILURE(readPack(path, image));
        ASSERT_EQ(image.countKind(harken::kPackEntryKindBindingDefinition), 1);
        const auto* entry = image.findKind(harken::kPackEntryKindBindingDefinition);
        VnBindingId id{};
        VnBindingDesc desc{};
        ASSERT_TRUE(harken::ParsePackV2BindingDefinition(
            image.data() + entry->offset, entry->size, &id, &desc));
        EXPECT_EQ(desc.targetScopeKind, meta.scopeKind);
        EXPECT_EQ(desc.targetId, meta.targetId);
        EXPECT_EQ(desc.transitionMs, 60000u);
    }
}

TEST(EditorProjectPackExporterInitPack, ValidateSnapshotBusIdsRejectsHighWord)
{
    EXPECT_TRUE(VNX::ValidateSnapshotBusIds(harkenstudio::kMasterBusObjectId, {}, {}, {},
                                            harkenstudio::kMasterBusObjectId,
                                            harkenstudio::kMasterBusObjectId));
    const harkenstudio::ObjectId high{1, 1};
    EXPECT_FALSE(VNX::ValidateSnapshotBusIds(high, {}, {}, {}, {}, {}));
    EXPECT_FALSE(VNX::ValidateSnapshotBusIds({}, high, {}, {}, {}, {}));
    EXPECT_FALSE(VNX::ValidateSnapshotBusIds({}, {}, high, {}, {}, {}));
    EXPECT_FALSE(VNX::ValidateSnapshotBusIds({}, {}, {}, high, {}, {}));
    EXPECT_FALSE(VNX::ValidateSnapshotBusIds({}, {}, {}, {}, high, {}));
    EXPECT_FALSE(VNX::ValidateSnapshotBusIds({}, {}, {}, {}, {}, high));
}
