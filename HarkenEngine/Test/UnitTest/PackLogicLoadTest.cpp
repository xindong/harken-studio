// Engine-level v2 pack LOGIC-ENTRY load/unload behavior: the staged,
// all-or-nothing load
// through Vn_LoadPack's format-version branch and the symmetric unload
// through Vn_UnloadPack. Legal pack cases are compiled by the real
// packc tool from v2 manifests (subprocess, the pipeline-test
// precedent); cases the compiler refuses are hand-assembled by a
// test-local byte builder over the v2 serializers so the loader's
// defense-in-depth half is exercised on real files. Covers the two
// happy paths (resident infrastructure, content cargo), cross-pack
// closure with the provider dependency it records (a binding
// definition resolved across packs included — and the caller-recycled
// definition id whose fresh imprint records none), kind discipline at
// both ends of the toolchain, the compiler's Action-name uniqueness
// refusal beside the loader's own duplicate-name gate on a
// byte-patched pack, compiler byte identity across cache-hit
// and clean rebuilds, corruption refusals, staging atomicity under
// every reachable refusal class (id conflict, pool capacity,
// binding capacity, command-queue admission), the widest indivisible batch the
// shipped per-block ceiling admits together with the isolated cost of the batch
// header opcode, the aggregate capacity
// arithmetic (every entry admissible alone, the sum refused), the
// injected staging faults proving reservations release, the unload
// refusal matrix
// (resident, provider-in-use, playing media, a foreign attachment left
// by a caller detach-swap on a pack definition, and a caller rebirth of
// the pack's own triple — detach then identical re-attach — which only
// the ownership imprint tells apart from the pack's original), content
// unload symmetry, the caller-recycled container and Action ids
// (unregister then same-id re-register) whose registry-entry imprint
// keeps the unload from gating on them or sweeping them out,
// and the staged v2 replace's activation timing (staged at the call,
// swapped at the next Vn_Update). Every refusal asserts zero observable
// mutation.
#include "Common/ObjectIdTestSupport.h"
#include "UnitTest/CanonicalPackSetActivate.h"
#include "Common/RecipePackTestUtil.h"
#include "Common/TestUtil.h"

#include "PackLogicStore.h"

#include "JsonReader.h"

#include "harken/core/LogRing.h"
#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"
#include "harken/media/PcmRegistry.h"
#include "harken/param/BusTargetStore.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/param/MeterProbe.h"
#include "harken/param/ParamTargets.h"
#include "harken/param/VariableService.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/BusTree.h"

#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef HARKEN_PACKC_PATH
#error "HARKEN_PACKC_PATH must point at the packc executable"
#endif
#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using harkentest::EngineSession;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

VnConfig PackLogicConfig(std::uint32_t maxVoices = 0,
                         std::uint32_t blockFrames = 0,
                         std::uint32_t sampleRate = 0) {
    auto config = harkentest::MakeNullConfig(maxVoices, blockFrames, sampleRate);
    // Pack lifecycle coverage does not require the default nested Action scratch.
    config.maxTriggerDepth = 2;
    return config;
}

std::string TempPath(const std::string& name) {
    return std::string(HARKEN_TEMP_DIR "/") + name;
}

void WriteTextFile(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "cannot open " << path;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    ASSERT_TRUE(out.good()) << "cannot write " << path;
}

void WriteFileBytes(const std::string& path,
                    const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "cannot open " << path;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good()) << "cannot write " << path;
}

bool ReadFileText(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    return true;
}

// Runs packc with the given argument list and returns the process exit
// code (the pipeline test's quoting rules for cmd.exe).
int RunPackc(const std::vector<std::string>& args) {
    std::string exe = HARKEN_PACKC_PATH;
    for (char& c : exe) {
        if (c == '/') {
            c = '\\';
        }
    }
    std::string command = "\"\"" + exe + "\"";
    for (const std::string& arg : args) {
        command += " \"" + arg + "\"";
    }
    command += "\"";
    return std::system(command.c_str());
}

// Minimal IEEE float32 RIFF/WAVE writer (the pipeline test's shape):
// samples travel verbatim, so the pack blob carries the exact floats
// the test synthesized.
void WriteWavF32(const std::string& path,
                 const std::vector<float>& interleaved,
                 std::uint32_t channels, std::uint32_t sampleRate) {
    ASSERT_NE(0u, channels);
    ASSERT_EQ(0u, interleaved.size() % channels);
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "cannot open " << path;

    const std::uint32_t bytesPerSample = 4;
    const std::uint16_t blockAlign =
        static_cast<std::uint16_t>(channels * bytesPerSample);
    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(interleaved.size() * bytesPerSample);
    const std::uint32_t byteRate = sampleRate * blockAlign;
    const std::uint32_t fmtSize = 18;
    const std::uint32_t factSize = 4;
    const std::uint32_t riffSize =
        4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);
    const std::uint16_t formatIeeeFloat = 3;
    const std::uint16_t bitsPerSample = 32;
    const std::uint16_t cbSize = 0;
    const std::uint16_t channels16 = static_cast<std::uint16_t>(channels);
    const std::uint32_t sampleFrames =
        static_cast<std::uint32_t>(interleaved.size() / channels);

    auto write = [&](const void* data, std::size_t bytes) {
        out.write(static_cast<const char*>(data),
                  static_cast<std::streamsize>(bytes));
    };
    write("RIFF", 4);
    write(&riffSize, 4);
    write("WAVE", 4);
    write("fmt ", 4);
    write(&fmtSize, 4);
    write(&formatIeeeFloat, 2);
    write(&channels16, 2);
    write(&sampleRate, 4);
    write(&byteRate, 4);
    write(&blockAlign, 2);
    write(&bitsPerSample, 2);
    write(&cbSize, 2);
    write("fact", 4);
    write(&factSize, 4);
    write(&sampleFrames, 4);
    write("data", 4);
    write(&dataBytes, 4);
    write(interleaved.data(), interleaved.size() * sizeof(float));
    ASSERT_TRUE(out.good()) << "cannot write " << path;
}

// Writes the manifest and runs packc; false when the build refused.
[[nodiscard]] bool BuildPackFile(const std::string& name,
                                 const std::string& manifestJson) {
    const std::string manifestPath = TempPath(name + "_manifest.json");
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    {
        std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return false;
        }
        out.write(manifestJson.data(),
                  static_cast<std::streamsize>(manifestJson.size()));
        if (!out.good()) {
            return false;
        }
    }
    return RunPackc({manifestPath}) == 0;
}

harkentest::RecipePackArtifact BuildDelayAndToneRecipePack(
    const std::string& name) {
    const std::string unique =
        name + "_" + std::to_string(_getpid());
    const std::string wavPath = TempPath(unique + ".wav");
    const std::string packPath = TempPath(unique + ".pack");
    harkentest::RecipePackArtifact artifact;
    artifact.path = packPath;
    if (!harkentest::WriteRecipeWav(wavPath)) {
        return artifact;
    }
    const std::string manifest =
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + packPath + "\",\n"
        "  \"packKind\": \"content\",\n"
        "  \"entries\": [{\"path\": \"" + wavPath +
        "\", \"id\": \"3da91a64-b947-4560-0000-00000000238d\"}],\n"
        "  \"voiceRecipes\": [{\n"
        "    \"playable\": \"3da91a64-b947-4560-0000-00000000238d\",\n"
        "    \"effects\": [\n"
        "      {\"slot\": 0, \"type\": \"feedbackDelay\", "
        "\"descriptorVintage\": 2, \"bypass\": false, "
        "\"parameters\": [10.0, 0.25, 0.75, 0.5]},\n"
        "      {\"slot\": 1, \"type\": \"threeBandTone\", "
        "\"descriptorVintage\": 2, \"bypass\": false, "
        "\"parameters\": [4.0, 400.0, -3.0, 1200.0, 2.0, -4.0, "
        "6000.0]}\n"
        "    ],\n"
        "    \"bindings\": []\n"
        "  }]\n"
        "}\n";
    artifact.built = BuildPackFile(unique, manifest);
    return artifact;
}

harkentest::RecipePackArtifact BuildPlaybackRecipePack(
    const std::string& name, std::uint32_t loopCount) {
    const std::string unique =
        name + "_" + std::to_string(_getpid());
    const std::string wavPath = TempPath(unique + ".wav");
    const std::string packPath = TempPath(unique + ".pack");
    harkentest::RecipePackArtifact artifact;
    artifact.path = packPath;
    if (!harkentest::WriteRecipeWav(wavPath)) {
        return artifact;
    }
    const std::string manifest =
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + packPath + "\",\n"
        "  \"packKind\": \"content\",\n"
        "  \"entries\": [{\"path\": \"" + wavPath +
        "\", \"id\": \"3da91a64-b947-4560-0000-00000000238d\"}],\n"
        "  \"voiceRecipes\": [{\n"
        "    \"playable\": \"3da91a64-b947-4560-0000-00000000238d\",\n"
        "    \"playback\": {\"loopCount\": " +
        std::to_string(loopCount) + "},\n"
        "    \"effects\": [],\n"
        "    \"bindings\": []\n"
        "  }]\n"
        "}\n";
    artifact.built = BuildPackFile(unique, manifest);
    return artifact;
}

std::string V2LogicManifest(const std::string& outputPath,
                            const char* packKind,
                            const std::string& sectionsJson) {
    std::string manifest = "{\n\"manifestVersion\": 3,\n\"output\": \"" +
                           outputPath + "\",\n\"packKind\": \"" +
                           std::string(packKind) + "\"";
    if (!sectionsJson.empty()) {
        manifest += ",\n" + sectionsJson;
    }
    manifest += "\n}\n";
    return manifest;
}

bool BuildResidentBusEffectPack(const std::string& name,
                                const std::string& outputPath,
                                VnBusId targetBusId,
                                VnBusId sourceBusId,
                                const char* effectType,
                                bool bypass,
                                const std::string& parametersJson,
                                bool includeDynamicRoute,
                                float routeLevelDb) {
    std::string sections =
        "\"buses\": ["
        "{\"id\": " + harkentest::JsonObjectId(targetBusId) +
        ", \"parent\": \"00000000-0000-0000-0000-000000000001\"}, "
        "{\"id\": " + harkentest::JsonObjectId(sourceBusId) +
        ", \"parent\": \"00000000-0000-0000-0000-000000000001\"}],\n"
        "\"busEffects\": [{"
        "\"bus\": " + harkentest::JsonObjectId(targetBusId) +
        ", \"slot\": 0, \"type\": \"" + effectType +
        "\", \"descriptorVintage\": 2, \"bypass\": " +
        (bypass ? "true" : "false") +
        ", \"faderPointGuard\": 4, \"parameters\": " +
        parametersJson + "}]";
    if (includeDynamicRoute) {
        sections +=
            ",\n\"dynamicRoutes\": [{"
            "\"targetBus\": " + harkentest::JsonObjectId(targetBusId) +
            ", \"effectSlot\": 0, \"routeSlot\": 0, "
            "\"sourceBus\": " +
            harkentest::JsonObjectId(sourceBusId) +
            ", \"tap\": \"postEffects\", \"levelDb\": " +
            std::to_string(routeLevelDb) + "}]";
    }
    return BuildPackFile(
        name, V2LogicManifest(outputPath, "resident", sections));
}

// A resident pack whose activation publishes exactly `commandCount` records as
// one indivisible batch. The activation counts one command per bus effect plus
// one plan swap (bypassed effects would add a second command each, so none are
// bypassed here), and the plan swap is the batch header, so the manifest needs
// commandCount - 1 effects spread across ceil((commandCount - 1) / slotsPerBus)
// buses. Gain is the cheapest effect with a single parameter, which keeps the
// manifest's size the only thing that grows.
//
// slotsPerBus does double duty, and it has to: an effect landing on a Bus the
// same pack declares must carry a fader guard equal to the engine's configured
// fxSlotsPerBus, because that Bus is not live yet and will start with the
// default fader split behind all of its insert slots. Deriving the guard from
// the same parameter that fills the slots keeps the manifest and the engine
// config from drifting apart -- the caller passes this value to
// config.fxSlotsPerBus, so one number satisfies both.
bool BuildWideBatchPack(const std::string& name,
                        const std::string& outputPath,
                        VnBusId firstBusId,
                        std::uint32_t commandCount,
                        std::uint32_t slotsPerBus) {
    const std::uint32_t effects = commandCount - 1u;
    const std::uint32_t buses =
        (effects + slotsPerBus - 1u) / slotsPerBus;
    std::string busJson;
    std::string effectJson;
    std::uint32_t emitted = 0;
    for (std::uint32_t bus = 0; bus < buses; ++bus) {
        const VnObjectId busId = harkentest::OffsetObjectId(firstBusId, bus);
        if (bus != 0) {
            busJson += ", ";
        }
        busJson += "{ \"id\": " + harkentest::JsonObjectId(busId) +
                   ", \"parent\": \"00000000-0000-0000-0000-000000000001\" }";
        for (std::uint32_t slot = 0;
             slot < slotsPerBus && emitted < effects; ++slot, ++emitted) {
            if (emitted != 0) {
                effectJson += ", ";
            }
            effectJson += "{ \"bus\": " + harkentest::JsonObjectId(busId) +
                          ", \"slot\": " + std::to_string(slot) +
                          ", \"type\": \"gain\", \"descriptorVintage\": 2,"
                          " \"bypass\": false, \"faderPointGuard\": " +
                          std::to_string(slotsPerBus) +
                          ", \"parameters\": [-6.0] }";
        }
    }
    return BuildPackFile(
        name,
        V2LogicManifest(outputPath, "resident",
                        "\"buses\": [ " + busJson + " ],\n"
                        "\"busEffects\": [ " + effectJson + " ]"));
}

// A resident pack that adds buses and nothing else, so its activation publishes
// exactly one record: the topology plan swap. That record is the batch header
// opcode, and it is the only work such a block's drain does.
bool BuildBusOnlyPack(const std::string& name,
                      const std::string& outputPath,
                      VnBusId firstBusId,
                      std::uint32_t buses) {
    std::string busJson;
    for (std::uint32_t bus = 0; bus < buses; ++bus) {
        if (bus != 0) {
            busJson += ", ";
        }
        busJson += "{ \"id\": " + harkentest::JsonObjectId(harkentest::OffsetObjectId(firstBusId, bus)) +
                   ", \"parent\": \"00000000-0000-0000-0000-000000000001\" }";
    }
    return BuildPackFile(
        name,
        V2LogicManifest(outputPath, "resident",
                        "\"buses\": [ " + busJson + " ]"));
}

bool BuildInitialAtomicityTopologyPack(const std::string& name,
                                       const std::string& outputPath,
                                       VnBusId targetBus,
                                       VnBusId sourceBus) {
    return BuildPackFile(
        name,
        V2LogicManifest(
            outputPath, "resident",
            "\"buses\": [ { \"id\": " + harkentest::JsonObjectId(targetBus) +
                ", \"parent\": \"00000000-0000-0000-0000-000000000001\" }, { \"id\": " +
                harkentest::JsonObjectId(sourceBus) + ", \"parent\": " +
                harkentest::JsonObjectId(targetBus) + " } ],\n"
                "\"busSends\": [ { \"from\": " +
                harkentest::JsonObjectId(sourceBus) +
                ", \"to\": \"00000000-0000-0000-0000-000000000001\", \"levelDb\": -6.0 } ],\n"
                "\"busEffects\": [ { \"bus\": " +
                harkentest::JsonObjectId(targetBus) +
                ", \"slot\": 0, \"type\": \"compressor\", "
                "\"descriptorVintage\": 2, \"bypass\": false, "
                "\"faderPointGuard\": 1, \"parameters\": "
                "[-18.0, 4.0, 10.0, 120.0, 6.0, 0.0] } ],\n"
                "\"dynamicRoutes\": [ { \"targetBus\": " +
                harkentest::JsonObjectId(targetBus) +
                ", \"effectSlot\": 0, \"routeSlot\": 0, "
                "\"sourceBus\": " + harkentest::JsonObjectId(sourceBus) +
                ", \"tap\": \"postEffects\", \"levelDb\": -3.0 } ]"));
}

VnResult LoadPack(const std::string& path, VnPackHandle* outHandle) {
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    desc.utf8Path = path.c_str();
    return Vn_LoadPack(&desc, outHandle);
}

VnResult ReplacePack(VnPackHandle handle, const std::string& path) {
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    desc.utf8Path = path.c_str();
    return Vn_ReplacePack(handle, &desc);
}

VnSnapshot Snap() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

std::uint32_t FindBusSendSlot(const harken::Engine& engine,
                              VnBusId fromBusId, VnBusId toBusId) {
    const std::uint32_t from = engine.buses.FindIndex(fromBusId);
    const std::uint32_t to = engine.buses.FindIndex(toBusId);
    if (from == harken::kInvalidBusIndex ||
        to == harken::kInvalidBusIndex) {
        return engine.config.busSendSlotsPerBus;
    }
    const std::uint32_t slots = engine.config.busSendSlotsPerBus;
    const std::uint32_t base = from * slots;
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        if (engine.busSendAdj[base + slot] == to) {
            return slot;
        }
    }
    return slots;
}

// Drains every pending logic log record (no log callback is configured,
// so records stay in the ring until popped here).
std::vector<harken::LogRecord> DrainLogicLog(harken::Engine* engine) {
    std::vector<harken::LogRecord> records;
    harken::LogRecord record{};
    while (engine->logicLogRing.TryPop(record)) {
        records.push_back(record);
    }
    return records;
}

std::size_t CountCode(const std::vector<harken::LogRecord>& records,
                      std::uint32_t code) {
    std::size_t n = 0;
    for (const harken::LogRecord& record : records) {
        if (record.code == code) {
            ++n;
        }
    }
    return n;
}

const harken::LogRecord* FindCode(const std::vector<harken::LogRecord>& records,
                                 std::uint32_t code) {
    for (const harken::LogRecord& record : records) {
        if (record.code == code) {
            return &record;
        }
    }
    return nullptr;
}

// Clears the staging fault even when an assertion aborts the test body,
// so one failed case never poisons the rest of the suite.
struct StagingFaultGuard {
    explicit StagingFaultGuard(harken::PackStagingFault fault) {
        harken::SetPackStagingFaultForTests(fault);
    }
    ~StagingFaultGuard() {
        harken::SetPackStagingFaultForTests(harken::PackStagingFault::None);
    }
    StagingFaultGuard(const StagingFaultGuard&) = delete;
    StagingFaultGuard& operator=(const StagingFaultGuard&) = delete;
};

struct PcmRegisterFaultGuard {
    explicit PcmRegisterFaultGuard(harken::PcmRegisterFault fault) {
        harken::SetPcmRegisterFaultForTests(fault);
    }
    ~PcmRegisterFaultGuard() {
        harken::SetPcmRegisterFaultForTests(harken::PcmRegisterFault::None);
    }
    PcmRegisterFaultGuard(const PcmRegisterFaultGuard&) = delete;
    PcmRegisterFaultGuard& operator=(const PcmRegisterFaultGuard&) = delete;
};

// --- test-local v2 byte builder ---
// Assembles complete v2 files directly over the wire serializers, for
// the cases packc itself refuses (kind-discipline defense in depth)
// and for byte-flip corruption. Mirrors the v2 format test's builder:
// header, typed directory, 8-aligned payloads, 64-aligned media blobs,
// FNV-1a checksums over the final bytes.
class V2Builder {
public:
    explicit V2Builder(std::uint32_t packKind) : m_packKind(packKind) {}

    void AddBus(VnBusId busId, VnBusId parentBusId) {
        Entry entry;
        entry.kind = harken::kPackEntryKindBus;
        entry.id = busId;
        entry.payload.resize(
            static_cast<std::size_t>(harken::kPackV2BusPayloadBytes));
        harken::PackBusEntry bus;
        bus.busId = busId;
        bus.parentBusId = parentBusId;
        harken::SerializePackV2Bus(bus, entry.payload.data());
        m_entries.push_back(std::move(entry));
    }

    void AddBusMix(VnBusId busId, VnBusId parentBusId, float volume,
                   float outputBusVolume, float makeUpGain) {
        Entry entry;
        entry.kind = harken::kPackEntryKindBus;
        entry.vintage = harken::kPackV2BusMixSchemaVintage;
        entry.id = busId;
        entry.payload.resize(
            static_cast<std::size_t>(harken::kPackV6BusPayloadBytes));
        harken::PackBusEntry bus;
        bus.busId = busId;
        bus.parentBusId = parentBusId;
        bus.volume = volume;
        bus.outputBusVolume = outputBusVolume;
        bus.makeUpGain = makeUpGain;
        harken::SerializePackV6Bus(bus, entry.payload.data());
        m_entries.push_back(std::move(entry));
    }

    void AddDiscreteVariable(VnVariableId variableId) {
        Entry entry;
        entry.kind = harken::kPackEntryKindDiscreteVariable;
        entry.id = variableId;
        entry.payload.resize(static_cast<std::size_t>(
            harken::kPackV2DiscreteVariablePayloadBytes));
        harken::SerializePackV2DiscreteVariable(variableId,
                                               entry.payload.data());
        m_entries.push_back(std::move(entry));
    }

    void AddMeterWiring(VnBusId busId, VnVariableId variableId) {
        Entry entry;
        entry.kind = harken::kPackEntryKindMeterWiring;
        entry.id = busId;
        entry.payload.resize(static_cast<std::size_t>(
            harken::kPackV2MeterWiringPayloadBytes));
        harken::PackMeterWiringEntry wiring{};
        wiring.busId = busId;
        wiring.variableId = variableId;
        harken::SerializePackV2MeterWiring(wiring, entry.payload.data());
        m_entries.push_back(std::move(entry));
    }

    void AddMedia(VnSoundId soundId, std::vector<float> planar) {
        Entry entry;
        entry.kind = harken::kPackEntryKindMedia;
        entry.id = soundId;
        entry.isMedia = true;
        entry.soundId = soundId;
        entry.planar = std::move(planar);
        m_entries.push_back(std::move(entry));
    }

    void AddBindingDefinition(VnBindingId bindingId,
                              const VnBindingDesc& desc) {
        Entry entry;
        entry.kind = harken::kPackEntryKindBindingDefinition;
        entry.id = bindingId;
        entry.payload.resize(static_cast<std::size_t>(
            harken::kPackV2BindingDefinitionPayloadBytes));
        harken::SerializePackV2BindingDefinition(bindingId, desc,
                                                 entry.payload.data());
        m_entries.push_back(std::move(entry));
    }

    void AddBindingAttachment(std::uint64_t ordinal, VnBindingId bindingId,
                              std::uint32_t scopeKind,
                              VnObjectId objectId) {
        Entry entry;
        entry.kind = harken::kPackEntryKindBindingAttachment;
        entry.relationOrdinal = ordinal;
        entry.payload.resize(static_cast<std::size_t>(
            harken::kPackV2BindingAttachmentPayloadBytes));
        harken::PackBindingAttachmentEntry attachment;
        attachment.bindingId = bindingId;
        attachment.scopeKind = scopeKind;
        attachment.objectId = harkentest::ObjectId(objectId);
        harken::SerializePackV2BindingAttachment(attachment,
                                                entry.payload.data());
        m_entries.push_back(std::move(entry));
    }

    void AddVoiceRecipe(VnSoundId playableId, VnBindingId bindingId,
                        std::uint32_t slot, std::uint32_t effectType,
                        const float* params, std::uint32_t paramCount) {
        Entry entry;
        entry.kind = harken::kPackEntryKindVoiceRecipe;
        entry.vintage = harken::kPackV2PlaybackRecipeSchemaVintage;
        entry.id = playableId;
        entry.payload.resize(static_cast<std::size_t>(
            harken::PackV5VoiceRecipePayloadBytes(1, 1)));
        harken::PackVoiceRecipeEntryHeader header;
        header.playableId = playableId;
        header.installCount = 1;
        header.bindingCount = 1;
        harken::SerializePackV2VoiceRecipeHeader(header,
                                                entry.payload.data());
        harken::PackVoiceRecipeTone tone;
        harken::SerializePackV3VoiceRecipeTone(
            tone, entry.payload.data() + harken::kPackV2VoiceRecipeHeaderBytes);
        harken::PackVoiceRecipeSpatial spatial;
        spatial.distanceScale = 0.0f;
        harken::SerializePackV4VoiceRecipeSpatial(
            spatial,
            entry.payload.data() + harken::kPackV2VoiceRecipeHeaderBytes +
                harken::kPackV3VoiceRecipeToneBytes);
        harken::PackVoiceRecipePlayback playback;
        harken::SerializePackV5VoiceRecipePlayback(
            playback,
            entry.payload.data() + harken::kPackV2VoiceRecipeHeaderBytes +
                harken::kPackV3VoiceRecipeToneBytes +
                harken::kPackV4VoiceRecipeSpatialBytes);
        std::uint8_t* installBase =
            entry.payload.data() + harken::kPackV2VoiceRecipeHeaderBytes +
            harken::kPackV3VoiceRecipeToneBytes +
            harken::kPackV4VoiceRecipeSpatialBytes +
            harken::kPackV5VoiceRecipePlaybackBytes;
        harken::PackVoiceRecipeInstall install;
        install.slot = slot;
        install.effectType = effectType;
        install.descriptorVintage =
            harken::kPackV2EffectDescriptorVintage;
        install.bypass = 0;
        install.paramCount = paramCount;
        for (std::uint32_t i = 0; i < paramCount; ++i) {
            install.params[i] = params[i];
        }
        harken::SerializePackV2VoiceRecipeInstall(install, installBase);
        harken::SerializePackV2VoiceRecipeBinding(
            bindingId, installBase + harken::kPackV2VoiceRecipeInstallBytes);
        m_entries.push_back(std::move(entry));
    }

    std::vector<std::uint8_t> Build() {
        const std::uint64_t dirOffset = harken::kPackV2HeaderBytes;
        const std::uint64_t dirBytes =
            static_cast<std::uint64_t>(m_entries.size()) *
            harken::kPackV2DirectoryEntryBytes;
        const std::uint64_t dirEnd = dirOffset + dirBytes;

        m_payloadOffsets.assign(m_entries.size(), 0);
        std::vector<std::uint64_t> blobOffsets(m_entries.size(), 0);

        std::uint64_t cursor = AlignUp(dirEnd, harken::kPackV2PayloadAlign);
        std::uint64_t contentEnd = dirEnd;
        for (std::size_t i = 0; i < m_entries.size(); ++i) {
            m_payloadOffsets[i] = cursor;
            contentEnd = cursor + PayloadSize(m_entries[i]);
            cursor = AlignUp(contentEnd, harken::kPackV2PayloadAlign);
        }
        std::uint64_t blobCursor = AlignUp(contentEnd, harken::kPackBlobAlign);
        for (std::size_t i = 0; i < m_entries.size(); ++i) {
            if (!m_entries[i].isMedia) {
                continue;
            }
            blobOffsets[i] = blobCursor;
            contentEnd = blobCursor + BlobBytes(m_entries[i]);
            blobCursor = AlignUp(contentEnd, harken::kPackBlobAlign);
        }
        const std::uint64_t fileBytes = contentEnd;

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileBytes),
                                        0);
        for (std::size_t i = 0; i < m_entries.size(); ++i) {
            const Entry& entry = m_entries[i];
            std::uint8_t* payload =
                bytes.data() + static_cast<std::size_t>(m_payloadOffsets[i]);
            if (entry.isMedia) {
                std::memcpy(
                    bytes.data() + static_cast<std::size_t>(blobOffsets[i]),
                    entry.planar.data(),
                    static_cast<std::size_t>(BlobBytes(entry)));
                harken::PackTocEntry toc;
                toc.soundId = entry.soundId;
                toc.sampleRate = kRate;
                toc.channelCount = 1;
                toc.sampleFormat = harken::kPackSampleFormatF32Planar;
                toc.frameCount = entry.planar.size();
                toc.blobOffset = blobOffsets[i];
                toc.blobBytes = BlobBytes(entry);
                toc.blobChecksum = harken::Fnv1a64(
                    bytes.data() + static_cast<std::size_t>(blobOffsets[i]),
                    static_cast<std::size_t>(toc.blobBytes));
                toc.flags = 0;
                toc.loopStartFrame = 0;
                toc.loopEndFrame = 0;
                harken::SerializePackTocEntry(toc, payload);
            } else {
                std::memcpy(payload, entry.payload.data(),
                            entry.payload.size());
            }
        }
        for (std::size_t i = 0; i < m_entries.size(); ++i) {
            const Entry& entry = m_entries[i];
            harken::PackV2DirectoryEntry record;
            record.kind = entry.kind;
            record.vintage = entry.vintage;
            record.id = entry.id;
            record.relationOrdinal = entry.relationOrdinal;
            record.offset = m_payloadOffsets[i];
            record.size = PayloadSize(entry);
            record.payloadChecksum = harken::Fnv1a64(
                bytes.data() + static_cast<std::size_t>(m_payloadOffsets[i]),
                static_cast<std::size_t>(record.size));
            harken::SerializePackV2DirectoryEntry(
                record, bytes.data() + static_cast<std::size_t>(dirOffset) +
                            i * harken::kPackV2DirectoryEntryBytes);
        }
        harken::PackV2Header header;
        header.formatVersion = harken::kPackFormatVersion2;
        header.headerBytes = harken::kPackV2HeaderBytes;
        header.fileBytes = fileBytes;
        header.packKind = m_packKind;
        header.entryCount = static_cast<std::uint32_t>(m_entries.size());
        header.directoryOffset = dirOffset;
        header.directoryChecksum = harken::Fnv1a64(
            bytes.data() + static_cast<std::size_t>(dirOffset),
            static_cast<std::size_t>(dirBytes));
        harken::SerializePackV2Header(header, bytes.data());
        return bytes;
    }

    std::uint64_t PayloadOffset(std::size_t i) const {
        return m_payloadOffsets[i];
    }

private:
    struct Entry {
        std::uint32_t kind = 0;
        std::uint32_t vintage = harken::kPackV2SchemaVintage;
        VnObjectId id{};
        std::uint64_t relationOrdinal = 0;
        std::vector<std::uint8_t> payload;
        bool isMedia = false;
        VnSoundId soundId = harkentest::ObjectId(0);
        std::vector<float> planar;
    };

    static std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align) {
        return (value + align - 1) / align * align;
    }
    static std::uint64_t PayloadSize(const Entry& entry) {
        return entry.isMedia ? harken::kPackV2MediaPayloadBytes
                             : entry.payload.size();
    }
    static std::uint64_t BlobBytes(const Entry& entry) {
        return static_cast<std::uint64_t>(entry.planar.size()) *
               sizeof(float);
    }

    std::uint32_t m_packKind;
    std::vector<Entry> m_entries;
    std::vector<std::uint64_t> m_payloadOffsets;
};


VnBindingDesc MakeBusVolumeBinding(VnVariableId variableId, float xLow,
                                   float yLow, float xHigh, float yHigh) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = variableId;
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS;
    desc.targetId = 0; // BusVolume
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {xLow, yLow};
    desc.curve.points[1] = {xHigh, yHigh};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    return desc;
}

VnBindingDesc MakeBusEffectParamBinding(
    VnVariableId variableId, std::uint32_t effectType,
    std::uint32_t slot, std::uint32_t parameterIndex) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = variableId;
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    desc.targetId = harken::PackEffectParamLocator(
        effectType, slot, parameterIndex);
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 1;
    desc.curve.points[0] = {0.0f, 0.0f};
    return desc;
}

VnPackHandle LoadResidentBusOnly(const char* name, VnBusId busId) {
    const std::string path = TempPath(std::string(name) + ".pack");
    EXPECT_TRUE(BuildPackFile(
        name, V2LogicManifest(
                  path, "resident",
                  "\"buses\": [ { \"id\": " + harkentest::JsonObjectId(busId) +
                      ", \"parent\": \"00000000-0000-0000-0000-000000000001\" } ]")));
    VnPackHandle pack = 0;
    EXPECT_EQ(VN_OK, LoadPack(path, &pack));
    return pack;
}

VnPackHandle LoadMixedReversibleResident(const char* name,
                                         VnBusId targetBus,
                                         VnBusId sourceBus) {
    const std::string path = TempPath(std::string(name) + ".pack");
    EXPECT_TRUE(BuildPackFile(
        name,
        V2LogicManifest(
            path, "resident",
            "\"buses\": [ { \"id\": " + harkentest::JsonObjectId(targetBus) +
                ", \"parent\": \"00000000-0000-0000-0000-000000000001\" }, { \"id\": " +
                harkentest::JsonObjectId(sourceBus) + ", \"parent\": " +
                harkentest::JsonObjectId(targetBus) + " } ],\n"
                "\"busSends\": [ { \"from\": " +
                harkentest::JsonObjectId(sourceBus) +
                ", \"to\": \"00000000-0000-0000-0000-000000000001\", \"levelDb\": -6.0 } ],\n"
                "\"busEffects\": [ { \"bus\": " +
                harkentest::JsonObjectId(targetBus) +
                ", \"slot\": 0, \"type\": \"compressor\", "
                "\"descriptorVintage\": 2, \"bypass\": false, "
                "\"faderPointGuard\": 1, \"parameters\": "
                "[-18.0, 4.0, 10.0, 120.0, 6.0, 0.0] } ],\n"
                "\"dynamicRoutes\": [ { \"targetBus\": " +
                harkentest::JsonObjectId(targetBus) +
                ", \"effectSlot\": 0, \"routeSlot\": 0, "
                "\"sourceBus\": " + harkentest::JsonObjectId(sourceBus) +
                ", \"tap\": \"postEffects\", \"levelDb\": -3.0 } ]")));
    VnPackHandle pack = 0;
    EXPECT_EQ(VN_OK, LoadPack(path, &pack));
    return pack;
}

struct MixedResidentState {
    std::uint32_t targetIndex = harken::kInvalidBusIndex;
    std::uint32_t sourceIndex = harken::kInvalidBusIndex;
    std::uint32_t sendSlot = 0;
    harken::EffectInstanceRef effect;
    harken::DynamicRouteDefinition route;
    std::uint32_t commandFreeSlots = 0;
    std::uint32_t commandOpcodeMask = 0;
    std::uint64_t topologyPlanCount = 0;
    std::uint64_t dynamicInstalledRevision = 0;
    std::size_t stampCount = 0;
};

MixedResidentState ObserveMixedResidentState(
    const harken::Engine& engine, VnPackHandle pack,
    VnBusId targetBus, VnBusId sourceBus) {
    MixedResidentState state;
    state.targetIndex = engine.buses.FindIndex(targetBus);
    state.sourceIndex = engine.buses.FindIndex(sourceBus);
    EXPECT_NE(harken::kInvalidBusIndex, state.targetIndex);
    EXPECT_NE(harken::kInvalidBusIndex, state.sourceIndex);
    if (state.targetIndex == harken::kInvalidBusIndex ||
        state.sourceIndex == harken::kInvalidBusIndex) {
        return state;
    }
    state.sendSlot = FindBusSendSlot(engine, sourceBus, VN_BUS_MASTER);
    EXPECT_LT(state.sendSlot, engine.config.busSendSlotsPerBus);
    state.effect = engine.busEffectInstanceRefs[state.targetIndex][0];
    const harken::DynamicRouteDefinition* route =
        engine.dynamicRoutes.Find(targetBus, 0, 0);
    EXPECT_NE(nullptr, route);
    if (route != nullptr) {
        state.route = *route;
    }
    state.commandFreeSlots = engine.commands.FreeSlots();
    state.commandOpcodeMask = engine.commands.OpcodeSeenMask();
    state.topologyPlanCount = engine.topologyPlanCount;
    state.dynamicInstalledRevision =
        engine.dynamicRoutes.InstalledRevision();
    const harken::PackLogicRecord* record =
        engine.logicPacks.RecordFor(pack);
    EXPECT_NE(nullptr, record);
    if (record != nullptr) {
        state.stampCount = record->stamps.size();
    }
    return state;
}

void ExpectMixedResidentStateUnchanged(
    const harken::Engine& engine, VnPackHandle pack,
    VnBusId targetBus, VnBusId sourceBus,
    const MixedResidentState& before) {
    ASSERT_NE(harken::kInvalidBusIndex, before.targetIndex);
    ASSERT_NE(harken::kInvalidBusIndex, before.sourceIndex);
    EXPECT_EQ(before.targetIndex, engine.buses.FindIndex(targetBus));
    EXPECT_EQ(before.sourceIndex, engine.buses.FindIndex(sourceBus));
    EXPECT_EQ(0u, engine.buses.ParentOf(before.targetIndex));
    EXPECT_EQ(before.targetIndex,
              engine.buses.ParentOf(before.sourceIndex));
    EXPECT_EQ(pack, engine.busOwnerCookies[before.targetIndex]);
    EXPECT_EQ(pack, engine.busOwnerCookies[before.sourceIndex]);
    EXPECT_EQ(before.sendSlot,
              FindBusSendSlot(engine, sourceBus, VN_BUS_MASTER));
    const std::uint32_t sendFlat =
        before.sourceIndex * engine.config.busSendSlotsPerBus +
        before.sendSlot;
    EXPECT_EQ(pack, engine.busSendOwnerCookies[sendFlat]);
    const harken::EffectInstanceRef effect =
        engine.busEffectInstanceRefs[before.targetIndex][0];
    EXPECT_EQ(before.effect.type, effect.type);
    EXPECT_EQ(before.effect.poolIndex, effect.poolIndex);
    EXPECT_EQ(before.effect.generation, effect.generation);
    EXPECT_EQ(pack,
              engine.busEffectOwnerCookies[before.targetIndex][0]);
    const harken::DynamicRouteDefinition* route =
        engine.dynamicRoutes.Find(targetBus, 0, 0);
    ASSERT_NE(nullptr, route);
    EXPECT_EQ(before.route.tableIndex, route->tableIndex);
    EXPECT_EQ(before.route.generation, route->generation);
    EXPECT_EQ(before.route.revision, route->revision);
    EXPECT_EQ(before.route.tapGeneration, route->tapGeneration);
    EXPECT_EQ(before.route.ownerKind, route->ownerKind);
    EXPECT_EQ(before.route.ownerCookie, route->ownerCookie);
    EXPECT_EQ(before.commandFreeSlots, engine.commands.FreeSlots());
    EXPECT_EQ(before.commandOpcodeMask, engine.commands.OpcodeSeenMask());
    EXPECT_EQ(before.topologyPlanCount, engine.topologyPlanCount);
    EXPECT_EQ(before.dynamicInstalledRevision,
              engine.dynamicRoutes.InstalledRevision());
    const harken::PackLogicRecord* record =
        engine.logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, record);
    EXPECT_EQ(2u, record->buses.size());
    EXPECT_EQ(1u, record->busSends.size());
    EXPECT_EQ(1u, record->busEffects.size());
    EXPECT_EQ(1u, record->dynamicRoutes.size());
    EXPECT_EQ(before.stampCount, record->stamps.size());
    EXPECT_EQ(1u, engine.packs.LoadedCount());
}

