#include "SoundSourceSemantics.h"

#include "ProjectPlatform.h"
#include "ProjectLanguageSemantics.h"

#include <QCoreApplication>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace VNX::SoundSourceSemantics {

namespace {

std::optional<uint64_t> optionalFrame(const QJsonObject& json, const QString& key)
{
    if (!json.contains(key)) return std::nullopt;
    const QJsonValue value = json.value(key);
    if (!value.isDouble()) return std::nullopt;
    const double raw = value.toDouble();
    if (!std::isfinite(raw) || raw < 0.0 || raw > static_cast<double>(kMaxSourceFrame)
        || std::trunc(raw) != raw)
        return std::nullopt;
    return static_cast<uint64_t>(raw);
}

bool rowHasDeepRangeSemantics(
    const SoundSourceDef& source,
    const std::function<const AssetInfo*(uint32_t)>& resolve)
{
    const AssetInfo* asset = resolve ? resolve(source.sourceId) : nullptr;
    const SoundSourceDef authored = authoredView(source, asset);
    if (authored.trimStartFrame != 0 || authored.loopRegion.has_value())
        return true;
    if (!authored.trimEndFrame.has_value())
        return false;
    if (!asset || asset->metadata.totalSamples == 0)
        return true;
    const SoundSourceDef canonical = canonicalized(
        authored, asset->metadata.totalSamples);
    return canonical.trimEndFrame.value_or(asset->metadata.totalSamples)
        != asset->metadata.totalSamples;
}

bool sameAuthoredRange(const SoundSourceDef& a, const SoundSourceDef& b)
{
    return a.trimStartFrame == b.trimStartFrame && a.trimEndFrame == b.trimEndFrame
        && a.loopRegion == b.loopRegion;
}

}

uint64_t framesFromSeconds(double seconds, uint32_t sampleRate)
{
    if (sampleRate == 0 || !std::isfinite(seconds) || seconds <= 0.0) return 0;
    const double frames = std::round(seconds * sampleRate);
    if (frames >= static_cast<double>(kMaxSourceFrame)) return kMaxSourceFrame;
    return static_cast<uint64_t>(frames);
}

std::optional<double> secondsFromFrames(uint64_t frames, uint32_t sampleRate)
{
    if (sampleRate == 0) return std::nullopt;
    return static_cast<double>(frames) / sampleRate;
}

std::vector<SoundSourceSet*> sourceSets(SoundSources& sources)
{
    std::vector<SoundSourceSet*> result{&sources.plain};
    for (auto& set : sources.localized)
        result.push_back(&set);
    return result;
}

std::vector<const SoundSourceSet*> sourceSets(const SoundSources& sources)
{
    std::vector<const SoundSourceSet*> result{&sources.plain};
    for (const auto& set : sources.localized)
        result.push_back(&set);
    return result;
}

const SoundSourceSet* sourceSet(const VSound& sound, const QString& language)
{
    if (!sound.distinguishLanguage) return &sound.sources.plain;
    const auto found = sound.sources.localized.constFind(language.trimmed());
    return found == sound.sources.localized.cend() ? nullptr : &found.value();
}

SoundSourceDef* findEntry(SoundSources& sources, const QUuid& entryId)
{
    for (auto* set : sourceSets(sources))
        for (auto& entry : set->entries)
            if (entry.entryId == entryId) return &entry;
    return nullptr;
}

const SoundSourceDef* findEntry(const VSound& sound, const QUuid& entryId)
{
    if (!sound.distinguishLanguage) {
        for (const auto& entry : sound.sources.plain.entries)
            if (entry.entryId == entryId) return &entry;
    } else {
        for (const auto& set : sound.sources.localized)
            for (const auto& entry : set.entries)
                if (entry.entryId == entryId) return &entry;
    }
    return nullptr;
}

void assignActive(SoundSourceSet& sources, const QUuid& entryId, const QString& platform)
{
    if (platform.isEmpty()) {
        sources.active = entryId;
        sources.platforms.clear();
    } else {
        sources.platforms[platform].active = entryId;
    }
}

