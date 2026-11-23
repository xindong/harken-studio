#pragma once

#include "Utils/ObjectId.h"
#include "RuntimeBridge/MediaRegistry.h"
#include "RuntimeBridge/ProjectSyncBridge.h"
#include "RuntimeBridge/MediaServices.h"

// PackagingBridge -- packaging / asset-pipeline cluster of the runtime bridge.
//
// Scope: building and exporting Packs -- codec encoding
// against the runtime's pack format, pack layout/manifest generation, media
// registry interaction, and load/unload/prepare of built packs for in-editor
// audition (AssetConverter, MediaPool,
// ProjectPackExporter, ProjectSerializer).
//
// Interface intent (M4 alignment): pack contents are keyed by stable u64
// asset IDs assigned and persisted by the editor (spec §2.3); the editor owns
// the name->ID mapping and the runtime never derives IDs from names. On-disk
// format identities (magics, codec ID strings, artifact extensions) are data
// contracts owned by the runtime side of this bridge -- they cross this
// interface as opaque build inputs/outputs, not as editor vocabulary.
//
// Public bridge headers expose editor-owned types only (Qt/std/editor
// structs); the legacy engine surface never appears here.

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <vector>

namespace harkenizer::resourcecompiler {
class MediaCache;
}

namespace harken::studio {
namespace VorbisEncoder { struct ResidentVorbisBlob; }
namespace bridge {

//==============================================================================
// Frozen legacy-contract accessors.
//
// The literal values are on-disk / on-wire identities of the legacy pack
// format. They live inside the bridge implementation; editor code that needs
// to spell them (file dialogs, deploy paths, manifests) goes through these
// accessors so the contract has exactly one owner.
//==============================================================================

QString packArtifactExtension();     // pack file extension, dot included
uint32_t legacyPackContainerMagic(); // legacy .vp container magic

//==============================================================================
// Checksum / media registry / decode wrappers.
//==============================================================================

/// CRC32 as defined by the runtime pack format (loose media body checksum).
uint32_t packFormatCrc32(const uint8_t* data, uint32_t length);

std::shared_ptr<MediaServices> makeBuildMediaServices();

/// Pack language identity (legacy language-id string mapping).
uint32_t packLanguageIdFromName(const QString& name);

/// Decode an audio file to interleaved float samples via the runtime decoder.
/// Returns false when the file cannot be decoded or decodes to zero frames.
bool decodeAudioFileInterleaved(const QString& filePath,
                                std::vector<float>& outInterleaved,
                                int& outChannels,
                                int& outSampleRate);

/// Audio file metadata probe via the runtime decoder (no PCM load).
/// valid == false when the file cannot be identified.
struct AudioFileInfoData
{
    bool valid = false;
    QString codec;
    int channels = 0;
    int sampleRate = 0;
    int bitDepth = 0;
    int64_t frameCount = 0;
    double durationSeconds = 0.0;
};
AudioFileInfoData audioFileInfo(const QString& path);

//==============================================================================
// Pack building -- editor-owned mirrors of the pack build inputs plus a
// build session that owns the runtime pack builder behind a pimpl.
//==============================================================================

/// Media payload format. Ordinals match the runtime pack audio-format wire
/// values.
enum class PackMediaFormat : uint8_t { Pcm16, PcmFloat, Adpcm, Vorbis };

struct PackEffectPreset
{
    harken::studio::types::ObjectId id{};
    uint32_t type = 0;
    std::vector<float> parameters;
};

struct PackBusEffect
{
    harken::studio::types::ObjectId id{};
    uint32_t type = 0;
    bool bypass = false;
    std::vector<float> parameters;
};

struct PackBus
{
    harken::studio::types::ObjectId id{};
    // A root bus may parent to itself or to 0; the bridge normalizes both
    // spellings to the runtime master bus when emitting the manifest.
    harken::studio::types::ObjectId parentId = {};
    QString name;
    float volume = 0.0f;
    float outputBusVolume = 0.0f;
    uint16_t limiterMaxInstances = 0;
    uint8_t limiterLimitBehavior = 0;
    std::vector<PackBusEffect> effects;
};

struct PackBusSend
{
    harken::studio::types::ObjectId fromBusId = {};
    harken::studio::types::ObjectId toBusId = {};
    float levelDb = 0.0f;
};

struct PackModulator
{
    uint32_t id = 0;
    uint8_t subtype = 0; // 0=LFO, 1=Envelope, 2=Time
    uint8_t scope = 0;

    float lfoFrequency = 1.0f;
    float lfoDepth = 1.0f;
    float lfoOffset = 0.0f;
    float lfoPwm = 0.5f;
    float lfoSmooth = 0.0f;
    uint8_t lfoWaveform = 0;
    uint8_t lfoUnipolar = 0;

    float envAttackTime = 0.01f;
    float envHoldTime = 0.0f;
    float envDecayTime = 0.1f;
    float envSustainLevel = 1.0f;
    float envReleaseTime = 0.1f;
    uint8_t envAttackCurve = 0;
    uint8_t envDecayCurve = 0;
    uint8_t envReleaseCurve = 0;
    uint8_t envAutoRelease = 0;