// --- load: resident happy path ---

TEST(PackLogicLoad, CurrentAndHistoricalContainerVersionsReachLogicLoader) {
    EngineSession session(PackLogicConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto sound = harkentest::ObjectId(71);
    const auto path = TempPath("pll_container_versions.pack");
    std::vector<std::uint32_t> versions{harken::kPackFormatVersion};
    if (harken::kPackFormatVersion3 < harken::kPackFormatVersion) {
        versions.push_back(harken::kPackFormatVersion3);
    }
    for (const auto version : versions) {
        V2Builder builder(harken::kPackKindContent);
        builder.AddMedia(sound, harkentest::MakeDC(16, 0.25f));
        auto bytes = builder.Build();
        harken::StoreLE32(bytes.data() + 4, version);
        WriteFileBytes(path, bytes);
        VnPackHandle pack = 0;
        ASSERT_EQ(VN_OK, LoadPack(path, &pack)) << version;
        EXPECT_TRUE(session.Engine()->registry.Lookup(sound).Valid());
        EXPECT_NE(nullptr, session.Engine()->logicPacks.RecordFor(pack));
        ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
        EXPECT_FALSE(session.Engine()->registry.Lookup(sound).Valid());
    }
}

TEST(PackLogicLoad, LegacyContainerRowsRefuseBeforeLoadOrReplaceMutation) {
    const std::string wavPath = TempPath("pll_legacy_container.wav");
    WriteWavF32(wavPath, harkentest::MakeDC(kBlock * 4, 0.25f), 1, kRate);
    const auto contentManifest =
        [&](const std::string& output, VnSoundId soundId,
            bool includeContainer) {
            std::string sections =
                "\"entries\": [ { \"path\": \"" + wavPath +
                "\", \"id\": " + harkentest::JsonObjectId(soundId) + " } ]";
            if (includeContainer) {
                sections +=
                    ",\n\"containers\": [ { \"id\": " +
                    harkentest::JsonObjectId(harkentest::OffsetObjectId(soundId, 1)) +
                    ", \"order\": \"weightedRandom\", "
                    "\"children\": [ { \"playable\": " +
                    harkentest::JsonObjectId(soundId) +
                    ", \"weight\": 1 } ] } ]";
            }
            return V2LogicManifest(output, "content", sections);
        };

    const std::string rejectedLoadPath =
        TempPath("pll_legacy_container_load.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_legacy_container_load",
        contentManifest(rejectedLoadPath, harkentest::ObjectId(7001), true)));

    EngineSession session(PackLogicConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::uint32_t loadedBefore = engine->packs.LoadedCount();
    VnPackHandle rejected = 0;
    EXPECT_EQ(VN_BAD_PACK, LoadPack(rejectedLoadPath, &rejected));
    EXPECT_EQ(loadedBefore, engine->packs.LoadedCount());
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(7001)).Valid());
    EXPECT_TRUE(engine->logicPacks.pending.empty());

    const std::string basePath = TempPath("pll_container_free_base.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_container_free_base",
        contentManifest(basePath, harkentest::ObjectId(7101), false)));
    VnPackHandle base = 0;
    ASSERT_EQ(VN_OK, LoadPack(basePath, &base));
    ASSERT_NE(nullptr, engine->logicPacks.RecordFor(base));
    ASSERT_TRUE(engine->registry.Lookup(harkentest::ObjectId(7101)).Valid());

    const std::string rejectedReplacePath =
        TempPath("pll_legacy_container_replace.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_legacy_container_replace",
        contentManifest(rejectedReplacePath, harkentest::ObjectId(7101), true)));
    EXPECT_EQ(VN_BAD_PACK, ReplacePack(base, rejectedReplacePath));
    EXPECT_EQ(loadedBefore + 1, engine->packs.LoadedCount());
    EXPECT_TRUE(engine->registry.Lookup(harkentest::ObjectId(7101)).Valid());
    EXPECT_NE(nullptr, engine->logicPacks.RecordFor(base));
    EXPECT_TRUE(engine->logicPacks.pending.empty());
}

TEST(PackLogicLoad, ResidentPackLoadsInfrastructure) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const harken::VariableDefinitionV1 definitions[] = {
        harkentest::MakeVariableDefinition(harkentest::ObjectId(7301), harken::VariableKindV1::Step,
                                         0.0f, 8.0f, 0.0f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, definitions, 1).status);

    // Bus 501 under master, 502 under 501, one leveled send edge back to
    // the master (acyclic beside the tree edges), a bus-scope binding on an
    // installed Variable attached to 501, and an attenuation whose volume
    // override carries a step segment (the override grammar admits every
    // binding-curve shape).
    const std::string packPath = TempPath("pll_resident_happy.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_resident_happy",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-0000000001f5", "parent": "00000000-0000-0000-0000-000000000001" },
            { "id": "3da91a64-b947-4560-0000-0000000001f6", "parent": "3da91a64-b947-4560-0000-0000000001f5" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-0000000001f6", "to": "00000000-0000-0000-0000-000000000001", "levelDb": -6.0 } ],
"attenuations": [ { "id": "3da91a64-b947-4560-0000-000000000209", "rolloff": "linear",
                    "minDistance": 1.0, "maxDistance": 100.0,
                    "airAbsorptionNearHz": 1000000000.0,
                    "airAbsorptionFarHz": 1000000000.0,
                    "volumeCurve": { "points": [[1.0, 0.0], [50.0, -12.0]],
                                     "handles": [[1.0, 0.0, 50.0, -12.0]] } } ],
"bindings": [ { "id": "3da91a64-b947-4560-0000-000000000259", "variable": "3da91a64-b947-4560-0000-000000001c85", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -12.0]],
                           "handles": [[0.0, 0.0, 1.0, -12.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-000000000259", "scope": "bus", "object": "3da91a64-b947-4560-0000-0000000001f5" } ])")));

    (void)DrainLogicLog(engine);
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    EXPECT_NE(0u, pack);

    // The buses are live public objects.
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(501), -1.0f, 0));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(502), -1.0f, 0));

    // The separately installed Step definition is in force: integers pass,
    // fractions refuse. The Pack contributes no Variable authority.
    EXPECT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7301), 2.0f));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetValue(harkentest::ObjectId(7301), 0.5f));

    // The binding definition and its attachment landed.
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(601)));
    EXPECT_EQ(1u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(601)));

    // The step-shaped volume override compiled and registered.
    EXPECT_TRUE(engine->spatial.HasProfile(harkentest::ObjectId(521)));

    // Staging proved every apply refusal absent: no defect records.
    const std::vector<harken::LogRecord> records = DrainLogicLog(engine);
    EXPECT_EQ(0u, CountCode(records, harken::kLogCodePackApplyDefect));

    // Multiple resident packs are legal (append-only infrastructure).
    const std::string secondPath = TempPath("pll_resident_second.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_resident_second",
        V2LogicManifest(secondPath, "resident",
                        R"("buses": [ { "id": "3da91a64-b947-4560-0000-0000000001ff", "parent": "00000000-0000-0000-0000-000000000001" } ])")));
    VnPackHandle second = 0;
    ASSERT_EQ(VN_OK, LoadPack(secondPath, &second));
    EXPECT_EQ(2u, engine->packs.LoadedCount());
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(511), -2.0f, 0));
}

TEST(PackLogicLoad, BusAndSendOwnershipCookiesTrackLiveSlots) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string packPath =
        TempPath("pll_bus_send_provenance.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_bus_send_provenance",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000002ee1", "parent": "00000000-0000-0000-0000-000000000001" },
            { "id": "3da91a64-b947-4560-0000-000000002ee2", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-000000002ee1", "to": "3da91a64-b947-4560-0000-000000002ee2", "levelDb": -6.0 } ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    const std::uint32_t first = engine->buses.FindIndex(harkentest::ObjectId(12001));
    const std::uint32_t second = engine->buses.FindIndex(harkentest::ObjectId(12002));
    ASSERT_NE(harken::kInvalidBusIndex, first);
    ASSERT_NE(harken::kInvalidBusIndex, second);
    EXPECT_EQ(pack, engine->busOwnerCookies[first]);
    EXPECT_EQ(pack, engine->busOwnerCookies[second]);

    const std::uint32_t sendSlot =
        FindBusSendSlot(*engine, harkentest::ObjectId(12001), harkentest::ObjectId(12002));
    ASSERT_LT(sendSlot, engine->config.busSendSlotsPerBus);
    EXPECT_EQ(pack,
              engine->busSendOwnerCookies[
                  first * engine->config.busSendSlotsPerBus + sendSlot]);

    const harken::PackLogicRecord* record =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, record);
    ASSERT_EQ(2u, record->buses.size());
    EXPECT_EQ(harkentest::ObjectId(12001u), record->buses[0].busId);
    EXPECT_EQ(VN_BUS_MASTER, record->buses[0].parentBusId);
    EXPECT_EQ(harkentest::ObjectId(12002u), record->buses[1].busId);
    EXPECT_EQ(VN_BUS_MASTER, record->buses[1].parentBusId);
}

TEST(PackLogicLoad, BusMixFieldsApplyAtSeparateGainStages) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string packPath = TempPath("pll_bus_mix_apply.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_bus_mix_apply",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-0000000032c9", "parent": "00000000-0000-0000-0000-000000000001", "volume": -6.0,
            "outputBusVolume": -3.5, "makeUpGain": 2.0 },
            { "id": "3da91a64-b947-4560-0000-0000000032ca", "parent": "00000000-0000-0000-0000-000000000001" } ],
            "busSends": [ { "from": "3da91a64-b947-4560-0000-0000000032c9", "to": "3da91a64-b947-4560-0000-0000000032ca", "levelDb": 0.0 } ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    const harken::PackLogicRecord* record =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, record);
    ASSERT_EQ(2u, record->buses.size());
    EXPECT_FLOAT_EQ(-6.0f, record->buses[0].volume);
    EXPECT_FLOAT_EQ(-3.5f, record->buses[0].outputBusVolume);
    EXPECT_FLOAT_EQ(2.0f, record->buses[0].makeUpGain);
    EXPECT_FLOAT_EQ(0.0f, record->buses[1].volume);
    EXPECT_FLOAT_EQ(0.0f, record->buses[1].outputBusVolume);
    EXPECT_FLOAT_EQ(0.0f, record->buses[1].makeUpGain);

    const std::uint32_t mixed = engine->buses.FindIndex(harkentest::ObjectId(13001));
    const std::uint32_t zeros = engine->buses.FindIndex(harkentest::ObjectId(13002));
    ASSERT_NE(harken::kInvalidBusIndex, mixed);
    ASSERT_NE(harken::kInvalidBusIndex, zeros);
    EXPECT_FLOAT_EQ(
        -4.0f, engine->busTargets.LastCommitted(
                   mixed, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(
        0.0f, engine->busTargets.LastCommitted(
                  zeros, harken::kBusTargetBusVolume));
    EXPECT_TRUE(engine->busTargets.SlotActive(
        mixed, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(
        -4.0f, engine->busTargets.SlotValue(
                   mixed, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FALSE(engine->busTargets.SlotActive(
        mixed, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex));
    EXPECT_FALSE(engine->busTargets.SlotActive(
        mixed, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotVariableIndex));

    const std::vector<float> dc = harkentest::MakeDC(kBlock, 0.8f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = harkentest::ObjectId(13001);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harkentest::Capture out;
    harkentest::PumpAndCapture(engine, 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    const float expected = 0.8f * (std::pow(10.0f, -7.5f / 20.0f) +
                                   std::pow(10.0f, -4.0f / 20.0f));
    for (std::size_t i = 0; i < kBlock; ++i) {
        ASSERT_NEAR(expected, out.left[i], 1e-5f) << "left sample " << i;
        ASSERT_NEAR(expected, out.right[i], 1e-5f)
            << "right sample " << i;
    }
}

TEST(PackLogicLoad, VintageTwoBusPayloadAppliesAsZeroMix) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    V2Builder builder(harken::kPackKindResident);
    builder.AddBus(harkentest::ObjectId(13011), VN_BUS_MASTER);
    const std::string packPath = TempPath("pll_bus_mix_v2.pack");
    WriteFileBytes(packPath, builder.Build());

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    const harken::PackLogicRecord* record =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, record);
    ASSERT_EQ(1u, record->buses.size());
    EXPECT_FLOAT_EQ(0.0f, record->buses[0].volume);
    EXPECT_FLOAT_EQ(0.0f, record->buses[0].outputBusVolume);
    EXPECT_FLOAT_EQ(0.0f, record->buses[0].makeUpGain);

    const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(13011));
    ASSERT_NE(harken::kInvalidBusIndex, index);
    EXPECT_FLOAT_EQ(
        0.0f, engine->busTargets.LastCommitted(
                  index, harken::kBusTargetBusVolume));

    const std::vector<float> dc = harkentest::MakeDC(kBlock, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = harkentest::ObjectId(13011);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harkentest::Capture out;
    harkentest::PumpAndCapture(engine, 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    for (std::size_t i = 0; i < kBlock; ++i) {
        ASSERT_NEAR(0.5f, out.left[i], 1e-5f) << "left sample " << i;
        ASSERT_NEAR(0.5f, out.right[i], 1e-5f) << "right sample " << i;
    }
}

TEST(PackLogicReplace, BusMixRelevelPreservesOutputGain) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const auto buildRevision = [&](const char* name, float volume,
                                   float outputBusVolume,
                                   float makeUpGain) {
        const std::string path = TempPath(std::string(name) + ".pack");
        const bool built = BuildPackFile(
            name,
            V2LogicManifest(
                path, "resident",
                "\"buses\": [ { \"id\": \"3da91a64-b947-4560-0000-0000000032dd\", \"parent\": \"00000000-0000-0000-0000-000000000001\", "
                "\"volume\": " +
                    std::to_string(volume) +
                    ", \"outputBusVolume\": " +
                    std::to_string(outputBusVolume) +
                    ", \"makeUpGain\": " +
                    std::to_string(makeUpGain) + " } ]"));
        EXPECT_TRUE(built);
        return path;
    };
    const std::string v1Path =
        buildRevision("plr_bus_mix_v1", -6.0f, -3.5f, 2.0f);
    const std::string v2Path =
        buildRevision("plr_bus_mix_v2", -12.0f, -3.5f, 2.0f);

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(13021));
    ASSERT_NE(harken::kInvalidBusIndex, index);
    EXPECT_FLOAT_EQ(
        -4.0f, engine->busTargets.LastCommitted(
                   index, harken::kBusTargetBusVolume));

    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(
        -10.0f, engine->busTargets.LastCommitted(
                    index, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(
        -10.0f, engine->busTargets.SlotValue(
                    index, harken::kBusTargetBusVolume,
                    harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FALSE(engine->busTargets.SlotActive(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex));

    const std::vector<float> dc = harkentest::MakeDC(kBlock, 0.8f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = harkentest::ObjectId(13021);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harkentest::Capture out;
    harkentest::PumpAndCapture(engine, 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    const float expected = 0.8f * std::pow(10.0f, -13.5f / 20.0f);
    for (std::size_t i = 0; i < kBlock; ++i) {
        ASSERT_NEAR(expected, out.left[i], 1e-5f) << "left sample " << i;
        ASSERT_NEAR(expected, out.right[i], 1e-5f)
            << "right sample " << i;
    }
    const auto v3Path = buildRevision("plr_bus_mix_output_only", -12.0f, 0.0f, 2.0f);
    ASSERT_EQ(VN_OK, ReplacePack(pack, v3Path));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(-10.0f, engine->busTargets.LastCommitted(
                               index, harken::kBusTargetBusVolume));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    harkentest::Capture revised;
    harkentest::PumpAndCapture(engine, 1, revised);
    const float revisedExpected = 0.8f * std::pow(10.0f, -10.0f / 20.0f);
    for (float sample : revised.left) EXPECT_NEAR(revisedExpected, sample, 1e-5f);
}

TEST(PackLogicLoad, BusMixAuthoredSlotLeavesApiDirectUntouched) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(13101)}, 0.0f, 1.0f).status);

    const std::string packPath = TempPath("pll_bus_mix_auth.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_bus_mix_auth",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-00000000332e", "parent": "00000000-0000-0000-0000-000000000001", "volume": -6.0,
            "outputBusVolume": -3.5, "makeUpGain": 2.0 } ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(13102));
    ASSERT_NE(harken::kInvalidBusIndex, index);

    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(13102), -1.0f, 0));
    const VnBindingDesc binding =
        MakeBusVolumeBinding(harkentest::ObjectId(13101), 0.0f, 0.0f, 1.0f, -4.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(13103), &binding));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = harkentest::ObjectId(13102);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(13103), &attach));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(13101), 1.0f));
    harkentest::AdvanceVariableWrite(engine);

    EXPECT_FLOAT_EQ(
        -4.0f, engine->busTargets.SlotValue(
                   index, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(
        -1.0f, engine->busTargets.SlotValue(
                   index, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotApiDirectIndex));
    EXPECT_FLOAT_EQ(
        -4.0f, engine->busTargets.SlotValue(
                   index, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotVariableIndex));
    EXPECT_FLOAT_EQ(
        -9.0f, engine->busTargets.Reduce(index, harken::kBusTargetBusVolume));

    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK, Vn_CapturePropertyTargets(nullptr, 0, &required));
    ASSERT_GE(required, 1u);
    std::vector<VnPropertyTargetInfo> rows(required);
    rows[0].structSize = sizeof(VnPropertyTargetInfo);
    ASSERT_EQ(VN_OK, Vn_CapturePropertyTargets(
                         rows.data(), required, &required));
    const VnPropertyTargetInfo* captured = nullptr;
    for (const VnPropertyTargetInfo& candidate : rows) {
        if (candidate.objectId == harkentest::ObjectId(13102) &&
            candidate.targetId == harken::kBusTargetBusVolume) {
            captured = &candidate;
            break;
        }
    }
    ASSERT_NE(nullptr, captured);
    EXPECT_FLOAT_EQ(-1.0f, captured->apiDirectValue);
    EXPECT_FLOAT_EQ(-4.0f, captured->variableValue);
    EXPECT_FLOAT_EQ(-9.0f, captured->finalValue);
}

TEST(PackLogicReplace, BusMixKeepsApiDirectAndVariableOnRetainedBus) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(13111)}, 0.0f, 1.0f).status);

    const auto buildRevision = [&](const char* name, float volume) {
        const std::string path = TempPath(std::string(name) + ".pack");
        const bool built = BuildPackFile(
            name,
            V2LogicManifest(
                path, "resident",
                "\"buses\": [ { \"id\": \"3da91a64-b947-4560-0000-000000003338\", \"parent\": \"00000000-0000-0000-0000-000000000001\", "
                "\"volume\": " +
                    std::to_string(volume) +
                    ", \"outputBusVolume\": -3.5, \"makeUpGain\": 2.0 } ]"));
        EXPECT_TRUE(built);
        return path;
    };
    const std::string v1Path = buildRevision("plr_bus_mix_keep_v1", -6.0f);
    const std::string v2Path = buildRevision("plr_bus_mix_keep_v2", -12.0f);

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(13112));
    ASSERT_NE(harken::kInvalidBusIndex, index);
    const std::uint32_t generation = engine->buses.GenerationAt(index);

    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(13112), -1.0f, 0));
    const VnBindingDesc binding =
        MakeBusVolumeBinding(harkentest::ObjectId(13111), 0.0f, 0.0f, 1.0f, -4.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(13113), &binding));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = harkentest::ObjectId(13112);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(13113), &attach));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(13111), 1.0f));
    harkentest::AdvanceVariableWrite(engine);

    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(generation, engine->buses.GenerationAt(index));
    EXPECT_FLOAT_EQ(
        -10.0f, engine->busTargets.SlotValue(
                    index, harken::kBusTargetBusVolume,
                    harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(
        -1.0f, engine->busTargets.SlotValue(
                   index, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotApiDirectIndex));
    EXPECT_FLOAT_EQ(
        -4.0f, engine->busTargets.SlotValue(
                   index, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotVariableIndex));
    EXPECT_FLOAT_EQ(
        -15.0f, engine->busTargets.Reduce(index, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(
        -15.0f, engine->busTargets.LastCommitted(
                    index, harken::kBusTargetBusVolume));

    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(13112), -2.0f, 0));
    EXPECT_FLOAT_EQ(
        -10.0f, engine->busTargets.SlotValue(
                    index, harken::kBusTargetBusVolume,
                    harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(
        -2.0f, engine->busTargets.SlotValue(
                   index, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotApiDirectIndex));
    EXPECT_FLOAT_EQ(
        -4.0f, engine->busTargets.SlotValue(
                   index, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotVariableIndex));

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(13111), 0.0f));
    harkentest::AdvanceVariableWrite(engine);
    EXPECT_FLOAT_EQ(
        -10.0f, engine->busTargets.SlotValue(
                    index, harken::kBusTargetBusVolume,
                    harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(
        -2.0f, engine->busTargets.SlotValue(
                   index, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotApiDirectIndex));
    EXPECT_FLOAT_EQ(
        -12.0f, engine->busTargets.Reduce(index, harken::kBusTargetBusVolume));
}

TEST(PackLogicLoad, NonFiniteBusMixSumRefusesWithoutMutation) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    V2Builder builder(harken::kPackKindResident);
    builder.AddBusMix(harkentest::ObjectId(13121), VN_BUS_MASTER, 3.0e38f, 3.0e38f, 3.0e38f);
    const std::string packPath = TempPath("pll_bus_mix_overflow.pack");
    WriteFileBytes(packPath, builder.Build());

    VnPackHandle pack = 0;
    EXPECT_EQ(VN_BAD_PACK, LoadPack(packPath, &pack));
    EXPECT_EQ(0u, pack);
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(1u, engine->buses.BusCount());
    EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(13121)));
}

TEST(PackLogicLoad, FiniteOverflowingBusMixRefusesWithoutMutation) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    V2Builder builder(harken::kPackKindResident);
    builder.AddBusMix(harkentest::ObjectId(13191), VN_BUS_MASTER, 800.0f, 0.0f, 0.0f);
    const std::string packPath = TempPath("pll_bus_mix_finite_overflow.pack");
    WriteFileBytes(packPath, builder.Build());

    VnPackHandle pack = 0;
    EXPECT_EQ(VN_BAD_PACK, LoadPack(packPath, &pack));
    EXPECT_EQ(0u, pack);
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(1u, engine->buses.BusCount());
    EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(13191)));
}

TEST(PackLogicReplace, NonFiniteBusMixSumLeavesLoadedRevisionIntact) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string v1Path = TempPath("plr_bus_mix_overflow_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_bus_mix_overflow_v1",
        V2LogicManifest(
            v1Path, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-00000000334b", "parent": "00000000-0000-0000-0000-000000000001", "volume": -6.0,
            "outputBusVolume": -3.5, "makeUpGain": 2.0 } ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(13131));
    ASSERT_NE(harken::kInvalidBusIndex, index);
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(13131), -1.0f, 0));
    const float authoredBefore = engine->busTargets.SlotValue(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotAuthoredIndex);
    const float apiBefore = engine->busTargets.SlotValue(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex);
    const float committedBefore = engine->busTargets.LastCommitted(
        index, harken::kBusTargetBusVolume);
    const std::uint32_t generation = engine->buses.GenerationAt(index);

    V2Builder overflow(harken::kPackKindResident);
    overflow.AddBusMix(harkentest::ObjectId(13131), VN_BUS_MASTER, 3.0e38f, 3.0e38f, 3.0e38f);
    const std::string overflowPath =
        TempPath("plr_bus_mix_overflow_v2.pack");
    WriteFileBytes(overflowPath, overflow.Build());

    EXPECT_EQ(VN_BAD_PACK, ReplacePack(pack, overflowPath));
    EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
    EXPECT_EQ(index, engine->buses.FindIndex(harkentest::ObjectId(13131)));
    EXPECT_EQ(generation, engine->buses.GenerationAt(index));
    EXPECT_FLOAT_EQ(authoredBefore,
                    engine->busTargets.SlotValue(
                        index, harken::kBusTargetBusVolume,
                        harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(apiBefore,
                    engine->busTargets.SlotValue(
                        index, harken::kBusTargetBusVolume,
                        harken::BusTargetStore::kSlotApiDirectIndex));
    EXPECT_FLOAT_EQ(committedBefore,
                    engine->busTargets.LastCommitted(
                        index, harken::kBusTargetBusVolume));
}

TEST(PackLogicReplace, FiniteOverflowingBusMixLeavesLoadedRevisionIntact) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    V2Builder first(harken::kPackKindResident);
    first.AddBusMix(harkentest::ObjectId(13192), VN_BUS_MASTER, -6.0f, -3.5f, 2.0f);
    const std::string v1Path =
        TempPath("plr_bus_mix_finite_overflow_v1.pack");
    WriteFileBytes(v1Path, first.Build());

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(13192));
    ASSERT_NE(harken::kInvalidBusIndex, index);
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(13192), -1.0f, 0));
    const float authoredBefore = engine->busTargets.SlotValue(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotAuthoredIndex);
    const float apiBefore = engine->busTargets.SlotValue(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex);
    const float committedBefore = engine->busTargets.LastCommitted(
        index, harken::kBusTargetBusVolume);
    const std::uint32_t generation = engine->buses.GenerationAt(index);

    V2Builder overflow(harken::kPackKindResident);
    overflow.AddBusMix(harkentest::ObjectId(13192), VN_BUS_MASTER, 800.0f, 0.0f, 0.0f);
    const std::string overflowPath =
        TempPath("plr_bus_mix_finite_overflow_v2.pack");
    WriteFileBytes(overflowPath, overflow.Build());

    EXPECT_EQ(VN_BAD_PACK, ReplacePack(pack, overflowPath));
    EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
    EXPECT_EQ(index, engine->buses.FindIndex(harkentest::ObjectId(13192)));
    EXPECT_EQ(generation, engine->buses.GenerationAt(index));
    EXPECT_FLOAT_EQ(authoredBefore,
                    engine->busTargets.SlotValue(
                        index, harken::kBusTargetBusVolume,
                        harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(apiBefore,
                    engine->busTargets.SlotValue(
                        index, harken::kBusTargetBusVolume,
                        harken::BusTargetStore::kSlotApiDirectIndex));
    EXPECT_FLOAT_EQ(committedBefore,
                    engine->busTargets.LastCommitted(
                        index, harken::kBusTargetBusVolume));
}

TEST(PackLogicReplace, SingleStageOverflowLeavesLoadedRevisionIntact) {
    for (const bool outputOnly : {false, true}) {
        SCOPED_TRACE(outputOnly);
        EngineSession session(PackLogicConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const float oldVolume = outputOnly ? 600.0f : 0.0f;
        const float oldOutput = outputOnly ? 0.0f : 600.0f;
        V2Builder first(harken::kPackKindResident);
        first.AddBusMix(harkentest::ObjectId(13193), VN_BUS_MASTER, oldVolume, oldOutput, 0.0f);
        const std::string firstPath = TempPath("plr_single_stage_v1.pack");
        WriteFileBytes(firstPath, first.Build());
        VnPackHandle pack = 0;
        ASSERT_EQ(VN_OK, LoadPack(firstPath, &pack));
        engine->nullBackend.Pump(1);
        const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(13193));
        ASSERT_NE(harken::kInvalidBusIndex, index);
        const auto* bus = engine->buses.BusState(index);
        ASSERT_NE(nullptr, bus);
        const float oldGain = bus->gain.target;
        const float oldOutputGain = bus->outputGain.target;
        const std::uint32_t generation = engine->buses.GenerationAt(index);

        V2Builder overflow(harken::kPackKindResident);
        overflow.AddBusMix(harkentest::ObjectId(13193), VN_BUS_MASTER, 600.0f, 600.0f, 0.0f);
        const std::string overflowPath = TempPath("plr_single_stage_v2.pack");
        WriteFileBytes(overflowPath, overflow.Build());
        EXPECT_EQ(VN_BAD_PACK, ReplacePack(pack, overflowPath));
        const auto* record = engine->logicPacks.RecordFor(pack);
        ASSERT_NE(nullptr, record);
        ASSERT_EQ(1u, record->buses.size());
        EXPECT_FLOAT_EQ(oldVolume, record->buses[0].volume);
        EXPECT_FLOAT_EQ(oldOutput, record->buses[0].outputBusVolume);
        EXPECT_EQ(generation, engine->buses.GenerationAt(index));
        EXPECT_FLOAT_EQ(oldVolume, engine->busTargets.LastCommitted(
                                      index, harken::kBusTargetBusVolume));
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        EXPECT_FLOAT_EQ(oldGain, bus->gain.target);
        EXPECT_FLOAT_EQ(oldOutputGain, bus->outputGain.target);
    }
}

TEST(PackLogicReplace, ConvertibleReducedMixPublishesAuthoredContribution) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    V2Builder first(harken::kPackKindResident);
    first.AddBusMix(harkentest::ObjectId(13194), VN_BUS_MASTER, 0.0f, 0.0f, 0.0f);
    const std::string firstPath = TempPath("plr_reduced_mix_v1.pack");
    WriteFileBytes(firstPath, first.Build());
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(firstPath, &pack));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(13194), -800.0f, 0));
    engine->nullBackend.Pump(1);

    V2Builder replacement(harken::kPackKindResident);
    replacement.AddBusMix(harkentest::ObjectId(13194), VN_BUS_MASTER, 800.0f, 0.0f, 0.0f);
    const std::string replacementPath = TempPath("plr_reduced_mix_v2.pack");
    WriteFileBytes(replacementPath, replacement.Build());
    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(13194));
    EXPECT_FLOAT_EQ(800.0f, engine->busTargets.SlotValue(
                               index, harken::kBusTargetBusVolume,
                               harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(-800.0f, engine->busTargets.SlotValue(
                                index, harken::kBusTargetBusVolume,
                                harken::BusTargetStore::kSlotApiDirectIndex));
    EXPECT_FLOAT_EQ(0.0f, engine->busTargets.LastCommitted(
                             index, harken::kBusTargetBusVolume));
    const auto* bus = engine->buses.BusState(index);
    ASSERT_NE(nullptr, bus);
    EXPECT_FLOAT_EQ(1.0f, bus->gain.target);
}

TEST(PackLogicLoad, FiniteAuthoredMixPlusApiDirectOverflowRefusesSetter) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    V2Builder builder(harken::kPackKindResident);
    builder.AddBusMix(harkentest::ObjectId(13141), VN_BUS_MASTER, -6.0f, 0.0f, 0.0f);
    const std::string packPath =
        TempPath("pll_bus_mix_full_reduce_setter.pack");
    WriteFileBytes(packPath, builder.Build());

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(13141));
    ASSERT_NE(harken::kInvalidBusIndex, index);
    EXPECT_FLOAT_EQ(
        -6.0f, engine->busTargets.SlotValue(
                   index, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FALSE(engine->busTargets.SlotActive(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex));
    const float committedBefore = engine->busTargets.LastCommitted(
        index, harken::kBusTargetBusVolume);

    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusVolume(harkentest::ObjectId(13141), 3.0e38f, 0));
    EXPECT_FLOAT_EQ(
        -6.0f, engine->busTargets.SlotValue(
                   index, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FALSE(engine->busTargets.SlotActive(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex));
    EXPECT_FLOAT_EQ(committedBefore,
                    engine->busTargets.LastCommitted(
                        index, harken::kBusTargetBusVolume));
}

TEST(PackLogicReplace, FiniteAuthoredMixPlusApiDirectOverflowLeavesLoadedRevisionIntact) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    V2Builder first(harken::kPackKindResident);
    first.AddBusMix(harkentest::ObjectId(13151), VN_BUS_MASTER, -6.0f, -3.5f, 2.0f);
    const std::string v1Path =
        TempPath("plr_bus_mix_full_reduce_v1.pack");
    WriteFileBytes(v1Path, first.Build());

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(13151));
    ASSERT_NE(harken::kInvalidBusIndex, index);
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusVolume(harkentest::ObjectId(13151), 3.0e38f, 0));
    EXPECT_FALSE(engine->busTargets.SlotActive(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(13151), -1.0f, 0));
    EXPECT_TRUE(engine->busTargets.SlotActive(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex));
    const float authoredBefore = engine->busTargets.SlotValue(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotAuthoredIndex);
    const float apiBefore = engine->busTargets.SlotValue(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex);
    EXPECT_FLOAT_EQ(-1.0f, apiBefore);
    const float committedBefore = engine->busTargets.LastCommitted(
        index, harken::kBusTargetBusVolume);
    const std::uint32_t generation = engine->buses.GenerationAt(index);

    // Finite 3e38/0/0 is no longer a legal LoadPack image. This v2 uses a
    // non-finite volume+makeup mix sum so ReplacePack stays VN_BAD_PACK
    // without planting an overflowing API-direct slot.
    V2Builder overflow(harken::kPackKindResident);
    overflow.AddBusMix(harkentest::ObjectId(13151), VN_BUS_MASTER, 3.0e38f, 0.0f, 3.0e38f);
    const std::string overflowPath =
        TempPath("plr_bus_mix_full_reduce_v2.pack");
    WriteFileBytes(overflowPath, overflow.Build());

    EXPECT_EQ(VN_BAD_PACK, ReplacePack(pack, overflowPath));
    EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
    EXPECT_EQ(index, engine->buses.FindIndex(harkentest::ObjectId(13151)));
    EXPECT_EQ(generation, engine->buses.GenerationAt(index));
    EXPECT_FLOAT_EQ(authoredBefore,
                    engine->busTargets.SlotValue(
                        index, harken::kBusTargetBusVolume,
                        harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(apiBefore,
                    engine->busTargets.SlotValue(
                        index, harken::kBusTargetBusVolume,
                        harken::BusTargetStore::kSlotApiDirectIndex));
    EXPECT_FLOAT_EQ(committedBefore,
                    engine->busTargets.LastCommitted(
                        index, harken::kBusTargetBusVolume));
}

TEST(PackLogicLoad, FiniteAuthoredMixPlusVariableOverflowLeavesAuthoredIntact) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(13161)}, 0.0f, 1.0f).status);

    V2Builder builder(harken::kPackKindResident);
    builder.AddBusMix(harkentest::ObjectId(13162), VN_BUS_MASTER, -6.0f, 0.0f, 0.0f);
    const std::string packPath =
        TempPath("pll_bus_mix_variable_overflow.pack");
    WriteFileBytes(packPath, builder.Build());

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(13162));
    ASSERT_NE(harken::kInvalidBusIndex, index);
    const float committedBefore = engine->busTargets.LastCommitted(
        index, harken::kBusTargetBusVolume);
    EXPECT_FLOAT_EQ(-6.0f, committedBefore);
    EXPECT_TRUE(engine->busTargets.SlotActive(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotAuthoredIndex));

    const VnBindingDesc binding =
        MakeBusVolumeBinding(harkentest::ObjectId(13161), 0.0f, 0.0f, 1.0f, 3.0e38f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(13163), &binding));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = harkentest::ObjectId(13162);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(13163), &attach));
    const auto dropsBefore = engine->paramDropCount;
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(13161), 1.0f));
    harkentest::AdvanceVariableWrite(engine);

    EXPECT_FLOAT_EQ(
        -6.0f, engine->busTargets.SlotValue(
                   index, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(
        -6.0f, engine->busTargets.Reduce(
                   index, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(committedBefore,
                    engine->busTargets.LastCommitted(
                        index, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.PendingPublication(
        index, harken::kBusTargetBusVolume));
    EXPECT_EQ(dropsBefore + 1u, engine->paramDropCount);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(dropsBefore + 1u, engine->paramDropCount);
}

TEST(PackLogicLoad, FiniteAuthoredMixPlusMeterProbeOverflowLeavesAuthoredIntact) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    V2Builder builder(harken::kPackKindResident);
    builder.AddBusMix(harkentest::ObjectId(13172), VN_BUS_MASTER, -6.0f, 0.0f, 0.0f);
    const std::string packPath =
        TempPath("pll_bus_mix_meter_overflow.pack");
    WriteFileBytes(packPath, builder.Build());

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    const std::uint32_t index = engine->buses.FindIndex(harkentest::ObjectId(13172));
    ASSERT_NE(harken::kInvalidBusIndex, index);
    const float committedBefore = engine->busTargets.LastCommitted(
        index, harken::kBusTargetBusVolume);
    EXPECT_FLOAT_EQ(-6.0f, committedBefore);
    const std::uint32_t generation =
        engine->busTargets.ActiveGeneration(index);

    // Enter the native probe lifecycle before injecting an orphan contribution.
    std::uint32_t probeGeneration = 0;
    ASSERT_EQ(VN_OK, harken::InstallMeterProbe(*engine,
        harkentest::ObjectId(13173), harkentest::ObjectId(13172),
        VN_BUS_METER_TAP_POST_EFFECTS, 0, 0, &probeGeneration));
    ASSERT_NE(0u, probeGeneration);

    engine->busTargets.BeginMeterProbeSweep();
    engine->busTargets.AccumulateMeterProbe(
        index, generation, harken::kBusTargetBusVolume, 3.0e38f, true, 17);
    engine->busTargets.EndMeterProbeSweep();
    // EndMeterProbeSweep is isfinite-only: -6 + 3e38 is finite, so the
    // meter slot lands here. Vn_Update's Evaluate then Begin/End-sweeps
    // with no live probe attachment and clears that slot before flushBuses,
    // so this store injection never reaches the dirty-drop path.
    EXPECT_TRUE(engine->busTargets.SlotActive(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotMeterProbeIndex));
    EXPECT_TRUE(std::isfinite(engine->busTargets.Reduce(
        index, harken::kBusTargetBusVolume)));
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_FLOAT_EQ(
        -6.0f, engine->busTargets.SlotValue(
                   index, harken::kBusTargetBusVolume,
                   harken::BusTargetStore::kSlotAuthoredIndex));
    EXPECT_FLOAT_EQ(committedBefore,
                    engine->busTargets.LastCommitted(
                        index, harken::kBusTargetBusVolume));
    EXPECT_FALSE(engine->busTargets.PendingPublication(
        index, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(
        -6.0f, engine->busTargets.Reduce(
                   index, harken::kBusTargetBusVolume));
    engine->nullBackend.Pump(1);
    harken::BusTree::Bus* bus = engine->buses.BusState(index);
    ASSERT_NE(nullptr, bus);
    EXPECT_TRUE(std::isfinite(bus->gain.current));
    EXPECT_TRUE(std::isfinite(bus->gain.target));
    EXPECT_EQ(0u, engine->buses.InvalidVolumeApplyCount());
}

// --- load: content happy path ---


TEST(PackLogicLoad, AuthoredToneRecipeShapesTheFirstRenderedBlock) {
    // The authored tone must ride the SAME admitted command batch as the
    // play (the "+4 tone" reservation): the first rendered block already
    // carries it, with no Vn_Update between the raw start and the first pump.
    // The pin is an absolute level, not a ratio against a settled block
    // from the same run: a regression that starves the whole run of tone
    // commands (priming retires the demand but the commands never reach
    // the batch) drags the first AND the settled block to the default
    // tone together, which a ratio oracle cannot see.
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Authored values chosen for a closed-form expectation: an RBJ low
    // shelf passes DC at exactly 10^(gainDb/20), and a 400 Hz corner
    // settles the step transient well inside one block (pole radius
    // ~0.968, decayed below 1e-4 of the transient by the block tail).
    // The +4 dB shelf against the recipe's fixed -6 dB slot-0 gain
    // effect keeps the expected level distinct from the raw DC source,
    // from a tone-less render, and from an effect-less render, so
    // dropping the tone, the insert effect, or both all land off the pin.
    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildToneRecipePack(
            "pll_tone_first_block",
            "      \"lowpassEnabled\": false, \"lowpassFrequencyHz\": "
            "8000.0, \"lowpassQ\": 0.707,\n"
            "      \"highpassEnabled\": false, \"highpassFrequencyHz\": "
            "80.0, \"highpassQ\": 0.707,\n"
            "      \"lowShelfEnabled\": true, \"lowShelfFrequencyHz\": "
            "400.0, \"lowShelfQ\": 0.707, \"lowShelfGainDb\": 4.0,\n"
            "      \"highShelfEnabled\": false, \"highShelfFrequencyHz\": "
            "6000.0, \"highShelfQ\": 0.707, \"highShelfGainDb\": 0.0\n");
    ASSERT_TRUE(artifact.built);
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(artifact.path, &pack));

    // The recipe source is DC 0.5 through the +4 dB shelf and the -6 dB
    // gain effect; both snap at voice birth (initial instantiation).
    const float expectedTail =
        0.5f * std::pow(10.0f, (4.0f - 6.0f) / 20.0f);

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(9101));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture first;
    harkentest::PumpAndCapture(engine, 1, first);
    ASSERT_EQ(kBlock, first.left.size());
    const float firstBlockTail = std::fabs(first.left.back());
    ASSERT_GT(firstBlockTail, harkentest::kSilenceFloor);
    EXPECT_NEAR(expectedTail, firstBlockTail, 2e-3f);

    // Second layer: after an update the settled level holds the same
    // absolute pin, proving the first block was already final rather
    // than transiently near it.
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture settled;
    harkentest::PumpAndCapture(engine, 4, settled);
    const float settledTail = std::fabs(settled.left.back());
    ASSERT_GT(settledTail, harkentest::kSilenceFloor);
    EXPECT_NEAR(expectedTail, settledTail, 2e-3f);
}

TEST(PackLogicLoad,
     VoiceEffectBindingRecipeShapesFirstBlockAndUnloadsAfterDrain) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(harkentest::ObjectId(7901),
                                         harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(
                  engine, &definition, 1).status);

    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 4, 0.5f);
    const std::string wavPath = TempPath("pll_scope2_recipe.wav");
    WriteWavF32(wavPath, source, 1, kRate);
    const std::string packPath = TempPath("pll_scope2_recipe.pack");
    const std::uint32_t target = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    ASSERT_TRUE(BuildPackFile(
        "pll_scope2_recipe",
        V2LogicManifest(
            packPath, "content",
            "\"entries\": [ { \"path\": \"" + wavPath +
                "\", \"id\": \"3da91a64-b947-4560-0000-000000001edf\" } ],\n"
                "\"bindings\": [ { \"id\": \"3da91a64-b947-4560-0000-000000001ede\", \"variable\": \"3da91a64-b947-4560-0000-000000001edd\", "
                "\"scope\": \"voiceEffectParam\", \"target\": " +
                std::to_string(target) +
                ", \"transitionMs\": 0, "
            "\"curve\": { \"points\": [[0.0, 0.0], [1.0, 6.0]], "
            "\"handles\": [[0.0, 0.0, 1.0, 6.0]] } } ],\n"
                "\"voiceRecipes\": [ { \"playable\": \"3da91a64-b947-4560-0000-000000001edf\", "
                "\"effects\": [ { \"slot\": 0, \"type\": \"gain\", "
                "\"descriptorVintage\": 2, \"bypass\": false, "
                "\"parameters\": [-6.0] } ], "
                "\"bindings\": [\"3da91a64-b947-4560-0000-000000001ede\"] } ]")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    ASSERT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(7902)));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7901), 1.0f));
    harkentest::AdvanceVariableWrite(engine);

    VnVoiceHandle voice = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7903));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture first;
    harkentest::PumpAndCapture(engine, 1, first);
    ASSERT_EQ(kBlock, first.left.size());
    EXPECT_NEAR(0.5f, std::fabs(first.left.back()), 2e-3f);

    // Public unregister remains strict even though the Recipe and definition
    // share one pack owner. A live instance also keeps whole-pack unload
    // atomic.
    EXPECT_EQ(VN_IN_USE, Vn_UnregisterBinding(harkentest::ObjectId(7902)));
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(7902)));

    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(7902)));
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(7903)).Valid());
}