SourceSelectionResolution resolveSourceSelection(
    const VSound& sound, const QString& platform, const QString& language)
{
    if (!platform.isEmpty() && !ProjectPlatformSemantics::parseId(platform))
        return {nullptr, QStringLiteral("build platform must be a project platform ID")};
    if (sound.distinguishLanguage && language.trimmed().isEmpty())
        return {nullptr, QStringLiteral("build language must not be empty")};
    const auto* set = sourceSet(sound, language);
    if (!set)
        return {nullptr, QStringLiteral("source active is not configured for %1/%2")
                             .arg(platform, language)};
    const auto override = set->platforms.constFind(platform);
    const QUuid active = override == set->platforms.cend() ? set->active : override->active;
    if (active.isNull())
        return {nullptr, QStringLiteral("source active is not configured for %1/%2")
                             .arg(platform, language)};
    for (const auto& entry : set->entries)
        if (entry.entryId == active) return {&entry, {}};
    return {nullptr, QStringLiteral("source active entryId is dangling")};
}

QJsonObject toJson(const SoundSources& sources)
{
    const auto serializeSet = [](const SoundSourceSet& set) {
        QJsonObject platforms;
        for (auto it = set.platforms.cbegin(); it != set.platforms.cend(); ++it)
            platforms[it.key()] = QJsonObject{{QStringLiteral("active"),
                it->active.isNull() ? QJsonValue(QJsonValue::Null)
                                   : QJsonValue(it->active.toString(QUuid::WithoutBraces))}};
        return QJsonObject{
            {QStringLiteral("entries"), entriesToJson(set.entries)},
            {QStringLiteral("active"), set.active.isNull() ? QJsonValue(QJsonValue::Null)
                : QJsonValue(set.active.toString(QUuid::WithoutBraces))},
            {QStringLiteral("platforms"), platforms}};
    };
    QJsonObject localized;
    for (auto it = sources.localized.cbegin(); it != sources.localized.cend(); ++it)
        localized[it.key()] = serializeSet(it.value());
    return {{QStringLiteral("plain"), serializeSet(sources.plain)},
            {QStringLiteral("localized"), localized}};
}

std::optional<SoundSources> fromJson(const QJsonObject& json, QString* error)
{
    const auto fail = [error](const QString& message) -> std::optional<SoundSources> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (json.size() != 2 || !json.value(QStringLiteral("plain")).isObject()
        || !json.value(QStringLiteral("localized")).isObject())
        return fail(QStringLiteral("sources requires plain and localized objects"));
    QSet<QUuid> entryIds;
    const auto parseSet = [&](const QJsonValue& value, SoundSourceSet& set) {
        const QJsonObject object = value.toObject();
        if (!value.isObject() || object.size() != 3
            || !object.value(QStringLiteral("entries")).isArray()
            || !object.value(QStringLiteral("platforms")).isObject()) {
            if (error) *error = QStringLiteral("source set requires entries, active and platforms");
            return false;
        }
        const auto entries = entriesFromJson(object.value(QStringLiteral("entries")).toArray(), error);
        if (!entries) return false;
        set.entries = *entries;
        QSet<QUuid> localIds;
        for (const auto& entry : set.entries) {
            if (entryIds.contains(entry.entryId)) {
                if (error) *error = QStringLiteral("source entry IDs must be unique across all modes");
                return false;
            }
            entryIds.insert(entry.entryId);
            localIds.insert(entry.entryId);
        }
        const auto parseActive = [&](const QJsonValue& active, QUuid& result) {
            if (active.isNull()) { result = QUuid{}; return true; }
            result = QUuid(active.toString());
            if (!active.isString() || result.isNull() || !localIds.contains(result)) {
                if (error) *error = QStringLiteral("source active must reference an entry in its own set");
                return false;
            }
            return true;
        };
        if (!parseActive(object.value(QStringLiteral("active")), set.active)) return false;
        const QJsonObject platforms = object.value(QStringLiteral("platforms")).toObject();
        for (auto it = platforms.begin(); it != platforms.end(); ++it) {
            if (!ProjectPlatformSemantics::parseId(it.key()) || !it->isObject()
                || it->toObject().size() != 1) {
                if (error) *error = QStringLiteral("source platform override is invalid");
                return false;
            }
            if (!parseActive(it->toObject().value(QStringLiteral("active")),
                             set.platforms[it.key()].active)) return false;
        }
        return true;
    };
    SoundSources result;
    if (!parseSet(json.value(QStringLiteral("plain")), result.plain)) return std::nullopt;
    const QJsonObject localized = json.value(QStringLiteral("localized")).toObject();
    for (auto it = localized.begin(); it != localized.end(); ++it) {
        if (it.key().isEmpty() || it.key() != it.key().trimmed()
            || ProjectLanguageSemantics::isSfxName(it.key()))
            return fail(QStringLiteral("source language is invalid"));
        if (!parseSet(it.value(), result.localized[it.key()])) return std::nullopt;
    }
    return result;
}

