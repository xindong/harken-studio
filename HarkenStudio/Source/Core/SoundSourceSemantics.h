#pragma once

#include "Components/VSound.h"

#include <QFlags>
#include <QJsonArray>
#include <QString>
#include <QSet>
#include <QtGlobal>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace VNX {

struct AssetInfo;

namespace SoundSourceSemantics {

enum class SourceIntegrityIssue : unsigned int
{
    None = 0,
    TrimStartPastFileEnd = 1U << 0U,
    TrimEndPastFileEnd = 1U << 1U,
    TrimWindowEmpty = 1U << 2U,
    LoopClampedIntoTrimWindow = 1U << 3U,
    LoopDroppedAsDegenerate = 1U << 4U,
    SourceFileChangedSinceAuthoring = 1U << 5U
};
Q_DECLARE_FLAGS(SourceIntegrityIssues, SourceIntegrityIssue)
Q_DECLARE_OPERATORS_FOR_FLAGS(SourceIntegrityIssues)

struct EffectiveRange
{
    uint64_t trimStartFrame = 0;
    uint64_t trimEndFrame = 0;
    std::optional<SoundLoopRegion> loopRegion;
    SourceIntegrityIssues issues;

    bool intact() const { return issues == SourceIntegrityIssues{}; }
};

struct PcmSlice
{
    uint64_t startFrame = 0;
    uint64_t frameCount = 0;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    uint64_t windowStartFrame = 0;
    uint64_t windowFrameCount = 0;
    bool loops = false;

    bool valid() const { return frameCount > 0 && channels > 0 && sampleRate > 0; }
    size_t sampleOffset() const
    {
        return static_cast<size_t>(startFrame) * static_cast<size_t>(channels);
    }
};

struct PlayheadExtrapolation
{
    uint64_t startFrame = 0;
    uint64_t windowStartFrame = 0;
    uint64_t windowFrameCount = 0;
    uint32_t sampleRate = 0;
    bool loops = false;
    bool playing = false;
};

struct SourceSelectionResolution
{
    const SoundSourceDef* source = nullptr;
    QString error;

    explicit operator bool() const { return source != nullptr; }
};

inline constexpr uint64_t kMaxSourceFrame = 1ull << 53;

uint64_t framesFromSeconds(double seconds, uint32_t sampleRate);
std::optional<double> secondsFromFrames(uint64_t frames, uint32_t sampleRate);

QJsonObject toJson(const SoundSources& sources);
std::optional<SoundSources> fromJson(const QJsonObject& json, QString* error = nullptr);
QJsonArray entriesToJson(const std::vector<SoundSourceDef>& entries);
std::optional<std::vector<SoundSourceDef>> entriesFromJson(
    const QJsonArray& json, QString* error = nullptr);
std::vector<SoundSourceSet*> sourceSets(SoundSources& sources);
std::vector<const SoundSourceSet*> sourceSets(const SoundSources& sources);
const SoundSourceSet* sourceSet(const VSound& sound, const QString& language);
SoundSourceDef* findEntry(SoundSources& sources, const QUuid& entryId);
const SoundSourceDef* findEntry(const VSound& sound, const QUuid& entryId);
SourceSelectionResolution resolveSourceSelection(
    const VSound& sound, const QString& platform, const QString& language);
void assignActive(SoundSourceSet& sources, const QUuid& entryId, const QString& platform = {});
void removeEntry(SoundSources& sources, QUuid entryId);
void moveToLanguage(SoundSources& sources, const QSet<QUuid>& entries, const QString& language);

std::optional<SoundLoopRegion> fileLoopRegion(const AssetInfo* asset);
bool loopIsFileSeeded(const SoundSourceDef& source, const AssetInfo* asset);
SoundSourceDef authoredView(const SoundSourceDef& source, const AssetInfo* asset);
void settleLoopProvenance(std::vector<SoundSourceDef>& sources,
                          const std::function<const AssetInfo*(uint32_t)>& resolve);
bool hasDeepSourceSemantics(const VSound& sound);
bool hasDeepSourceSemantics(
    const VSound& sound,
    const std::function<const AssetInfo*(uint32_t)>& resolve);
SoundSourceDef canonicalized(const SoundSourceDef& source, uint64_t totalFrames);
void stampAuthoringAnchors(std::vector<SoundSourceDef>& sources,
                           const std::vector<SoundSourceDef>& previous,
                           const std::function<const AssetInfo*(uint32_t)>& resolve);
std::optional<EffectiveRange> effectiveRange(const SoundSourceDef& source,
                                             const AssetInfo* asset);
QString integrityText(const SoundSourceDef& source, const AssetInfo* asset);
std::optional<PcmSlice> pcmSlice(const SoundSourceDef& source, const AssetInfo* asset,
                                 uint64_t startFrame = 0);
std::optional<uint64_t> extrapolatedPlayheadFrame(const PlayheadExtrapolation& state,
                                                  qint64 elapsedNanoseconds);
void remintEntries(VSound& sound);

}
}