TEST(PackLogicLoad, InvalidVoiceEffectRecipeLocatorsRefuseWithoutResidue) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerVoice = 2;
    config.gainEffectPoolCapacity = 4;
    config.delayEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    struct Case {
        const char* name;
        std::uint32_t target;
        std::uint32_t effectType;
        std::uint32_t effectSlot;
        std::uint32_t paramCount;
        float params[4];
        bool includeDefinition;
        VnResult expected;
    };
    const Case cases[] = {
        {"unknown_type", harken::PackEffectParamLocator(99, 0, 0),
         VN_EFFECT_GAIN, 0, 1, {-6.0f, 0.0f, 0.0f, 0.0f},
         true, VN_INVALID_ARG},
        {"bad_index",
         harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0, 1),
         VN_EFFECT_GAIN, 0, 1, {-6.0f, 0.0f, 0.0f, 0.0f},
         true, VN_INVALID_ARG},
        {"unbindable",
         harken::PackEffectParamLocator(VN_EFFECT_FEEDBACK_DELAY, 0, 0),
         VN_EFFECT_FEEDBACK_DELAY, 0, 4,
         {10.0f, 0.25f, 0.75f, 0.5f},
         true, VN_INVALID_ARG},
        {"slot_mismatch",
         harken::PackEffectParamLocator(VN_EFFECT_GAIN, 1, 0),
         VN_EFFECT_GAIN, 0, 1, {-6.0f, 0.0f, 0.0f, 0.0f},
         true, VN_INVALID_ARG},
        {"type_mismatch",
         harken::PackEffectParamLocator(VN_EFFECT_LIMITER, 0, 0),
         VN_EFFECT_GAIN, 0, 1, {-6.0f, 0.0f, 0.0f, 0.0f},
         true, VN_INVALID_ARG},
        {"missing_definition",
         harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0, 0),
         VN_EFFECT_GAIN, 0, 1, {-6.0f, 0.0f, 0.0f, 0.0f},
         false, VN_UNKNOWN_ID},
    };

    for (std::size_t i = 0; i < std::size(cases); ++i) {
        const Case& test = cases[i];
        const VnSoundId playable = harkentest::ObjectId(7920 + i);
        const VnBindingId bindingId = harkentest::ObjectId(7930 + i);
        const std::string packPath =
            TempPath(std::string("pll_scope2_") + test.name + ".pack");
        V2Builder builder(harken::kPackKindContent);
        builder.AddMedia(playable, harkentest::MakeDC(kBlock, 0.5f));
        if (test.includeDefinition) {
            VnBindingDesc desc{};
            desc.structSize = sizeof(VnBindingDesc);
            desc.variableId = harkentest::ObjectId(7991);
            desc.targetScopeKind = VN_TARGET_SCOPE_VOICE_EFFECT_PARAM;
            desc.targetId = test.target;
            desc.curve.structSize = sizeof(VnCurveDesc);
            desc.curve.pointCount = 1;
            desc.curve.points[0] = {0.0f, 0.0f};
            builder.AddBindingDefinition(bindingId, desc);
        }
        builder.AddVoiceRecipe(playable, bindingId, test.effectSlot,
                               test.effectType, test.params,
                               test.paramCount);
        WriteFileBytes(packPath, builder.Build());

        const std::uint32_t packsBefore = engine->packs.LoadedCount();
        const std::uint32_t definitionsBefore =
            engine->bindings.DefinitionCount();
        VnPackHandle pack = 0;
        EXPECT_EQ(test.expected, LoadPack(packPath, &pack)) << test.name;
        EXPECT_EQ(packsBefore, engine->packs.LoadedCount()) << test.name;
        EXPECT_EQ(definitionsBefore, engine->bindings.DefinitionCount())
            << test.name;
        EXPECT_FALSE(engine->registry.Lookup(playable).Valid()) << test.name;
        EXPECT_EQ(nullptr, engine->bindings.Find(bindingId)) << test.name;
        EXPECT_EQ(nullptr, engine->logicPacks.FindVoiceRecipe(playable))
            << test.name;
    }
}



TEST(PackLogicLoad, BindingTransitionRangeRefusesBeforeEngineStateChanges) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    for (std::uint32_t ms : {60000u, 60001u, UINT32_MAX}) {
        V2Builder builder(harken::kPackKindContent);
        VnBindingDesc desc{};
        desc.structSize = sizeof(desc);
        desc.variableId = harkentest::ObjectId(7991);
        desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
        desc.targetId = VN_TARGET_VOICE_VOLUME;
        desc.transitionMs = ms;
        desc.curve.structSize = sizeof(desc.curve);
        desc.curve.pointCount = 1;
        desc.curve.points[0] = {0.0f, 0.0f};
        builder.AddBindingDefinition(harkentest::ObjectId(7992), desc);
        const auto bytes = builder.Build();
        harken::PackLogicImage image;
        EXPECT_EQ(ms == 60000 ? VN_OK : VN_BAD_PACK,
            harken::ParsePackV2Image(bytes.data(), bytes.size(), &image));
        if (ms == 60000) continue;
        const std::string path = TempPath("binding_invalid_duration_" + std::to_string(ms) + ".pack");
        WriteFileBytes(path, bytes);
        const auto packs = engine->packs.LoadedCount();
        const auto definitions = engine->bindings.DefinitionCount();
        VnPackHandle pack = 0;
        EXPECT_EQ(VN_BAD_PACK, LoadPack(path, &pack));
        EXPECT_EQ(packs, engine->packs.LoadedCount());
        EXPECT_EQ(definitions, engine->bindings.DefinitionCount());
        EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(7992)));
    }
}

TEST(PackLogicUnload, ParkedScope2RecipeRefusesWithoutPartialUnload) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 4;
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> source = harkentest::MakeDC(kRate, 0.5f);
    const VnPCMDesc pcm = MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7960), &pcm));
    const VnAttenuationDesc attenuation =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(7961), &attenuation));

    const std::uint32_t target = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    const std::string packPath = TempPath("plu_scope2_parked.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_scope2_parked",
        V2LogicManifest(
            packPath, "content",
            "\"bindings\": [ { \"id\": \"3da91a64-b947-4560-0000-000000001f1c\", \"variable\": \"3da91a64-b947-4560-0000-000000001f1d\", "
            "\"scope\": \"voiceEffectParam\", \"target\": " +
                std::to_string(target) +
                ", \"transitionMs\": 0, \"curve\": { \"points\": "
                "[[0.0, 0.0], [1.0, 6.0]], \"handles\": "
                "[[0.0, 0.0, 1.0, 6.0]] } } ],\n"
                "\"voiceRecipes\": [ { \"playable\": \"3da91a64-b947-4560-0000-000000001f18\", "
                "\"effects\": [ { \"slot\": 0, \"type\": \"gain\", "
                "\"descriptorVintage\": 2, \"bypass\": false, "
                "\"parameters\": [-6.0] } ], \"bindings\": [\"3da91a64-b947-4560-0000-000000001f1c\"] } ]")));

    const auto listenerAt = [](float x) {
        return Vn_SetListener(harkentest::Vec3(x, 0.0f, 0.0f),
                              harkentest::Vec3(0.0f, 0.0f, -1.0f),
                              harkentest::Vec3(0.0f, 1.0f, 0.0f));
    };
    ASSERT_EQ(VN_OK, listenerAt(99.0f));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    VnPlayDesc play =
        harkentest::MakePlayDesc3D(harkentest::ObjectId(7960), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, harkentest::ObjectId(7961)));
    engine->nullBackend.Pump(1);
    // Acknowledge the initial read cut before the next audibility pass.
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto started = engine->ledger.Classify(voice);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, started.kind);
    ASSERT_FALSE(engine->variables.HasNativePublicationHold(
        started.renderHandle));
    ASSERT_EQ(VN_OK, listenerAt(0.0f));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(voice).kind);

    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_EQ(1u, engine->packs.LoadedCount());
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(7964)));
    EXPECT_NE(nullptr, engine->logicPacks.FindVoiceRecipe(harkentest::ObjectId(7960)));
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(voice).kind);
}

TEST(PackLogicLoad,
     GenericDelayAndToneEffectsAreFullyInstalledForTheFirstRenderedBlock) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerVoice = 2;
    config.delayEffectPoolCapacity = 2;
    config.toneEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // This is the generic kind-11 Effect chain. The manifest deliberately
    // contains no separately authored "tone" block.
    const harkentest::RecipePackArtifact artifact =
        BuildDelayAndToneRecipePack("pll_generic_delay_tone");
    ASSERT_TRUE(artifact.built);
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(artifact.path, &pack));

    VnVoiceHandle voice = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(9101));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture first;
    harkentest::PumpAndCapture(engine, 1, first);
    ASSERT_EQ(kBlock, first.left.size());

    // The source is DC 0.5. Delay is 480 samples, beyond this 256-frame
    // first block, so only its 0.5 dry path reaches the Tone. At steady DC
    // the peak and high shelf are unity; the +4 dB low shelf contributes
    // exactly 10^(4/20). This absolute pin distinguishes the full chain
    // (~0.396) from missing Delay (~0.792), missing Tone (0.25), and both
    // missing (0.5).
    const float expectedTail =
        0.5f * 0.5f * std::pow(10.0f, 4.0f / 20.0f);
    const float firstBlockTail = std::fabs(first.left.back());
    ASSERT_GT(firstBlockTail, harkentest::kSilenceFloor);
    EXPECT_NEAR(expectedTail, firstBlockTail, 2e-3f);

    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    harken::VoiceRenderContext* context = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    ASSERT_EQ(2u, context->fxSlotCount);
    EXPECT_EQ(VN_EFFECT_FEEDBACK_DELAY,
              context->fxSlotEffectRef[0].type);
    EXPECT_EQ(VN_EFFECT_THREE_BAND_TONE,
              context->fxSlotEffectRef[1].type);

    const auto* delay =
        static_cast<const harken::DelayEffectState*>(
            context->fxSlots[0].state);
    ASSERT_NE(nullptr, delay);
    EXPECT_EQ(kRate, delay->sampleRate);
    EXPECT_EQ(480u, delay->delaySamples);
    EXPECT_FLOAT_EQ(0.25f, delay->feedback);
    EXPECT_FLOAT_EQ(0.75f, delay->wet);
    EXPECT_FLOAT_EQ(0.5f, delay->dry);

    const auto* tone =
        static_cast<const harken::ToneEffectState*>(
            context->fxSlots[1].state);
    ASSERT_NE(nullptr, tone);
    EXPECT_EQ(kRate, tone->sampleRate);
    EXPECT_FLOAT_EQ(4.0f, tone->lowShelfGainDb);
    EXPECT_FLOAT_EQ(400.0f, tone->lowShelfFreqHz);
    EXPECT_FLOAT_EQ(-3.0f, tone->peakGainDb);
    EXPECT_FLOAT_EQ(1200.0f, tone->peakFreqHz);
    EXPECT_FLOAT_EQ(2.0f, tone->peakQ);
    EXPECT_FLOAT_EQ(-4.0f, tone->highShelfGainDb);
    EXPECT_FLOAT_EQ(6000.0f, tone->highShelfFreqHz);
}

// The playback block is emitted only for an authored non-default pass count,
// so a recipe that carries one outranks the descriptor's loopCount — here the
// full-vintage default of a single pass.
TEST(PackLogicLoad, RecipePlaybackLoopCountOverridesThePlayDescriptor) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const harkentest::RecipePackArtifact artifact =
        BuildPlaybackRecipePack("pll_playback_loop_count", 3);
    ASSERT_TRUE(artifact.built);
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(artifact.path, &pack));

    VnVoiceHandle voice = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(9101));
    ASSERT_EQ(1u, play.loopCount);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    harken::VoiceRenderContext* context = engine->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    EXPECT_FALSE(context->loopInfinite);
    EXPECT_EQ(3u, context->loopPassesRemaining);
}

VnBindingDesc CraftedBusEffectBinding() {
    VnBindingDesc desc{};
    desc.variableId = harkentest::ObjectId(7801);
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    desc.targetId = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    desc.curve.pointCount = 1;
    desc.curve.points[0] = {0.0f, 6.0f};
    return desc;
}

void PrepareMasterProbe(harken::Engine* engine, std::vector<float>* storage) {
    if (engine->variables.Definition(harkentest::ObjectId(7801)) == nullptr) {
        const harken::VariableDefinitionV1 definition =
            harkentest::MakeVariableDefinition(harkentest::ObjectId(7801),
                                             harken::VariableKindV1::Smooth,
                                             0.0f, 1.0f, 0.0f);
        ASSERT_EQ(harken::WorldInstallStatus::Ok,
                  harkentest::InstallDefinitionsWorld(
                      engine, &definition, 1).status);
    }
    const VnGainDesc gain = [] {
        VnGainDesc desc{};
        desc.structSize = sizeof(VnGainDesc);
        desc.gainDb = -6.0f;
        return desc;
    }();
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         VN_BUS_MASTER, 0, VN_EFFECT_GAIN,
                         reinterpret_cast<const VnEffectDesc*>(&gain)));
    *storage = harkentest::MakeDC(kBlock * 4, 0.5f);
    const VnPCMDesc pcm = MonoDesc(*storage, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &pcm));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(2));
    play.loopCount = 0;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7801), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    harkentest::Capture warm;
    harkentest::PumpAndCapture(engine, 2, warm);
}

float MasterProbeLevel(harken::Engine* engine) {
    harkentest::Capture captured;
    harkentest::PumpAndCapture(engine, 2, captured);
    return captured.left.empty() ? 0.0f : std::fabs(captured.left.back());
}

TEST(PackLogicLoad, BusEffectScope3LoadAppliesAndUnloadsAtomically) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    std::vector<float> probe;
    PrepareMasterProbe(engine, &probe);
    const float baseline = MasterProbeLevel(engine);
    EXPECT_NEAR(0.5f * std::pow(10.0f, -6.0f / 20.0f), baseline, 2e-3f);
    const std::uint32_t recordsBefore =
        engine->effectParamTargets.LiveRecordCount();
    const std::uint32_t publisherBefore =
        engine->propertyPublisher.WorstCaseCommits();

    V2Builder builder(harken::kPackKindContent);
    builder.AddBindingAttachment(0, harkentest::ObjectId(6001),
                                 VN_TARGET_SCOPE_BUS_EFFECT_PARAM,
                                 VN_BUS_MASTER);
    builder.AddBindingDefinition(harkentest::ObjectId(6001), CraftedBusEffectBinding());
    const std::string path = TempPath("pll_scope3_load.pack");
    WriteFileBytes(path, builder.Build());

    VnPackHandle handle = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &handle));
    ASSERT_NE(0u, handle);
    EXPECT_EQ(1u, engine->packs.LoadedCount());
    EXPECT_EQ(1u, engine->bindings.DefinitionCount());
    EXPECT_EQ(1u, engine->bindings.AttachmentCount());
    EXPECT_EQ(recordsBefore, engine->effectParamTargets.LiveRecordCount());
    EXPECT_EQ(publisherBefore,
              engine->propertyPublisher.WorstCaseCommits());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(0.5f, MasterProbeLevel(engine), 2e-3f);
    EXPECT_FLOAT_EQ(
        0.0f,
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Bus, 0, 0,
            harken::kGainParamGainDb));

    ASSERT_EQ(VN_OK, Vn_UnloadPack(handle));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(0u, engine->bindings.DefinitionCount());
    EXPECT_EQ(0u, engine->bindings.AttachmentCount());
    EXPECT_EQ(publisherBefore,
              engine->propertyPublisher.WorstCaseCommits());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(baseline, MasterProbeLevel(engine), 2e-3f);
}

TEST(PackLogicLoad, BusEffectScope3LoadRefusesStaleTargetsWithoutResidue) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    std::vector<float> probe;
    PrepareMasterProbe(engine, &probe);
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    const std::uint32_t recordsBefore =
        engine->effectParamTargets.LiveRecordCount();

    struct Case {
        const char* name;
        VnBusId bus;
        std::uint32_t target;
        VnResult expected;
    };
    const Case cases[] = {
        {"unknown_bus", harkentest::ObjectId(999),
         harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0,
                                       harken::kGainParamGainDb),
         VN_UNKNOWN_ID},
        {"empty_slot", harkentest::ObjectId(10),
         harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0,
                                       harken::kGainParamGainDb),
         VN_INVALID_ARG},
        {"wrong_type", VN_BUS_MASTER,
         harken::PackEffectParamLocator(VN_EFFECT_LIMITER, 0,
                                       harken::kLimiterParamCeilingDb),
         VN_INVALID_ARG},
        {"wrong_slot", VN_BUS_MASTER,
         harken::PackEffectParamLocator(VN_EFFECT_GAIN, 1,
                                       harken::kGainParamGainDb),
         VN_INVALID_ARG},
    };

    for (std::size_t i = 0; i < std::size(cases); ++i) {
        VnBindingDesc desc = CraftedBusEffectBinding();
        desc.targetId = cases[i].target;
        const VnBindingId bindingId = harkentest::ObjectId(6100 + i);
        V2Builder builder(harken::kPackKindContent);
        builder.AddBindingDefinition(bindingId, desc);
        builder.AddBindingAttachment(0, bindingId,
                                     VN_TARGET_SCOPE_BUS_EFFECT_PARAM,
                                     cases[i].bus);
        const std::string path =
            TempPath(std::string("pll_scope3_refuse_") + cases[i].name +
                     ".pack");
        WriteFileBytes(path, builder.Build());

        VnPackHandle handle = 0;
        const VnResult result = LoadPack(path, &handle);
        EXPECT_EQ(cases[i].expected, result) << cases[i].name;
        if (result == VN_OK) {
            ASSERT_NE(0u, handle);
            ASSERT_EQ(VN_OK, Vn_UnloadPack(handle));
        }
        EXPECT_EQ(0u, engine->packs.LoadedCount()) << cases[i].name;
        EXPECT_EQ(0u, engine->bindings.DefinitionCount()) << cases[i].name;
        EXPECT_EQ(0u, engine->bindings.AttachmentCount()) << cases[i].name;
        EXPECT_EQ(recordsBefore, engine->effectParamTargets.LiveRecordCount())
            << cases[i].name;
    }
}

TEST(PackLogicLoad, BusEffectScope3AttachmentUsesResolvedDenseBusIndex) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7801)}, 0.0f, 1.0f).status);

    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    VnGainDesc gain{};
    gain.structSize = sizeof(VnGainDesc);
    gain.gainDb = -6.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         harkentest::ObjectId(10), 0, VN_EFFECT_GAIN,
                         reinterpret_cast<const VnEffectDesc*>(&gain)));
    const std::vector<float> probe = harkentest::MakeDC(kBlock * 6, 0.5f);
    const VnPCMDesc pcm = MonoDesc(probe, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &pcm));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(2));
    play.busId = harkentest::ObjectId(10);
    play.loopCount = 0;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7801), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    const float baseline = MasterProbeLevel(engine);
    EXPECT_NEAR(0.5f * std::pow(10.0f, -6.0f / 20.0f), baseline, 2e-3f);

    V2Builder builder(harken::kPackKindContent);
    builder.AddBindingDefinition(harkentest::ObjectId(6201), CraftedBusEffectBinding());
    builder.AddBindingAttachment(0, harkentest::ObjectId(6201),
                                 VN_TARGET_SCOPE_BUS_EFFECT_PARAM, harkentest::ObjectId(10));
    const std::string path = TempPath("pll_scope3_dense_bus.pack");
    WriteFileBytes(path, builder.Build());
    VnPackHandle handle = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &handle));

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(0.5f, MasterProbeLevel(engine), 2e-3f);
    const std::uint32_t busIndex = engine->buses.FindIndex(harkentest::ObjectId(10));
    ASSERT_NE(harken::kInvalidBusIndex, busIndex);
    EXPECT_FLOAT_EQ(
        0.0f,
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Bus, busIndex, 0,
            harken::kGainParamGainDb));

    ASSERT_EQ(VN_OK, Vn_UnloadPack(handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(baseline, MasterProbeLevel(engine), 2e-3f);
}

TEST(PackLogicLoad, BusEffectScope3ActivationGapDiscardsRevisionWhole) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 4;
    config.limiterEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    std::vector<float> probe;
    PrepareMasterProbe(engine, &probe);
    const float baseline = MasterProbeLevel(engine);

    V2Builder initial(harken::kPackKindContent);
    initial.AddBindingDefinition(harkentest::ObjectId(6002), CraftedBusEffectBinding());
    initial.AddBindingAttachment(0, harkentest::ObjectId(6002),
                                 VN_TARGET_SCOPE_BUS_EFFECT_PARAM,
                                 VN_BUS_MASTER);
    const std::string initialPath = TempPath("pll_scope3_replace_v1.pack");
    WriteFileBytes(initialPath, initial.Build());
    VnPackHandle handle = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &handle));
    ASSERT_EQ(1u, engine->packs.LoadedCount());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(0.5f, MasterProbeLevel(engine), 2e-3f);

    VnBindingDesc revised = CraftedBusEffectBinding();
    revised.curve.points[0].y = 3.0f;
    V2Builder replacement(harken::kPackKindContent);
    replacement.AddBindingDefinition(harkentest::ObjectId(6002), revised);
    replacement.AddBindingAttachment(0, harkentest::ObjectId(6002),
                                      VN_TARGET_SCOPE_BUS_EFFECT_PARAM,
                                      VN_BUS_MASTER);
    const std::string replacementPath =
        TempPath("pll_scope3_replace_v2.pack");
    WriteFileBytes(replacementPath, replacement.Build());

    ASSERT_EQ(VN_OK, ReplacePack(handle, replacementPath));
    VnLimiterDesc limiter{};
    limiter.structSize = sizeof(VnLimiterDesc);
    limiter.ceilingDb = -1.0f;
    limiter.releaseMs = 100.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         VN_BUS_MASTER, 0, VN_EFFECT_LIMITER,
                         reinterpret_cast<const VnEffectDesc*>(&limiter)));
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(1u, engine->packs.LoadedCount());
    ASSERT_EQ(1u, engine->bindings.DefinitionCount());
    ASSERT_EQ(1u, engine->bindings.AttachmentCount());
    const harken::BindingTable::Definition* live =
        engine->bindings.Find(harkentest::ObjectId(6002));
    ASSERT_NE(nullptr, live);
    EXPECT_FLOAT_EQ(6.0f, live->curve.Eval(1.0f));

    VnGainDesc gain{};
    gain.structSize = sizeof(VnGainDesc);
    gain.gainDb = -6.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         VN_BUS_MASTER, 0, VN_EFFECT_GAIN,
                         reinterpret_cast<const VnEffectDesc*>(&gain)));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(0.5f, MasterProbeLevel(engine), 2e-3f);

    ASSERT_EQ(VN_OK, Vn_UnloadPack(handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(baseline, MasterProbeLevel(engine), 2e-3f);
}

TEST(PackLogicLoad, BusEffectScope3ReplaceAddsAttachmentToResolvedBus) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7801)}, 0.0f, 1.0f).status);

    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    VnGainDesc gain{};
    gain.structSize = sizeof(VnGainDesc);
    gain.gainDb = -6.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         harkentest::ObjectId(10), 0, VN_EFFECT_GAIN,
                         reinterpret_cast<const VnEffectDesc*>(&gain)));
    const std::vector<float> probe = harkentest::MakeDC(kBlock * 6, 0.5f);
    const VnPCMDesc pcm = MonoDesc(probe, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &pcm));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(2));
    play.busId = harkentest::ObjectId(10);
    play.loopCount = 0;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7801), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    const float baseline = MasterProbeLevel(engine);

    V2Builder initial(harken::kPackKindContent);
    initial.AddBindingDefinition(harkentest::ObjectId(6301), CraftedBusEffectBinding());
    const std::string initialPath =
        TempPath("pll_scope3_add_attachment_v1.pack");
    WriteFileBytes(initialPath, initial.Build());
    VnPackHandle handle = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &handle));
    ASSERT_EQ(0u, engine->bindings.AttachmentCount());
    const std::uint32_t publisherBefore =
        engine->propertyPublisher.WorstCaseCommits();

    V2Builder replacement(harken::kPackKindContent);
    replacement.AddBindingDefinition(harkentest::ObjectId(6301), CraftedBusEffectBinding());
    replacement.AddBindingAttachment(0, harkentest::ObjectId(6301),
                                      VN_TARGET_SCOPE_BUS_EFFECT_PARAM, harkentest::ObjectId(10));
    const std::string replacementPath =
        TempPath("pll_scope3_add_attachment_v2.pack");
    WriteFileBytes(replacementPath, replacement.Build());
    ASSERT_EQ(VN_OK, ReplacePack(handle, replacementPath));
    ASSERT_EQ(VN_OK, Vn_Update());

    ASSERT_EQ(1u, engine->bindings.AttachmentCount());
    EXPECT_EQ(publisherBefore,
              engine->propertyPublisher.WorstCaseCommits());
    EXPECT_NEAR(0.5f, MasterProbeLevel(engine), 2e-3f);
    const std::uint32_t busIndex = engine->buses.FindIndex(harkentest::ObjectId(10));
    ASSERT_NE(harken::kInvalidBusIndex, busIndex);
    EXPECT_FLOAT_EQ(
        0.0f,
        engine->effectParamTargets.LastCommitted(
            harken::EffectParamTargetStore::Scope::Bus, busIndex, 0,
            harken::kGainParamGainDb));

    ASSERT_EQ(VN_OK, Vn_UnloadPack(handle));
    EXPECT_EQ(publisherBefore,
              engine->propertyPublisher.WorstCaseCommits());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NEAR(baseline, MasterProbeLevel(engine), 2e-3f);
}

// --- load: cross-pack closure ---

TEST(PackLogicLoad, ResidentOnlyKindRefusedAtLoad) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::uint32_t busesBefore = engine->buses.BusCount();
    const std::uint64_t registryBefore = engine->registry.Count();

    // A CONTENT-kind image carrying a bus entry: packc never emits this
    // shape, so the loader's own refusal is what stands between a
    // tampered file and an unreclaimable bus.
    {
        V2Builder builder(harken::kPackKindContent);
        builder.AddBus(harkentest::ObjectId(751), VN_BUS_MASTER);
        builder.AddMedia(harkentest::ObjectId(752), harkentest::MakeDC(64, 0.5f));
        const std::string path = TempPath("pll_kind_content_bus.pack");
        WriteFileBytes(path, builder.Build());
        VnPackHandle pack = 0;
        EXPECT_EQ(VN_BAD_PACK, LoadPack(path, &pack));
    }
    // A RESIDENT-kind image carrying media.
    {
        V2Builder builder(harken::kPackKindResident);
        builder.AddBus(harkentest::ObjectId(753), VN_BUS_MASTER);
        builder.AddMedia(harkentest::ObjectId(754), harkentest::MakeDC(64, 0.5f));
        const std::string path = TempPath("pll_kind_resident_media.pack");
        WriteFileBytes(path, builder.Build());
        VnPackHandle pack = 0;
        EXPECT_EQ(VN_BAD_PACK, LoadPack(path, &pack));
    }

    EXPECT_EQ(busesBefore, engine->buses.BusCount());
    EXPECT_EQ(registryBefore, engine->registry.Count());
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(751), -1.0f, 0));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(753), -1.0f, 0));
}

TEST(PackLogicLoad, CompilerRefusesResidentSectionInContentManifest) {
    // The compiler half of the defense in depth: a content manifest
    // carrying a "buses" section refuses at build with a report.
    const std::vector<float> tone = harkentest::MakeDC(kBlock, 0.5f);
    const std::string wavPath = TempPath("pll_kind_twin_tone.wav");
    WriteWavF32(wavPath, tone, 1, kRate);

    const std::string packPath = TempPath("pll_kind_twin.pack");
    const std::string manifestPath = TempPath("pll_kind_twin_manifest.json");
    WriteTextFile(
        manifestPath,
        V2LogicManifest(packPath, "content",
                        "\"entries\": [ { \"path\": \"" + wavPath +
                            "\", \"id\": \"3da91a64-b947-4560-0000-0000000002f9\" } ],\n"
                            R"("buses": [ { "id": "3da91a64-b947-4560-0000-0000000002fa", "parent": "00000000-0000-0000-0000-000000000001" } ])"));
    std::filesystem::remove(packPath);
    std::filesystem::remove(packPath + ".report.json");
    std::filesystem::remove(manifestPath + ".report.json");

    EXPECT_NE(0, RunPackc({manifestPath}));
    EXPECT_FALSE(std::filesystem::exists(packPath));

    // A refused build still emits its report (the v1 refusal contract,
    // unchanged for v2). A manifest refused during parsing never
    // registered its output path, so the report-location fallback lands
    // the report beside the MANIFEST — the malformed-JSON precedent —
    // never beside the output it refused to name.
    EXPECT_FALSE(std::filesystem::exists(packPath + ".report.json"));
    std::string reportText;
    ASSERT_TRUE(ReadFileText(manifestPath + ".report.json", &reportText));
    harkenhost::JsonValue report;
    std::string error;
    ASSERT_TRUE(harkenhost::ParseJson(reportText, &report, &error)) << error;
    const harkenhost::JsonValue* status = report.Find("status");
    ASSERT_NE(nullptr, status);
    EXPECT_EQ("refused", status->text);
    const harkenhost::JsonValue* refusal = report.Find("refusal");
    ASSERT_NE(nullptr, refusal);
    const harkenhost::JsonValue* message = refusal->Find("message");
    ASSERT_NE(nullptr, message);
    EXPECT_FALSE(message->text.empty());
}

TEST(PackLogicLoad, CompilerRefusesRetiredActionsSection) {
    const std::vector<float> tone = harkentest::MakeDC(kBlock, 0.5f);
    const std::string wavPath = TempPath("pll_dupname_tone.wav");
    WriteWavF32(wavPath, tone, 1, kRate);

    const std::string packPath = TempPath("pll_dupname.pack");
    const std::string manifestPath = TempPath("pll_dupname_manifest.json");
    WriteTextFile(
        manifestPath,
        V2LogicManifest(
            packPath, "content",
            "\"entries\": [ { \"path\": \"" + wavPath +
                "\", \"id\": \"3da91a64-b947-4560-0000-0000000003d5\" } ],\n" +
                std::string(
                    R"("actions": [ { "id": "3da91a64-b947-4560-0000-0000000003d6", "steps": [
      { "type": "play", "playable": "3da91a64-b947-4560-0000-0000000003d5", "bus": "00000000-0000-0000-0000-000000000001" } ] } ])")));
    std::filesystem::remove(packPath);

    EXPECT_NE(0, RunPackc({manifestPath}));
    EXPECT_FALSE(std::filesystem::exists(packPath));
}


// --- compiler determinism: byte-identical output across runs ---

TEST(PackLogicLoad, CompilerOutputByteIdenticalAcrossRuns) {
    // A full-shape content manifest: media, container, voice binding, and
    // attachment. The same manifest builds three times — clean,
    // cache-hit, and cache-deleted — and the cache discipline demands
    // the pack bytes never move: a cache may only save work, never
    // change output.
    const std::vector<float> tone = harkentest::MakeDC(kBlock * 2, 0.5f);
    const std::string wavPath = TempPath("pll_byteident_tone.wav");
    WriteWavF32(wavPath, tone, 1, kRate);

    const std::string packPath = TempPath("pll_byteident.pack");
    const std::string manifestPath = TempPath("pll_byteident_manifest.json");
    WriteTextFile(
        manifestPath,
        V2LogicManifest(
            packPath, "content",
            "\"entries\": [ { \"path\": \"" + wavPath +
                "\", \"id\": \"3da91a64-b947-4560-0000-0000000003cb\" } ],\n" +
                std::string(
                    R"("containers": [ { "id": "3da91a64-b947-4560-0000-0000000003cc", "order": "weightedRandom",
                    "children": [ { "playable": "3da91a64-b947-4560-0000-0000000003cb", "weight": 1 } ] } ],
"bindings": [ { "id": "3da91a64-b947-4560-0000-0000000003ce", "variable": "3da91a64-b947-4560-0000-000000001d39", "scope": "voice", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-0000000003ce", "scope": "voice", "object": "3da91a64-b947-4560-0000-0000000003cb" } ])")));

    const std::string cachePath = packPath + ".cache.json";
    const std::string reportPath = packPath + ".report.json";
    std::filesystem::remove(packPath);
    std::filesystem::remove(cachePath);
    std::filesystem::remove(reportPath);

    // Build #1: clean decode; the report appears and the cache seeds.
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::string first;
    ASSERT_TRUE(ReadFileText(packPath, &first));
    ASSERT_FALSE(first.empty());
    EXPECT_TRUE(std::filesystem::exists(reportPath));
    ASSERT_TRUE(std::filesystem::exists(cachePath));

    // Build #2: nothing touched, so every entry may serve from the
    // cache — and the output must still be byte-identical.
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::string second;
    ASSERT_TRUE(ReadFileText(packPath, &second));
    EXPECT_TRUE(std::filesystem::exists(reportPath));
    EXPECT_TRUE(first == second) << "cache-hit rebuild moved the pack bytes";

    // Build #3: only the cache file removed — a full re-decode of the
    // unchanged sources must land on the same bytes again.
    ASSERT_TRUE(std::filesystem::remove(cachePath));
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::string third;
    ASSERT_TRUE(ReadFileText(packPath, &third));
    EXPECT_TRUE(std::filesystem::exists(reportPath));
    EXPECT_TRUE(first == third)
        << "clean re-decode diverged from the cache-assisted build";
}