QJsonArray entriesToJson(const std::vector<SoundSourceDef>& sources)
{
    QJsonArray array;
    for (const SoundSourceDef& source : sources) {
        QJsonObject json{
            {QStringLiteral("entryId"), source.entryId.toString(QUuid::WithoutBraces)},
            {QStringLiteral("sourceId"), static_cast<qint64>(source.sourceId)}};
        if (source.trimStartFrame != 0)
            json[QStringLiteral("trimStartFrame")] = static_cast<qint64>(source.trimStartFrame);
        if (source.trimEndFrame)
            json[QStringLiteral("trimEndFrame")] = static_cast<qint64>(*source.trimEndFrame);
        if (source.loopRegion) {
            json[QStringLiteral("loopRegion")] = QJsonObject{
                {QStringLiteral("startFrame"), static_cast<qint64>(source.loopRegion->startFrame)},
                {QStringLiteral("endFrame"), static_cast<qint64>(source.loopRegion->endFrame)}};
            if (source.loopProvenance == SoundLoopProvenance::File)
                json[QStringLiteral("loopProvenance")] = QStringLiteral("file");
        }
        if (!source.rangeContentHash.isEmpty())
            json[QStringLiteral("rangeContentHash")] = source.rangeContentHash;
        array.append(json);
    }
    return array;
}

std::optional<std::vector<SoundSourceDef>> entriesFromJson(const QJsonArray& json,
                                                    QString* error)
{
    auto fail = [error](const QString& message) -> std::optional<std::vector<SoundSourceDef>> {
        if (error) *error = message;
        return std::nullopt;
    };
    std::vector<SoundSourceDef> result;
    QSet<QUuid> entries;
    result.reserve(static_cast<size_t>(json.size()));
    for (const QJsonValue& value : json) {
        if (!value.isObject()) return fail(QStringLiteral("source row must be an object"));
        const QJsonObject row = value.toObject();
        static const QSet<QString> fields{
            QStringLiteral("entryId"), QStringLiteral("sourceId"),
            QStringLiteral("trimStartFrame"), QStringLiteral("trimEndFrame"),
            QStringLiteral("loopRegion"), QStringLiteral("loopProvenance"),
            QStringLiteral("rangeContentHash")};
        for (auto it = row.begin(); it != row.end(); ++it)
            if (!fields.contains(it.key())) return fail(QStringLiteral("unknown source entry field"));
        SoundSourceDef source;
        if (!row.value(QStringLiteral("entryId")).isString()
            || !row.value(QStringLiteral("sourceId")).isDouble())
            return fail(QStringLiteral("source row identity is invalid"));
        source.entryId = QUuid::fromString(row.value(QStringLiteral("entryId")).toString());
        const qint64 sourceId = row.value(QStringLiteral("sourceId")).toInteger();
        if (source.entryId.isNull() || entries.contains(source.entryId)
            || sourceId <= 0 || sourceId > std::numeric_limits<uint32_t>::max())
            return fail(QStringLiteral("source row identity is invalid"));
        entries.insert(source.entryId);
        source.sourceId = static_cast<uint32_t>(sourceId);

        if (row.contains(QStringLiteral("trimStartFrame"))) {
            const auto value = optionalFrame(row, QStringLiteral("trimStartFrame"));
            if (!value) return fail(QStringLiteral("trimStartFrame is invalid"));
            source.trimStartFrame = *value;
        }
        if (row.contains(QStringLiteral("trimEndFrame"))) {
            const auto value = optionalFrame(row, QStringLiteral("trimEndFrame"));
            if (!value) return fail(QStringLiteral("trimEndFrame is invalid"));
            source.trimEndFrame = *value;
        }
        if (row.contains(QStringLiteral("loopRegion"))) {
            if (!row.value(QStringLiteral("loopRegion")).isObject())
                return fail(QStringLiteral("loopRegion is invalid"));
            const QJsonObject loop = row.value(QStringLiteral("loopRegion")).toObject();
            const auto start = optionalFrame(loop, QStringLiteral("startFrame"));
            const auto end = optionalFrame(loop, QStringLiteral("endFrame"));
            if (!start || !end || *end <= *start)
                return fail(QStringLiteral("loopRegion is invalid"));
            source.loopRegion = SoundLoopRegion{*start, *end};
        }
        if (row.contains(QStringLiteral("loopProvenance"))) {
            const QString provenance = row.value(QStringLiteral("loopProvenance")).toString();
            if (!source.loopRegion
                || (provenance != QStringLiteral("file")
                    && provenance != QStringLiteral("authored")))
                return fail(QStringLiteral("loopProvenance is invalid"));
            source.loopProvenance = provenance == QStringLiteral("file")
                ? SoundLoopProvenance::File : SoundLoopProvenance::Authored;
        }
        if (row.contains(QStringLiteral("rangeContentHash"))) {
            const QJsonValue anchor = row.value(QStringLiteral("rangeContentHash"));
            if (!anchor.isString() || anchor.toString().isEmpty())
                return fail(QStringLiteral("rangeContentHash is invalid"));
            source.rangeContentHash = anchor.toString();
        }
        result.push_back(std::move(source));
    }
    return result;
}

