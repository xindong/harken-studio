#pragma once

#include "Utils/ObjectId.h"

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

namespace HarkenEditor {
namespace Bridge {

//==============================================================================
// Frozen legacy-contract accessors.
//
// The literal values are on-disk / on-wire identities of the legacy pack
// format. They live inside the bridge implementation; editor code that needs
// to spell them (file dialogs, deploy paths, manifests) goes through these
// accessors so the contract has exactly one owner.
//==============================================================================

QString packArtifactExtension();     // pack file extension, dot included
QString packManifestFileName();      // per-platform pack list file name
QString packIndexFileName();         // multi-platform index at the export root
QString packVorbisCodecName();       // retired codec identifier (unavailable in .pack)
QString packOpusCodecName();         // retired codec identifier (unavailable in .pack)
uint32_t legacyPackContainerMagic(); // legacy .vp container magic

//==============================================================================
// Checksum / media registry / decode wrappers.
//==============================================================================

/// CRC32 as defined by the runtime pack format (loose media body checksum).
uint32_t packFormatCrc32(const uint8_t* data, uint32_t length);

/// Absolute path registered for a source ID; empty when not registered.
QString mediaRegistryFindPath(uint32_t sourceId);
void mediaRegistryRegister(uint32_t sourceId, const QString& absolutePath,
                           uint64_t fileSize = 0);
void mediaRegistryUnregister(uint32_t sourceId);
uint32_t mediaRegistryCount();
void mediaRegistryClear();

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
    int channels = 0;
    int sampleRate = 0;
    int bitDepth = 0;
    int64_t frameCount = 0;
    double durationSeconds = 0.0;
};
AudioFileInfoData audioFileInfo(const QString& path);

/// Source ID registered for an absolute media path; 0 when not registered.
uint32_t mediaRegistryFindSourceIdByPath(const QString& absolutePath);

//==============================================================================
// Pack codec side-table inspection.
//
// Read-only parsing of the pack container's codec metadata (CODC) and shared
// codebook (CDBK) sections for the asset inspector. The section payloads are
// raw bytes read by the editor's own container walk (the container layout is
// a frozen wire format); the record layouts, versions, and identity strings
// live inside the bridge.
//==============================================================================

struct PackCodecEntryInfo
{
    uint32_t sourceId = 0;
    uint16_t codecId = 0;
    uint16_t containerId = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint32_t channelLayoutMask = 0;
    uint64_t frameCount = 0;
    uint64_t loopStartFrame = 0;
    uint64_t loopEndFrame = 0;
    uint32_t packetTableOffset = 0;
    uint32_t packetTableCount = 0;
    uint32_t seekTableOffset = 0;
    uint32_t seekTableCount = 0;
    uint32_t codebookId = 0;
    uint32_t codebookHash = 0;
    uint32_t decodeAllocBytes = 0;
    uint32_t flags = 0;
};

struct PackCodecCodebookInfo
{
    uint32_t codebookId = 0;
    uint32_t codebookHash = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct PackCodecFlagsView
{
    bool fullDecodeFallback = false;
    bool compressedResident = false;
    bool hasPacketTable = false;
    bool hasSeekTable = false;
    bool hasSharedCodebook = false;
    bool loopEnabled = false;
    bool vorbisVbrQuality = false;
    bool vorbisManagedBitrate = false;
};

QString packCodecName(uint16_t codecId);            // display identity, incl. persisted codec strings
QString packCodecContainerName(uint16_t containerId);
PackCodecFlagsView decodePackCodecFlags(uint32_t flags);
bool parsePackCodecMetadataSection(const QByteArray& payload,
                                   std::vector<PackCodecEntryInfo>& out,
                                   QString* error);
bool parsePackCodecCodebookSection(const QByteArray& payload,
                                   std::vector<PackCodecCodebookInfo>& out,
                                   QString* error);
bool isPackCodecMetadataSection(uint32_t chunkFourcc);   // codec metadata section id
bool isPackCodecCodebookSection(uint32_t chunkFourcc);   // shared codebook section id

//==============================================================================
// Pack codec conversion.
//==============================================================================

/// Opaque per-media codec metadata produced by the pack encoders. The blob is
/// the runtime's fixed-size codec metadata record; the named fields mirror
/// the few values editor logic needs to read (loop validation, shared
/// codebook identity) without exposing the record layout.
struct PackCodecMetadata
{
    std::vector<uint8_t> entryBlob;
    uint64_t frameCount = 0;
    uint32_t codebookId = 0;
    uint32_t codebookHash = 0;
    bool isValid() const { return !entryBlob.empty(); }
};

struct PackVorbisEncodeSettings
{
    bool managedBitrate = false; // false: VBR quality mode
    float vbrQuality = 0.6f;     // 0..1, VBR mode only
    uint32_t averageBitrate = 0; // managed mode; required non-zero
    uint32_t minBitrate = 0;     // managed mode; 0 = unconstrained
    uint32_t maxBitrate = 0;     // managed mode; 0 = unconstrained
};

bool encodePackVorbis(const std::vector<float>& interleaved,
                      int channels, int sampleRate,
                      const PackVorbisEncodeSettings& settings,
                      std::vector<uint8_t>& outData,
                      PackCodecMetadata& outMetadata,
                      std::vector<uint8_t>* outSharedCodebook = nullptr);

bool encodePackOpus(const std::vector<float>& interleaved,
                    int channels, int sampleRate,
                    std::vector<uint8_t>& outData,
                    PackCodecMetadata& outMetadata);

/// Stamp a loop region onto already-produced codec metadata. The caller
/// validates the region against frameCount before calling.
void applyLoopRegion(PackCodecMetadata& metadata,
                     uint64_t loopStartFrame,
                     uint64_t loopEndFrame);

//==============================================================================
// Pack building -- editor-owned mirrors of the pack build inputs plus a
// build session that owns the runtime pack builder behind a pimpl.
//==============================================================================

/// Media payload format. Ordinals match the runtime pack audio-format wire
/// values.
enum class PackMediaFormat : uint8_t { Pcm16, PcmFloat, Adpcm, Vorbis };

struct PackBusEffect
{
    harkenstudio::ObjectId id{};
    uint32_t type = 0;
    bool bypass = false;
    std::vector<float> parameters;
};

struct PackBus
{
    harkenstudio::ObjectId id{};
    // A root bus may parent to itself or to 0; the bridge normalizes both
    // spellings to the runtime master bus when emitting the manifest.
    harkenstudio::ObjectId parentId = {};
    QString name;
    float volume = 0.0f;
    float outputBusVolume = 0.0f;
    float makeUpGain = 0.0f;
    uint16_t limiterMaxInstances = 0;
    uint8_t limiterLimitBehavior = 0;
    std::vector<PackBusEffect> effects;
};

struct PackBusSend
{
    harkenstudio::ObjectId fromBusId = {};
    harkenstudio::ObjectId toBusId = {};
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
    harkenstudio::ObjectId busId = {};
    harkenstudio::ObjectId variableId = {};
    uint32_t attackMs = 0;
    uint32_t releaseMs = 0;
};

struct PackCurveHandle
{
    float h1x = 0.0f;
    float h1y = 0.0f;
    float h2x = 0.0f;
    float h2y = 0.0f;
};

struct PackParameterBinding
{
    harkenstudio::ObjectId id{};
    harkenstudio::ObjectId variableId = {};
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
    harkenstudio::ObjectId bindingId{};
    uint8_t scope = 0; // runtime target-scope ordinal (0 voice, 1 bus)
    harkenstudio::ObjectId objectId = {};
};

struct PackAttenuation
{
    harkenstudio::ObjectId id{};
    float maxDistance = 100.0f;