// --- load: corruption ---

TEST(PackLogicLoad, CorruptedPackLeavesEngineUntouched) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    V2Builder builder(harken::kPackKindResident);
    builder.AddBus(harkentest::ObjectId(771), VN_BUS_MASTER);
    const std::vector<std::uint8_t> intact = builder.Build();

    const std::uint32_t busesBefore = engine->buses.BusCount();
    const std::uint32_t variablesBefore = engine->params.VariableCount();

    // One flipped payload byte: the payload checksum refuses.
    {
        std::vector<std::uint8_t> bytes = intact;
        bytes[static_cast<std::size_t>(builder.PayloadOffset(0)) + 3] ^= 0x01;
        const std::string path = TempPath("pll_corrupt_payload.pack");
        WriteFileBytes(path, bytes);
        VnPackHandle pack = 0;
        EXPECT_EQ(VN_BAD_PACK, LoadPack(path, &pack));
    }
    // One flipped directory byte: the directory checksum refuses.
    {
        std::vector<std::uint8_t> bytes = intact;
        bytes[harken::kPackV2HeaderBytes + 16] ^= 0x01;
        const std::string path = TempPath("pll_corrupt_directory.pack");
        WriteFileBytes(path, bytes);
        VnPackHandle pack = 0;
        EXPECT_EQ(VN_BAD_PACK, LoadPack(path, &pack));
    }

    // Zero side effects across both refusals.
    EXPECT_EQ(busesBefore, engine->buses.BusCount());
    EXPECT_EQ(variablesBefore, engine->params.VariableCount());
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(771), -1.0f, 0));

    // The intact file still loads cleanly afterwards.
    const std::string intactPath = TempPath("pll_corrupt_intact.pack");
    WriteFileBytes(intactPath, intact);
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(intactPath, &pack));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(771), -1.0f, 0));
}

TEST(PackLogicLoad, LegacyVariableEntryKindsRefuseWithZeroSideEffects) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    V2Builder variableBuilder(harken::kPackKindResident);
    variableBuilder.AddBus(harkentest::ObjectId(772), VN_BUS_MASTER);
    variableBuilder.AddDiscreteVariable(harkentest::ObjectId(7351));
    const std::string variablePath = TempPath("pll_legacy_variable.pack");
    WriteFileBytes(variablePath, variableBuilder.Build());

    V2Builder meterBuilder(harken::kPackKindResident);
    meterBuilder.AddBus(harkentest::ObjectId(773), VN_BUS_MASTER);
    meterBuilder.AddMeterWiring(harkentest::ObjectId(773), harkentest::ObjectId(7352));
    const std::string meterPath = TempPath("pll_legacy_meter.pack");
    WriteFileBytes(meterPath, meterBuilder.Build());

    const std::uint32_t busesBefore = engine->buses.BusCount();
    const std::uint32_t variablesBefore = engine->params.VariableCount();
    for (const std::string& path : {variablePath, meterPath}) {
        VnPackHandle pack = 0;
        EXPECT_EQ(VN_BAD_PACK, LoadPack(path, &pack));
        EXPECT_EQ(0u, pack);
    }
    EXPECT_EQ(busesBefore, engine->buses.BusCount());
    EXPECT_EQ(variablesBefore, engine->params.VariableCount());
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(772), -1.0f, 0));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(773), -1.0f, 0));
}

// --- staging atomicity ---


TEST(PackLogicStaging, BusPoolExhaustionRefusesWholeLoad) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.maxBuses = 3; // master + 2: three pack buses cannot fit
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string packPath = TempPath("pll_no_bus.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_no_bus",
        V2LogicManifest(packPath, "resident",
                        R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000321", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000322", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000323", "parent": "00000000-0000-0000-0000-000000000001" } ])")));

    VnPackHandle pack = 0;
    EXPECT_EQ(VN_NO_BUS, LoadPack(packPath, &pack));

    // Zero mutation: no bus landed.
    EXPECT_EQ(1u, engine->buses.BusCount()); // the master alone
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(801), -1.0f, 0));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
}

TEST(PackLogicStaging, BindingPoolExhaustionRefusesWholeLoad) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.maxBindings = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // The single definition slot is already taken by a live registration.
    const VnBindingDesc live = MakeBusVolumeBinding(harkentest::ObjectId(7410), 0.0f, 0.0f,
                                                    1.0f, -3.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(890), &live));

    // The pack stages an attenuation before the binding pool refuses;
    // order-independence demands that neither object lands.
    const std::string packPath = TempPath("pll_no_binding.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_no_binding",
        V2LogicManifest(
            packPath, "resident",
            R"("attenuations": [ { "id": "3da91a64-b947-4560-0000-00000000037b", "rolloff": "linear",
                    "minDistance": 1.0, "maxDistance": 100.0,
                    "airAbsorptionNearHz": 1000000000.0,
                    "airAbsorptionFarHz": 1000000000.0 } ],
"bindings": [ { "id": "3da91a64-b947-4560-0000-00000000037c", "variable": "3da91a64-b947-4560-0000-000000001cf4", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ])")));

    const std::uint32_t profilesBefore = engine->spatial.ProfileCount();
    VnPackHandle pack = 0;
    EXPECT_EQ(VN_NO_BINDING, LoadPack(packPath, &pack));

    EXPECT_EQ(profilesBefore, engine->spatial.ProfileCount());
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(892)));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
}

TEST(PackLogicStaging, CommandQueuePressureRefusesThenAdmits) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.commandQueueDepth = 2; // rounds to 2; easy to fill
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string packPath = TempPath("pll_backpressure.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_backpressure",
        V2LogicManifest(packPath, "resident",
                        R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000335", "parent": "00000000-0000-0000-0000-000000000001" } ])")));

    // Fill the command queue without pumping (the pressure technique).
    VnResult r = VN_OK;
    int pushed = 0;
    while ((r = Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0)) == VN_OK &&
           pushed < 64) {
        ++pushed;
    }
    ASSERT_EQ(VN_BACKPRESSURE, r);

    // A bus-bearing load needs command admission for its plan publish:
    // refused whole, zero mutation.
    VnPackHandle pack = 0;
    EXPECT_EQ(VN_BACKPRESSURE, LoadPack(packPath, &pack));
    EXPECT_EQ(1u, engine->buses.BusCount());
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(821), -1.0f, 0));
    EXPECT_EQ(0u, engine->packs.LoadedCount());

    // One rendered block drains the queue; the same load succeeds.
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(821), -1.0f, 0));
}

// An activation batch is indivisible: the audio thread applies all of it in
// one block or none of it, because half a topology transaction is worse than
// the whole one arriving a block later.
TEST(PackLogicStaging, IndivisibleActivationBatchWaitsForABlockThatTakesItWhole) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1);

    // One bus carrying one effect: the plan swap plus the effect install, two
    // commands published as one batch.
    const std::string packPath = TempPath("pll_atomic_batch.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_atomic_batch",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-00000000033f", "parent": "00000000-0000-0000-0000-000000000001" } ],)"
            "\n"
            R"("busEffects": [ { "bus": "3da91a64-b947-4560-0000-00000000033f", "slot": 0, "type": "gain",)"
            R"( "descriptorVintage": 2, "bypass": false,)"
            R"( "faderPointGuard": 1, "parameters": [-6.0] } ])")));

    // A ceiling of three with two singletons queued ahead leaves room for one
    // more command -- one short of the batch.
    engine->mixer.SetMaxCommandsPerQuantumForTest(3);
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -2.0f, 0));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    ASSERT_EQ(4u, engine->commands.PublishedHead() -
                      engine->commands.ConsumedCount());
    ASSERT_EQ(1u, Snap().busCount);

    // The block takes the two singletons and leaves the batch alone: the plan
    // is not installed, so the audio side still sees one bus. A drain that cut
    // between the batch's own records would have swapped the plan in here,
    // with its effect install still queued.
    engine->nullBackend.Pump(1);
    EXPECT_EQ(2u, engine->mixer.CommandsDeferredLastBlock());
    EXPECT_EQ(1u, Snap().busCount);

    // The next block has room for all of it.
    engine->nullBackend.Pump(1);
    EXPECT_EQ(0u, engine->mixer.CommandsDeferredLastBlock());
    EXPECT_EQ(2u, Snap().busCount);
    EXPECT_EQ(0u, engine->commands.PublishedHead() -
                      engine->commands.ConsumedCount());
}

// A batch wider than the queue itself can never be applied, however long the
// caller waits. That is a mismatch between the operation and the configured
// capacities, so it is refused as one -- reporting backpressure would send the
// caller round a retry loop that cannot terminate.
TEST(PackLogicStaging, ActivationBatchWiderThanTheQueueRefusesAsAConfigMismatch) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.commandQueueDepth = 2; // rounds to 2
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1);

    // Two buses, one effect each: the plan swap plus two effect installs, a
    // three-command batch a two-slot queue can never hold.
    const std::string packPath = TempPath("pll_oversized_batch.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_oversized_batch",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000349", "parent": "00000000-0000-0000-0000-000000000001" },)"
            R"( { "id": "3da91a64-b947-4560-0000-00000000034a", "parent": "00000000-0000-0000-0000-000000000001" } ],)"
            "\n"
            R"("busEffects": [ { "bus": "3da91a64-b947-4560-0000-000000000349", "slot": 0, "type": "gain",)"
            R"( "descriptorVintage": 2, "bypass": false,)"
            R"( "faderPointGuard": 1, "parameters": [-6.0] },)"
            R"( { "bus": "3da91a64-b947-4560-0000-00000000034a", "slot": 0, "type": "gain",)"
            R"( "descriptorVintage": 2, "bypass": false,)"
            R"( "faderPointGuard": 1, "parameters": [-3.0] } ])")));

    // The queue is empty, so nothing here is transient shortage.
    ASSERT_EQ(0u, engine->commands.PublishedHead() -
                      engine->commands.ConsumedCount());
    VnPackHandle pack = 0;
    EXPECT_EQ(VN_INVALID_CONFIG, LoadPack(packPath, &pack));

    // Draining changes nothing, which is exactly why it is not backpressure.
    engine->nullBackend.Pump(4);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_INVALID_CONFIG, LoadPack(packPath, &pack));

    // Refused whole, and the engine keeps serving.
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(1u, engine->buses.BusCount());
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(841), -1.0f, 0));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
}

// The other half of the same rule: a batch the queue could hold but one block
// could never apply. Admission tests the ceiling the drain actually honours, so
// tightening that ceiling tightens admission with it -- without which this batch
// would be accepted and then deferred for ever, since the drain may not split it.
TEST(PackLogicStaging, ActivationBatchWiderThanOneBlockRefusesAsAConfigMismatch) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.commandQueueDepth = 1024; // the queue is not the constraint here
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1);

    const std::string packPath = TempPath("pll_batch_over_ceiling.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_batch_over_ceiling",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000353", "parent": "00000000-0000-0000-0000-000000000001" },)"
            R"( { "id": "3da91a64-b947-4560-0000-000000000354", "parent": "00000000-0000-0000-0000-000000000001" } ],)"
            "\n"
            R"("busEffects": [ { "bus": "3da91a64-b947-4560-0000-000000000353", "slot": 0, "type": "gain",)"
            R"( "descriptorVintage": 2, "bypass": false,)"
            R"( "faderPointGuard": 1, "parameters": [-6.0] },)"
            R"( { "bus": "3da91a64-b947-4560-0000-000000000354", "slot": 0, "type": "gain",)"
            R"( "descriptorVintage": 2, "bypass": false,)"
            R"( "faderPointGuard": 1, "parameters": [-3.0] } ])")));

    // Three commands into a 1024-slot queue whose blocks apply two.
    engine->mixer.SetMaxCommandsPerQuantumForTest(2);
    VnPackHandle pack = 0;
    EXPECT_EQ(VN_INVALID_CONFIG, LoadPack(packPath, &pack));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(1u, engine->buses.BusCount());

    // Raising the ceiling to fit the batch is what makes it loadable, which is
    // what "configuration mismatch" claimed.
    engine->mixer.SetMaxCommandsPerQuantumForTest(3);
    EXPECT_EQ(VN_OK, LoadPack(packPath, &pack));
    engine->nullBackend.Pump(1);
    EXPECT_EQ(0u, engine->mixer.CommandsDeferredLastBlock());
    EXPECT_EQ(3u, Snap().busCount);
}

// This deliberately uses only the public configuration field: the two engine
// sessions differ in maxControlCommandsPerQuantum and therefore pin the whole
// public config -> resolved config -> Mixer -> batch admission/drain chain.
TEST(PackLogicStaging, PublicCommandCeilingControlsLiveBatchAdmissionAndDrain) {
    constexpr std::uint32_t kBatch = 3;
    constexpr std::uint32_t kSlotsPerBus = 1;
    const std::string packPath = TempPath("pll_public_command_ceiling.pack");
    ASSERT_TRUE(BuildWideBatchPack("pll_public_command_ceiling", packPath,
                                   harkentest::ObjectId(855), kBatch, kSlotsPerBus));

    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.commandQueueDepth = 1024;
    config.fxSlotsPerBus = kSlotsPerBus;
    config.gainEffectPoolCapacity = 4;
    config.maxControlCommandsPerQuantum = 2;
    {
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        engine->nullBackend.Pump(1);

        VnPackHandle pack = 0;
        EXPECT_EQ(VN_INVALID_CONFIG, LoadPack(packPath, &pack));
        EXPECT_EQ(0u, engine->packs.LoadedCount());
        EXPECT_EQ(1u, engine->buses.BusCount());
    }

    config.maxControlCommandsPerQuantum = kBatch;
    {
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        engine->nullBackend.Pump(1);

        VnPackHandle pack = 0;
        ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
        ASSERT_EQ(kBatch, engine->commands.PublishedHead() -
                              engine->commands.ConsumedCount());
        engine->nullBackend.Pump(1);
        EXPECT_EQ(0u, engine->mixer.CommandsDeferredLastBlock());
        EXPECT_EQ(0u, engine->commands.PublishedHead() -
                          engine->commands.ConsumedCount());
        EXPECT_EQ(3u, Snap().busCount);
    }
}

// The ceiling's own worst case, at the shipped default rather than a test hook:
// a single indivisible batch exactly as wide as one block may apply. Everything
// else about the budget was measured on single commands, which bound the drain
// by letting it stop anywhere; this batch may not be cut, so the block has to
// take all 256 records or none, and the default ceiling is what says it can.
//
// This is the case the ceiling exists for, so it is the case whose cost has to
// be known rather than extrapolated from cheaper traffic.
TEST(PackLogicStaging, TheDefaultCeilingAdmitsAMaximalBatchAndOneBlockTakesIt) {
    constexpr std::uint32_t kBatch = 256; // the shipped default ceiling
    constexpr std::uint32_t kSlotsPerBus = 8;
    constexpr std::uint32_t kDeadlineUs = 1000000u * kBlock / kRate;
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    // Enough buses for 255 effects at eight slots each, plus master, and a gain
    // pool that can actually hand out 255 instances. Neither is the subject
    // here: they only have to stop being the binding constraint before the
    // command ceiling is.
    config.maxBuses = 64;
    config.fxSlotsPerBus = kSlotsPerBus;
    config.gainEffectPoolCapacity = 256;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1);
    // The default is what is under test, so nothing may retune it.
    ASSERT_EQ(kBatch, engine->mixer.MaxCommandsPerQuantum());
    ASSERT_EQ(kBatch, engine->commands.Capacity())
        << "the queue must admit the batch too, or this measures the queue";

    const std::string packPath = TempPath("pll_maximal_batch.pack");
    ASSERT_TRUE(BuildWideBatchPack("pll_maximal_batch", packPath, harkentest::ObjectId(8600), kBatch,
                                   kSlotsPerBus));

    // Admitted: a batch at the ceiling fits, where one record more would be the
    // configuration mismatch the neighbouring test pins.
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack))
        << "VN_BAD_PACK (-401) here is a fixture fault, not an engine one: the "
           "most likely cause is a fader guard that no longer equals "
           "config.fxSlotsPerBus (" << kSlotsPerBus << "), which a pack "
           "declaring its own Buses is required to match";
    ASSERT_EQ(kBatch, engine->commands.PublishedHead() -
                          engine->commands.ConsumedCount())
        << "the activation did not publish one maximal batch";

    // One block takes the whole batch: nothing deferred, and every bus the plan
    // carried is audible-side visible.
    engine->nullBackend.Pump(1);
    const VnSnapshot snapshot = Snap();
    EXPECT_EQ(0u, engine->mixer.CommandsDeferredLastBlock())
        << "an indivisible batch at the ceiling was left for a later block";
    EXPECT_EQ(0u, engine->commands.PublishedHead() -
                      engine->commands.ConsumedCount());
    EXPECT_EQ(1u + (kBatch - 1u + kSlotsPerBus - 1u) / kSlotsPerBus,
              snapshot.busCount);

    // The cost of that block, measured rather than inferred from single-command
    // traffic. A quarter of the block period is a bound, not an expectation:
    // heterogeneous 256-command drains have measured in the single-digit
    // microseconds, so this leaves room for a scheduling hiccup without letting
    // a real regression through.
    RecordProperty("maximalBatchDrainUs",
                   std::to_string(snapshot.audioDrainLastUs));
    EXPECT_LT(snapshot.audioDrainLastUs, kDeadlineUs / 4u)
        << "a maximal batch drained in " << snapshot.audioDrainLastUs
        << " us of a " << kDeadlineUs << " us block";
}

// A staged replacement keeps the old revision live until Vn_Update. The
// replacement adds 256 non-bypassed effects and one topology-plan header, so
// its command scratch must account for all 257 records before activation.
TEST(PackLogicReplace,
     PlanHeaderCountsTowardOverCeilingRevisionWithoutPartialApply) {
    constexpr std::uint32_t kCeiling = 256;
    constexpr std::uint32_t kBatch = kCeiling + 1u;
    constexpr std::uint32_t kSlotsPerBus = 8;
    constexpr VnBusId kFirstBusId = harkentest::ObjectId(8800);

    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.commandQueueDepth = 512;
    config.maxBuses = 64;
    config.fxSlotsPerBus = kSlotsPerBus;
    config.gainEffectPoolCapacity = kCeiling;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(1);

    ASSERT_EQ(kCeiling, engine->mixer.MaxCommandsPerQuantum());
    ASSERT_GT(engine->commands.Capacity(), kBatch)
        << "the queue must not be the binding capacity for this pin";

    const std::string initialPath =
        TempPath("plr_plan_header_overflow_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_plan_header_overflow_v1",
        V2LogicManifest(
            initialPath, "resident",
            "\"buses\": [ { \"id\": " +
                harkentest::JsonObjectId(kFirstBusId) +
                ", \"parent\": \"00000000-0000-0000-0000-000000000001\" } ]")));
    const std::string replacementPath =
        TempPath("plr_plan_header_overflow_v2.pack");
    ASSERT_TRUE(BuildWideBatchPack("plr_plan_header_overflow_v2",
                                   replacementPath, kFirstBusId, kBatch,
                                   kSlotsPerBus));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t oldBusIndex =
        engine->buses.FindIndex(kFirstBusId);
    ASSERT_NE(harken::kInvalidBusIndex, oldBusIndex);
    const std::uint32_t oldBusCount = engine->buses.BusCount();
    const std::uint64_t oldTopologyPublications =
        engine->topologyPlanCount;
    const std::uint64_t oldAcceptedRevision =
        engine->acceptedBusTopologyRevision;
    const auto oldPublished = engine->commands.PublishedHead();
    const auto oldConsumed = engine->commands.ConsumedCount();
    const auto oldFreeSlots = engine->commands.FreeSlots();
    const auto oldEffectType = engine->busEffectSlotTypes[oldBusIndex][0];
    const auto oldEffectOwner =
        engine->busEffectOwnerCookies[oldBusIndex][0];
    const std::uint32_t oldLoadedPacks = engine->packs.LoadedCount();
    const harken::PackLogicRecord* oldRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, oldRecord);
    ASSERT_EQ(1u, oldRecord->buses.size());
    ASSERT_TRUE(oldRecord->busEffects.empty());

    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));
    harken::PackPendingRevision* pending =
        engine->logicPacks.PendingFor(pack);
    ASSERT_NE(nullptr, pending);
    ASSERT_TRUE(pending->needsPlan);
    ASSERT_EQ(kCeiling, pending->addBusEffects.size());
    ASSERT_TRUE(pending->removeBusEffects.empty());
    ASSERT_TRUE(pending->replaceBusEffects.empty());

    // The queue has room, but the indivisible revision exceeds the mixer
    // ceiling. Activation therefore remains pending and the old revision
    // stays authoritative with no partial Bus or effect publication.
    ASSERT_EQ(VN_OK, Vn_Update());
    pending = engine->logicPacks.PendingFor(pack);
    ASSERT_NE(nullptr, pending);
    ASSERT_EQ(kBatch, pending->activationCommands.size());
    ASSERT_EQ(harken::kOpcodeSetBusTopologyPlan,
              pending->activationCommands.front().opcode);
    EXPECT_EQ(kBatch,
              pending->activationCommands.front()
                  .payload.setBusTopologyPlan.batchCommandCount);
    EXPECT_EQ(oldBusCount, engine->buses.BusCount());
    EXPECT_EQ(harken::kInvalidBusIndex,
              engine->buses.FindIndex(harkentest::OffsetObjectId(kFirstBusId, 1)));
    EXPECT_EQ(oldTopologyPublications, engine->topologyPlanCount);
    EXPECT_EQ(oldAcceptedRevision,
              engine->acceptedBusTopologyRevision);
    EXPECT_EQ(oldPublished, engine->commands.PublishedHead());
    EXPECT_EQ(oldConsumed, engine->commands.ConsumedCount());
    EXPECT_EQ(oldFreeSlots, engine->commands.FreeSlots());
    EXPECT_EQ(oldEffectType,
              engine->busEffectSlotTypes[oldBusIndex][0]);
    EXPECT_EQ(oldEffectOwner,
              engine->busEffectOwnerCookies[oldBusIndex][0]);
    EXPECT_EQ(oldLoadedPacks, engine->packs.LoadedCount());
    const harken::PackLogicRecord* liveRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, liveRecord);
    EXPECT_EQ(1u, liveRecord->buses.size());
    EXPECT_TRUE(liveRecord->busEffects.empty());
}

// The batch header opcode on its own. Every other command in a batch is one
// bus's or one voice's work, but the header runs the topology swap, the dynamic
// plan install, and the tombstone migration that walks every bus and every live
// voice -- once per batch, at a cost set by the engine's bus and voice
// capacities rather than by the batch's width. A bus-only activation publishes
// exactly that one record, so this block's drain time is the header's, isolated,
// with no instrumentation added to the audio path.
TEST(PackLogicStaging, TheBatchHeaderOpcodeCostIsBoundedWithBusesAndVoicesLive) {
    constexpr std::uint32_t kBuses = 32;
    constexpr std::uint32_t kVoices = 16;
    constexpr std::uint32_t kDeadlineUs = 1000000u * kBlock / kRate;
    VnConfig config = PackLogicConfig(kVoices, kBlock, kRate);
    config.maxBuses = 64;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Live voices are what give the tombstone migration something to walk: it
    // reparents every voice off a destroyed slot, so a block with no voices
    // would time the header's cheapest possible shape.
    const std::vector<float> source = harkentest::MakeDC(kBlock * 64, 0.5f);
    const VnPCMDesc pcm = MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(8700), &pcm));
    for (std::uint32_t voice = 0; voice < kVoices; ++voice) {
        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(8700));
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle)) << "voice " << voice;
    }
    engine->nullBackend.Pump(1);
    ASSERT_EQ(kVoices, Snap().activeVoiceCount);

    const std::string packPath = TempPath("pll_header_only.pack");
    ASSERT_TRUE(
        BuildBusOnlyPack("pll_header_only", packPath, harkentest::ObjectId(8720), kBuses));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    ASSERT_EQ(1u, engine->commands.PublishedHead() -
                      engine->commands.ConsumedCount())
        << "a bus-only activation must publish exactly the plan swap";

    engine->nullBackend.Pump(1);
    const VnSnapshot snapshot = Snap();
    ASSERT_EQ(kBuses + 1u, snapshot.busCount) << "the header did not apply";
    ASSERT_EQ(0u, engine->mixer.CommandsDeferredLastBlock());

    // Recorded because the number itself is the point: the ceiling counts
    // records, and this is the one record whose cost does not scale with the
    // count. The bound is deliberately loose for the same reason as above.
    RecordProperty("batchHeaderDrainUs",
                   std::to_string(snapshot.audioDrainLastUs));
    EXPECT_LT(snapshot.audioDrainLastUs, kDeadlineUs / 4u)
        << "the batch header alone drained in " << snapshot.audioDrainLastUs
        << " us of a " << kDeadlineUs << " us block, with " << kBuses
        << " buses and " << kVoices << " voices live";
}

TEST(PackLogicStaging, InjectedFaultsLeaveZeroFootprintAndReleaseReservations) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    // One live plan slot plus one free: a leaked plan reservation would
    // make the final clean load impossible.
    config.topologyPlanPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // A full-shape resident pack so every reservation engages (plan
    // slot for buses+send, publication growth for the attachment).
    const std::string packPath = TempPath("pll_fault.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_fault",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000349", "parent": "00000000-0000-0000-0000-000000000001" },
            { "id": "3da91a64-b947-4560-0000-00000000034a", "parent": "3da91a64-b947-4560-0000-000000000349" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-00000000034a", "to": "00000000-0000-0000-0000-000000000001", "levelDb": -3.0 } ],
"bindings": [ { "id": "3da91a64-b947-4560-0000-00000000035d", "variable": "3da91a64-b947-4560-0000-000000001d11", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -12.0]],
                           "handles": [[0.0, 0.0, 1.0, -12.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-00000000035d", "scope": "bus", "object": "3da91a64-b947-4560-0000-000000000349" } ])")));

    const auto expectUntouched = [&]() {
        EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(841)));
        EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(842)));
        EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(861)));
        EXPECT_EQ(0u, engine->packs.LoadedCount());
    };

    // BeforeApply is the last gate: both reservations are held when it
    // fires, so two consecutive refusals prove release, not leakage
    // (the plan pool has exactly one free slot).
    for (int attempt = 0; attempt < 2; ++attempt) {
        StagingFaultGuard guard(harken::PackStagingFault::BeforeApply);
        VnPackHandle pack = 0;
        EXPECT_EQ(VN_BAD_PACK, LoadPack(packPath, &pack)) << attempt;
        expectUntouched();
    }
    {
        StagingFaultGuard guard(harken::PackStagingFault::AfterClosure);
        VnPackHandle pack = 0;
        EXPECT_EQ(VN_BAD_PACK, LoadPack(packPath, &pack));
        expectUntouched();
    }
    {
        StagingFaultGuard guard(harken::PackStagingFault::AfterCapacity);
        VnPackHandle pack = 0;
        EXPECT_EQ(VN_BAD_PACK, LoadPack(packPath, &pack));
        expectUntouched();
    }

    // Fault cleared: the same file loads, proving nothing was consumed.
    ASSERT_EQ(harken::PackStagingFault::None,
              harken::PackStagingFaultForTests());
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(841), -1.0f, 0));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(842), -1.0f, 0));
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(861)));
}

TEST(PackLogicInitialAtomicity,
     CommandScratchBadAllocLeavesMediaAndPackSlotUntouched) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnSoundId kSound = harkentest::ObjectId(8621);
    V2Builder builder(harken::kPackKindContent);
    builder.AddMedia(kSound, harkentest::MakeDC(64, 0.5f));
    const std::string packPath =
        TempPath("plia_command_scratch_bad_alloc.pack");
    WriteFileBytes(packPath, builder.Build());

    const std::uint64_t registryBefore = engine->registry.Count();
    const std::uint32_t packsBefore = engine->packs.LoadedCount();
    constexpr VnPackHandle kSentinel = 0xA5A5A5A5u;
    VnPackHandle pack = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        pack = kSentinel;
        {
            StagingFaultGuard guard(
                harken::PackStagingFault::InitialCommandScratchAllocation);
            EXPECT_EQ(VN_DEVICE_ERROR, LoadPack(packPath, &pack)) << attempt;
        }
        EXPECT_EQ(kSentinel, pack) << attempt;
        EXPECT_EQ(registryBefore, engine->registry.Count()) << attempt;
        EXPECT_FALSE(engine->registry.Lookup(kSound).Valid()) << attempt;
        EXPECT_EQ(packsBefore, engine->packs.LoadedCount()) << attempt;
    }

    pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    EXPECT_TRUE(engine->registry.Lookup(kSound).Valid());
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
}

TEST(PackLogicInitialAtomicity,
     ZeroCommandContentSkipsQueueReservationAndCommit) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnSoundId kSound = harkentest::ObjectId(8622);
    V2Builder builder(harken::kPackKindContent);
    builder.AddMedia(kSound, harkentest::MakeDC(64, 0.25f));
    const std::string packPath =
        TempPath("plia_zero_command_content.pack");
    WriteFileBytes(packPath, builder.Build());

    const std::uint32_t commandsBefore = engine->commands.FreeSlots();
    VnPackHandle pack = 0;
    {
        // A zero-command content load must never visit queue reservation.
        StagingFaultGuard guard(
            harken::PackStagingFault::InitialQueueReservation);
        ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    }
    EXPECT_EQ(commandsBefore, engine->commands.FreeSlots());
    EXPECT_TRUE(engine->registry.Lookup(kSound).Valid());
    EXPECT_EQ(1u, engine->packs.LoadedCount());
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
}

TEST(PackLogicInitialAtomicity,
     PreparedPlansBeforeTicketFailureLeavesTopologyUntouched) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    config.dynamicRouteCapacity = 1;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 1;
    config.topologyPlanPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kTarget = harkentest::ObjectId(8631);
    constexpr VnBusId kSource = harkentest::ObjectId(8632);
    const std::string packPath =
        TempPath("plia_plan_before_ticket.pack");
    ASSERT_TRUE(BuildInitialAtomicityTopologyPack(
        "plia_plan_before_ticket", packPath, kTarget, kSource));

    const std::uint32_t busesBefore = engine->buses.BusCount();
    const std::uint64_t sendsBefore = engine->busSendActive;
    const std::uint32_t packsBefore = engine->packs.LoadedCount();
    const std::uint32_t commandsBefore = engine->commands.FreeSlots();
    const std::uint32_t topologyPlansBefore =
        engine->topologyPlans.InUse();
    const std::uint32_t dynamicPlansBefore =
        engine->dynamicRoutes.PlanInUse();
    const std::uint32_t tapsBefore =
        engine->dynamicRoutes.BusTapInUse();
    const std::uint32_t effectsBefore =
        engine->effectBank.InUseOf(VN_EFFECT_COMPRESSOR);
    const auto expectUntouched = [&]() {
        EXPECT_EQ(busesBefore, engine->buses.BusCount());
        EXPECT_EQ(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(kTarget));
        EXPECT_EQ(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(kSource));
        EXPECT_EQ(sendsBefore, engine->busSendActive);
        EXPECT_EQ(packsBefore, engine->packs.LoadedCount());
        EXPECT_EQ(commandsBefore, engine->commands.FreeSlots());
        EXPECT_EQ(topologyPlansBefore, engine->topologyPlans.InUse());
        EXPECT_EQ(dynamicPlansBefore, engine->dynamicRoutes.PlanInUse());
        EXPECT_EQ(tapsBefore, engine->dynamicRoutes.BusTapInUse());
        EXPECT_EQ(effectsBefore,
                  engine->effectBank.InUseOf(VN_EFFECT_COMPRESSOR));
        EXPECT_EQ(0u, engine->dynamicRoutes.ActiveRouteCount());
    };

    VnPackHandle pack = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        pack = 0;
        {
            StagingFaultGuard guard(
                harken::PackStagingFault::
                    InitialPlanPreparedBeforeQueueReservation);
            EXPECT_EQ(VN_BACKPRESSURE, LoadPack(packPath, &pack)) << attempt;
        }
        expectUntouched();
    }

    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
}

TEST(PackLogicInitialAtomicity,
     QueueReservationFailureLeavesTopologyAndReservationsUntouched) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    config.dynamicRouteCapacity = 1;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 1;
    config.topologyPlanPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kTarget = harkentest::ObjectId(8641);
    constexpr VnBusId kSource = harkentest::ObjectId(8642);
    const std::string packPath =
        TempPath("plia_queue_reservation.pack");
    ASSERT_TRUE(BuildInitialAtomicityTopologyPack(
        "plia_queue_reservation", packPath, kTarget, kSource));

    const std::uint32_t busesBefore = engine->buses.BusCount();
    const std::uint64_t sendsBefore = engine->busSendActive;
    const std::uint32_t packsBefore = engine->packs.LoadedCount();
    const std::uint32_t commandsBefore = engine->commands.FreeSlots();
    const std::uint32_t topologyPlansBefore =
        engine->topologyPlans.InUse();
    const std::uint32_t dynamicPlansBefore =
        engine->dynamicRoutes.PlanInUse();
    const std::uint32_t tapsBefore =
        engine->dynamicRoutes.BusTapInUse();
    const std::uint32_t effectsBefore =
        engine->effectBank.InUseOf(VN_EFFECT_COMPRESSOR);
    const auto expectUntouched = [&]() {
        EXPECT_EQ(busesBefore, engine->buses.BusCount());
        EXPECT_EQ(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(kTarget));
        EXPECT_EQ(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(kSource));
        EXPECT_EQ(sendsBefore, engine->busSendActive);
        EXPECT_EQ(packsBefore, engine->packs.LoadedCount());
        EXPECT_EQ(commandsBefore, engine->commands.FreeSlots());
        EXPECT_EQ(topologyPlansBefore, engine->topologyPlans.InUse());
        EXPECT_EQ(dynamicPlansBefore, engine->dynamicRoutes.PlanInUse());
        EXPECT_EQ(tapsBefore, engine->dynamicRoutes.BusTapInUse());
        EXPECT_EQ(effectsBefore,
                  engine->effectBank.InUseOf(VN_EFFECT_COMPRESSOR));
        EXPECT_EQ(0u, engine->dynamicRoutes.ActiveRouteCount());
    };

    VnPackHandle pack = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        pack = 0;
        {
            StagingFaultGuard guard(
                harken::PackStagingFault::InitialQueueReservation);
            EXPECT_EQ(VN_BACKPRESSURE, LoadPack(packPath, &pack)) << attempt;
        }
        expectUntouched();
    }

    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
}

TEST(PackLogicInitialAtomicity,
     MediaThrowWithLiveTicketLeavesTopologyAndReservationsUntouched) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    config.dynamicRouteCapacity = 1;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 1;
    config.topologyPlanPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kTarget = harkentest::ObjectId(8651);
    constexpr VnBusId kSource = harkentest::ObjectId(8652);
    const std::string packPath =
        TempPath("plia_media_with_live_ticket.pack");
    ASSERT_TRUE(BuildInitialAtomicityTopologyPack(
        "plia_media_with_live_ticket", packPath, kTarget, kSource));

    const std::uint32_t busesBefore = engine->buses.BusCount();
    const std::uint64_t sendsBefore = engine->busSendActive;
    const std::uint32_t packsBefore = engine->packs.LoadedCount();
    const std::uint32_t commandsBefore = engine->commands.FreeSlots();
    const std::uint32_t topologyPlansBefore =
        engine->topologyPlans.InUse();
    const std::uint32_t dynamicPlansBefore =
        engine->dynamicRoutes.PlanInUse();
    const std::uint32_t tapsBefore =
        engine->dynamicRoutes.BusTapInUse();
    const std::uint32_t effectsBefore =
        engine->effectBank.InUseOf(VN_EFFECT_COMPRESSOR);
    const auto expectUntouched = [&]() {
        EXPECT_EQ(busesBefore, engine->buses.BusCount());
        EXPECT_EQ(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(kTarget));
        EXPECT_EQ(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(kSource));
        EXPECT_EQ(sendsBefore, engine->busSendActive);
        EXPECT_EQ(packsBefore, engine->packs.LoadedCount());
        EXPECT_EQ(commandsBefore, engine->commands.FreeSlots());
        EXPECT_EQ(topologyPlansBefore, engine->topologyPlans.InUse());
        EXPECT_EQ(dynamicPlansBefore, engine->dynamicRoutes.PlanInUse());
        EXPECT_EQ(tapsBefore, engine->dynamicRoutes.BusTapInUse());
        EXPECT_EQ(effectsBefore,
                  engine->effectBank.InUseOf(VN_EFFECT_COMPRESSOR));
        EXPECT_EQ(0u, engine->dynamicRoutes.ActiveRouteCount());
    };

    VnPackHandle pack = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        pack = 0;
        {
            StagingFaultGuard guard(
                harken::PackStagingFault::InitialMediaPreparation);
            EXPECT_EQ(VN_DEVICE_ERROR, LoadPack(packPath, &pack)) << attempt;
        }
        expectUntouched();
    }

    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
}