std::optional<SoundLoopRegion> fileLoopRegion(const AssetInfo* asset)
{
    if (!asset) return std::nullopt;
    const LoopInfo& loop = asset->metadata.loopInfo;
    if (!loop.enabled || loop.loopEnd < loop.loopStart) return std::nullopt;
    const uint64_t end = loop.loopEnd + 1;
    if (end > kMaxSourceFrame) return std::nullopt;
    if (asset->metadata.totalSamples != 0 && end > asset->metadata.totalSamples)
        return std::nullopt;
    return SoundLoopRegion{loop.loopStart, end};
}

bool loopIsFileSeeded(const SoundSourceDef& source, const AssetInfo* asset)
{
    return source.loopProvenance == SoundLoopProvenance::File
        && source.loopRegion.has_value()
        && source.loopRegion == fileLoopRegion(asset);
}

SoundSourceDef authoredView(const SoundSourceDef& source, const AssetInfo* asset)
{
    if (!loopIsFileSeeded(source, asset)) return source;
    SoundSourceDef result = source;
    result.loopRegion.reset();
    result.loopProvenance = SoundLoopProvenance::Authored;
    return result;
}

void settleLoopProvenance(std::vector<SoundSourceDef>& sources,
                          const std::function<const AssetInfo*(uint32_t)>& resolve)
{
    for (SoundSourceDef& source : sources) {
        if (source.loopProvenance != SoundLoopProvenance::File) continue;
        if (!source.loopRegion) {
            source.loopProvenance = SoundLoopProvenance::Authored;
            continue;
        }
        const AssetInfo* asset = resolve ? resolve(source.sourceId) : nullptr;
        if (asset && !loopIsFileSeeded(source, asset))
            source.loopProvenance = SoundLoopProvenance::Authored;
    }
}

bool hasDeepSourceSemantics(const VSound& sound)
{
    return hasDeepSourceSemantics(sound, {});
}

bool hasDeepSourceSemantics(
    const VSound& sound,
    const std::function<const AssetInfo*(uint32_t)>& resolve)
{
    if (sound.scheduleOffsetSec != 0.0) return true;
    for (const auto* set : sourceSets(sound.sources)) {
        if (set->entries.size() > 1) return true;
        for (const auto& source : set->entries)
            if (rowHasDeepRangeSemantics(source, resolve)) return true;
    }
    return false;
}

SoundSourceDef canonicalized(const SoundSourceDef& source, uint64_t totalFrames)
{
    SoundSourceDef result = source;
    result.trimStartFrame = std::min(result.trimStartFrame, totalFrames);
    const uint64_t trimEnd = std::clamp(result.trimEndFrame.value_or(totalFrames),
                                        result.trimStartFrame, totalFrames);
    result.trimEndFrame = trimEnd;
    if (result.loopRegion) {
        const uint64_t start = std::clamp(result.loopRegion->startFrame,
                                          result.trimStartFrame, trimEnd);
        const uint64_t end = std::clamp(result.loopRegion->endFrame, start, trimEnd);
        if (end > start)
            result.loopRegion = SoundLoopRegion{start, end};
        else
            result.loopRegion.reset();
    }
    return result;
}