    float timeDurationSec = 1.0f;
    float timeInitialValue = 0.0f;
    float timeTargetValue = 1.0f;
    uint8_t timeCurveType = 0;
    uint8_t timeLoop = 0;
    uint8_t timePingPong = 0;

    struct Binding
    {
        uint8_t targetParam = 0;
        uint8_t curveType = 0;
        uint8_t enabled = 1;
        float mappedMin = 0.0f;
        float mappedMax = 1.0f;
    };
    std::vector<Binding> bindings;
};

struct PackMeterWiring
{
    harken::studio::types::ObjectId busId = {};
    harken::studio::types::ObjectId variableId = {};
    uint32_t attackMs = 0;
    uint32_t releaseMs = 0;
};

struct PackParameterBinding
{
    harken::studio::types::ObjectId id{};
    harken::studio::types::ObjectId variableId = {};
    uint8_t scope = 0; // runtime target-scope ordinal (0 voice, 1 bus)
    uint32_t target = 0;
    uint32_t transitionMs = 0;

    struct CurvePoint
    {
        float x = 0.0f;
        float y = 0.0f;
    };
    std::vector<CurvePoint> points;
    std::vector<PackCurveHandle> handles;
};

struct PackBindingAttachment
{
    harken::studio::types::ObjectId bindingId{};
    uint8_t scope = 0; // runtime target-scope ordinal (0 voice, 1 bus)
    harken::studio::types::ObjectId objectId = {};
};

struct PackMediaDesc
{
    harken::studio::types::ObjectId sourceId = {}; // new .pack entry id (runtime stable sound id)
    uint32_t authoringSourceId = 0; // editor source row, for diagnostics/UI only
    QString sourcePath;
    std::shared_ptr<const std::vector<float>> interleavedSamples;
    std::shared_ptr<const VorbisEncoder::ResidentVorbisBlob> residentVorbis;
    uint64_t encodedFrameCount = 0;
    QString contentHash;
    bool downmixToMono = false;
    bool loopable = false;
    uint64_t loopStartFrame = 0;
    uint64_t loopEndFrame = 0;
    std::vector<uint8_t> data;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    PackMediaFormat format = PackMediaFormat::Pcm16;
};

bool computePackMediaContentRevision(const PackMediaDesc& media, uint64_t* revision);

/// One pack build. Owns the runtime pack builder; add content, then build.
class PackBuildSession
{
public:
    PackBuildSession();
    ~PackBuildSession();
    PackBuildSession(const PackBuildSession&) = delete;
    PackBuildSession& operator=(const PackBuildSession&) = delete;

    void setInitPack(bool enable);
    void setMediaCache(std::shared_ptr<harkenizer::resourcecompiler::MediaCache> cache);

    void addBus(const PackBus& bus);
    void addBusSend(const PackBusSend& send);
    void addModulatorBinding(harken::studio::types::ObjectId targetObjectId, const PackModulator& modulator);
    void addAttenuation(const PackAttenuation& attenuation);
    void addEffectPreset(const PackEffectPreset& preset);
    void addDiscreteVariable(harken::studio::types::ObjectId variableId);
    void addMeterWiring(const PackMeterWiring& wiring);
    void addBinding(const PackParameterBinding& binding);
    void addBindingAttachment(const PackBindingAttachment& attachment);
    void addMedia(const PackMediaDesc& media);
    void addWarning(const QString& warning);

    QString inputFingerprint();

    enum class CanonicalPackKind : uint32_t { Init = 2, CanonicalContent = 3 };

    struct CanonicalPackEntry
    {
        uint32_t kind = 0;
        uint32_t vintage = 1;
        harken::studio::types::ObjectId id{};
        uint64_t relationOrdinal = 0;
        std::vector<uint8_t> payload;
        std::shared_ptr<const std::vector<float>> media;
        std::shared_ptr<const std::vector<uint8_t>> encodedBytes;

        const uint8_t* mediaData() const
        {
            if (encodedBytes) return encodedBytes->data();
            return media ? reinterpret_cast<const uint8_t*>(media->data()) : nullptr;
        }

        std::size_t mediaBytes() const
        {
            if (encodedBytes) return encodedBytes->size();
            return media ? media->size() * sizeof(float) : 0;
        }

        std::size_t payloadBytes() const
        {
            return payload.size() + mediaBytes();
        }
    };

    bool buildEntries(std::vector<CanonicalPackEntry>* entries);

    static bool wrapCanonicalPackFile(const QString& sourcePath,
                                      const QString& outputPath,
                                      CanonicalPackKind packKind,
                                      const std::vector<CanonicalPackEntry>& extraEntries,
                                      QString* error);

    static bool writeCanonicalPackFile(const QString& outputPath,
                                       CanonicalPackKind packKind,
                                       const std::vector<CanonicalPackEntry>& entries,
                                       QString* error);

    QString lastError() const;

    /// Authored content the v2 pack format cannot carry (unmappable action
    /// steps, curve slots without a wire representation, ...) is dropped at
    /// manifest emission and reported here, one line per drop.
    QStringList warnings() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridge
} // namespace harken::studio