TEST(PackLogicInitialSuccess,
     NewBusCompressorRawRouteReadsSourceInFirstPublishedBlock) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 4;
    config.compressorEffectPoolCapacity = 1;
    config.dynamicRouteCapacity = 1;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 1;
    config.topologyPlanPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kTarget = harkentest::ObjectId(8661);
    constexpr VnBusId kSource = harkentest::ObjectId(8662);
    const std::string packPath =
        TempPath("plis_same_block_raw_route.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plis_same_block_raw_route", packPath, kTarget, kSource,
        "compressor", false,
        "[-40.0, 20.0, 0.1, 100.0, 0.0, 0.0]", true, 0.0f));

    // Nothing from the transaction is visible before its successful call.
    ASSERT_EQ(0u, engine->topologyPlans.InUse());
    ASSERT_EQ(0u, engine->dynamicRoutes.PlanInUse());
    ASSERT_EQ(0u, engine->dynamicRoutes.BusTapInUse());
    ASSERT_EQ(0u, engine->dynamicRoutes.ActiveRouteCount());
    ASSERT_EQ(harken::kInvalidBusIndex,
              engine->buses.FindIndex(kTarget));
    ASSERT_EQ(harken::kInvalidBusIndex,
              engine->buses.FindIndex(kSource));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    const std::uint32_t target =
        engine->buses.FindIndex(kTarget);
    const std::uint32_t source =
        engine->buses.FindIndex(kSource);
    ASSERT_NE(harken::kInvalidBusIndex, target);
    ASSERT_NE(harken::kInvalidBusIndex, source);
    ASSERT_EQ(1u, engine->topologyPlans.InUse());
    ASSERT_EQ(1u, engine->dynamicRoutes.PlanInUse());
    ASSERT_EQ(1u, engine->dynamicRoutes.BusTapInUse());
    ASSERT_EQ(1u, engine->dynamicRoutes.ActiveRouteCount());

    // The one prepared plan is already dependency-ordered before the queue
    // publishes it: the raw source must render before the compressor target.
    bool foundPreparedPlan = false;
    for (std::uint32_t planIndex = 0;
         planIndex < engine->topologyPlans.Capacity(); ++planIndex) {
        const harken::TopologyPlanView plan =
            engine->topologyPlans.View(planIndex);
        if (plan.orderCount != engine->buses.BusCount()) {
            continue;
        }
        std::uint32_t sourceOrdinal = plan.orderCount;
        std::uint32_t targetOrdinal = plan.orderCount;
        for (std::uint32_t ordinal = 0;
             ordinal < plan.orderCount; ++ordinal) {
            sourceOrdinal =
                plan.order[ordinal] == source
                    ? ordinal
                    : sourceOrdinal;
            targetOrdinal =
                plan.order[ordinal] == target
                    ? ordinal
                    : targetOrdinal;
        }
        ASSERT_LT(sourceOrdinal, plan.orderCount);
        ASSERT_LT(targetOrdinal, plan.orderCount);
        EXPECT_LT(sourceOrdinal, targetOrdinal);
        foundPreparedPlan = true;
        break;
    }
    ASSERT_TRUE(foundPreparedPlan);

    std::uint32_t routeCount = 0;
    VnEffectDynamicRouteInfo pending{};
    pending.structSize = sizeof(pending);
    ASSERT_EQ(VN_OK, Vn_CaptureEffectDynamicRoutes(
                         &pending, 1, &routeCount));
    ASSERT_EQ(1u, routeCount);
    EXPECT_NE(0u, pending.stateFlags & VN_DYNAMIC_ROUTE_PENDING);

    const std::vector<float> targetSamples =
        harkentest::MakeDC(kBlock * 4, 0.25f);
    const std::vector<float> sourceSamples =
        harkentest::MakeDC(kBlock * 4, 1.0f);
    const VnPCMDesc targetPcm = MonoDesc(targetSamples, kRate);
    const VnPCMDesc sourcePcm = MonoDesc(sourceSamples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(8663), &targetPcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(8664), &sourcePcm));
    VnPlayDesc targetPlay = MakePlayDesc(harkentest::ObjectId(8663));
    targetPlay.busId = kTarget;
    VnPlayDesc sourcePlay = MakePlayDesc(harkentest::ObjectId(8664));
    sourcePlay.busId = kSource;
    VnVoiceHandle targetVoice = 0;
    VnVoiceHandle sourceVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&targetPlay, &targetVoice));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&sourcePlay, &sourceVoice));

    harkentest::Capture first;
    harkentest::PumpAndCapture(engine, 1, first);
    harken::BusTree::Bus* targetBus =
        engine->buses.BusState(target);
    ASSERT_NE(nullptr, targetBus);
    auto* state = static_cast<harken::CompressorState*>(
        targetBus->fxSlots[0].state);
    ASSERT_NE(nullptr, state);
    EXPECT_NEAR(0.25f, state->telemetry.mainInputPeak, 1.0e-6f);
    EXPECT_NEAR(1.0f, state->telemetry.detectorPeak, 1.0e-6f)
        << "a delayed plan or target-before-source order reads zero here";

    VnEffectDynamicRouteInfo active{};
    active.structSize = sizeof(active);
    ASSERT_EQ(VN_OK, Vn_CaptureEffectDynamicRoutes(
                         &active, 1, &routeCount));
    ASSERT_EQ(1u, routeCount);
    EXPECT_NE(0u, active.stateFlags & VN_DYNAMIC_ROUTE_ACTIVE);
    EXPECT_EQ(0u, active.stateFlags & VN_DYNAMIC_ROUTE_PENDING);
}

TEST(PackLogicStaging, PublicMediaAllocationFailureReturnsDeviceError) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    V2Builder builder(harken::kPackKindContent);
    builder.AddMedia(harkentest::ObjectId(851), harkentest::MakeDC(64, 0.5f));
    const std::string packPath = TempPath("pll_pcm_fault.pack");
    WriteFileBytes(packPath, builder.Build());

    VnPackHandle pack = 0;
    {
        PcmRegisterFaultGuard guard(harken::PcmRegisterFault::MapCommit);
        EXPECT_EQ(VN_DEVICE_ERROR, LoadPack(packPath, &pack));
    }

    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(851)).Valid());
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    EXPECT_TRUE(engine->registry.Lookup(harkentest::ObjectId(851)).Valid());
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
}

// --- staging aggregate capacity arithmetic ---
// Every per-entry admission preflight refuses only a pool that is
// ALREADY full, so a pack whose entries are each individually
// admissible can still overflow: with exactly one free slot and two
// staged entries, the summed-count arithmetic in the staging pass is
// the only check that can refuse. Each case pins the exact refusal
// code and proves zero mutation through another entry kind of the same
// pack (non-mutating probes only).

TEST(PackLogicAggregateCapacity, BindingDefinitionSumRefusesWholeLoad) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.maxBindings = 2; // one taken below: one free slot, pack needs two
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const VnBindingDesc live =
        MakeBusVolumeBinding(harkentest::ObjectId(7501), 0.0f, 0.0f, 1.0f, -3.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1001), &live));

    // Two bus-scope definitions — either alone would seat in the free
    // slot — together exceed the remaining capacity.
    const std::string packPath = TempPath("plagg_binding_defs.pack");
    ASSERT_TRUE(BuildPackFile(
        "plagg_binding_defs",
        V2LogicManifest(
            packPath, "resident",
            R"("bindings": [ { "id": "3da91a64-b947-4560-0000-0000000003ea", "variable": "3da91a64-b947-4560-0000-000000001d4f", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } },
              { "id": "3da91a64-b947-4560-0000-0000000003eb", "variable": "3da91a64-b947-4560-0000-000000001d50", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -9.0]],
                           "handles": [[0.0, 0.0, 1.0, -9.0]] } } ])")));

    VnPackHandle pack = 0;
    EXPECT_EQ(VN_NO_BINDING, LoadPack(packPath, &pack));

    // Zero mutation: neither definition landed.
    EXPECT_EQ(1u, engine->bindings.DefinitionCount());
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_UnregisterBinding(harkentest::ObjectId(1002)));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_UnregisterBinding(harkentest::ObjectId(1003)));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
}

TEST(PackLogicAggregateCapacity, BindingAttachmentSumRefusesWholeLoad) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.maxBindingAttachments = 2; // one taken below; pack carries two
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // One live definition with one live BUS attachment on the master:
    // exactly one attachment slot stays free.
    const VnBindingDesc live =
        MakeBusVolumeBinding(harkentest::ObjectId(7511), 0.0f, 0.0f, 1.0f, -3.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1011), &live));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1011), &attach));

    // The pack's attachments reference its OWN definition, so the
    // per-attachment CanAttach preflight never runs for them — the
    // attachment-pool sum is the sole guard.
    const std::vector<float> tone = harkentest::MakeDC(kBlock, 0.5f);
    const std::string wavA = TempPath("plagg_attach_a.wav");
    const std::string wavB = TempPath("plagg_attach_b.wav");
    WriteWavF32(wavA, tone, 1, kRate);
    WriteWavF32(wavB, tone, 1, kRate);
    const std::string packPath = TempPath("plagg_attachments.pack");
    ASSERT_TRUE(BuildPackFile(
        "plagg_attachments",
        V2LogicManifest(
            packPath, "content",
            "\"entries\": [ { \"path\": \"" + wavA +
                "\", \"id\": \"3da91a64-b947-4560-0000-0000000003f4\" },\n  { \"path\": \"" + wavB +
                "\", \"id\": \"3da91a64-b947-4560-0000-0000000003f5\" } ],\n" +
                std::string(
                    R"("bindings": [ { "id": "3da91a64-b947-4560-0000-0000000003f6", "variable": "3da91a64-b947-4560-0000-000000001d58",
                "scope": "voice", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-0000000003f6", "scope": "voice", "object": "3da91a64-b947-4560-0000-0000000003f4" },
                 { "binding": "3da91a64-b947-4560-0000-0000000003f6", "scope": "voice", "object": "3da91a64-b947-4560-0000-0000000003f5" } ])"))));

    VnPackHandle pack = 0;
    EXPECT_EQ(VN_NO_BINDING, LoadPack(packPath, &pack));

    // Zero mutation: no media, no definition, no attachment landed.
    EXPECT_EQ(1u, engine->bindings.AttachmentCount());
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(1014)));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_UnregisterBinding(harkentest::ObjectId(1014)));
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(1012)).Valid());
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(1013)).Valid());
    EXPECT_EQ(0u, engine->packs.LoadedCount());
}

TEST(PackLogicAggregateCapacity, AttenuationSumPreparesWholeLoadBeyond64) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Native registrations coexist with a revision-wide batch crossing 64.
    const VnAttenuationDesc profile =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    for (std::uint32_t i = 0; i < 63; ++i) {
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9001 + i), &profile)) << i;
    }
    ASSERT_EQ(63u, engine->spatial.ProfileCount());

    const std::string packPath = TempPath("plagg_attenuations.pack");
    ASSERT_TRUE(BuildPackFile(
        "plagg_attenuations",
        V2LogicManifest(
            packPath, "resident",
            R"("attenuations": [ { "id": "3da91a64-b947-4560-0000-00000000238d", "rolloff": "linear",
                    "minDistance": 1.0, "maxDistance": 100.0,
                    "airAbsorptionNearHz": 1000000000.0,
                    "airAbsorptionFarHz": 1000000000.0 },
                  { "id": "3da91a64-b947-4560-0000-00000000238e", "rolloff": "linear",
                    "minDistance": 1.0, "maxDistance": 100.0,
                    "airAbsorptionNearHz": 1000000000.0,
                    "airAbsorptionFarHz": 1000000000.0 } ])")));

    VnPackHandle pack = 0;
    EXPECT_EQ(VN_OK, LoadPack(packPath, &pack));

    EXPECT_EQ(65u, engine->spatial.ProfileCount());
    EXPECT_EQ(1u, engine->packs.LoadedCount());
    EXPECT_NE(nullptr, engine->spatial.FindProfile(harkentest::ObjectId(9001)));
    EXPECT_NE(nullptr, engine->spatial.FindProfile(harkentest::ObjectId(9101)));
    EXPECT_NE(nullptr, engine->spatial.FindProfile(harkentest::ObjectId(9102)));
}



TEST(PackLogicAggregateCapacity, SendSlotBudgetSumRefusesWholeLoad) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Six pack buses and five send edges, all FROM the same pack bus to
    // five distinct pack buses. Any single edge fits the default
    // four-slot per-bus budget; only the per-from-bus SUM over the
    // combined graph exceeds it, and no per-edge check ever counts that
    // sum.
    const std::string packPath = TempPath("plagg_send_budget.pack");
    ASSERT_TRUE(BuildPackFile(
        "plagg_send_budget",
        V2LogicManifest(packPath, "resident",
                        R"("buses": [ { "id": "3da91a64-b947-4560-0000-00000000041b", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-00000000041c", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-00000000041d", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-00000000041e", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-00000000041f", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000420", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-00000000041b", "to": "3da91a64-b947-4560-0000-00000000041c", "levelDb": -3.0 },
              { "from": "3da91a64-b947-4560-0000-00000000041b", "to": "3da91a64-b947-4560-0000-00000000041d", "levelDb": -3.0 },
              { "from": "3da91a64-b947-4560-0000-00000000041b", "to": "3da91a64-b947-4560-0000-00000000041e", "levelDb": -3.0 },
              { "from": "3da91a64-b947-4560-0000-00000000041b", "to": "3da91a64-b947-4560-0000-00000000041f", "levelDb": -3.0 },
              { "from": "3da91a64-b947-4560-0000-00000000041b", "to": "3da91a64-b947-4560-0000-000000000420", "levelDb": -3.0 } ])")));

    VnPackHandle pack = 0;
    EXPECT_EQ(VN_NO_SEND, LoadPack(packPath, &pack));

    // Zero mutation: no bus landed and the discrete declaration did
    // not happen.
    EXPECT_EQ(1u, engine->buses.BusCount());
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(1051), -1.0f, 0));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(1056), -1.0f, 0));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
}

// --- unload ---

TEST(PackLogicUnload,
     ReversibleResidentBusSendEffectAndRouteUnloadAsOneBatch) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 2;
    config.compressorEffectPoolCapacity = 1;
    config.dynamicRouteCapacity = 2;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string packPath =
        TempPath("plu_reversible_resident_full.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_reversible_resident_full",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-0000000032c9", "parent": "00000000-0000-0000-0000-000000000001" },
            { "id": "3da91a64-b947-4560-0000-0000000032ca", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-0000000032ca", "to": "00000000-0000-0000-0000-000000000001", "levelDb": -6.0 } ],
"busEffects": [ { "bus": "3da91a64-b947-4560-0000-0000000032c9", "slot": 0, "type": "compressor",
    "descriptorVintage": 2, "bypass": false, "faderPointGuard": 2,
    "parameters": [-18.0, 4.0, 10.0, 120.0, 6.0, 0.0] } ],
"dynamicRoutes": [ { "targetBus": "3da91a64-b947-4560-0000-0000000032c9", "effectSlot": 0,
    "routeSlot": 0, "sourceBus": "3da91a64-b947-4560-0000-0000000032ca", "tap": "postEffects",
    "levelDb": -3.0 } ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t target = engine->buses.FindIndex(harkentest::ObjectId(13001));
    ASSERT_NE(harken::kInvalidBusIndex, target);
    ASSERT_EQ(pack, engine->busEffectOwnerCookies[target][0]);
    ASSERT_EQ(1u, engine->effectBank.InUseOf(VN_EFFECT_COMPRESSOR));

    ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    std::uint32_t routes = 99;
    EXPECT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(nullptr, 0, &routes));
    EXPECT_EQ(0u, routes);
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(13001), 0.0f, 0));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(13002), 0.0f, 0));
    EXPECT_EQ(harken::kEffectTypeNone,
              engine->busEffectInstanceRefs[target][0].type);
    EXPECT_EQ(0u, engine->effectBank.InUseOf(VN_EFFECT_COMPRESSOR));
    EXPECT_EQ(nullptr, engine->logicPacks.RecordFor(pack));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
}

TEST(PackLogicUnload,
     ReversibleResidentClearsOwnedStateOnCallerBusesOnly) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    config.dynamicRouteCapacity = 1;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    constexpr VnBusId kSource = harkentest::ObjectId(13131);
    constexpr VnBusId kTarget = harkentest::ObjectId(13132);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSource, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTarget, VN_BUS_MASTER));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    const std::string path = TempPath("plu_external_owned.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_external_owned",
        V2LogicManifest(
            path, "resident",
            R"("busSends": [ { "from": "3da91a64-b947-4560-0000-00000000334b", "to": "3da91a64-b947-4560-0000-00000000334c",
    "levelDb": -6.0 } ],
"busEffects": [ { "bus": "3da91a64-b947-4560-0000-00000000334c", "slot": 0, "type": "compressor",
    "descriptorVintage": 2, "bypass": false, "faderPointGuard": 1,
    "parameters": [-18.0, 4.0, 10.0, 120.0, 6.0, 0.0] } ],
"dynamicRoutes": [ { "targetBus": "3da91a64-b947-4560-0000-00000000334c", "effectSlot": 0,
    "routeSlot": 0, "sourceBus": "3da91a64-b947-4560-0000-00000000334b", "tap": "postEffects",
    "levelDb": -3.0 } ])")));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    const std::uint32_t source = engine->buses.FindIndex(kSource);
    const std::uint32_t target = engine->buses.FindIndex(kTarget);
    ASSERT_NE(harken::kInvalidBusIndex, source);
    ASSERT_NE(harken::kInvalidBusIndex, target);
    const std::uint32_t sendSlot =
        FindBusSendSlot(*engine, kSource, kTarget);
    ASSERT_LT(sendSlot, engine->config.busSendSlotsPerBus);
    ASSERT_EQ(0u, engine->busOwnerCookies[source]);
    ASSERT_EQ(0u, engine->busOwnerCookies[target]);
    ASSERT_EQ(pack,
              engine->busSendOwnerCookies[
                  source * engine->config.busSendSlotsPerBus + sendSlot]);
    ASSERT_EQ(pack, engine->busEffectOwnerCookies[target][0]);
    ASSERT_NE(nullptr, engine->dynamicRoutes.Find(kTarget, 0, 0));

    ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_EQ(source, engine->buses.FindIndex(kSource));
    EXPECT_EQ(target, engine->buses.FindIndex(kTarget));
    EXPECT_EQ(0u, engine->busOwnerCookies[source]);
    EXPECT_EQ(0u, engine->busOwnerCookies[target]);
    EXPECT_EQ(engine->config.busSendSlotsPerBus,
              FindBusSendSlot(*engine, kSource, kTarget));
    EXPECT_EQ(harken::kEffectTypeNone,
              engine->busEffectInstanceRefs[target][0].type);
    EXPECT_EQ(0u, engine->busEffectOwnerCookies[target][0]);
    EXPECT_EQ(nullptr, engine->dynamicRoutes.Find(kTarget, 0, 0));
    EXPECT_EQ(nullptr, engine->logicPacks.RecordFor(pack));

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->effectBank.InUseOf(VN_EFFECT_COMPRESSOR));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(kSource, -1.0f, 0));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(kTarget, -1.0f, 0));
}

TEST(PackLogicUnload, ThreeLevelResidentBusTreeLeavesNoLiveDescendant) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path = TempPath("plu_three_level.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_three_level",
        V2LogicManifest(
            path, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000003355", "parent": "00000000-0000-0000-0000-000000000001" },
    { "id": "3da91a64-b947-4560-0000-000000003356", "parent": "3da91a64-b947-4560-0000-000000003355" },
    { "id": "3da91a64-b947-4560-0000-000000003357", "parent": "3da91a64-b947-4560-0000-000000003356" } ])")));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t root = engine->buses.FindIndex(harkentest::ObjectId(13141));
    const std::uint32_t child = engine->buses.FindIndex(harkentest::ObjectId(13142));
    const std::uint32_t leaf = engine->buses.FindIndex(harkentest::ObjectId(13143));
    ASSERT_NE(harken::kInvalidBusIndex, root);
    ASSERT_NE(harken::kInvalidBusIndex, child);
    ASSERT_NE(harken::kInvalidBusIndex, leaf);
    ASSERT_EQ(root, engine->buses.ParentOf(child));
    ASSERT_EQ(child, engine->buses.ParentOf(leaf));

    ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_FALSE(engine->buses.AliveAt(leaf));
    EXPECT_FALSE(engine->buses.AliveAt(child));
    EXPECT_FALSE(engine->buses.AliveAt(root));
    EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(13143)));
    EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(13142)));
    EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(13141)));
    EXPECT_EQ(nullptr, engine->logicPacks.RecordFor(pack));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
}

TEST(PackLogicUnload,
     ReversibleResidentOldHandleStaysStaleAfterPackSlotReuse) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.maxPacks = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const VnPackHandle oldPack =
        LoadResidentBusOnly("plu_stale_old", harkentest::ObjectId(13151));
    ASSERT_NE(0u, oldPack);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_UnloadPack(oldPack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    const std::uint64_t staleBefore =
        engine->packs.StaleHandleCount();
    EXPECT_EQ(VN_STALE_HANDLE, Vn_UnloadPack(oldPack));
    EXPECT_EQ(staleBefore + 1, engine->packs.StaleHandleCount());

    const VnPackHandle newPack =
        LoadResidentBusOnly("plu_stale_new", harkentest::ObjectId(13152));
    ASSERT_NE(0u, newPack);
    ASSERT_NE(oldPack, newPack);
    const std::uint32_t newBus = engine->buses.FindIndex(harkentest::ObjectId(13152));
    ASSERT_NE(harken::kInvalidBusIndex, newBus);
    ASSERT_EQ(newPack, engine->busOwnerCookies[newBus]);

    const std::uint64_t staleBeforeReuse =
        engine->packs.StaleHandleCount();
    EXPECT_EQ(VN_STALE_HANDLE, Vn_UnloadPack(oldPack));
    EXPECT_EQ(staleBeforeReuse + 1,
              engine->packs.StaleHandleCount());
    EXPECT_EQ(newBus, engine->buses.FindIndex(harkentest::ObjectId(13152)));
    EXPECT_EQ(newPack, engine->busOwnerCookies[newBus]);
    EXPECT_NE(nullptr, engine->logicPacks.RecordFor(newPack));
    EXPECT_EQ(1u, engine->packs.LoadedCount());

    EXPECT_EQ(VN_OK, Vn_UnloadPack(newPack));
    EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(13152)));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
}

TEST(PackLogicUnload,
     ForeignEffectInAnotherPackBusSlotRefusesWithoutMutation) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 2;
    config.compressorEffectPoolCapacity = 1;
    config.gainEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string packPath =
        TempPath("plu_foreign_effect_slot.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_foreign_effect_slot",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-0000000032d3", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busEffects": [ { "bus": "3da91a64-b947-4560-0000-0000000032d3", "slot": 0, "type": "compressor",
    "descriptorVintage": 2, "bypass": false, "faderPointGuard": 2,
    "parameters": [-18.0, 4.0, 10.0, 120.0, 6.0, 0.0] } ])")));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));

    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.gainDb = -6.0f;
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffect(
                  harkentest::ObjectId(13011), 1, VN_EFFECT_GAIN,
                  reinterpret_cast<const VnEffectDesc*>(&gain)));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    const std::uint32_t bus = engine->buses.FindIndex(harkentest::ObjectId(13011));
    ASSERT_NE(harken::kInvalidBusIndex, bus);
    const harken::EffectInstanceRef owned =
        engine->busEffectInstanceRefs[bus][0];
    const harken::EffectInstanceRef foreign =
        engine->busEffectInstanceRefs[bus][1];
    ASSERT_EQ(pack, engine->busEffectOwnerCookies[bus][0]);
    ASSERT_EQ(0u, engine->busEffectOwnerCookies[bus][1]);

    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_EQ(bus, engine->buses.FindIndex(harkentest::ObjectId(13011)));
    EXPECT_EQ(owned.type, engine->busEffectInstanceRefs[bus][0].type);
    EXPECT_EQ(owned.poolIndex,
              engine->busEffectInstanceRefs[bus][0].poolIndex);
    EXPECT_EQ(foreign.type, engine->busEffectInstanceRefs[bus][1].type);
    EXPECT_EQ(foreign.poolIndex,
              engine->busEffectInstanceRefs[bus][1].poolIndex);
    EXPECT_EQ(pack, engine->busEffectOwnerCookies[bus][0]);
    EXPECT_EQ(0u, engine->busEffectOwnerCookies[bus][1]);
    EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
    EXPECT_EQ(1u, engine->packs.LoadedCount());
}

TEST(PackLogicUnload,
     ForeignEffectParamBindingProtectsPackOwnedEffectOnCallerBus) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    constexpr VnBusId kBus = harkentest::ObjectId(13161);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kBus, VN_BUS_MASTER));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    const std::string path = TempPath("plu_effect_binding.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_effect_binding",
        V2LogicManifest(
            path, "resident",
            R"("busEffects": [ { "bus": "3da91a64-b947-4560-0000-000000003369", "slot": 0,
    "type": "compressor", "descriptorVintage": 2, "bypass": false,
    "faderPointGuard": 1,
    "parameters": [-18.0, 4.0, 10.0, 120.0, 6.0, 0.0] } ])")));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));
    const std::uint32_t bus = engine->buses.FindIndex(kBus);
    ASSERT_NE(harken::kInvalidBusIndex, bus);
    const harken::EffectInstanceRef effect =
        engine->busEffectInstanceRefs[bus][0];
    ASSERT_EQ(pack, engine->busEffectOwnerCookies[bus][0]);

    const VnBindingDesc binding = MakeBusEffectParamBinding(
        harkentest::ObjectId(13162), VN_EFFECT_COMPRESSOR, 0,
        harken::kCompressorParamThresholdDb);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(13163), &binding));
    VnBindingAttachDesc attachment{};
    attachment.structSize = sizeof(attachment);
    attachment.scopeKind = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    attachment.objectId = kBus;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(13163), &attachment));

    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_EQ(bus, engine->buses.FindIndex(kBus));
    EXPECT_EQ(effect.type, engine->busEffectInstanceRefs[bus][0].type);
    EXPECT_EQ(effect.poolIndex,
              engine->busEffectInstanceRefs[bus][0].poolIndex);
    EXPECT_EQ(effect.generation,
              engine->busEffectInstanceRefs[bus][0].generation);
    EXPECT_EQ(pack, engine->busEffectOwnerCookies[bus][0]);
    EXPECT_TRUE(engine->bindings.HasAttachment(
        harkentest::ObjectId(13163), VN_TARGET_SCOPE_BUS_EFFECT_PARAM, kBus));
    EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
}

TEST(PackLogicUnload,
     ForeignRouteTargetProtectsPackOwnedEffectOnCallerBus) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    config.dynamicRouteCapacity = 1;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    constexpr VnBusId kSource = harkentest::ObjectId(13171);
    constexpr VnBusId kTarget = harkentest::ObjectId(13172);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kSource, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(kTarget, VN_BUS_MASTER));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    const std::string path = TempPath("plu_route_target.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_route_target",
        V2LogicManifest(
            path, "resident",
            R"("busEffects": [ { "bus": "3da91a64-b947-4560-0000-000000003374", "slot": 0,
    "type": "compressor", "descriptorVintage": 2, "bypass": false,
    "faderPointGuard": 1,
    "parameters": [-18.0, 4.0, 10.0, 120.0, 6.0, 0.0] } ])")));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    VnEffectDynamicRouteDesc route{};
    route.structSize = sizeof(route);
    route.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    route.sourceKind = VN_DYNAMIC_SOURCE_BUS;
    route.tap = VN_DYNAMIC_TAP_BUS_POST_EFFECTS;
    route.sourceId = kSource;
    route.levelDb = 0.0f;
    route.expectedEffectType = VN_EFFECT_COMPRESSOR;
    ASSERT_EQ(VN_OK,
              Vn_SetBusEffectDynamicRoute(kTarget, 0, 0, &route));
    const harken::DynamicRouteDefinition* before =
        engine->dynamicRoutes.Find(kTarget, 0, 0);
    ASSERT_NE(nullptr, before);
    const std::uint64_t routeGeneration = before->generation;
    ASSERT_EQ(VN_DYNAMIC_ROUTE_OWNER_CALLER, before->ownerKind);

    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    const harken::DynamicRouteDefinition* after =
        engine->dynamicRoutes.Find(kTarget, 0, 0);
    ASSERT_NE(nullptr, after);
    EXPECT_EQ(routeGeneration, after->generation);
    EXPECT_EQ(VN_DYNAMIC_ROUTE_OWNER_CALLER, after->ownerKind);
    EXPECT_EQ(pack,
              engine->busEffectOwnerCookies[
                  engine->buses.FindIndex(kTarget)][0]);
    EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
}

TEST(PackLogicUnload,
     ForeignChildSendRouteBindingAndVoiceReferencesRefuseWhole) {
    {
        SCOPED_TRACE("foreign child bus");
        EngineSession session(PackLogicConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const VnPackHandle pack =
            LoadResidentBusOnly("plu_foreign_child", harkentest::ObjectId(13021));
        ASSERT_NE(0u, pack);
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(13022), harkentest::ObjectId(13021)));
        const std::uint32_t child = engine->buses.FindIndex(harkentest::ObjectId(13022));
        ASSERT_NE(harken::kInvalidBusIndex, child);

        EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
        EXPECT_EQ(child, engine->buses.FindIndex(harkentest::ObjectId(13022)));
        EXPECT_NE(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(harkentest::ObjectId(13021)));
        EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
    }
    {
        SCOPED_TRACE("foreign bus send");
        EngineSession session(PackLogicConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(13032), VN_BUS_MASTER));
        const VnPackHandle pack =
            LoadResidentBusOnly("plu_foreign_send", harkentest::ObjectId(13031));
        ASSERT_NE(0u, pack);
        ASSERT_EQ(VN_OK, Vn_ConnectBusSend(harkentest::ObjectId(13032), harkentest::ObjectId(13031), -6.0f));
        const std::uint32_t slot =
            FindBusSendSlot(*engine, harkentest::ObjectId(13032), harkentest::ObjectId(13031));
        ASSERT_LT(slot, engine->config.busSendSlotsPerBus);

        EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
        EXPECT_EQ(slot, FindBusSendSlot(*engine, harkentest::ObjectId(13032), harkentest::ObjectId(13031)));
        EXPECT_NE(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(harkentest::ObjectId(13031)));
        EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
    }
    {
        SCOPED_TRACE("foreign dynamic route");
        VnConfig config = PackLogicConfig(4, kBlock, kRate);
        config.fxSlotsPerBus = 1;
        config.compressorEffectPoolCapacity = 1;
        config.dynamicRouteCapacity = 1;
        config.dynamicRoutePlanPoolCapacity = 2;
        config.dynamicBusTapCapacity = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(13042), VN_BUS_MASTER));
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        const VnPackHandle pack =
            LoadResidentBusOnly("plu_foreign_route", harkentest::ObjectId(13041));
        ASSERT_NE(0u, pack);

        VnCompressorDesc compressor{};
        compressor.structSize = sizeof(compressor);
        compressor.thresholdDb = -18.0f;
        compressor.ratio = 4.0f;
        compressor.attackMs = 10.0f;
        compressor.releaseMs = 120.0f;
        compressor.kneeDb = 6.0f;
        ASSERT_EQ(
            VN_OK,
            Vn_SetBusEffect(
                harkentest::ObjectId(13042), 0, VN_EFFECT_COMPRESSOR,
                reinterpret_cast<const VnEffectDesc*>(&compressor)));
        VnEffectDynamicRouteDesc route{};
        route.structSize = sizeof(route);
        route.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
        route.sourceKind = VN_DYNAMIC_SOURCE_BUS;
        route.tap = VN_DYNAMIC_TAP_BUS_POST_EFFECTS;
        route.sourceId = harkentest::ObjectId(13041);
        route.levelDb = 0.0f;
        route.expectedEffectType = VN_EFFECT_COMPRESSOR;
        ASSERT_EQ(VN_OK,
                  Vn_SetBusEffectDynamicRoute(harkentest::ObjectId(13042), 0, 0, &route));

        EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
        std::uint32_t routes = 0;
        EXPECT_EQ(VN_OK,
                  Vn_CaptureEffectDynamicRoutes(nullptr, 0, &routes));
        EXPECT_EQ(1u, routes);
        EXPECT_NE(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(harkentest::ObjectId(13041)));
        EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
    }
    {
        SCOPED_TRACE("foreign binding");
        EngineSession session(PackLogicConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const VnPackHandle pack =
            LoadResidentBusOnly("plu_foreign_control", harkentest::ObjectId(13051));
        ASSERT_NE(0u, pack);

        const VnBindingDesc binding =
            MakeBusVolumeBinding(harkentest::ObjectId(13053), 0.0f, 0.0f, 1.0f, -6.0f);
        ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(13054), &binding));
        VnBindingAttachDesc attachment{};
        attachment.structSize = sizeof(attachment);
        attachment.scopeKind = VN_TARGET_SCOPE_BUS;
        attachment.objectId = harkentest::ObjectId(13051);
        ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(13054), &attachment));
        EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
        EXPECT_TRUE(engine->bindings.HasAttachment(
            harkentest::ObjectId(13054), VN_TARGET_SCOPE_BUS, harkentest::ObjectId(13051)));
        EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
    }
    {
        SCOPED_TRACE("foreign routed voice");
        EngineSession session(PackLogicConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const VnPackHandle pack =
            LoadResidentBusOnly("plu_foreign_voice", harkentest::ObjectId(13061));
        ASSERT_NE(0u, pack);
        const std::vector<float> samples =
            harkentest::MakeDC(kBlock * 2, 0.25f);
        const VnPCMDesc pcm = MonoDesc(samples, kRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(13062), &pcm));
        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(13062));
        play.busId = harkentest::ObjectId(13061);
        play.loopCount = 0;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        engine->nullBackend.Pump(1);

        EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
        EXPECT_NE(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(harkentest::ObjectId(13061)));
        EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
        EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
                  engine->ledger.Classify(voice).kind);
    }
}