void stampAuthoringAnchors(std::vector<SoundSourceDef>& sources,
                           const std::vector<SoundSourceDef>& previous,
                           const std::function<const AssetInfo*(uint32_t)>& resolve)
{
    if (!resolve) return;
    for (SoundSourceDef& source : sources) {
        const auto before = std::find_if(previous.begin(), previous.end(),
                                         [&source](const SoundSourceDef& row) {
                                             return row.entryId == source.entryId;
                                         });
        const SoundSourceDef* previousSource = before == previous.end() ? nullptr : &*before;
        const AssetInfo* asset = resolve(source.sourceId);
        if (!sameAuthoredRange(authoredView(source, asset),
                               previousSource ? authoredView(*previousSource, asset)
                                              : SoundSourceDef{}))
            source.rangeContentHash = asset ? asset->contentHash : QString{};
        else if (source.rangeContentHash.isEmpty()
                 && (!previousSource || source.sourceId != previousSource->sourceId))
            source.rangeContentHash = asset ? asset->contentHash : QString{};
    }
}

std::optional<EffectiveRange> effectiveRange(const SoundSourceDef& source,
                                             const AssetInfo* asset)
{
    if (!asset || asset->absolutePath.isEmpty() || asset->metadata.totalSamples == 0)
        return std::nullopt;
    const uint64_t totalFrames = asset->metadata.totalSamples;
    const SoundSourceDef canonical = canonicalized(source, totalFrames);
    EffectiveRange range{canonical.trimStartFrame,
                         canonical.trimEndFrame.value_or(totalFrames),
                         canonical.loopRegion,
                         {}};
    if (!source.rangeContentHash.isEmpty()
        && source.rangeContentHash != asset->contentHash)
        range.issues |= SourceIntegrityIssue::SourceFileChangedSinceAuthoring;
    if (source.trimStartFrame > totalFrames)
        range.issues |= SourceIntegrityIssue::TrimStartPastFileEnd;
    if (source.trimEndFrame.value_or(totalFrames) > totalFrames)
        range.issues |= SourceIntegrityIssue::TrimEndPastFileEnd;
    if (range.trimEndFrame <= range.trimStartFrame)
        range.issues |= SourceIntegrityIssue::TrimWindowEmpty;
    if (source.loopRegion) {
        if (!range.loopRegion)
            range.issues |= SourceIntegrityIssue::LoopDroppedAsDegenerate;
        else if (*range.loopRegion != *source.loopRegion)
            range.issues |= SourceIntegrityIssue::LoopClampedIntoTrimWindow;
    }
    return range;
}

QString integrityText(const SoundSourceDef& source, const AssetInfo* asset)
{
    const auto range = effectiveRange(source, asset);
    if (!range || range->intact())
        return {};
    QStringList messages;
    if (range->issues.testFlag(SourceIntegrityIssue::SourceFileChangedSinceAuthoring))
        messages.push_back(QCoreApplication::translate(
            "SoundSourceSemantics",
            "The source file changed after this range was authored."));
    if (range->issues.testFlag(SourceIntegrityIssue::TrimStartPastFileEnd))
        messages.push_back(QCoreApplication::translate(
            "SoundSourceSemantics", "Trim start is past the end of the source file."));
    if (range->issues.testFlag(SourceIntegrityIssue::TrimEndPastFileEnd))
        messages.push_back(QCoreApplication::translate(
            "SoundSourceSemantics", "Trim end is past the end of the source file."));
    if (range->issues.testFlag(SourceIntegrityIssue::TrimWindowEmpty))
        messages.push_back(QCoreApplication::translate(
            "SoundSourceSemantics", "The trim range is empty."));
    if (range->issues.testFlag(SourceIntegrityIssue::LoopClampedIntoTrimWindow))
        messages.push_back(QCoreApplication::translate(
            "SoundSourceSemantics", "The loop range extends outside the trim range."));
    if (range->issues.testFlag(SourceIntegrityIssue::LoopDroppedAsDegenerate))
        messages.push_back(QCoreApplication::translate(
            "SoundSourceSemantics", "The loop range is empty."));
    return QCoreApplication::translate(
        "SoundSourceSemantics", "Source integrity: %1").arg(messages.join(QLatin1Char(' ')));
}