    struct CurvePoint
    {
        float distance = 0.0f;
        float value = 0.0f;
    };
    struct Slot
    {
        uint8_t slot = 0;    // runtime curve slot (0 volume)
        std::vector<CurvePoint> points;
        std::vector<PackCurveHandle> handles;
    };
    // Named curveSlots (not "slots") because Qt's keyword macros are in
    // effect wherever QObject-based editor code includes this header.
    std::vector<Slot> curveSlots;
};

struct PackMediaDesc
{
    harkenstudio::ObjectId sourceId = {}; // new .pack entry id (runtime stable sound id)
    uint32_t authoringSourceId = 0; // editor source row, for diagnostics/UI only
    QString sourcePath; // WAV path consumed by harken_packc
    bool downmixToMono = false;
    bool loopable = false;
    uint64_t loopStartFrame = 0;
    uint64_t loopEndFrame = 0;
    std::vector<uint8_t> data;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    PackMediaFormat format = PackMediaFormat::Pcm16;
    // Empty blob => the bridge synthesizes a full-decode-fallback metadata
    // entry from the format/channels/sampleRate above.
    PackCodecMetadata codecMetadata;
    std::vector<uint8_t> sharedCodebook;
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

    void addBus(const PackBus& bus);
    void addBusSend(const PackBusSend& send);
    void addModulatorBinding(harkenstudio::ObjectId targetObjectId, const PackModulator& modulator);
    void addAttenuation(const PackAttenuation& attenuation);
    void addDiscreteVariable(harkenstudio::ObjectId variableId);
    void addMeterWiring(const PackMeterWiring& wiring);
    void addBinding(const PackParameterBinding& binding);
    void addBindingAttachment(const PackBindingAttachment& attachment);
    void addMedia(const PackMediaDesc& media);
    void addStringMapping(harkenstudio::ObjectId objectId, const QString& name);
    void addObjectMetadata(harkenstudio::ObjectId ownerObjectId,
                           const QByteArray& guidRfc4122,
                           const QString& objectPath);
    void addWarning(const QString& warning);

    bool buildToFile(const QString& outputPath);
    QString inputFingerprint();

    enum class CanonicalPackKind : uint32_t { Init = 2, CanonicalContent = 3 };

    struct CanonicalPackEntry
    {
        uint32_t kind = 0;
        uint32_t vintage = 1;
        harkenstudio::ObjectId id{};
        uint64_t relationOrdinal = 0;
        std::vector<uint8_t> payload;
    };

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

} // namespace Bridge
} // namespace HarkenEditor