TEST(PackLogicUnload,
     VoiceDryAndSendReferencesCoverLivePendingParkedAndEndedStates) {
    for (bool voiceSend : {false, true}) {
        SCOPED_TRACE(voiceSend ? "live voice send" : "live dry route");
        EngineSession session(PackLogicConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const VnPackHandle pack = LoadResidentBusOnly(
            voiceSend ? "plu_live_voice_send" : "plu_live_voice_dry",
            harkentest::ObjectId(voiceSend ? 13181 : 13182));
        ASSERT_NE(0u, pack);
        const VnBusId packBus = harkentest::ObjectId(voiceSend ? 13181 : 13182);
        const std::vector<float> samples =
            harkentest::MakeDC(kBlock * 4, 0.25f);
        const VnPCMDesc pcm = MonoDesc(samples, kRate);
        ASSERT_EQ(VN_OK,
                  Vn_RegisterPCM(harkentest::ObjectId(voiceSend ? 13183 : 13184), &pcm));
        VnPlayDesc play =
            MakePlayDesc(harkentest::ObjectId(voiceSend ? 13183 : 13184));
        play.busId = voiceSend ? VN_BUS_MASTER : packBus;
        play.loopCount = 0;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        if (voiceSend) {
            ASSERT_EQ(VN_OK,
                      Vn_SetVoiceSend(voice, 0, packBus, -3.0f));
        }
        engine->nullBackend.Pump(1);
        ASSERT_EQ(harken::VoiceLedger::HandleKind::Live,
                  engine->ledger.Classify(voice).kind);

        EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
        EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
        EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
                  engine->ledger.Classify(voice).kind);
    }

    for (bool voiceSend : {false, true}) {
        SCOPED_TRACE(voiceSend ? "pending voice send" :
                                "pending dry route");
        EngineSession session(PackLogicConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const VnBusId packBus = harkentest::ObjectId(voiceSend ? 13185 : 13186);
        const VnPackHandle pack = LoadResidentBusOnly(
            voiceSend ? "plu_pending_voice_send" :
                        "plu_pending_voice_dry",
            packBus);
        ASSERT_NE(0u, pack);
        const std::uint32_t bus = engine->buses.FindIndex(packBus);
        ASSERT_NE(harken::kInvalidBusIndex, bus);
        ASSERT_TRUE(engine->pendingStarts.Emplace(0));
        harken::PendingStartRecord& pending = engine->pendingStarts[0];
        pending.active = true;
        pending.busIndex = voiceSend ? 0 : bus;
        if (voiceSend) {
            pending.voiceSends[0] = {true, bus, -6.0f};
        }

        EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
        EXPECT_TRUE(pending.active);
        EXPECT_EQ(voiceSend ? 0u : bus, pending.busIndex);
        EXPECT_EQ(voiceSend, pending.voiceSends[0].active);
        EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
        pending.Reset();
    }

    for (bool voiceSend : {false, true}) {
        SCOPED_TRACE(voiceSend ? "parked voice send" :
                                "parked dry route");
        EngineSession session(PackLogicConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const VnBusId packBus = harkentest::ObjectId(voiceSend ? 13187 : 13188);
        const VnPackHandle pack = LoadResidentBusOnly(
            voiceSend ? "plu_parked_voice_send" :
                        "plu_parked_voice_dry",
            packBus);
        ASSERT_NE(0u, pack);
        const std::uint32_t bus = engine->buses.FindIndex(packBus);
        ASSERT_NE(harken::kInvalidBusIndex, bus);
        ASSERT_TRUE(engine->parkRecords.Emplace(0));
        harken::ParkRecord& parked = engine->parkRecords[0];
        parked.active = true;
        parked.facts.busIndex = voiceSend ? 0 : bus;
        if (voiceSend) {
            parked.voiceSends[0] = {true, bus, -9.0f};
        }

        EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
        EXPECT_TRUE(parked.active);
        EXPECT_EQ(voiceSend ? 0u : bus, parked.facts.busIndex);
        EXPECT_EQ(voiceSend, parked.voiceSends[0].active);
        EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
        parked = harken::ParkRecord{};
    }

    for (bool voiceSend : {false, true}) {
        SCOPED_TRACE(voiceSend ? "ended voice send" :
                                "ended dry route");
        EngineSession session(PackLogicConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const VnBusId packBus = harkentest::ObjectId(voiceSend ? 13189 : 13190);
        const VnPackHandle pack = LoadResidentBusOnly(
            voiceSend ? "plu_ended_voice_send" :
                        "plu_ended_voice_dry",
            packBus);
        ASSERT_NE(0u, pack);
        const std::vector<float> samples =
            harkentest::MakeDC(kBlock, 0.25f);
        const VnPCMDesc pcm = MonoDesc(samples, kRate);
        ASSERT_EQ(VN_OK,
                  Vn_RegisterPCM(harkentest::ObjectId(voiceSend ? 13191 : 13192), &pcm));
        VnPlayDesc play =
            MakePlayDesc(harkentest::ObjectId(voiceSend ? 13191 : 13192));
        play.busId = voiceSend ? VN_BUS_MASTER : packBus;
        play.loopCount = 1;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        if (voiceSend) {
            ASSERT_EQ(VN_OK,
                      Vn_SetVoiceSend(voice, 0, packBus, -3.0f));
        }
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(harken::VoiceLedger::HandleKind::Stale,
                  engine->ledger.Classify(voice).kind);

        EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
        EXPECT_EQ(nullptr, engine->logicPacks.RecordFor(pack));
        EXPECT_EQ(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(packBus));
    }
}

TEST(PackLogicUnload,
     OwnershipPreventsDirectDriftBeforeAtomicUnload) {
    {
        SCOPED_TRACE("parent drift");
        EngineSession session(PackLogicConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(13072), VN_BUS_MASTER));
        const VnPackHandle pack =
            LoadResidentBusOnly("plu_parent_drift", harkentest::ObjectId(13071));
        ASSERT_NE(0u, pack);
        const std::uint32_t bus = engine->buses.FindIndex(harkentest::ObjectId(13071));
        const std::uint32_t parent = engine->buses.FindIndex(harkentest::ObjectId(13072));
        ASSERT_NE(harken::kInvalidBusIndex, bus);
        ASSERT_NE(harken::kInvalidBusIndex, parent);
        ASSERT_EQ(VN_IN_USE, Vn_ReparentBus(harkentest::ObjectId(13071), harkentest::ObjectId(13072)));
        EXPECT_EQ(0u, engine->buses.ParentOf(bus));
        EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
        EXPECT_EQ(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(harkentest::ObjectId(13071)));
    }
    {
        SCOPED_TRACE("owner-cookie drift");
        EngineSession session(PackLogicConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const VnPackHandle pack =
            LoadResidentBusOnly("plu_cookie_drift", harkentest::ObjectId(13081));
        ASSERT_NE(0u, pack);
        const std::uint32_t oldBus = engine->buses.FindIndex(harkentest::ObjectId(13081));
        ASSERT_NE(harken::kInvalidBusIndex, oldBus);
        ASSERT_EQ(pack, engine->busOwnerCookies[oldBus]);
        ASSERT_EQ(VN_IN_USE, Vn_DestroyBus(harkentest::ObjectId(13081)));
        EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
        EXPECT_EQ(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(harkentest::ObjectId(13081)));
    }
    {
        SCOPED_TRACE("disconnect-reconnect send drift");
        EngineSession session(PackLogicConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const std::string path = TempPath("plu_send_drift.pack");
        ASSERT_TRUE(BuildPackFile(
            "plu_send_drift",
            V2LogicManifest(
                path, "resident",
                R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000003323", "parent": "00000000-0000-0000-0000-000000000001" },
                { "id": "3da91a64-b947-4560-0000-000000003324", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-000000003323", "to": "3da91a64-b947-4560-0000-000000003324", "levelDb": -6.0 } ])")));
        VnPackHandle pack = 0;
        ASSERT_EQ(VN_OK, LoadPack(path, &pack));
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_IN_USE, Vn_DisconnectBusSend(harkentest::ObjectId(13091), harkentest::ObjectId(13092)));
        ASSERT_EQ(VN_IN_USE,
                  Vn_SetBusSendLevel(harkentest::ObjectId(13091), harkentest::ObjectId(13092), -9.0f, 0));
        const std::uint32_t from = engine->buses.FindIndex(harkentest::ObjectId(13091));
        const std::uint32_t slot =
            FindBusSendSlot(*engine, harkentest::ObjectId(13091), harkentest::ObjectId(13092));
        ASSERT_LT(slot, engine->config.busSendSlotsPerBus);
        ASSERT_EQ(pack,
                  engine->busSendOwnerCookies[
                      from * engine->config.busSendSlotsPerBus + slot]);

        EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
        EXPECT_EQ(harken::kInvalidBusIndex,
                  engine->buses.FindIndex(harkentest::ObjectId(13091)));
    }
}

TEST(PackLogicUnload,
     OneSlotShortCommandAdmissionKeepsResidentGraphAndLedgerWhole) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.commandQueueDepth = 8;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(13102), VN_BUS_MASTER));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const VnPackHandle pack =
        LoadResidentBusOnly("plu_command_short", harkentest::ObjectId(13101));
    ASSERT_NE(0u, pack);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(harkentest::ObjectId(13101), 1, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    unsigned write = 0;
    while (engine->commands.FreeSlots() > 1) {
        ASSERT_EQ(VN_OK,
                  Vn_SetBusVolume(
                      VN_BUS_MASTER,
                      (write++ & 1u) == 0u ? -1.0f : -2.0f, 0));
    }
    ASSERT_EQ(1u, engine->commands.FreeSlots());
    const std::uint32_t bus = engine->buses.FindIndex(harkentest::ObjectId(13101));
    ASSERT_NE(harken::kInvalidBusIndex, bus);
    const harken::PackLogicRecord* before =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, before);
    ASSERT_EQ(1u, before->buses.size());

    EXPECT_EQ(VN_BACKPRESSURE, Vn_UnloadPack(pack));
    EXPECT_EQ(bus, engine->buses.FindIndex(harkentest::ObjectId(13101)));
    EXPECT_EQ(pack, engine->busOwnerCookies[bus]);
    EXPECT_EQ(1u, engine->busSoloActiveCount);
    const harken::PackLogicRecord* after =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, after);
    ASSERT_EQ(1u, after->buses.size());
    EXPECT_EQ(before->buses[0].busId, after->buses[0].busId);
    EXPECT_EQ(before->buses[0].parentBusId,
              after->buses[0].parentBusId);
    EXPECT_EQ(1u, engine->packs.LoadedCount());
}

TEST(PackLogicUnload,
     TopologyPlanExhaustionKeepsResidentGraphAndLedgerWhole) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.maxBuses = 8;
    config.topologyPlanPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(13112), VN_BUS_MASTER));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const VnPackHandle pack =
        LoadResidentBusOnly("plu_plan_exhausted", harkentest::ObjectId(13111));
    ASSERT_NE(0u, pack);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    // The only non-live plan is now held by this unrelated publish.
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(13113), VN_BUS_MASTER));
    const std::uint32_t bus = engine->buses.FindIndex(harkentest::ObjectId(13111));
    ASSERT_NE(harken::kInvalidBusIndex, bus);
    const harken::PackLogicRecord* before =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, before);
    ASSERT_EQ(1u, before->buses.size());

    EXPECT_EQ(VN_BACKPRESSURE, Vn_UnloadPack(pack));
    EXPECT_EQ(bus, engine->buses.FindIndex(harkentest::ObjectId(13111)));
    EXPECT_EQ(pack, engine->busOwnerCookies[bus]);
    EXPECT_NE(harken::kInvalidBusIndex,
              engine->buses.FindIndex(harkentest::ObjectId(13113)));
    const harken::PackLogicRecord* after =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, after);
    ASSERT_EQ(1u, after->buses.size());
    EXPECT_EQ(before->buses[0].busId, after->buses[0].busId);
    EXPECT_EQ(before->buses[0].parentBusId,
              after->buses[0].parentBusId);
    EXPECT_EQ(1u, engine->packs.LoadedCount());
}

TEST(PackLogicUnload,
     MixedGraphOneSlotShortKeepsEveryOwnedIdentityAndPlanRevision) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.maxBuses = 8;
    config.fxSlotsPerBus = 1;
    config.busSendSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 1;
    config.dynamicRouteCapacity = 1;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 1;
    config.commandQueueDepth = 8;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const VnPackHandle pack = LoadMixedReversibleResident(
        "plu_mixed_command_short", harkentest::ObjectId(13201), harkentest::ObjectId(13202));
    ASSERT_NE(0u, pack);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    unsigned write = 0;
    while (engine->commands.FreeSlots() > 1) {
        ASSERT_EQ(VN_OK,
                  Vn_SetBusVolume(
                      VN_BUS_MASTER,
                      (write++ & 1u) == 0u ? -1.0f : -2.0f, 0));
    }
    ASSERT_EQ(1u, engine->commands.FreeSlots());
    const MixedResidentState before =
        ObserveMixedResidentState(*engine, pack, harkentest::ObjectId(13201), harkentest::ObjectId(13202));

    EXPECT_EQ(VN_BACKPRESSURE, Vn_UnloadPack(pack));
    ExpectMixedResidentStateUnchanged(
        *engine, pack, harkentest::ObjectId(13201), harkentest::ObjectId(13202), before);
}

TEST(PackLogicUnload,
     MixedGraphTopologyPlanExhaustionKeepsEveryOwnedIdentityAndRevision) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.maxBuses = 8;
    config.fxSlotsPerBus = 1;
    config.busSendSlotsPerBus = 1;
    config.topologyPlanPoolCapacity = 2;
    config.compressorEffectPoolCapacity = 1;
    config.dynamicRouteCapacity = 1;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const VnPackHandle pack = LoadMixedReversibleResident(
        "plu_mixed_topology_pressure", harkentest::ObjectId(13211), harkentest::ObjectId(13212));
    ASSERT_NE(0u, pack);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    const std::uint32_t heldPlan = engine->topologyPlans.Acquire();
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, heldPlan);
    const MixedResidentState before =
        ObserveMixedResidentState(*engine, pack, harkentest::ObjectId(13211), harkentest::ObjectId(13212));

    EXPECT_EQ(VN_BACKPRESSURE, Vn_UnloadPack(pack));
    ExpectMixedResidentStateUnchanged(
        *engine, pack, harkentest::ObjectId(13211), harkentest::ObjectId(13212), before);
    engine->topologyPlans.Recycle(heldPlan);
}

TEST(PackLogicUnload,
     MixedGraphDynamicPlanExhaustionKeepsEveryOwnedIdentityAndRevision) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.maxBuses = 8;
    config.fxSlotsPerBus = 1;
    config.busSendSlotsPerBus = 1;
    config.topologyPlanPoolCapacity = 3;
    config.compressorEffectPoolCapacity = 1;
    config.dynamicRouteCapacity = 1;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const VnPackHandle pack = LoadMixedReversibleResident(
        "plu_mixed_dynamic_pressure", harkentest::ObjectId(13221), harkentest::ObjectId(13222));
    ASSERT_NE(0u, pack);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->dynamicRoutes.DrainAudioMessages();

    const std::uint32_t heldPlan =
        engine->dynamicRoutes.AcquirePlan();
    ASSERT_NE(harken::kInvalidDynamicRoutePlanIndex, heldPlan);
    const MixedResidentState before =
        ObserveMixedResidentState(*engine, pack, harkentest::ObjectId(13221), harkentest::ObjectId(13222));

    EXPECT_EQ(VN_BACKPRESSURE, Vn_UnloadPack(pack));
    ExpectMixedResidentStateUnchanged(
        *engine, pack, harkentest::ObjectId(13221), harkentest::ObjectId(13222), before);
    engine->dynamicRoutes.AbandonPlan(heldPlan);
}

TEST(PackLogicUnload,
     SoloedResidentBusUnloadRestoresSurvivingBusAudibility) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(13122), VN_BUS_MASTER));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const VnPackHandle pack =
        LoadResidentBusOnly("plu_solo_restore", harkentest::ObjectId(13121));
    ASSERT_NE(0u, pack);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    const std::vector<float> samples =
        harkentest::MakeDC(kBlock, 0.25f);
    const VnPCMDesc pcm = MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(13123), &pcm));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(13123));
    play.busId = harkentest::ObjectId(13122);
    play.loopCount = 0;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_SetBusSolo(harkentest::ObjectId(13121), 1, 0));
    harkentest::Capture muted;
    harkentest::PumpAndCapture(engine, 1, muted);
    ASSERT_EQ(0.0f, muted.left[kBlock - 1]);
    ASSERT_EQ(1u, engine->busSoloActiveCount);

    const std::uint32_t surviving = engine->buses.FindIndex(harkentest::ObjectId(13122));
    ASSERT_NE(harken::kInvalidBusIndex, surviving);
    const harken::VariableSliceRef owner{41u, 6u};
    ASSERT_TRUE(engine->busTargets.TryClaim(
        surviving, harken::kBusTargetBusVolume, owner,
        engine->busTargets.TargetRevision(
            surviving, harken::kBusTargetBusVolume)));
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_BACKPRESSURE, Vn_UnloadPack(pack));
    EXPECT_NE(harken::kInvalidBusIndex,
              engine->buses.FindIndex(harkentest::ObjectId(13121)));
    EXPECT_EQ(1u, engine->busSoloActiveCount);
    EXPECT_EQ(1u, engine->packs.LoadedCount());
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_TRUE(engine->busTargets.ForeignClaim(
        surviving, harken::kBusTargetBusVolume));
    engine->busTargets.ReleaseClaim(
        surviving, harken::kBusTargetBusVolume, owner);

    ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
    harkentest::Capture restored;
    harkentest::PumpAndCapture(engine, 1, restored);
    ASSERT_NEAR(0.25f, restored.left[kBlock - 1], 1e-5f);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->busSoloActiveCount);
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusSolo(harkentest::ObjectId(13121), 1, 0));
    EXPECT_EQ(0x3u, Snap().busSoloEffectiveMask);
    EXPECT_EQ(0u, engine->packs.LoadedCount());
}


TEST(PackLogicUnload, BindingProviderRefusedWhileConsumerLoaded) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Provider P: one media entry plus a voice-scope binding DEFINITION,
    // no attachment of its own. Consumer C: its own media plus an
    // ATTACHMENT on P's definition — the consumer's only cross-pack
    // reference is the binding id, so the provider edge this case pins
    // can come from nothing but the definition's provenance imprint.
    const std::vector<float> tone = harkentest::MakeDC(kBlock, 0.5f);
    const std::string wavP = TempPath("plu_bprov_p.wav");
    const std::string wavC = TempPath("plu_bprov_c.wav");
    WriteWavF32(wavP, tone, 1, kRate);
    WriteWavF32(wavC, tone, 1, kRate);
    const std::string providerPath = TempPath("plu_bprov_provider.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_bprov_provider",
        V2LogicManifest(
            providerPath, "content",
            "\"entries\": [ { \"path\": \"" + wavP +
                "\", \"id\": \"3da91a64-b947-4560-0000-00000000044d\" } ],\n" +
                std::string(
                    R"("bindings": [ { "id": "3da91a64-b947-4560-0000-00000000044f", "variable": "3da91a64-b947-4560-0000-000000001db1",
                "scope": "voice", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ])"))));
    const std::string consumerPath = TempPath("plu_bprov_consumer.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_bprov_consumer",
        V2LogicManifest(
            consumerPath, "content",
            "\"entries\": [ { \"path\": \"" + wavC +
                "\", \"id\": \"3da91a64-b947-4560-0000-00000000044e\" } ],\n" +
                std::string(
                    R"("attachments": [ { "binding": "3da91a64-b947-4560-0000-00000000044f", "scope": "voice",
                 "object": "3da91a64-b947-4560-0000-00000000044e" } ])"))));

    VnPackHandle provider = 0;
    VnPackHandle consumer = 0;
    ASSERT_EQ(VN_OK, LoadPack(providerPath, &provider));
    ASSERT_EQ(VN_OK, LoadPack(consumerPath, &consumer));
    EXPECT_EQ(1u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(1103)));

    // Unloading the provider refuses whole and names both parties.
    (void)DrainLogicLog(engine);
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(provider));
    const std::vector<harken::LogRecord> refused = DrainLogicLog(engine);
    ASSERT_EQ(1u, CountCode(refused, harken::kLogCodePackProviderInUse));
    const harken::LogRecord* record =
        FindCode(refused, harken::kLogCodePackProviderInUse);
    EXPECT_EQ(provider, record->arg0);
    EXPECT_EQ(consumer, record->arg1);
    // The refusal was whole: the provider's entries keep serving and
    // the consumer's attachment still rides the definition.
    EXPECT_TRUE(engine->registry.Lookup(harkentest::ObjectId(1101)).Valid());
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(1103)));
    EXPECT_EQ(1u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(1103)));
    EXPECT_EQ(2u, engine->packs.LoadedCount());

    // Reverse-dependency order clears both; the definition leaves with
    // its pack.
    EXPECT_EQ(VN_OK, Vn_UnloadPack(consumer));
    EXPECT_EQ(VN_OK, Vn_UnloadPack(provider));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(1103)));
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(1101)).Valid());
}

TEST(PackLogicUnload, ForeignRecipeProtectsBindingProvider) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::uint32_t target = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    const std::string providerPath =
        TempPath("plu_scope2_provider.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_scope2_provider",
        V2LogicManifest(
            providerPath, "content",
            "\"bindings\": [ { \"id\": \"3da91a64-b947-4560-0000-000000001fa6\", \"variable\": \"3da91a64-b947-4560-0000-000000001fa5\", "
            "\"scope\": \"voiceEffectParam\", \"target\": " +
                std::to_string(target) +
                ", \"transitionMs\": 0, "
            "\"curve\": { \"points\": [[0.0, 0.0], [1.0, 6.0]], "
            "\"handles\": [[0.0, 0.0, 1.0, 6.0]] } } ]")));

    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 2, 0.5f);
    const std::string wavPath = TempPath("plu_scope2_consumer.wav");
    WriteWavF32(wavPath, source, 1, kRate);
    const std::string consumerPath =
        TempPath("plu_scope2_consumer.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_scope2_consumer",
        V2LogicManifest(
            consumerPath, "content",
            "\"entries\": [ { \"path\": \"" + wavPath +
                "\", \"id\": \"3da91a64-b947-4560-0000-000000001fa7\" } ],\n"
                "\"voiceRecipes\": [ { \"playable\": \"3da91a64-b947-4560-0000-000000001fa7\", "
                "\"effects\": [ { \"slot\": 0, \"type\": \"gain\", "
                "\"descriptorVintage\": 2, \"bypass\": false, "
                "\"parameters\": [-6.0] } ], "
                "\"bindings\": [\"3da91a64-b947-4560-0000-000000001fa6\"] } ]")));

    VnPackHandle provider = 0;
    VnPackHandle consumer = 0;
    ASSERT_EQ(VN_OK, LoadPack(providerPath, &provider));
    ASSERT_EQ(VN_OK, LoadPack(consumerPath, &consumer));
    ASSERT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(8102)));

    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(provider));
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(8102)));
    EXPECT_EQ(2u, engine->packs.LoadedCount());

    EXPECT_EQ(VN_OK, Vn_UnloadPack(consumer));
    EXPECT_EQ(VN_OK, Vn_UnloadPack(provider));
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(8102)));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
}

TEST(PackLogicUnload,
     BusEffectAttachmentCapturesBusAndBindingDefinitionProviders) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kProviderBus = harkentest::ObjectId(8201);
    constexpr VnBindingId kProviderBinding = harkentest::ObjectId(8202);
    constexpr VnVariableId kBindingVariable = harkentest::ObjectId(8203);
    const std::string busProviderPath =
        TempPath("plu_scope3_bus_provider.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_scope3_bus_provider",
        V2LogicManifest(
            busProviderPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000002009", "parent": "00000000-0000-0000-0000-000000000001" } ])")));

    const std::uint32_t target = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    const std::string bindingProviderPath =
        TempPath("plu_scope3_binding_provider.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_scope3_binding_provider",
        V2LogicManifest(
            bindingProviderPath, "content",
            "\"bindings\": [ { \"id\": " +
                harkentest::JsonObjectId(kProviderBinding) +
                ", \"variable\": " + harkentest::JsonObjectId(kBindingVariable) +
                ", \"scope\": \"busEffectParam\", \"target\": " +
                std::to_string(target) +
                ", \"transitionMs\": 0, "
                "\"curve\": { \"points\": [[0.0, 0.0], [1.0, 6.0]], "
                "\"handles\": [[0.0, 0.0, 1.0, 6.0]] } } ]")));

    const std::string consumerPath =
        TempPath("plu_scope3_attachment_consumer.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_scope3_attachment_consumer",
        V2LogicManifest(
            consumerPath, "content",
            "\"attachments\": [ { \"binding\": " +
                harkentest::JsonObjectId(kProviderBinding) +
                ", \"scope\": \"busEffectParam\", \"object\": " +
                harkentest::JsonObjectId(kProviderBus) + " } ]")));

    VnPackHandle busProvider = 0;
    VnPackHandle bindingProvider = 0;
    VnPackHandle consumer = 0;
    ASSERT_EQ(VN_OK, LoadPack(busProviderPath, &busProvider));

    VnGainDesc gain{};
    gain.structSize = sizeof(VnGainDesc);
    gain.gainDb = -6.0f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(
                         kProviderBus, 0, VN_EFFECT_GAIN,
                         reinterpret_cast<const VnEffectDesc*>(&gain)));

    ASSERT_EQ(VN_OK, LoadPack(bindingProviderPath, &bindingProvider));
    ASSERT_EQ(VN_OK, LoadPack(consumerPath, &consumer));
    const harken::PackLogicRecord* consumerRecord =
        engine->logicPacks.RecordFor(consumer);
    ASSERT_NE(nullptr, consumerRecord);
    ASSERT_EQ(2u, consumerRecord->providers.size());
    EXPECT_NE(consumerRecord->providers.end(),
              std::find(consumerRecord->providers.begin(),
                        consumerRecord->providers.end(), busProvider));
    EXPECT_NE(consumerRecord->providers.end(),
              std::find(consumerRecord->providers.begin(),
                        consumerRecord->providers.end(), bindingProvider));

    // The content binding provider proves dependency ordering: it refuses
    // while the consumer is loaded, then unloads after the consumer leaves.
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(bindingProvider));
    EXPECT_EQ(3u, engine->packs.LoadedCount());

    ASSERT_EQ(VN_OK, Vn_UnloadPack(consumer));
    EXPECT_EQ(VN_OK, Vn_UnloadPack(bindingProvider));
    EXPECT_EQ(nullptr, engine->bindings.Find(kProviderBinding));
    // Resident ownership refuses unload independently of provider edges.
    // This final check proves only that the resident Bus remains installed.
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(busProvider));
    EXPECT_EQ(1u, engine->packs.LoadedCount());
    EXPECT_NE(harken::kInvalidBusIndex,
              engine->buses.FindIndex(kProviderBus));
}

TEST(PackLogicUnload, CallerRecycledBindingYieldsProviderEdge) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // The provider/consumer shape above, but the caller recycles the
    // definition id BEFORE the consumer loads: unregister the pack's
    // definition, then re-register the same id as a caller
    // registration. Ownership is the provenance imprint minted at
    // registration, so the consumer's closure resolves the LIVE
    // caller-owned definition and records no provider edge — a ledger
    // scan would keep claiming the id for the pack instead.
    const std::vector<float> tone = harkentest::MakeDC(kBlock, 0.5f);
    const std::string wavP = TempPath("plu_brec_p.wav");
    const std::string wavC = TempPath("plu_brec_c.wav");
    WriteWavF32(wavP, tone, 1, kRate);
    WriteWavF32(wavC, tone, 1, kRate);
    const std::string providerPath = TempPath("plu_brec_provider.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_brec_provider",
        V2LogicManifest(
            providerPath, "content",
            "\"entries\": [ { \"path\": \"" + wavP +
                "\", \"id\": \"3da91a64-b947-4560-0000-000000000457\" } ],\n" +
                std::string(
                    R"("bindings": [ { "id": "3da91a64-b947-4560-0000-000000000459", "variable": "3da91a64-b947-4560-0000-000000001dbb",
                "scope": "voice", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ])"))));
    const std::string consumerPath = TempPath("plu_brec_consumer.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_brec_consumer",
        V2LogicManifest(
            consumerPath, "content",
            "\"entries\": [ { \"path\": \"" + wavC +
                "\", \"id\": \"3da91a64-b947-4560-0000-000000000458\" } ],\n" +
                std::string(
                    R"("attachments": [ { "binding": "3da91a64-b947-4560-0000-000000000459", "scope": "voice",
                 "object": "3da91a64-b947-4560-0000-000000000458" } ])"))));

    VnPackHandle provider = 0;
    ASSERT_EQ(VN_OK, LoadPack(providerPath, &provider));

    // The recycle: the pack's definition (attachment-free, so the
    // unregister admits) leaves, and the caller mints a fresh same-id
    // definition under its own imprint.
    ASSERT_EQ(VN_OK, Vn_UnregisterBinding(harkentest::ObjectId(1113)));
    VnBindingDesc own{};
    own.structSize = sizeof(VnBindingDesc);
    own.variableId = harkentest::ObjectId(7612);
    own.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    own.targetId = 0;
    own.curve.structSize = sizeof(VnCurveDesc);
    own.curve.pointCount = 2;
    own.curve.points[0] = {0.0f, 0.0f};
    own.curve.points[1] = {1.0f, -12.0f};
    own.curve.handles[0] = harkentest::EndpointHandles(
        own.curve.points[0], own.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1113), &own));

    // The consumer's attachment resolves through the live caller-owned
    // definition and lands on it.
    VnPackHandle consumer = 0;
    ASSERT_EQ(VN_OK, LoadPack(consumerPath, &consumer));
    EXPECT_EQ(1u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(1113)));

    // No provider edge: the provider unloads immediately with the
    // consumer still loaded, and no provider-naming record surfaces.
    (void)DrainLogicLog(engine);
    EXPECT_EQ(VN_OK, Vn_UnloadPack(provider));
    const std::vector<harken::LogRecord> records = DrainLogicLog(engine);
    EXPECT_EQ(0u, CountCode(records, harken::kLogCodePackProviderInUse));
    EXPECT_EQ(1u, engine->packs.LoadedCount());
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(1111)).Valid());

    // The caller's definition outlives the pack's departure: still
    // registered, the consumer's attachment still riding, its media
    // untouched, and a fresh caller triple attaches and detaches
    // cleanly against it.
    ASSERT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(1113)));
    EXPECT_EQ(1u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(1113)));
    EXPECT_TRUE(engine->registry.Lookup(harkentest::ObjectId(1112)).Valid());
    const std::vector<float> probeTone = harkentest::MakeDC(64, 0.25f);
    const VnPCMDesc probeDesc = MonoDesc(probeTone, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1114), &probeDesc));
    VnBindingAttachDesc probe{};
    probe.structSize = sizeof(VnBindingAttachDesc);
    probe.scopeKind = VN_TARGET_SCOPE_VOICE;
    probe.objectId = harkentest::ObjectId(1114);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1113), &probe));
    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(1113), &probe));

    // The consumer unloads cleanly, taking only its own attachment;
    // the caller keeps full authority over the recycled definition.
    EXPECT_EQ(VN_OK, Vn_UnloadPack(consumer));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    ASSERT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(1113)));
    EXPECT_EQ(0u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(1113)));
    EXPECT_EQ(VN_OK, Vn_UnregisterBinding(harkentest::ObjectId(1113)));
}


TEST(PackLogicUnload, RefusedWholeWhileVoicePlaysPackMedia) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Two blocks of material so the voice is still live after one pump.
    const std::vector<float> tone = harkentest::MakeDC(kBlock * 2, 0.5f);
    HARKENTEST_ASSERT_NON_SILENT(tone);
    const std::string wavPath = TempPath("plu_inuse_tone.wav");
    WriteWavF32(wavPath, tone, 1, kRate);
    const std::string packPath = TempPath("plu_inuse.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_inuse",
        V2LogicManifest(
            packPath, "content",
            "\"entries\": [ { \"path\": \"" + wavPath +
                "\", \"id\": \"3da91a64-b947-4560-0000-0000000003ad\" } ]")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(941));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    engine->nullBackend.Pump(1);

    // The whole unload refuses and the media still resolves.
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_TRUE(engine->registry.Lookup(harkentest::ObjectId(941)).Valid());

    // Stop and drain; the unload is admitted.
    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(941)).Valid());
}

TEST(PackLogicUnload,
     RecipePackStaysOwnedAcrossPendingAndLiveVoiceThenUnloadsAfterDrain) {
    VnConfig config = PackLogicConfig(1, kBlock, kRate);
    config.voiceStealPolicy = 2;
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const harkentest::RecipePackArtifact artifact =
        harkentest::BuildRecipePack("plu_recipe_pending_live", "gain", "-12.0",
                                   {}, kRate);
    ASSERT_TRUE(artifact.built);
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(artifact.path, &pack));

    // A caller-owned long voice occupies the only render slot. The Recipe play
    // steals it, but remains pending until the victim's declick completes.
    const std::vector<float> incumbentMedia =
        harkentest::MakeDC(kRate, 0.25f);
    const VnPCMDesc incumbentDesc = MonoDesc(incumbentMedia, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9001), &incumbentDesc));
    VnVoiceHandle incumbent = 0;
    const VnPlayDesc incumbentPlay = MakePlayDesc(harkentest::ObjectId(9001));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&incumbentPlay, &incumbent));
    engine->nullBackend.Pump(1);

    VnVoiceHandle recipeVoice = 0;
    const VnPlayDesc recipePlay = MakePlayDesc(harkentest::ObjectId(9101));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&recipePlay, &recipeVoice));
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(recipeVoice).kind);

    // Pending ownership protects both the Recipe and its media provider. The
    // refused unload is atomic: neither entry nor the pack record is removed.
    ASSERT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_EQ(1u, engine->packs.LoadedCount());
    EXPECT_TRUE(engine->registry.Lookup(harkentest::ObjectId(9101)).Valid());
    EXPECT_NE(nullptr, engine->logicPacks.FindVoiceRecipe(harkentest::ObjectId(9101)));

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    harkentest::Capture live;
    harkentest::PumpAndCapture(engine, 1, live);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(recipeVoice).kind);
    HARKENTEST_ASSERT_NON_SILENT(live.left);

    // The same public handle now owns a live Recipe instance, which keeps the
    // same pack/media protection without any partial unload mutation.
    ASSERT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_EQ(1u, engine->packs.LoadedCount());
    EXPECT_TRUE(engine->registry.Lookup(harkentest::ObjectId(9101)).Valid());
    EXPECT_NE(nullptr, engine->logicPacks.FindVoiceRecipe(harkentest::ObjectId(9101)));

    ASSERT_EQ(VN_OK, Vn_Stop(recipeVoice, 0));
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(9101)).Valid());
    EXPECT_EQ(nullptr, engine->logicPacks.FindVoiceRecipe(harkentest::ObjectId(9101)));
}

TEST(PackLogicUnload, DetachSwapOnSameDefinitionRefusesUnload) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // One definition with one own attachment on the pack's media.
    const std::vector<float> tone = harkentest::MakeDC(kBlock, 0.5f);
    const std::string wavPath = TempPath("plu_swap_tone.wav");
    WriteWavF32(wavPath, tone, 1, kRate);
    const std::string packPath = TempPath("plu_swap.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_swap",
        V2LogicManifest(
            packPath, "content",
            "\"entries\": [ { \"path\": \"" + wavPath +
                "\", \"id\": \"3da91a64-b947-4560-0000-000000000425\" } ],\n" +
                std::string(
                    R"("bindings": [ { "id": "3da91a64-b947-4560-0000-000000000427", "variable": "3da91a64-b947-4560-0000-000000001d7f",
                "scope": "voice", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-000000000427", "scope": "voice", "object": "3da91a64-b947-4560-0000-000000000425" } ])"))));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));

    // A caller-registered playable for the swapped-in attachment.
    const std::vector<float> own = harkentest::MakeDC(64, 0.25f);
    const VnPCMDesc ownDesc = MonoDesc(own, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1062), &ownDesc));

    // The caller detaches the pack's own triple and attaches a DIFFERENT
    // one to the same definition: the total attachment count still reads
    // one, but the own triple is gone and the live one is foreign.
    VnBindingAttachDesc ownTriple{};
    ownTriple.structSize = sizeof(VnBindingAttachDesc);
    ownTriple.scopeKind = VN_TARGET_SCOPE_VOICE;
    ownTriple.objectId = harkentest::ObjectId(1061);
    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(1063), &ownTriple));
    VnBindingAttachDesc foreignTriple{};
    foreignTriple.structSize = sizeof(VnBindingAttachDesc);
    foreignTriple.scopeKind = VN_TARGET_SCOPE_VOICE;
    foreignTriple.objectId = harkentest::ObjectId(1062);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1063), &foreignTriple));
    EXPECT_EQ(1u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(1063)));

    // Count arithmetic would balance (one recorded, one live); the
    // per-exact-triple screen refuses because the live triple is not
    // the pack's — the unload changes nothing.
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_TRUE(engine->registry.Lookup(harkentest::ObjectId(1061)).Valid());
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(1063)));
    EXPECT_EQ(1u, engine->packs.LoadedCount());

    // With the foreign triple detached the unload is admitted, even
    // though the pack's recorded own attachment is long gone: the
    // caller-detached triple neither gates the unload nor is touched by
    // it, so the drain sees no apply defect.
    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(1063), &foreignTriple));
    (void)DrainLogicLog(engine);
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
    const std::vector<harken::LogRecord> records = DrainLogicLog(engine);
    EXPECT_EQ(0u, CountCode(records, harken::kLogCodePackApplyDefect));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(1063)));
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(1061)).Valid());
}

TEST(PackLogicUnload, IdenticalReattachAfterDetachStaysCallerOwned) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // One definition with one own attachment on the pack's media.
    const std::vector<float> tone = harkentest::MakeDC(kBlock, 0.5f);
    const std::string wavPath = TempPath("plu_rebirth_tone.wav");
    WriteWavF32(wavPath, tone, 1, kRate);
    const std::string packPath = TempPath("plu_rebirth.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_rebirth",
        V2LogicManifest(
            packPath, "content",
            "\"entries\": [ { \"path\": \"" + wavPath +
                "\", \"id\": \"3da91a64-b947-4560-0000-00000000042f\" } ],\n" +
                std::string(
                    R"("bindings": [ { "id": "3da91a64-b947-4560-0000-000000000431", "variable": "3da91a64-b947-4560-0000-000000001d89",
                "scope": "voice", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-000000000431", "scope": "voice", "object": "3da91a64-b947-4560-0000-00000000042f" } ])"))));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));

    // The caller detaches the pack's own triple and re-attaches the
    // IDENTICAL triple. Detach clears the slot whole, so the rebirth
    // mints a fresh CALLER-owned attachment on the pack's definition —
    // ownership is a provenance imprint, not existence.
    VnBindingAttachDesc triple{};
    triple.structSize = sizeof(VnBindingAttachDesc);
    triple.scopeKind = VN_TARGET_SCOPE_VOICE;
    triple.objectId = harkentest::ObjectId(1071);
    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(1073), &triple));
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1073), &triple));
    EXPECT_EQ(1u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(1073)));

    // Existence and count read exactly like the pack's own attachment;
    // the unload gate must see a FOREIGN reference to the definition and
    // refuse whole — never swallow the caller's attachment as its own.
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_EQ(1u, engine->packs.LoadedCount());
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(1073)));
    EXPECT_EQ(1u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(1073)));
    EXPECT_TRUE(engine->registry.Lookup(harkentest::ObjectId(1071)).Valid());

    // Releasing the caller's attachment admits the unload; the pack's
    // definition leaves with it, so the very same triple has nothing
    // left to attach to.
    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(1073), &triple));
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(1073)));
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(1071)).Valid());
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_AttachBinding(harkentest::ObjectId(1073), &triple));
}



// --- replace: staged revision, activation at the update tick ---

TEST(PackLogicReplace, ReplaceStagesAndActivatesAtUpdate) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Two one-block DC media at cleanly separated amplitudes make the staged
    // registry swap observable through raw Voice starts.
    const std::string wavOld = TempPath("plu_replace_old.wav");
    const std::string wavNew = TempPath("plu_replace_new.wav");
    WriteWavF32(wavOld, harkentest::MakeDC(kBlock, 0.25f), 1, kRate);
    WriteWavF32(wavNew, harkentest::MakeDC(kBlock, 0.5f), 1, kRate);

    const std::string v1Path = TempPath("plu_replace_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_replace_v1",
        V2LogicManifest(
            v1Path, "content",
            "\"entries\": [ { \"path\": \"" + wavOld +
                "\", \"id\": \"3da91a64-b947-4560-0000-0000000003c1\" } ]")));
    const std::string v2Path = TempPath("plu_replace_v2.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_replace_v2",
        V2LogicManifest(
            v2Path, "content",
            "\"entries\": [ { \"path\": \"" + wavNew +
                "\", \"id\": \"3da91a64-b947-4560-0000-0000000003c2\" } ]")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));

    // A logic-bearing replace STAGES: VN_OK means the revision is booked
    // (the hot-reload counter moves at the call) but nothing swapped yet.
    const std::uint64_t appliedBefore = Snap().hotReloadCount;
    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));
    EXPECT_EQ(appliedBefore + 1, Snap().hotReloadCount);

    // The gap still sees the old registry.
    const VnPlayDesc oldPlay = MakePlayDesc(harkentest::ObjectId(961));
    VnVoiceHandle oldVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&oldPlay, &oldVoice));
    harkentest::Capture gap;
    harkentest::PumpAndCapture(engine, 1, gap);
    HARKENTEST_ASSERT_NON_SILENT(gap.left);
    EXPECT_FLOAT_EQ(0.25f, gap.left[kBlock / 2]);

    // One update pump is the single activation point.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(961)).Valid());
    const VnPlayDesc newPlay = MakePlayDesc(harkentest::ObjectId(962));
    VnVoiceHandle newVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&newPlay, &newVoice));
    harkentest::Capture swapped;
    harkentest::PumpAndCapture(engine, 1, swapped);
    HARKENTEST_ASSERT_NON_SILENT(swapped.left);
    EXPECT_FLOAT_EQ(0.5f, swapped.left[kBlock / 2]);
}