std::optional<PcmSlice> pcmSlice(const SoundSourceDef& source, const AssetInfo* asset,
                                 uint64_t startFrame)
{
    const auto range = effectiveRange(source, asset);
    if (!range || range->trimEndFrame <= range->trimStartFrame)
        return std::nullopt;
    const uint32_t channels = asset->metadata.channels;
    const uint32_t sampleRate = asset->metadata.sampleRate;
    if (channels == 0 || sampleRate == 0)
        return std::nullopt;

    const bool loops = range->loopRegion.has_value();
    uint64_t windowBegin = range->trimStartFrame;
    uint64_t windowEnd = range->trimEndFrame;
    if (loops) {
        windowBegin = range->loopRegion->startFrame;
        windowEnd = range->loopRegion->endFrame;
    }
    if (windowEnd <= windowBegin)
        return std::nullopt;

    uint64_t playBegin = windowBegin;
    if (startFrame != 0) {
        if (startFrame < windowBegin || startFrame >= windowEnd)
            return std::nullopt;
        playBegin = startFrame;
    }

    PcmSlice slice;
    slice.startFrame = playBegin;
    slice.windowStartFrame = windowBegin;
    slice.windowFrameCount = windowEnd - windowBegin;
    slice.frameCount = loops ? slice.windowFrameCount : (windowEnd - playBegin);
    slice.channels = channels;
    slice.sampleRate = sampleRate;
    slice.loops = loops;
    return slice;
}

std::optional<uint64_t> extrapolatedPlayheadFrame(const PlayheadExtrapolation& state,
                                                  qint64 elapsedNanoseconds)
{
    if (!state.playing || state.sampleRate == 0 || state.windowFrameCount == 0
        || elapsedNanoseconds < 0)
        return std::nullopt;

    const uint64_t windowEnd = state.windowStartFrame + state.windowFrameCount;
    if (state.startFrame < state.windowStartFrame || state.startFrame >= windowEnd)
        return std::nullopt;

    const uint64_t elapsedNs = static_cast<uint64_t>(elapsedNanoseconds);
    const uint64_t elapsedFrames =
        (elapsedNs / 1000000000ull) * state.sampleRate
        + ((elapsedNs % 1000000000ull) * state.sampleRate) / 1000000000ull;
    if (state.loops) {
        const uint64_t offset = state.startFrame - state.windowStartFrame;
        return state.windowStartFrame + ((offset + elapsedFrames) % state.windowFrameCount);
    }

    const uint64_t remaining = windowEnd - state.startFrame;
    if (elapsedFrames >= remaining)
        return std::nullopt;
    return state.startFrame + elapsedFrames;
}

void remintEntries(VSound& sound)
{
    for (auto* set : sourceSets(sound.sources)) {
        QHash<QUuid, QUuid> ids;
        for (auto& source : set->entries) {
            const QUuid oldId = source.entryId;
            source.entryId = QUuid::createUuid();
            ids.insert(oldId, source.entryId);
        }
        set->active = ids.value(set->active);
        for (auto& override : set->platforms)
            override.active = ids.value(override.active);
    }
}

void removeEntry(SoundSources& sources, QUuid entryId)
{
    for (auto* set : sourceSets(sources)) {
        std::erase_if(set->entries, [&](const auto& entry) { return entry.entryId == entryId; });
        if (set->active == entryId) set->active = QUuid{};
        set->platforms.removeIf([&](QMap<QString, SoundSourcePlatformOverride>::iterator it) { return it->active == entryId; });
    }
}

void moveToLanguage(SoundSources& sources, const QSet<QUuid>& entries, const QString& language)
{
    auto& target = sources.localized[language];
    for (auto it = sources.localized.begin(); it != sources.localized.end();) {
        if (it.key() == language) { ++it; continue; }
        auto& origin = it.value();
        bool moved = false;
        for (const auto& entry : origin.entries) {
            if (!entries.contains(entry.entryId)) continue;
            moved = true;
            target.entries.push_back(entry);
            if (origin.active == entry.entryId) {
                if (target.active.isNull()) target.active = entry.entryId;
                origin.active = QUuid{};
            }
            for (auto p = origin.platforms.begin(); p != origin.platforms.end();) {
                if (p->active != entry.entryId) { ++p; continue; }
                if (!target.platforms.contains(p.key()))
                    target.platforms.insert(p.key(), p.value());
                p = origin.platforms.erase(p);
            }
        }
        std::erase_if(origin.entries, [&](const auto& entry) { return entries.contains(entry.entryId); });
        if (moved && origin.entries.empty()) it = sources.localized.erase(it);
        else ++it;
    }
}

}