TEST(PackLogicReplace,
     ZeroCommandContentRevisionPublishesMediaAndLedgerExactlyOnce) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string wavOld =
        TempPath("plr_zero_command_old.wav");
    const std::string wavNew =
        TempPath("plr_zero_command_new.wav");
    WriteWavF32(wavOld, harkentest::MakeDC(kBlock, 0.25f), 1, kRate);
    WriteWavF32(wavNew, harkentest::MakeDC(kBlock, 0.5f), 1, kRate);
    const auto buildRevision = [&](const char* name,
                                   const std::string& wavPath) {
        const std::string packPath =
            TempPath(std::string(name) + ".pack");
        const bool built = BuildPackFile(
            name,
            V2LogicManifest(
                packPath, "content",
                "\"entries\": [ { \"path\": \"" + wavPath +
                    "\", \"id\": \"3da91a64-b947-4560-0000-0000000003c7\" } ]"));
        EXPECT_TRUE(built);
        return packPath;
    };
    const std::string initialPath =
        buildRevision("plr_zero_command_v1", wavOld);
    const std::string replacementPath =
        buildRevision("plr_zero_command_v2", wavNew);

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    const auto mediaChecksum = [](const harken::PackLogicRecord& record) {
        for (const harken::PackLogicImage::Stamp& stamp : record.stamps) {
            if (stamp.kind == harken::kPackEntryKindMedia &&
                stamp.id == harkentest::ObjectId(967)) {
                return stamp.checksum;
            }
        }
        return std::uint64_t{0};
    };
    const harken::PackLogicRecord* oldRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, oldRecord);
    const std::uint64_t oldChecksum = mediaChecksum(*oldRecord);
    ASSERT_NE(0u, oldChecksum);
    const std::uint64_t hotReloadBefore = Snap().hotReloadCount;

    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));
    ASSERT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    const VnPlayDesc gapPlay = MakePlayDesc(harkentest::ObjectId(967));
    VnVoiceHandle gapVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&gapPlay, &gapVoice));
    harkentest::Capture gap;
    harkentest::PumpAndCapture(engine, 1, gap);
    HARKENTEST_ASSERT_NON_SILENT(gap.left);
    EXPECT_FLOAT_EQ(0.25f, gap.left[kBlock / 2]);

    const std::uint32_t queueFreeBefore =
        engine->commands.FreeSlots();
    const std::uint32_t opcodeMaskBefore =
        engine->commands.OpcodeSeenMask();
    const std::uint64_t topologyBefore =
        engine->topologyPlanCount;
    const std::uint64_t dynamicRevisionBefore =
        engine->dynamicRoutes.LastIssuedPlanRevision();
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(queueFreeBefore, engine->commands.FreeSlots());
    EXPECT_EQ(opcodeMaskBefore, engine->commands.OpcodeSeenMask());
    EXPECT_EQ(topologyBefore, engine->topologyPlanCount);
    EXPECT_EQ(dynamicRevisionBefore,
              engine->dynamicRoutes.LastIssuedPlanRevision());
    EXPECT_EQ(hotReloadBefore + 1, Snap().hotReloadCount);
    const harken::PackLogicRecord* liveRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, liveRecord);
    const std::uint64_t newChecksum = mediaChecksum(*liveRecord);
    EXPECT_NE(0u, newChecksum);
    EXPECT_NE(oldChecksum, newChecksum);

    const VnPlayDesc swappedPlay = MakePlayDesc(harkentest::ObjectId(967));
    VnVoiceHandle swappedVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&swappedPlay, &swappedVoice));
    harkentest::Capture swapped;
    harkentest::PumpAndCapture(engine, 1, swapped);
    HARKENTEST_ASSERT_NON_SILENT(swapped.left);
    EXPECT_FLOAT_EQ(0.5f, swapped.left[kBlock / 2]);

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(hotReloadBefore + 1, Snap().hotReloadCount);
    EXPECT_EQ(newChecksum,
              mediaChecksum(*engine->logicPacks.RecordFor(pack)));
    EXPECT_EQ(topologyBefore, engine->topologyPlanCount);
    EXPECT_EQ(dynamicRevisionBefore,
              engine->dynamicRoutes.LastIssuedPlanRevision());
    EXPECT_EQ(0u, engine->packs.RetiringCount());
}

TEST(PackLogicReplace, BusSendRelevelRetainsOwnerCookie) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const auto buildRevision = [&](const char* name, float levelDb) {
        const std::string path =
            TempPath(std::string(name) + ".pack");
        const bool built = BuildPackFile(
            name,
            V2LogicManifest(
                path, "resident",
                "\"buses\": [ { \"id\": \"3da91a64-b947-4560-0000-000000002f45\", \"parent\": \"00000000-0000-0000-0000-000000000001\" }, "
                "{ \"id\": \"3da91a64-b947-4560-0000-000000002f46\", \"parent\": \"00000000-0000-0000-0000-000000000001\" } ],\n"
                "\"busSends\": [ { \"from\": \"3da91a64-b947-4560-0000-000000002f45\", \"to\": \"3da91a64-b947-4560-0000-000000002f46\", "
                "\"levelDb\": " +
                    std::to_string(levelDb) + " } ]"));
        EXPECT_TRUE(built);
        return path;
    };
    const std::string v1Path =
        buildRevision("plr_send_relevel_v1", -6.0f);
    const std::string v2Path =
        buildRevision("plr_send_relevel_v2", -12.0f);

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    const std::uint32_t from = engine->buses.FindIndex(harkentest::ObjectId(12101));
    ASSERT_NE(harken::kInvalidBusIndex, from);
    const std::uint32_t oldSlot =
        FindBusSendSlot(*engine, harkentest::ObjectId(12101), harkentest::ObjectId(12102));
    ASSERT_LT(oldSlot, engine->config.busSendSlotsPerBus);
    ASSERT_EQ(pack,
              engine->busSendOwnerCookies[
                  from * engine->config.busSendSlotsPerBus + oldSlot]);

    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t liveSlot =
        FindBusSendSlot(*engine, harkentest::ObjectId(12101), harkentest::ObjectId(12102));
    EXPECT_EQ(oldSlot, liveSlot);
    EXPECT_EQ(pack,
              engine->busSendOwnerCookies[
                  from * engine->config.busSendSlotsPerBus + liveSlot]);
    const harken::PackLogicRecord* record =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, record);
    ASSERT_EQ(1u, record->busSends.size());
    EXPECT_FLOAT_EQ(-12.0f, record->busSends[0].levelDb);
}

TEST(PackLogicReplace, BusSendRemoveAndAddUpdateOwnerCookies) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const auto buildRevision = [&](const char* name, VnBusId target,
                                   bool includeThirdBus) {
        const std::string path =
            TempPath(std::string(name) + ".pack");
        std::string sections =
            "\"buses\": [ { \"id\": \"3da91a64-b947-4560-0000-000000002fa9\", \"parent\": \"00000000-0000-0000-0000-000000000001\" }, "
            "{ \"id\": \"3da91a64-b947-4560-0000-000000002faa\", \"parent\": \"00000000-0000-0000-0000-000000000001\" }";
        if (includeThirdBus) {
            sections += ", { \"id\": \"3da91a64-b947-4560-0000-000000002fab\", \"parent\": \"00000000-0000-0000-0000-000000000001\" }";
        }
        sections +=
            " ],\n\"busSends\": [ { \"from\": \"3da91a64-b947-4560-0000-000000002fa9\", \"to\": " +
            harkentest::JsonObjectId(target) + ", \"levelDb\": -3.0 } ]";
        const bool built = BuildPackFile(
            name,
            V2LogicManifest(path, "resident", sections));
        EXPECT_TRUE(built);
        return path;
    };
    const std::string v1Path =
        buildRevision("plr_send_replace_v1", harkentest::ObjectId(12202), false);
    const std::string v2Path =
        buildRevision("plr_send_replace_v2", harkentest::ObjectId(12203), true);

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    const std::uint32_t from = engine->buses.FindIndex(harkentest::ObjectId(12201));
    ASSERT_NE(harken::kInvalidBusIndex, from);
    const std::uint32_t oldSlot =
        FindBusSendSlot(*engine, harkentest::ObjectId(12201), harkentest::ObjectId(12202));
    ASSERT_LT(oldSlot, engine->config.busSendSlotsPerBus);

    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(engine->config.busSendSlotsPerBus,
              FindBusSendSlot(*engine, harkentest::ObjectId(12201), harkentest::ObjectId(12202)));
    const std::uint32_t newSlot =
        FindBusSendSlot(*engine, harkentest::ObjectId(12201), harkentest::ObjectId(12203));
    ASSERT_LT(newSlot, engine->config.busSendSlotsPerBus);
    const std::uint32_t addedBus = engine->buses.FindIndex(harkentest::ObjectId(12203));
    ASSERT_NE(harken::kInvalidBusIndex, addedBus);
    EXPECT_EQ(pack, engine->busOwnerCookies[addedBus]);
    EXPECT_EQ(pack,
              engine->busSendOwnerCookies[
                  from * engine->config.busSendSlotsPerBus + newSlot]);
    for (std::uint32_t slot = 0;
         slot < engine->config.busSendSlotsPerBus; ++slot) {
        if (slot != newSlot) {
            EXPECT_EQ(
                0u,
                engine->busSendOwnerCookies[
                    from * engine->config.busSendSlotsPerBus + slot]);
        }
    }
}

TEST(PackLogicLoad, DirectMutationCannotTakeOverPackOwnedBus) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string providerPath =
        TempPath("pll_bus_recycle_provider.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_bus_recycle_provider",
        V2LogicManifest(
            providerPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-00000000300d", "parent": "00000000-0000-0000-0000-000000000001" } ])")));
    VnPackHandle provider = 0;
    ASSERT_EQ(VN_OK, LoadPack(providerPath, &provider));
    const std::uint32_t oldIndex = engine->buses.FindIndex(harkentest::ObjectId(12301));
    ASSERT_NE(harken::kInvalidBusIndex, oldIndex);
    ASSERT_EQ(provider, engine->busOwnerCookies[oldIndex]);

    EXPECT_EQ(VN_IN_USE, Vn_DestroyBus(harkentest::ObjectId(12301)));
    EXPECT_EQ(VN_IN_USE, Vn_CreateBus(harkentest::ObjectId(12301), VN_BUS_MASTER));
    EXPECT_EQ(oldIndex, engine->buses.FindIndex(harkentest::ObjectId(12301)));
    EXPECT_EQ(provider, engine->busOwnerCookies[oldIndex]);
}

TEST(PackLogicLoad, DirectMutationCannotTakeOverPackOwnedSend) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string packPath =
        TempPath("pll_send_recycle.pack");
    ASSERT_TRUE(BuildPackFile(
        "pll_send_recycle",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000003071", "parent": "00000000-0000-0000-0000-000000000001" },
            { "id": "3da91a64-b947-4560-0000-000000003072", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-000000003071", "to": "3da91a64-b947-4560-0000-000000003072", "levelDb": -6.0 } ])")));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    const std::uint32_t from = engine->buses.FindIndex(harkentest::ObjectId(12401));
    ASSERT_NE(harken::kInvalidBusIndex, from);
    const std::uint32_t oldSlot =
        FindBusSendSlot(*engine, harkentest::ObjectId(12401), harkentest::ObjectId(12402));
    ASSERT_LT(oldSlot, engine->config.busSendSlotsPerBus);
    ASSERT_EQ(pack,
              engine->busSendOwnerCookies[
                  from * engine->config.busSendSlotsPerBus + oldSlot]);

    ASSERT_EQ(VN_IN_USE,
              Vn_SetBusSendLevel(harkentest::ObjectId(12401), harkentest::ObjectId(12402), -7.0f, 0));
    EXPECT_EQ(pack,
              engine->busSendOwnerCookies[
                  from * engine->config.busSendSlotsPerBus + oldSlot]);
    ASSERT_EQ(VN_IN_USE, Vn_DisconnectBusSend(harkentest::ObjectId(12401), harkentest::ObjectId(12402)));
    ASSERT_EQ(VN_IN_USE, Vn_ConnectBusSend(harkentest::ObjectId(12401), harkentest::ObjectId(12402), -9.0f));
    const std::uint32_t newSlot =
        FindBusSendSlot(*engine, harkentest::ObjectId(12401), harkentest::ObjectId(12402));
    ASSERT_LT(newSlot, engine->config.busSendSlotsPerBus);
    EXPECT_EQ(pack,
              engine->busSendOwnerCookies[
                  from * engine->config.busSendSlotsPerBus + newSlot]);
}

TEST(PackLogicReplace, DirectParentDriftIsBlockedByOwnership) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string packPath =
        TempPath("plr_bus_parent_drift.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_bus_parent_drift",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-0000000030d5", "parent": "00000000-0000-0000-0000-000000000001" },
            { "id": "3da91a64-b947-4560-0000-0000000030d6", "parent": "00000000-0000-0000-0000-000000000001" } ])")));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    const std::uint32_t child = engine->buses.FindIndex(harkentest::ObjectId(12501));
    const std::uint32_t parent = engine->buses.FindIndex(harkentest::ObjectId(12502));
    ASSERT_NE(harken::kInvalidBusIndex, child);
    ASSERT_NE(harken::kInvalidBusIndex, parent);
    ASSERT_EQ(VN_IN_USE, Vn_ReparentBus(harkentest::ObjectId(12501), harkentest::ObjectId(12502)));
    ASSERT_EQ(0u, engine->buses.ParentOf(child));
    ASSERT_EQ(pack, engine->busOwnerCookies[child]);

    EXPECT_EQ(VN_OK, ReplacePack(pack, packPath));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->buses.ParentOf(child));
    EXPECT_EQ(pack, engine->busOwnerCookies[child]);
}

TEST(PackLogicReplace,
     CompleteBusSnapshotRemovesAddsAndReparentsAtFullCapacity) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.maxBuses = 3;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string firstPath =
        TempPath("plr_complete_bus_snapshot_v1.pack");
    const std::string secondPath =
        TempPath("plr_complete_bus_snapshot_v2.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_complete_bus_snapshot_v1",
        V2LogicManifest(
            firstPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-00000000319d", "parent": "00000000-0000-0000-0000-000000000001" },
            { "id": "3da91a64-b947-4560-0000-00000000319e", "parent": "00000000-0000-0000-0000-000000000001" } ])")));
    ASSERT_TRUE(BuildPackFile(
        "plr_complete_bus_snapshot_v2",
        V2LogicManifest(
            secondPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-00000000319f", "parent": "00000000-0000-0000-0000-000000000001" },
            { "id": "3da91a64-b947-4560-0000-00000000319e", "parent": "3da91a64-b947-4560-0000-00000000319f" } ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(firstPath, &pack));
    const std::uint32_t removedSlot =
        engine->buses.FindIndex(harkentest::ObjectId(12701));
    const std::uint32_t retainedSlot =
        engine->buses.FindIndex(harkentest::ObjectId(12702));
    const std::uint32_t removedGeneration =
        engine->buses.GenerationAt(removedSlot);
    const std::uint32_t retainedGeneration =
        engine->buses.GenerationAt(retainedSlot);

    ASSERT_EQ(VN_OK, ReplacePack(pack, secondPath));
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(harken::kInvalidBusIndex,
              engine->buses.FindIndex(harkentest::ObjectId(12701)));
    EXPECT_EQ(retainedSlot, engine->buses.FindIndex(harkentest::ObjectId(12702)));
    EXPECT_EQ(retainedGeneration,
              engine->buses.GenerationAt(retainedSlot));
    EXPECT_EQ(removedSlot, engine->buses.FindIndex(harkentest::ObjectId(12703)));
    EXPECT_GT(engine->buses.GenerationAt(removedSlot),
              removedGeneration);
    EXPECT_EQ(removedSlot, engine->buses.ParentOf(retainedSlot));
    EXPECT_EQ(pack, engine->busOwnerCookies[removedSlot]);
    EXPECT_EQ(pack, engine->busOwnerCookies[retainedSlot]);
}

TEST(PackLogicReplace, DirectSendOwnershipDriftIsBlocked) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string packPath =
        TempPath("plr_send_slot_drift.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_send_slot_drift",
        V2LogicManifest(
            packPath, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000003139", "parent": "00000000-0000-0000-0000-000000000001" },
            { "id": "3da91a64-b947-4560-0000-00000000313a", "parent": "00000000-0000-0000-0000-000000000001" },
            { "id": "3da91a64-b947-4560-0000-00000000313b", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-000000003139", "to": "3da91a64-b947-4560-0000-00000000313a", "levelDb": -6.0 } ])")));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    const std::uint32_t from = engine->buses.FindIndex(harkentest::ObjectId(12601));
    ASSERT_NE(harken::kInvalidBusIndex, from);
    const std::uint32_t packSlot =
        FindBusSendSlot(*engine, harkentest::ObjectId(12601), harkentest::ObjectId(12602));
    ASSERT_LT(packSlot, engine->config.busSendSlotsPerBus);

    ASSERT_EQ(VN_IN_USE, Vn_DisconnectBusSend(harkentest::ObjectId(12601), harkentest::ObjectId(12602)));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(harkentest::ObjectId(12601), harkentest::ObjectId(12603), -3.0f));
    ASSERT_EQ(VN_IN_USE, Vn_ConnectBusSend(harkentest::ObjectId(12601), harkentest::ObjectId(12602), -9.0f));
    const std::uint32_t recycledSlot =
        FindBusSendSlot(*engine, harkentest::ObjectId(12601), harkentest::ObjectId(12602));
    ASSERT_LT(recycledSlot, engine->config.busSendSlotsPerBus);
    ASSERT_EQ(packSlot, recycledSlot);
    EXPECT_EQ(pack,
              engine->busSendOwnerCookies[
                  from * engine->config.busSendSlotsPerBus + recycledSlot]);

    EXPECT_EQ(VN_OK, ReplacePack(pack, packPath));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NE(packSlot, FindBusSendSlot(*engine, harkentest::ObjectId(12601), harkentest::ObjectId(12603)));
    EXPECT_EQ(recycledSlot,
              FindBusSendSlot(*engine, harkentest::ObjectId(12601), harkentest::ObjectId(12602)));
}

TEST(CanonicalPackSetActivate, ReplacementAndUnloadPreserveForeignPackSendSlot) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto source = harkentest::ObjectId(12801);
    const auto firstTarget = harkentest::ObjectId(12802);
    const auto secondTarget = harkentest::ObjectId(12803);
    const std::string foreignPath = TempPath("canonical_foreign_send_pack.pack");
    const std::string sections =
        "\"buses\": [{\"id\": " + harkentest::JsonObjectId(source) +
        ", \"parent\": \"00000000-0000-0000-0000-000000000001\"}, "
        "{\"id\": " + harkentest::JsonObjectId(firstTarget) +
        ", \"parent\": \"00000000-0000-0000-0000-000000000001\"}, "
        "{\"id\": " + harkentest::JsonObjectId(secondTarget) +
        ", \"parent\": \"00000000-0000-0000-0000-000000000001\"}], "
        "\"busSends\": [{\"from\": " + harkentest::JsonObjectId(source) +
        ", \"to\": " + harkentest::JsonObjectId(firstTarget) +
        ", \"levelDb\": -3.0}, {\"from\": " +
        harkentest::JsonObjectId(source) + ", \"to\": " +
        harkentest::JsonObjectId(secondTarget) + ", \"levelDb\": -9.0}]";
    ASSERT_TRUE(BuildPackFile("canonical_foreign_send_pack",
        V2LogicManifest(foreignPath, "resident", sections)));
    VnPackHandle foreignPack = 0;
    ASSERT_EQ(VN_OK, LoadPack(foreignPath, &foreignPack));
    ASSERT_NE(0u, foreignPack);
    const auto sourceIndex = engine->buses.FindIndex(source);
    ASSERT_NE(harken::kInvalidBusIndex, sourceIndex);
    const auto slotsPerBus = engine->config.busSendSlotsPerBus;
    const auto slotA = FindBusSendSlot(*engine, source, firstTarget);
    const auto slotB = FindBusSendSlot(*engine, source, secondTarget);
    ASSERT_LT(slotA, slotsPerBus);
    ASSERT_LT(slotB, slotsPerBus);
    ASSERT_NE(slotA, slotB);
    const bool preserveSecond = slotB != 0;
    const auto target = preserveSecond ? secondTarget : firstTarget;
    const auto expectedLevelDb = preserveSecond ? -9.0f : -3.0f;
    const auto physicalSlot = preserveSecond ? slotB : slotA;
    ASSERT_GT(physicalSlot, 0u);
    const auto flat = static_cast<std::size_t>(sourceIndex) * slotsPerBus +
        physicalSlot;
    const auto ownerId = engine->busSendTopologyOwnerIds[flat];
    const auto checkForeignSend = [&] {
        EXPECT_EQ(sourceIndex, engine->buses.FindIndex(source));
        EXPECT_EQ(physicalSlot, FindBusSendSlot(*engine, source, target));
        EXPECT_EQ(engine->buses.FindIndex(target), engine->busSendAdj[flat]);
        EXPECT_FLOAT_EQ(expectedLevelDb, engine->busSendLevelDb[flat]);
        EXPECT_EQ(ownerId, engine->busSendTopologyOwnerIds[flat]);
        EXPECT_EQ(foreignPack, engine->busSendOwnerCookies[flat]);
    };
    checkForeignSend();

    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) /
        "canonical-foreign-pack-send-replace";
    const auto first = harkentest::ActivateInitBytesV1(directory,
        harkentest::BuildMasterSendInitBytes(-6.0f),
        harkentest::packfoundation::kInitPackStableId);
    ASSERT_EQ(VN_OK, first.result);
    ASSERT_NE(foreignPack, first.initHandle);
    checkForeignSend();

    const std::string replacementPath = (directory / "replacement.pack").string();
    ASSERT_TRUE(harkentest::WriteBytes(replacementPath,
        harkentest::BuildMasterSendInitBytes(-12.0f)));
    VnCanonicalPackSetMemberV1 member{};
    member.structSize = sizeof(member);
    member.packStableId = harkentest::packfoundation::kInitPackStableId;
    member.packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    member.expectedHandle = first.initHandle;
    member.utf8Path = replacementPath.c_str();
    VnCanonicalPackSetMemberResultV1 row{};
    row.structSize = sizeof(row);
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        &member, 1, &row, report, first.initHandle,
        first.report.resultSetRevision));
    checkForeignSend();

    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = report.initHandle;
    unload.baseSetRevision = report.resultSetRevision;
    unload.unloadRevision = report.resultSetRevision + 1;
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    checkForeignSend();
    EXPECT_EQ(VN_IN_USE, Vn_DisconnectBusSend(source, target));
    EXPECT_EQ(VN_IN_USE, Vn_SetBusSendLevel(source, target, -12.0f, 0));
    checkForeignSend();
}

TEST(PackLogicReplace,
     Scope2IdentityChangeActivatesAndGapReferenceKeepsOldRevisionWhole) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> source =
        harkentest::MakeDC(kBlock * 8, 0.5f);
    const VnPCMDesc pcm = MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7980), &pcm));
    const std::uint32_t target = harken::PackEffectParamLocator(
        VN_EFFECT_GAIN, 0, harken::kGainParamGainDb);
    const auto buildRevision = [&](const char* name,
                                   VnVariableId variable) {
        const std::string path =
            TempPath(std::string(name) + ".pack");
        const bool built = BuildPackFile(
            name,
            V2LogicManifest(
                path, "content",
                "\"bindings\": [ { \"id\": \"3da91a64-b947-4560-0000-000000001f2d\", \"variable\": " +
                    harkentest::JsonObjectId(variable) +
                    ", \"scope\": \"voiceEffectParam\", \"target\": " +
                    std::to_string(target) +
                    ", \"transitionMs\": 0, \"curve\": { \"points\": "
                    "[[0.0, 0.0], [1.0, 6.0]], \"handles\": "
                    "[[0.0, 0.0, 1.0, 6.0]] } } ],\n"
                    "\"voiceRecipes\": [ { \"playable\": \"3da91a64-b947-4560-0000-000000001f2c\", "
                    "\"effects\": [ { \"slot\": 0, \"type\": \"gain\", "
                    "\"descriptorVintage\": 2, \"bypass\": false, "
                    "\"parameters\": [-6.0] } ], "
                    "\"bindings\": [\"3da91a64-b947-4560-0000-000000001f2d\"] } ]"));
        EXPECT_TRUE(built);
        return path;
    };

    const std::string v1Path = buildRevision("plr_scope2_v1", harkentest::ObjectId(7982));
    const std::string v2Path = buildRevision("plr_scope2_v2", harkentest::ObjectId(7983));
    const std::string v3Path = buildRevision("plr_scope2_v3", harkentest::ObjectId(7984));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    ASSERT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(7981)));
    EXPECT_EQ(harkentest::ObjectId(7982u), engine->bindings.Find(harkentest::ObjectId(7981))->variableId);

    // With no live instance, the identity-changing remove/add may ignore
    // only this pack's own Recipe reference and activates normally.
    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(7981)));
    EXPECT_EQ(harkentest::ObjectId(7983u), engine->bindings.Find(harkentest::ObjectId(7981))->variableId);
    ASSERT_NE(nullptr, engine->logicPacks.FindVoiceRecipe(harkentest::ObjectId(7980)));

    // Stage another identity change, then create a live scope-2 reference
    // during the activation gap through the still-visible old Recipe.
    ASSERT_EQ(VN_OK, ReplacePack(pack, v3Path));
    ASSERT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    VnVoiceHandle voice = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7980));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    EXPECT_EQ(VN_IN_USE, Vn_UnregisterBinding(harkentest::ObjectId(7981)));

    // The activation re-screen discards the complete staged revision. The
    // old definition identity, Recipe, pack slot, and live voice remain.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    ASSERT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(7981)));
    EXPECT_EQ(harkentest::ObjectId(7983u), engine->bindings.Find(harkentest::ObjectId(7981))->variableId);
    const auto* recipe = engine->logicPacks.FindVoiceRecipe(harkentest::ObjectId(7980));
    ASSERT_NE(nullptr, recipe);
    ASSERT_EQ(1u, recipe->bindings.size());
    EXPECT_EQ(harkentest::ObjectId(7981u), recipe->bindings[0]);
    EXPECT_EQ(1u, engine->packs.LoadedCount());
    EXPECT_NE(harken::VoiceLedger::HandleKind::Stale,
              engine->ledger.Classify(voice).kind);
}


TEST(PackLogicLoad, BusCompressorAndDynamicRouteInstallAsPackOwnedBatch) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 4;
    config.compressorEffectPoolCapacity = 2;
    config.dynamicRouteCapacity = 4;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string packPath =
        TempPath("plu_bus_compressor_route.pack");
    ASSERT_TRUE(BuildPackFile(
        "plu_bus_compressor_route",
        V2LogicManifest(
            packPath, "resident",
            "\"buses\": [{\"id\": \"3da91a64-b947-4560-0000-0000000020d1\", \"parent\": \"00000000-0000-0000-0000-000000000001\"}, "
            "{\"id\": \"3da91a64-b947-4560-0000-0000000020d2\", \"parent\": \"00000000-0000-0000-0000-000000000001\"}],\n"
            "\"busEffects\": [{\"bus\": \"3da91a64-b947-4560-0000-0000000020d1\", \"slot\": 0, "
            "\"type\": \"compressor\", \"descriptorVintage\": 2, "
            "\"bypass\": false, \"faderPointGuard\": 4, "
            "\"parameters\": [-18.0, 4.0, 10.0, 120.0, 6.0, 0.0]}],\n"
            "\"dynamicRoutes\": [{\"targetBus\": \"3da91a64-b947-4560-0000-0000000020d1\", "
            "\"effectSlot\": 0, \"routeSlot\": 0, "
            "\"sourceBus\": \"3da91a64-b947-4560-0000-0000000020d2\", \"tap\": \"postEffects\", "
            "\"levelDb\": -3.0}]")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
    const std::uint32_t target = engine->buses.FindIndex(harkentest::ObjectId(8401));
    ASSERT_NE(harken::kInvalidBusIndex, target);
    EXPECT_EQ(VN_EFFECT_COMPRESSOR,
              engine->busEffectSlotTypes[target][0]);
    EXPECT_EQ(pack, engine->busEffectOwnerCookies[target][0]);
    const harken::PackLogicRecord* record =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, record);
    ASSERT_EQ(1u, record->busEffects.size());
    ASSERT_EQ(1u, record->dynamicRoutes.size());
    const harken::EffectInstanceRef ownedRef =
        engine->busEffectInstanceRefs[target][0];

    VnEffectDynamicRouteInfo route{};
    route.structSize = sizeof(route);
    std::uint32_t required = 0;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(&route, 1, &required));
    ASSERT_EQ(1u, required);
    EXPECT_EQ(VN_DYNAMIC_ROUTE_OWNER_PACK, route.ownerKind);
    EXPECT_EQ(harkentest::ObjectId(8401u), route.targetBusId);
    EXPECT_EQ(harkentest::ObjectId(8402u), route.sourceId);

    VnCompressorDesc replacement{};
    replacement.structSize = sizeof(replacement);
    replacement.thresholdDb = -12.0f;
    replacement.ratio = 8.0f;
    replacement.attackMs = 20.0f;
    replacement.releaseMs = 240.0f;
    replacement.kneeDb = 4.0f;
    replacement.makeupDb = 2.0f;
    EXPECT_EQ(VN_IN_USE,
              Vn_SetBusEffect(
                  harkentest::ObjectId(8401), 0, VN_EFFECT_COMPRESSOR,
                  reinterpret_cast<const VnEffectDesc*>(&replacement)));
    EXPECT_EQ(VN_IN_USE,
              Vn_SetBusEffectParam(harkentest::ObjectId(8401), 0, 0, -12.0f));
    EXPECT_EQ(VN_IN_USE, Vn_SetBusEffectBypass(harkentest::ObjectId(8401), 0, 1));
    EXPECT_EQ(VN_IN_USE,
              Vn_ResetBusEffect(harkentest::ObjectId(8401), 0, VN_EFFECT_COMPRESSOR));
    VnEffectDynamicRouteDesc replacementRoute{};
    replacementRoute.structSize = sizeof(replacementRoute);
    replacementRoute.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    replacementRoute.sourceKind = VN_DYNAMIC_SOURCE_BUS;
    replacementRoute.tap = VN_DYNAMIC_TAP_BUS_PRE_FADER;
    replacementRoute.sourceId = harkentest::ObjectId(8402);
    replacementRoute.levelDb = -9.0f;
    replacementRoute.expectedEffectType = VN_EFFECT_COMPRESSOR;
    EXPECT_EQ(VN_IN_USE,
              Vn_SetBusEffectDynamicRoute(
                  harkentest::ObjectId(8401), 0, 0, &replacementRoute));
    EXPECT_EQ(VN_IN_USE,
              Vn_ClearBusEffectDynamicRoute(
                  harkentest::ObjectId(8401), 0, 0, VN_EFFECT_COMPRESSOR));
    EXPECT_EQ(VN_IN_USE, Vn_ClearBusEffect(harkentest::ObjectId(8401), 0));

    VnEffectDynamicRouteInfo after{};
    after.structSize = sizeof(after);
    std::uint32_t afterRequired = 0;
    ASSERT_EQ(VN_OK,
              Vn_CaptureEffectDynamicRoutes(
                  &after, 1, &afterRequired));
    ASSERT_EQ(1u, afterRequired);
    EXPECT_EQ(route.revision, after.revision);
    EXPECT_EQ(route.targetBusId, after.targetBusId);
    EXPECT_EQ(route.sourceId, after.sourceId);
    EXPECT_EQ(route.tap, after.tap);
    EXPECT_FLOAT_EQ(route.levelDb, after.levelDb);
    EXPECT_EQ(route.ownerKind, after.ownerKind);
    EXPECT_EQ(ownedRef.type,
              engine->busEffectInstanceRefs[target][0].type);
    EXPECT_EQ(ownedRef.poolIndex,
              engine->busEffectInstanceRefs[target][0].poolIndex);
    EXPECT_EQ(ownedRef.generation,
              engine->busEffectInstanceRefs[target][0].generation);
    engine->nullBackend.Pump(1);
}

TEST(PackLogicReplace,
     SameTypeBusEffectRevisionKeepsInstanceAndRouteIdentity) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 4;
    config.compressorEffectPoolCapacity = 2;
    config.dynamicRouteCapacity = 4;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kTargetBus = harkentest::ObjectId(8501);
    constexpr VnBusId kSourceBus = harkentest::ObjectId(8502);
    const std::string initialPath =
        TempPath("plr_bus_effect_same_type_v1.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_bus_effect_same_type_v1", initialPath, kTargetBus,
        kSourceBus, "compressor", false,
        "[-18.0, 4.0, 10.0, 120.0, 6.0, 0.0]", true, -3.0f));
    const std::string replacementPath =
        TempPath("plr_bus_effect_same_type_v2.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_bus_effect_same_type_v2", replacementPath, kTargetBus,
        kSourceBus, "compressor", true,
        "[-12.0, 8.0, 20.0, 240.0, 4.0, 2.0]", true, -9.0f));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t target = engine->buses.FindIndex(kTargetBus);
    ASSERT_NE(harken::kInvalidBusIndex, target);
    const harken::EffectInstanceRef oldRef =
        engine->busEffectInstanceRefs[target][0];
    const harken::DynamicRouteDefinition* oldRoute =
        engine->dynamicRoutes.Find(kTargetBus, 0, 0);
    ASSERT_NE(nullptr, oldRoute);
    const std::uint32_t oldTableIndex = oldRoute->tableIndex;
    const std::uint64_t oldGeneration = oldRoute->generation;
    const std::uint64_t oldRevision = oldRoute->revision;

    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));
    ASSERT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    const harken::PackLogicRecord* gapRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, gapRecord);
    ASSERT_EQ(1u, gapRecord->busEffects.size());
    ASSERT_EQ(1u, gapRecord->dynamicRoutes.size());
    EXPECT_EQ(0u, gapRecord->busEffects[0].bypass);
    EXPECT_FLOAT_EQ(-18.0f, gapRecord->busEffects[0].params[0]);
    EXPECT_FLOAT_EQ(-3.0f, gapRecord->dynamicRoutes[0].levelDb);
    EXPECT_EQ(oldRef.type,
              engine->busEffectInstanceRefs[target][0].type);
    EXPECT_EQ(oldRef.poolIndex,
              engine->busEffectInstanceRefs[target][0].poolIndex);
    EXPECT_EQ(oldRef.generation,
              engine->busEffectInstanceRefs[target][0].generation);

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    const harken::EffectInstanceRef liveRef =
        engine->busEffectInstanceRefs[target][0];
    EXPECT_EQ(oldRef.type, liveRef.type);
    EXPECT_EQ(oldRef.poolIndex, liveRef.poolIndex);
    EXPECT_EQ(oldRef.generation, liveRef.generation);
    EXPECT_EQ(pack, engine->busEffectOwnerCookies[target][0]);

    const harken::DynamicRouteDefinition* liveRoute =
        engine->dynamicRoutes.Find(kTargetBus, 0, 0);
    ASSERT_NE(nullptr, liveRoute);
    EXPECT_EQ(oldTableIndex, liveRoute->tableIndex);
    EXPECT_EQ(oldGeneration, liveRoute->generation);
    EXPECT_EQ(oldRevision + 1, liveRoute->revision);
    EXPECT_FLOAT_EQ(-9.0f, liveRoute->levelDb);
    EXPECT_EQ(VN_DYNAMIC_ROUTE_OWNER_PACK, liveRoute->ownerKind);
    EXPECT_EQ(pack, liveRoute->ownerCookie);

    const harken::PackLogicRecord* liveRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, liveRecord);
    ASSERT_EQ(1u, liveRecord->busEffects.size());
    ASSERT_EQ(1u, liveRecord->dynamicRoutes.size());
    EXPECT_EQ(VN_EFFECT_COMPRESSOR,
              liveRecord->busEffects[0].effectType);
    EXPECT_EQ(1u, liveRecord->busEffects[0].bypass);
    EXPECT_FLOAT_EQ(-12.0f, liveRecord->busEffects[0].params[0]);
    EXPECT_FLOAT_EQ(8.0f, liveRecord->busEffects[0].params[1]);
    EXPECT_FLOAT_EQ(2.0f, liveRecord->busEffects[0].params[5]);
    EXPECT_FLOAT_EQ(-9.0f, liveRecord->dynamicRoutes[0].levelDb);
    engine->nullBackend.Pump(1);
}

TEST(PackLogicReplace,
     BusEffectTypeSwapReplacesInstanceAndClearsDynamicRoute) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 4;
    config.compressorEffectPoolCapacity = 2;
    config.gainEffectPoolCapacity = 2;
    config.dynamicRouteCapacity = 4;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kTargetBus = harkentest::ObjectId(8601);
    constexpr VnBusId kSourceBus = harkentest::ObjectId(8602);
    const std::string initialPath =
        TempPath("plr_bus_effect_type_swap_v1.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_bus_effect_type_swap_v1", initialPath, kTargetBus,
        kSourceBus, "compressor", false,
        "[-18.0, 4.0, 10.0, 120.0, 6.0, 0.0]", true, -3.0f));
    const std::string replacementPath =
        TempPath("plr_bus_effect_type_swap_v2.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_bus_effect_type_swap_v2", replacementPath, kTargetBus,
        kSourceBus, "gain", true, "[-6.0]", false, 0.0f));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t target = engine->buses.FindIndex(kTargetBus);
    ASSERT_NE(harken::kInvalidBusIndex, target);
    const harken::EffectInstanceRef oldRef =
        engine->busEffectInstanceRefs[target][0];
    ASSERT_EQ(VN_EFFECT_COMPRESSOR, oldRef.type);
    ASSERT_NE(nullptr, engine->dynamicRoutes.Find(kTargetBus, 0, 0));

    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    const harken::EffectInstanceRef liveRef =
        engine->busEffectInstanceRefs[target][0];
    EXPECT_EQ(VN_EFFECT_GAIN, liveRef.type);
    EXPECT_NE(oldRef.type, liveRef.type);
    EXPECT_EQ(VN_EFFECT_GAIN, engine->busEffectSlotTypes[target][0]);
    EXPECT_EQ(pack, engine->busEffectOwnerCookies[target][0]);
    EXPECT_EQ(nullptr, engine->dynamicRoutes.Find(kTargetBus, 0, 0));

    const harken::PackLogicRecord* liveRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, liveRecord);
    ASSERT_EQ(1u, liveRecord->busEffects.size());
    EXPECT_TRUE(liveRecord->dynamicRoutes.empty());
    EXPECT_EQ(VN_EFFECT_GAIN, liveRecord->busEffects[0].effectType);
    EXPECT_EQ(1u, liveRecord->busEffects[0].bypass);
    EXPECT_FLOAT_EQ(-6.0f, liveRecord->busEffects[0].params[0]);
    engine->nullBackend.Pump(1);
}

TEST(PackLogicReplace,
     EffectParamSeedFailureKeepsPublishedRevisionWholeAndRetryable) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 4;
    config.compressorEffectPoolCapacity = 2;
    config.gainEffectPoolCapacity = 2;
    config.dynamicRouteCapacity = 4;
    config.dynamicRoutePlanPoolCapacity = 3;
    config.dynamicBusTapCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kTargetBus = harkentest::ObjectId(8651);
    constexpr VnBusId kSourceBus = harkentest::ObjectId(8652);
    const std::string initialPath =
        TempPath("plr_seed_atomic_v1.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_seed_atomic_v1", initialPath, kTargetBus, kSourceBus,
        "compressor", false,
        "[-18.0, 4.0, 10.0, 120.0, 6.0, 0.0]", true, -3.0f));
    const std::string replacementPath =
        TempPath("plr_seed_atomic_v2.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_seed_atomic_v2", replacementPath, kTargetBus, kSourceBus,
        "gain", true, "[-6.0]", false, 0.0f));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->dynamicRoutes.DrainAudioMessages();
    const std::uint32_t target = engine->buses.FindIndex(kTargetBus);
    ASSERT_NE(harken::kInvalidBusIndex, target);
    const harken::EffectInstanceRef oldRef =
        engine->busEffectInstanceRefs[target][0];
    const harken::DynamicRouteDefinition* oldRoute =
        engine->dynamicRoutes.Find(kTargetBus, 0, 0);
    ASSERT_NE(nullptr, oldRoute);
    const harken::DynamicRouteDefinition oldRouteValue = *oldRoute;
    const std::uint32_t oldFreeSlots = engine->commands.FreeSlots();
    const std::uint32_t oldOpcodeMask =
        engine->commands.OpcodeSeenMask();
    const std::uint64_t oldTopologyPublications =
        engine->topologyPlanCount;
    const std::uint64_t oldDynamicInstalledRevision =
        engine->dynamicRoutes.InstalledRevision();
    const std::uint32_t oldTopologyPlansInUse =
        engine->topologyPlans.InUse();
    const std::uint32_t oldDynamicPlansInUse =
        engine->dynamicRoutes.PlanInUse();
    const std::uint32_t oldParamRecords =
        engine->effectParamTargets.LiveRecordCount();
    const std::size_t oldRetiringMedia =
        engine->packs.RetiringCount();

    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));
    engine->effectParamTargets.SetSeedFailureForTests(true);
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    const harken::EffectInstanceRef failedRef =
        engine->busEffectInstanceRefs[target][0];
    EXPECT_EQ(oldRef.type, failedRef.type);
    EXPECT_EQ(oldRef.poolIndex, failedRef.poolIndex);
    EXPECT_EQ(oldRef.generation, failedRef.generation);
    const harken::DynamicRouteDefinition* failedRoute =
        engine->dynamicRoutes.Find(kTargetBus, 0, 0);
    ASSERT_NE(nullptr, failedRoute);
    EXPECT_EQ(oldRouteValue.tableIndex, failedRoute->tableIndex);
    EXPECT_EQ(oldRouteValue.generation, failedRoute->generation);
    EXPECT_EQ(oldRouteValue.revision, failedRoute->revision);
    EXPECT_EQ(oldRouteValue.tapGeneration, failedRoute->tapGeneration);
    EXPECT_FLOAT_EQ(oldRouteValue.levelDb, failedRoute->levelDb);
    const harken::PackLogicRecord* failedRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, failedRecord);
    ASSERT_EQ(1u, failedRecord->busEffects.size());
    ASSERT_EQ(1u, failedRecord->dynamicRoutes.size());
    EXPECT_EQ(VN_EFFECT_COMPRESSOR,
              failedRecord->busEffects[0].effectType);
    EXPECT_FLOAT_EQ(-18.0f, failedRecord->busEffects[0].params[0]);
    EXPECT_FLOAT_EQ(-3.0f, failedRecord->dynamicRoutes[0].levelDb);
    EXPECT_EQ(oldFreeSlots, engine->commands.FreeSlots());
    EXPECT_EQ(oldOpcodeMask, engine->commands.OpcodeSeenMask());
    EXPECT_EQ(oldTopologyPublications, engine->topologyPlanCount);
    EXPECT_EQ(oldDynamicInstalledRevision,
              engine->dynamicRoutes.InstalledRevision());
    EXPECT_EQ(oldTopologyPlansInUse, engine->topologyPlans.InUse());
    EXPECT_EQ(oldDynamicPlansInUse,
              engine->dynamicRoutes.PlanInUse());
    EXPECT_EQ(oldParamRecords,
              engine->effectParamTargets.LiveRecordCount());
    EXPECT_EQ(oldRetiringMedia, engine->packs.RetiringCount());

    engine->effectParamTargets.SetSeedFailureForTests(false);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    const harken::EffectInstanceRef liveRef =
        engine->busEffectInstanceRefs[target][0];
    EXPECT_EQ(VN_EFFECT_GAIN, liveRef.type);
    EXPECT_EQ(nullptr, engine->dynamicRoutes.Find(kTargetBus, 0, 0));
    const harken::PackLogicRecord* liveRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, liveRecord);
    ASSERT_EQ(1u, liveRecord->busEffects.size());
    EXPECT_EQ(VN_EFFECT_GAIN, liveRecord->busEffects[0].effectType);
    EXPECT_FLOAT_EQ(-6.0f, liveRecord->busEffects[0].params[0]);
    EXPECT_EQ(oldTopologyPublications + 1,
              engine->topologyPlanCount);
    engine->nullBackend.Pump(1);
}

TEST(PackLogicReplace,
     BusEffectActivationPressureKeepsOldLedgerAndLiveStateUntilRetry) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.commandQueueDepth = 32;
    config.fxSlotsPerBus = 4;
    config.compressorEffectPoolCapacity = 2;
    config.dynamicRouteCapacity = 4;
    config.dynamicRoutePlanPoolCapacity = 2;
    config.dynamicBusTapCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kTargetBus = harkentest::ObjectId(8701);
    constexpr VnBusId kSourceBus = harkentest::ObjectId(8702);
    const std::string initialPath =
        TempPath("plr_bus_effect_pressure_v1.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_bus_effect_pressure_v1", initialPath, kTargetBus,
        kSourceBus, "compressor", false,
        "[-18.0, 4.0, 10.0, 120.0, 6.0, 0.0]", true, -3.0f));
    const std::string replacementPath =
        TempPath("plr_bus_effect_pressure_v2.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_bus_effect_pressure_v2", replacementPath, kTargetBus,
        kSourceBus, "compressor", true,
        "[-12.0, 8.0, 20.0, 240.0, 4.0, 2.0]", true, -9.0f));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t target = engine->buses.FindIndex(kTargetBus);
    ASSERT_NE(harken::kInvalidBusIndex, target);
    const harken::EffectInstanceRef oldRef =
        engine->busEffectInstanceRefs[target][0];
    const harken::DynamicRouteDefinition* oldRoute =
        engine->dynamicRoutes.Find(kTargetBus, 0, 0);
    ASSERT_NE(nullptr, oldRoute);
    const std::uint32_t oldTableIndex = oldRoute->tableIndex;
    const std::uint64_t oldGeneration = oldRoute->generation;
    const std::uint64_t oldRevision = oldRoute->revision;

    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));
    VnResult pressure = VN_OK;
    int pushed = 0;
    while ((pressure =
                Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0)) == VN_OK &&
           pushed < 128) {
        ++pushed;
    }
    ASSERT_EQ(VN_BACKPRESSURE, pressure);

    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    const harken::PackLogicRecord* deferredRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, deferredRecord);
    ASSERT_EQ(1u, deferredRecord->busEffects.size());
    ASSERT_EQ(1u, deferredRecord->dynamicRoutes.size());
    EXPECT_EQ(0u, deferredRecord->busEffects[0].bypass);
    EXPECT_FLOAT_EQ(-18.0f, deferredRecord->busEffects[0].params[0]);
    EXPECT_FLOAT_EQ(-3.0f, deferredRecord->dynamicRoutes[0].levelDb);
    const harken::EffectInstanceRef deferredRef =
        engine->busEffectInstanceRefs[target][0];
    EXPECT_EQ(oldRef.type, deferredRef.type);
    EXPECT_EQ(oldRef.poolIndex, deferredRef.poolIndex);
    EXPECT_EQ(oldRef.generation, deferredRef.generation);
    const harken::DynamicRouteDefinition* deferredRoute =
        engine->dynamicRoutes.Find(kTargetBus, 0, 0);
    ASSERT_NE(nullptr, deferredRoute);
    EXPECT_EQ(oldTableIndex, deferredRoute->tableIndex);
    EXPECT_EQ(oldGeneration, deferredRoute->generation);
    EXPECT_EQ(oldRevision, deferredRoute->revision);
    EXPECT_FLOAT_EQ(-3.0f, deferredRoute->levelDb);

    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    const harken::PackLogicRecord* liveRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, liveRecord);
    ASSERT_EQ(1u, liveRecord->busEffects.size());
    ASSERT_EQ(1u, liveRecord->dynamicRoutes.size());
    EXPECT_EQ(1u, liveRecord->busEffects[0].bypass);
    EXPECT_FLOAT_EQ(-12.0f, liveRecord->busEffects[0].params[0]);
    EXPECT_FLOAT_EQ(-9.0f, liveRecord->dynamicRoutes[0].levelDb);
    const harken::EffectInstanceRef liveRef =
        engine->busEffectInstanceRefs[target][0];
    EXPECT_EQ(oldRef.type, liveRef.type);
    EXPECT_EQ(oldRef.poolIndex, liveRef.poolIndex);
    EXPECT_EQ(oldRef.generation, liveRef.generation);
    const harken::DynamicRouteDefinition* liveRoute =
        engine->dynamicRoutes.Find(kTargetBus, 0, 0);
    ASSERT_NE(nullptr, liveRoute);
    EXPECT_EQ(oldTableIndex, liveRoute->tableIndex);
    EXPECT_EQ(oldGeneration, liveRoute->generation);
    EXPECT_EQ(oldRevision + 1, liveRoute->revision);
    EXPECT_FLOAT_EQ(-9.0f, liveRoute->levelDb);
    engine->nullBackend.Pump(1);
}

TEST(PackLogicReplace,
     ProspectiveTopologyFailureKeepsAddedBusAndGraphHiddenUntilRetry) {
    VnConfig config = PackLogicConfig(6, kBlock, kRate);
    config.fxSlotsPerBus = 4;
    config.compressorEffectPoolCapacity = 2;
    config.dynamicRouteCapacity = 4;
    config.dynamicRoutePlanPoolCapacity = 3;
    config.dynamicBusTapCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kOldBus = harkentest::ObjectId(8801);
    constexpr VnBusId kSourceBus = harkentest::ObjectId(8802);
    constexpr VnBusId kAddedBus = harkentest::ObjectId(8803);
    const std::string initialPath =
        TempPath("plr_prospective_topology_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_prospective_topology_v1",
        V2LogicManifest(
            initialPath, "resident",
            R"("buses": [
                { "id": "3da91a64-b947-4560-0000-000000002261", "parent": "00000000-0000-0000-0000-000000000001" },
                { "id": "3da91a64-b947-4560-0000-000000002262", "parent": "00000000-0000-0000-0000-000000000001" }
            ])")));
    const std::string replacementPath =
        TempPath("plr_prospective_topology_v2.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_prospective_topology_v2",
        V2LogicManifest(
            replacementPath, "resident",
            R"("buses": [
                { "id": "3da91a64-b947-4560-0000-000000002261", "parent": "00000000-0000-0000-0000-000000000001" },
                { "id": "3da91a64-b947-4560-0000-000000002262", "parent": "00000000-0000-0000-0000-000000000001" },
                { "id": "3da91a64-b947-4560-0000-000000002263", "parent": "00000000-0000-0000-0000-000000000001" }
            ],
            "busSends": [
                { "from": "3da91a64-b947-4560-0000-000000002263", "to": "3da91a64-b947-4560-0000-000000002261", "levelDb": -6.0 }
            ],
            "busEffects": [
                { "bus": "3da91a64-b947-4560-0000-000000002263", "slot": 0, "type": "compressor",
                  "descriptorVintage": 2, "bypass": false,
                  "faderPointGuard": 4,
                  "parameters": [-18.0, 4.0, 10.0, 120.0, 6.0, 0.0] }
            ],
            "dynamicRoutes": [
                { "targetBus": "3da91a64-b947-4560-0000-000000002263", "effectSlot": 0, "routeSlot": 0,
                  "sourceBus": "3da91a64-b947-4560-0000-000000002262", "tap": "postEffects",
                  "levelDb": -3.0 }
            ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::PackLogicRecord* oldRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, oldRecord);
    ASSERT_EQ(2u, oldRecord->buses.size());
    const std::uint32_t oldBusCount = engine->buses.BusCount();
    const std::uint64_t oldTopologyPublications =
        engine->topologyPlanCount;

    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));
    {
        StagingFaultGuard guard(
            harken::PackStagingFault::ActivationProspectiveTopology);
        ASSERT_EQ(VN_OK, Vn_Update());
    }

    EXPECT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(oldBusCount, engine->buses.BusCount());
    EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(kAddedBus));
    EXPECT_EQ(engine->config.busSendSlotsPerBus,
              FindBusSendSlot(*engine, kAddedBus, kOldBus));
    EXPECT_EQ(nullptr, engine->dynamicRoutes.Find(kAddedBus, 0, 0));
    EXPECT_EQ(oldTopologyPublications, engine->topologyPlanCount);
    const harken::PackLogicRecord* failedRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, failedRecord);
    EXPECT_EQ(2u, failedRecord->buses.size());
    EXPECT_TRUE(failedRecord->busSends.empty());
    EXPECT_TRUE(failedRecord->busEffects.empty());
    EXPECT_TRUE(failedRecord->dynamicRoutes.empty());

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    const std::uint32_t added = engine->buses.FindIndex(kAddedBus);
    ASSERT_NE(harken::kInvalidBusIndex, added);
    EXPECT_LT(FindBusSendSlot(*engine, kAddedBus, kOldBus),
              engine->config.busSendSlotsPerBus);
    EXPECT_EQ(VN_EFFECT_COMPRESSOR,
              engine->busEffectSlotTypes[added][0]);
    EXPECT_EQ(pack, engine->busEffectOwnerCookies[added][0]);
    EXPECT_NE(nullptr, engine->dynamicRoutes.Find(kAddedBus, 0, 0));
    const harken::PackLogicRecord* liveRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, liveRecord);
    EXPECT_EQ(3u, liveRecord->buses.size());
    EXPECT_EQ(1u, liveRecord->busSends.size());
    EXPECT_EQ(1u, liveRecord->busEffects.size());
    EXPECT_EQ(1u, liveRecord->dynamicRoutes.size());
    engine->nullBackend.Pump(1);
}

TEST(PackLogicReplace,
     TopologyPlanPressureCannotLeaveEffectSeedBoundToStaleBusIndex) {
    VnConfig config = PackLogicConfig(6, kBlock, kRate);
    config.maxBuses = 8;
    config.fxSlotsPerBus = 4;
    config.topologyPlanPoolCapacity = 2;
    config.compressorEffectPoolCapacity = 2;
    config.dynamicRouteCapacity = 4;
    config.dynamicRoutePlanPoolCapacity = 3;
    config.dynamicBusTapCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kPackEffectBus = harkentest::ObjectId(8872);
    constexpr VnBusId kCallerBus = harkentest::ObjectId(8873);
    const std::string initialPath =
        TempPath("plr_stale_seed_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_stale_seed_v1",
        V2LogicManifest(
            initialPath, "resident",
            R"("buses": [
                { "id": "3da91a64-b947-4560-0000-0000000022a6", "parent": "00000000-0000-0000-0000-000000000001" },
                { "id": "3da91a64-b947-4560-0000-0000000022a7", "parent": "00000000-0000-0000-0000-000000000001" }
            ])")));
    const std::string replacementPath =
        TempPath("plr_stale_seed_v2.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_stale_seed_v2",
        V2LogicManifest(
            replacementPath, "resident",
            R"("buses": [
                { "id": "3da91a64-b947-4560-0000-0000000022a6", "parent": "00000000-0000-0000-0000-000000000001" },
                { "id": "3da91a64-b947-4560-0000-0000000022a7", "parent": "00000000-0000-0000-0000-000000000001" },
                { "id": "3da91a64-b947-4560-0000-0000000022a8", "parent": "00000000-0000-0000-0000-000000000001" }
            ],
            "busSends": [
                { "from": "3da91a64-b947-4560-0000-0000000022a8", "to": "3da91a64-b947-4560-0000-0000000022a6", "levelDb": -6.0 }
            ],
            "busEffects": [
                { "bus": "3da91a64-b947-4560-0000-0000000022a8", "slot": 0, "type": "compressor",
                  "descriptorVintage": 2, "bypass": false,
                  "faderPointGuard": 4,
                  "parameters": [-18.0, 4.0, 10.0, 120.0, 6.0, 0.0] }
            ],
            "dynamicRoutes": [
                { "targetBus": "3da91a64-b947-4560-0000-0000000022a8", "effectSlot": 0, "routeSlot": 0,
                  "sourceBus": "3da91a64-b947-4560-0000-0000000022a7", "tap": "postEffects",
                  "levelDb": -3.0 }
            ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));

    harken::PackPendingRevision* pending =
        engine->logicPacks.PendingFor(pack);
    ASSERT_NE(nullptr, pending);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, pending->planIndex);
    engine->topologyPlans.Recycle(pending->planIndex);
    pending->planIndex = harken::kInvalidTopologyPlanIndex;
    const std::uint32_t heldPlan = engine->topologyPlans.Acquire();
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, heldPlan);
    ASSERT_EQ(VN_OK, Vn_Update());

    pending = engine->logicPacks.PendingFor(pack);
    ASSERT_NE(nullptr, pending);
    EXPECT_FALSE(pending->effectParamSeedsPrepared);
    for (const auto& token : pending->effectParamSeedTokens) {
        EXPECT_FALSE(token.IsPrepared());
    }

    engine->topologyPlans.Recycle(heldPlan);
    ASSERT_EQ(VN_OK, Vn_CreateBus(kCallerBus, VN_BUS_MASTER));
    const std::uint32_t callerIndex =
        engine->buses.FindIndex(kCallerBus);
    ASSERT_NE(harken::kInvalidBusIndex, callerIndex);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    const std::uint32_t effectIndex =
        engine->buses.FindIndex(kPackEffectBus);
    ASSERT_NE(harken::kInvalidBusIndex, effectIndex);
    ASSERT_NE(callerIndex, effectIndex);
    EXPECT_FLOAT_EQ(
        -18.0f,
        engine->effectParamTargets.Reduce(
            harken::EffectParamTargetStore::Scope::Bus, effectIndex, 0, 0));
    EXPECT_FLOAT_EQ(
        0.0f,
        engine->effectParamTargets.Reduce(
            harken::EffectParamTargetStore::Scope::Bus, callerIndex, 0, 0));
    engine->nullBackend.Pump(1);
}

TEST(PackLogicReplace,
     MultiEffectMidSeedFailureCancelsEveryReservationBeforeRetry) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 4;
    config.compressorEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kTargetBus = harkentest::ObjectId(8881);
    const std::string initialPath =
        TempPath("plr_mid_seed_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_mid_seed_v1",
        V2LogicManifest(
            initialPath, "resident",
            R"("buses": [
                { "id": "3da91a64-b947-4560-0000-0000000022b1", "parent": "00000000-0000-0000-0000-000000000001" },
                { "id": "3da91a64-b947-4560-0000-0000000022b2", "parent": "00000000-0000-0000-0000-000000000001" }
            ])")));
    const std::string replacementPath =
        TempPath("plr_mid_seed_v2.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_mid_seed_v2",
        V2LogicManifest(
            replacementPath, "resident",
            R"("buses": [
                { "id": "3da91a64-b947-4560-0000-0000000022b1", "parent": "00000000-0000-0000-0000-000000000001" },
                { "id": "3da91a64-b947-4560-0000-0000000022b2", "parent": "00000000-0000-0000-0000-000000000001" }
            ],
            "busEffects": [
                { "bus": "3da91a64-b947-4560-0000-0000000022b1", "slot": 0, "type": "compressor",
                  "descriptorVintage": 2, "bypass": false,
                  "faderPointGuard": 4,
                  "parameters": [-18.0, 4.0, 10.0, 120.0, 6.0, 0.0] },
                { "bus": "3da91a64-b947-4560-0000-0000000022b1", "slot": 1, "type": "compressor",
                  "descriptorVintage": 2, "bypass": false,
                  "faderPointGuard": 4,
                  "parameters": [-12.0, 8.0, 20.0, 240.0, 4.0, 2.0] }
            ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));

    engine->effectParamTargets.SetPrepareSeedFailureAfterForTests(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    harken::PackPendingRevision* pending =
        engine->logicPacks.PendingFor(pack);
    ASSERT_NE(nullptr, pending);
    EXPECT_FALSE(pending->effectParamSeedsPrepared);
    for (const auto& token : pending->effectParamSeedTokens) {
        EXPECT_FALSE(token.IsPrepared());
    }
    EXPECT_EQ(
        0u,
        engine->effectParamTargets.ReservedRecordCountForTests());

    engine->effectParamTargets.SetPrepareSeedFailureAfterForTests(-1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(
        0u,
        engine->effectParamTargets.ReservedRecordCountForTests());
    const std::uint32_t target = engine->buses.FindIndex(kTargetBus);
    ASSERT_NE(harken::kInvalidBusIndex, target);
    EXPECT_FLOAT_EQ(
        -18.0f,
        engine->effectParamTargets.Reduce(
            harken::EffectParamTargetStore::Scope::Bus, target, 0, 0));
    EXPECT_FLOAT_EQ(
        -12.0f,
        engine->effectParamTargets.Reduce(
            harken::EffectParamTargetStore::Scope::Bus, target, 1, 0));
    engine->nullBackend.Pump(1);
}

TEST(PackLogicReplace,
     QueueReservationFailureKeepsPublishedRouteWholeBeforeRetry) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 4;
    config.compressorEffectPoolCapacity = 2;
    config.dynamicRouteCapacity = 4;
    config.dynamicRoutePlanPoolCapacity = 3;
    config.dynamicBusTapCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kTargetBus = harkentest::ObjectId(8901);
    constexpr VnBusId kSourceBus = harkentest::ObjectId(8902);
    const std::string initialPath =
        TempPath("plr_queue_ticket_v1.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_queue_ticket_v1", initialPath, kTargetBus,
        kSourceBus, "compressor", false,
        "[-18.0, 4.0, 10.0, 120.0, 6.0, 0.0]", true, -3.0f));
    const std::string replacementPath =
        TempPath("plr_queue_ticket_v2.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_queue_ticket_v2", replacementPath, kTargetBus,
        kSourceBus, "compressor", true,
        "[-12.0, 8.0, 20.0, 240.0, 4.0, 2.0]", true, -9.0f));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t target = engine->buses.FindIndex(kTargetBus);
    ASSERT_NE(harken::kInvalidBusIndex, target);
    const harken::EffectInstanceRef oldRef =
        engine->busEffectInstanceRefs[target][0];
    const harken::DynamicRouteDefinition* oldRoute =
        engine->dynamicRoutes.Find(kTargetBus, 0, 0);
    ASSERT_NE(nullptr, oldRoute);
    const std::uint64_t oldRouteRevision = oldRoute->revision;
    const std::uint64_t oldTopologyPublications =
        engine->topologyPlanCount;

    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));
    {
        StagingFaultGuard guard(
            harken::PackStagingFault::ActivationQueueReservation);
        ASSERT_EQ(VN_OK, Vn_Update());
    }

    EXPECT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(oldTopologyPublications, engine->topologyPlanCount);
    const harken::EffectInstanceRef failedRef =
        engine->busEffectInstanceRefs[target][0];
    EXPECT_EQ(oldRef.type, failedRef.type);
    EXPECT_EQ(oldRef.poolIndex, failedRef.poolIndex);
    EXPECT_EQ(oldRef.generation, failedRef.generation);
    const harken::DynamicRouteDefinition* failedRoute =
        engine->dynamicRoutes.Find(kTargetBus, 0, 0);
    ASSERT_NE(nullptr, failedRoute);
    EXPECT_EQ(oldRouteRevision, failedRoute->revision);
    EXPECT_FLOAT_EQ(-3.0f, failedRoute->levelDb);
    const harken::PackLogicRecord* failedRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, failedRecord);
    EXPECT_FLOAT_EQ(-18.0f, failedRecord->busEffects[0].params[0]);
    EXPECT_FLOAT_EQ(-3.0f, failedRecord->dynamicRoutes[0].levelDb);

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    const harken::DynamicRouteDefinition* liveRoute =
        engine->dynamicRoutes.Find(kTargetBus, 0, 0);
    ASSERT_NE(nullptr, liveRoute);
    EXPECT_EQ(oldRouteRevision + 1, liveRoute->revision);
    EXPECT_FLOAT_EQ(-9.0f, liveRoute->levelDb);
    EXPECT_EQ(oldTopologyPublications + 1,
              engine->topologyPlanCount);
    engine->nullBackend.Pump(1);
}

TEST(PackLogicReplace,
     NewRouteQueueFailureReusesPlanAndTapIdentityOnRetry) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 4;
    config.compressorEffectPoolCapacity = 2;
    config.dynamicRouteCapacity = 4;
    config.dynamicRoutePlanPoolCapacity = 3;
    config.dynamicBusTapCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kTargetBus = harkentest::ObjectId(8921);
    constexpr VnBusId kSourceBus = harkentest::ObjectId(8922);
    const std::string initialPath =
        TempPath("plr_new_route_ticket_v1.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_new_route_ticket_v1", initialPath, kTargetBus,
        kSourceBus, "compressor", false,
        "[-18.0, 4.0, 10.0, 120.0, 6.0, 0.0]", false, 0.0f));
    const std::string replacementPath =
        TempPath("plr_new_route_ticket_v2.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_new_route_ticket_v2", replacementPath, kTargetBus,
        kSourceBus, "compressor", false,
        "[-18.0, 4.0, 10.0, 120.0, 6.0, 0.0]", true, -9.0f));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->dynamicRoutes.DrainAudioMessages();
    ASSERT_EQ(nullptr,
              engine->dynamicRoutes.Find(kTargetBus, 0, 0));
    const std::uint64_t lastPlanRevisionBefore =
        engine->dynamicRoutes.LastIssuedPlanRevision();
    std::vector<std::uint64_t> planGenerationsBefore(
        engine->dynamicRoutes.PlanCapacity() + 1);
    for (std::uint32_t plan = 1;
         plan <= engine->dynamicRoutes.PlanCapacity(); ++plan) {
        planGenerationsBefore[plan] =
            engine->dynamicRoutes.PlanGeneration(plan);
    }
    const std::uint64_t tapGenerationBefore =
        engine->dynamicRoutes.TapGeneration(
            VN_DYNAMIC_SOURCE_BUS, 0);
    const std::uint64_t oldTopologyPublications =
        engine->topologyPlanCount;
    const std::uint64_t oldInstalledRevision =
        engine->dynamicRoutes.InstalledRevision();

    EXPECT_EQ(0u, engine->dynamicRoutes.TapSampleStorageBytes());
    engine->dynamicRoutes.SetTapSampleCommitPageLimitsForTests(0, 0);
    EXPECT_EQ(VN_NO_DYNAMIC_ROUTE, ReplacePack(pack, replacementPath));
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(0u, engine->dynamicRoutes.BusTapInUse());
    EXPECT_EQ(tapGenerationBefore,
              engine->dynamicRoutes.TapGeneration(VN_DYNAMIC_SOURCE_BUS, 0));
    engine->dynamicRoutes.SetTapSampleCommitPageLimitsForTests(SIZE_MAX, SIZE_MAX);
    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));
    const auto preparedTapBytes = engine->dynamicRoutes.TapSampleStorageBytes();
    EXPECT_GT(preparedTapBytes, 0u);
    EXPECT_EQ(0u, engine->dynamicRoutes.BusTapInUse());
    engine->dynamicRoutes.SetTapSampleCommitPageLimitsForTests(0, 0);
    {
        StagingFaultGuard guard(
            harken::PackStagingFault::ActivationQueueReservation);
        ASSERT_EQ(VN_OK, Vn_Update());
    }

    harken::PackPendingRevision* pending =
        engine->logicPacks.PendingFor(pack);
    ASSERT_NE(nullptr, pending);
    ASSERT_TRUE(pending->activationAttemptPrepared);
    ASSERT_NE(harken::kInvalidDynamicRoutePlanIndex,
              pending->dynamicPlanIndex);
    ASSERT_EQ(1u, pending->dynamicRouteCandidates.size());
    const std::uint32_t planIndex = pending->dynamicPlanIndex;
    const harken::DynamicRouteDefinition preparedRoute =
        pending->dynamicRouteCandidates[0];
    ASSERT_NE(harken::kInvalidDynamicTapSlot,
              preparedRoute.tapSlot);
    EXPECT_EQ(lastPlanRevisionBefore + 1,
              engine->dynamicRoutes.LastIssuedPlanRevision());
    EXPECT_EQ(planGenerationsBefore[planIndex] + 1,
              engine->dynamicRoutes.PlanGeneration(planIndex));
    EXPECT_EQ(tapGenerationBefore + 1,
              preparedRoute.tapGeneration);
    EXPECT_EQ(preparedRoute.tapGeneration,
              engine->dynamicRoutes.TapGeneration(
                  VN_DYNAMIC_SOURCE_BUS, preparedRoute.tapSlot));
    EXPECT_EQ(2u, engine->dynamicRoutes.TapRefCount(
                      VN_DYNAMIC_SOURCE_BUS, preparedRoute.tapSlot));
    EXPECT_EQ(nullptr,
              engine->dynamicRoutes.Find(kTargetBus, 0, 0));
    EXPECT_EQ(oldTopologyPublications, engine->topologyPlanCount);
    EXPECT_EQ(oldInstalledRevision,
              engine->dynamicRoutes.InstalledRevision());

    const std::uint64_t heldPlanGeneration =
        engine->dynamicRoutes.PlanGeneration(planIndex);
    const std::uint64_t heldPlanRevision =
        engine->dynamicRoutes.PlanRevision(planIndex);
    const std::uint64_t heldLastIssuedRevision =
        engine->dynamicRoutes.LastIssuedPlanRevision();
    const std::uint64_t heldTapGeneration =
        preparedRoute.tapGeneration;
    const std::uint32_t heldTapRefCount =
        engine->dynamicRoutes.TapRefCount(
            VN_DYNAMIC_SOURCE_BUS, preparedRoute.tapSlot);

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    const harken::DynamicRouteDefinition* liveRoute =
        engine->dynamicRoutes.Find(kTargetBus, 0, 0);
    ASSERT_NE(nullptr, liveRoute);
    EXPECT_EQ(1u, liveRoute->revision);
    EXPECT_FLOAT_EQ(-9.0f, liveRoute->levelDb);
    EXPECT_EQ(heldTapGeneration, liveRoute->tapGeneration);
    EXPECT_EQ(heldPlanGeneration,
              engine->dynamicRoutes.PlanGeneration(planIndex));
    EXPECT_EQ(heldPlanRevision,
              engine->dynamicRoutes.PlanRevision(planIndex));
    EXPECT_EQ(heldLastIssuedRevision,
              engine->dynamicRoutes.LastIssuedPlanRevision());
    EXPECT_EQ(heldTapGeneration,
              engine->dynamicRoutes.TapGeneration(
                  VN_DYNAMIC_SOURCE_BUS, preparedRoute.tapSlot));
    EXPECT_EQ(heldTapRefCount,
              engine->dynamicRoutes.TapRefCount(
                  VN_DYNAMIC_SOURCE_BUS, preparedRoute.tapSlot));
    EXPECT_EQ(oldTopologyPublications + 1,
              engine->topologyPlanCount);
    EXPECT_EQ(preparedTapBytes, engine->dynamicRoutes.TapSampleStorageBytes());
    engine->nullBackend.Pump(1);
}

TEST(PackLogicReplace,
     MediaPreparationFailureCancelsQueueTicketBeforeRetry) {
    EngineSession session(PackLogicConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const auto buildRevision = [&](const char* name, float levelDb) {
        const std::string path =
            TempPath(std::string(name) + ".pack");
        const bool built = BuildPackFile(
            name,
            V2LogicManifest(
                path, "resident",
                "\"buses\": [ { \"id\": \"3da91a64-b947-4560-0000-0000000022f7\", \"parent\": \"00000000-0000-0000-0000-000000000001\" }, "
                "{ \"id\": \"3da91a64-b947-4560-0000-0000000022f8\", \"parent\": \"00000000-0000-0000-0000-000000000001\" } ],\n"
                "\"busSends\": [ { \"from\": \"3da91a64-b947-4560-0000-0000000022f7\", \"to\": \"3da91a64-b947-4560-0000-0000000022f8\", "
                "\"levelDb\": " +
                    std::to_string(levelDb) + " } ]"));
        EXPECT_TRUE(built);
        return path;
    };
    const std::string initialPath =
        buildRevision("plr_media_prepare_v1", -6.0f);
    const std::string replacementPath =
        buildRevision("plr_media_prepare_v2", -12.0f);

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint64_t oldTopologyPublications =
        engine->topologyPlanCount;

    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));
    {
        StagingFaultGuard guard(
            harken::PackStagingFault::ActivationMediaPreparation);
        ASSERT_EQ(VN_OK, Vn_Update());
    }

    EXPECT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(oldTopologyPublications, engine->topologyPlanCount);
    const harken::PackLogicRecord* failedRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, failedRecord);
    ASSERT_EQ(1u, failedRecord->busSends.size());
    EXPECT_FLOAT_EQ(-6.0f, failedRecord->busSends[0].levelDb);

    // An ordinary producer command proves the unpublished ticket was
    // cancelled rather than left blocking the single producer.
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    const harken::PackLogicRecord* liveRecord =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, liveRecord);
    ASSERT_EQ(1u, liveRecord->busSends.size());
    EXPECT_FLOAT_EQ(-12.0f, liveRecord->busSends[0].levelDb);
    EXPECT_EQ(oldTopologyPublications + 1,
              engine->topologyPlanCount);
    engine->nullBackend.Pump(1);
}

TEST(PackLogicReplace,
     MediaPreparationFailureReusesDynamicRouteIdentityExactlyOnce) {
    VnConfig config = PackLogicConfig(4, kBlock, kRate);
    config.fxSlotsPerBus = 4;
    config.compressorEffectPoolCapacity = 2;
    config.dynamicRouteCapacity = 4;
    config.dynamicRoutePlanPoolCapacity = 3;
    config.dynamicBusTapCapacity = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    constexpr VnBusId kTargetBus = harkentest::ObjectId(8961);
    constexpr VnBusId kSourceBus = harkentest::ObjectId(8962);
    const std::string initialPath =
        TempPath("plr_media_route_v1.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_media_route_v1", initialPath, kTargetBus,
        kSourceBus, "compressor", false,
        "[-18.0, 4.0, 10.0, 120.0, 6.0, 0.0]", false, 0.0f));
    const std::string replacementPath =
        TempPath("plr_media_route_v2.pack");
    ASSERT_TRUE(BuildResidentBusEffectPack(
        "plr_media_route_v2", replacementPath, kTargetBus,
        kSourceBus, "compressor", false,
        "[-18.0, 4.0, 10.0, 120.0, 6.0, 0.0]", true, -7.0f));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(initialPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->dynamicRoutes.DrainAudioMessages();
    const std::uint64_t issuedBefore =
        engine->dynamicRoutes.LastIssuedPlanRevision();
    const std::uint64_t tapGenerationBefore =
        engine->dynamicRoutes.TapGeneration(
            VN_DYNAMIC_SOURCE_BUS, 0);
    const std::uint64_t topologyBefore =
        engine->topologyPlanCount;

    ASSERT_EQ(VN_OK, ReplacePack(pack, replacementPath));
    {
        StagingFaultGuard guard(
            harken::PackStagingFault::ActivationMediaPreparation);
        ASSERT_EQ(VN_OK, Vn_Update());
    }

    harken::PackPendingRevision* pending =
        engine->logicPacks.PendingFor(pack);
    ASSERT_NE(nullptr, pending);
    ASSERT_EQ(1u, pending->dynamicRouteCandidates.size());
    ASSERT_NE(harken::kInvalidDynamicRoutePlanIndex,
              pending->dynamicPlanIndex);
    const std::uint32_t planIndex = pending->dynamicPlanIndex;
    const harken::DynamicRouteDefinition prepared =
        pending->dynamicRouteCandidates[0];
    const std::uint64_t planGeneration =
        engine->dynamicRoutes.PlanGeneration(planIndex);
    const std::uint64_t planRevision =
        engine->dynamicRoutes.PlanRevision(planIndex);
    EXPECT_EQ(issuedBefore + 1,
              engine->dynamicRoutes.LastIssuedPlanRevision());
    EXPECT_EQ(tapGenerationBefore + 1, prepared.tapGeneration);
    EXPECT_EQ(nullptr,
              engine->dynamicRoutes.Find(kTargetBus, 0, 0));
    EXPECT_EQ(topologyBefore, engine->topologyPlanCount);

    ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    const harken::DynamicRouteDefinition* live =
        engine->dynamicRoutes.Find(kTargetBus, 0, 0);
    ASSERT_NE(nullptr, live);
    EXPECT_EQ(prepared.tableIndex, live->tableIndex);
    EXPECT_EQ(prepared.generation, live->generation);
    EXPECT_EQ(prepared.revision, live->revision);
    EXPECT_EQ(prepared.tapSlot, live->tapSlot);
    EXPECT_EQ(prepared.tapGeneration, live->tapGeneration);
    EXPECT_EQ(planGeneration,
              engine->dynamicRoutes.PlanGeneration(planIndex));
    EXPECT_EQ(planRevision,
              engine->dynamicRoutes.PlanRevision(planIndex));
    EXPECT_EQ(issuedBefore + 1,
              engine->dynamicRoutes.LastIssuedPlanRevision());
    EXPECT_EQ(topologyBefore + 1, engine->topologyPlanCount);
    engine->nullBackend.Pump(1);
}

} // namespace
