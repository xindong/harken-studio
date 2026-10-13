
#include "IntegrityScanner.h"
#include "SoundSourceSemantics.h"

#include "AssetPipeline/AssetTypes.h"
#include "AssetPipeline/PathUtils.h"
#include "BusRouteGraph.h"
#include "ProjectTypes.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VBus.h"
#include "Components/VAction.h"
#include "Components/VSound.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace VNX {

namespace {

QUuid busSyntheticGuid(uint32_t busId)
{
    QByteArray seed = QByteArray::number(busId);
    QByteArray hash = QCryptographicHash::hash(seed, QCryptographicHash::Md5);
    return QUuid::fromRfc4122(hash);
}

struct LookupTables
{
    std::unordered_set<uint32_t>                allObjectIDs;
    std::unordered_set<uint32_t>                busIDs;
    std::unordered_set<uint32_t>                sourceIDs;
    std::vector<const ProjectItem*>             buses;
};

void collectIdsRecursive(const ProjectItem* item, LookupTables& tbl)
{
    if (!item) return;
    if (item->objectID != 0) {
        tbl.allObjectIDs.insert(item->objectID);
        if (item->comp<VBus>()) {
            tbl.busIDs.insert(item->objectID);
            tbl.buses.push_back(item);
        }
    }
    for (const auto& child : item->children) {
        collectIdsRecursive(child.get(), tbl);
    }
}

LookupTables buildLookups(const ProjectData& project, const QList<AssetInfo>& assets)
{
    LookupTables tbl;
    for (const auto* binding : project.allFileBindings()) {
        if (binding && binding->rootItem) collectIdsRecursive(binding->rootItem.get(), tbl);
    }
    for (const auto& asset : assets)
        tbl.sourceIDs.insert(asset.sourceID);
    return tbl;
}

void scanItemRecursive(const ProjectItem* item,
                       const LookupTables& tbl,
                       std::vector<IntegrityScanner::Issue>& out)
{
    if (!item) return;

    if (const auto* mix = item->comp<VMixNode>();
        mix && item->type != ProjectItem::Type::Bus) {
        if (mix->outputBusId != 0 &&
            tbl.busIDs.find(mix->outputBusId) == tbl.busIDs.end())
        {
            IntegrityScanner::Issue iss;
            iss.kind          = IntegrityScanner::RefKind::OutputBus;
            iss.sourceGuid    = item->guid;
            iss.sourceName    = item->name;
            iss.detail        = QStringLiteral("outputBusId");
            iss.missingTarget = QString::number(mix->outputBusId);
            out.push_back(std::move(iss));
        }
    }

    if (item->type == ProjectItem::Type::Sound)
    {
        const auto* sound = item->comp<VSound>();
        if (sound) {
            for (const auto* set : SoundSourceSemantics::sourceSets(sound->sources))
            for (const SoundSourceDef& source : set->entries) {
                if (tbl.sourceIDs.contains(source.sourceId)) continue;
                IntegrityScanner::Issue iss;
                iss.kind = IntegrityScanner::RefKind::ItemSourceID;
                iss.sourceGuid = item->guid;
                iss.sourceName = item->name;
                iss.detail = source.entryId.toString(QUuid::WithoutBraces);
                iss.missingTarget = QString::number(source.sourceId);
                out.push_back(std::move(iss));
            }
        }
    }

    if (const auto* ev = item->comp<VAction>()) {
        for (const auto& action : ev->steps) {
            if (action.targetObjectID == 0) continue;
            if (tbl.allObjectIDs.find(action.targetObjectID)
                == tbl.allObjectIDs.end())
            {
                IntegrityScanner::Issue iss;
                iss.kind          = IntegrityScanner::RefKind::ActionStep;
                iss.sourceGuid    = item->guid;
                iss.sourceName    = item->name;
                iss.detail        = action.typeString();
                iss.missingTarget = QString::number(action.targetObjectID);
                out.push_back(std::move(iss));
            }
        }
    }


    for (const auto& child : item->children) {
        scanItemRecursive(child.get(), tbl, out);
    }
}

void scanBuses(const LookupTables& tbl,
               std::vector<IntegrityScanner::Issue>& out)
{
    for (const auto* item : tbl.buses) {
        const auto* mix = item->comp<VMixNode>();
        if (!mix) continue;

        if (item->parent &&
            tbl.busIDs.find(item->parent->objectID) == tbl.busIDs.end())
        {
            IntegrityScanner::Issue iss;
            iss.kind          = IntegrityScanner::RefKind::BusParent;
            iss.sourceBusId   = item->objectID;
            iss.sourceGuid    = busSyntheticGuid(item->objectID);
            iss.sourceName    = item->name;
            iss.detail        = QStringLiteral("parent");
            iss.missingTarget = QString::number(item->parent->objectID);
            out.push_back(std::move(iss));
        }

        for (const auto& send : mix->sends) {
            if (send.targetBusID == 0) continue;
            if (tbl.busIDs.find(send.targetBusID) == tbl.busIDs.end()) {
                IntegrityScanner::Issue iss;
                iss.kind          = IntegrityScanner::RefKind::BusSend;
                iss.sourceBusId   = item->objectID;
                iss.sourceGuid    = busSyntheticGuid(item->objectID);
                iss.sourceName    = item->name;
                iss.detail        = QStringLiteral("sends.targetBusID");
                iss.missingTarget = QString::number(send.targetBusID);
                out.push_back(std::move(iss));
            }
        }

    }

    // Cycle detection on the merged flow graph (parent chain + send edges).
    // Dangling send targets are excluded here — they are reported above.
    BusRouteSnapshot g;
    QHash<uint32_t, QString> busNames;
    for (const auto* item : tbl.buses) {
        if (!item || item->objectID == 0) continue;
        const auto* mix = item->comp<VMixNode>();
        if (!mix) continue;
        busNames.insert(item->objectID, item->name);
        const bool hasBusParent =
            item->parent && tbl.busIDs.find(item->parent->objectID) != tbl.busIDs.end();
        g.parentOf.insert(item->objectID, hasBusParent ? item->parent->objectID : 0u);
        QVector<uint32_t> targets;
        for (const auto& send : mix->sends) {
            if (send.targetBusID != 0 && tbl.busIDs.find(send.targetBusID) != tbl.busIDs.end())
                targets.push_back(send.targetBusID);
        }
        if (!targets.isEmpty()) g.sendTargets.insert(item->objectID, targets);
    }

    const QVector<uint32_t> cycleNodes = findBusRouteCycle(g);
    if (!cycleNodes.isEmpty()) {
        // Render one readable chain: follow edges inside the cycle set until
        // the start repeats (A -> B -> C -> A).
        QSet<uint32_t> inCycle(cycleNodes.begin(), cycleNodes.end());
        const uint32_t start = cycleNodes.first();
        QStringList chain;
        uint32_t cur = start;
        do {
            chain << busNames.value(cur, QString::number(cur));
            uint32_t next = 0;
            if (inCycle.contains(g.parentOf.value(cur, 0u)))
                next = g.parentOf.value(cur, 0u);
            if (next == 0) {
                for (uint32_t t : g.sendTargets.value(cur)) {
                    if (inCycle.contains(t)) { next = t; break; }
                }
            }
            cur = next;
        } while (cur != 0 && cur != start && chain.size() <= cycleNodes.size());
        chain << busNames.value(start, QString::number(start));

        IntegrityScanner::Issue iss;
        iss.kind          = IntegrityScanner::RefKind::BusRouteCycle;
        iss.sourceBusId   = start;
        iss.sourceGuid    = busSyntheticGuid(start);
        iss.sourceName    = busNames.value(start);
        iss.detail        = chain.join(QStringLiteral(" -> "));
        iss.missingTarget = QString::number(start);
        out.push_back(std::move(iss));
    }
}

QJsonObject issueToJson(const IntegrityScanner::Issue& iss)
{
    QJsonObject o;
    o["kind"]          = static_cast<int>(iss.kind);
    o["sourceGuid"]    = iss.sourceGuid.toString(QUuid::WithoutBraces);
    o["sourceBusId"]   = static_cast<qint64>(iss.sourceBusId);
    o["sourceName"]    = iss.sourceName;
    o["detail"]        = iss.detail;
    o["missingTarget"] = iss.missingTarget;
    return o;
}

bool issueFromJson(const QJsonObject& o, IntegrityScanner::Issue& iss)
{
    const int kind = o.value("kind").toInt(-1);
    if (kind < 0 || kind > static_cast<int>(IntegrityScanner::RefKind::BusRouteCycle)
        || !o.value("missingTarget").isString())
        return false;
    iss.kind          = static_cast<IntegrityScanner::RefKind>(kind);
    iss.sourceGuid    = QUuid::fromString(o["sourceGuid"].toString());
    iss.sourceBusId   = static_cast<uint32_t>(o["sourceBusId"].toInteger(0));
    iss.sourceName    = o["sourceName"].toString();
    iss.detail        = o["detail"].toString();
    iss.missingTarget = o["missingTarget"].toString();
    return true;
}

}

IntegrityScanner::Report
IntegrityScanner::scan(const ProjectData& project, const QList<AssetInfo>& assets) const
{
    Report report;
    report.version = kIntegrityFormatVersion;

    const LookupTables tbl = buildLookups(project, assets);

    for (const auto* binding : project.allFileBindings()) {
        if (binding && binding->rootItem) {
            scanItemRecursive(binding->rootItem.get(), tbl, report.issues);
        }
    }
    scanBuses(tbl, report.issues);

    report.scannedAtMs = QDateTime::currentMSecsSinceEpoch();
    return report;
}

bool IntegrityScanner::loadFromCache(const QString& projectRoot, Report& outReport) const
{
    const QString path = QDir(projectRoot).filePath(
        QStringLiteral(".cache/project_index/integrity.json"));
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return false;

    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (doc.isNull() || !doc.isObject()) return false;

    const QJsonObject root = doc.object();
    const int version = root.value("version").toInt(0);
    if (version != kIntegrityFormatVersion) return false;
    if (!root.value("scannedAtMs").isDouble()
        || !root.value("indexHash").isString()
        || !root.value("issues").isArray())
        return false;

    Report r;
    r.version     = version;
    r.scannedAtMs = root.value("scannedAtMs").toVariant().toLongLong();
    r.indexHash   = root.value("indexHash").toString().toUtf8();
    const QJsonArray arr = root.value("issues").toArray();
    r.issues.reserve(static_cast<size_t>(arr.size()));
    for (const auto& v : arr) {
        Issue iss;
        if (!v.isObject() || !issueFromJson(v.toObject(), iss))
            return false;
        r.issues.push_back(std::move(iss));
    }
    outReport = std::move(r);
    return true;
}

bool IntegrityScanner::saveToCache(const QString& projectRoot, const Report& report) const
{
    const QString path = QDir(projectRoot).filePath(
        QStringLiteral(".cache/project_index/integrity.json"));
    const QFileInfo fi(path);
    if (!QDir().mkpath(fi.absolutePath())) return false;

    QJsonArray issues;
    for (const auto& iss : report.issues) {
        issues.append(issueToJson(iss));
    }
    QJsonObject root;
    root["version"]     = report.version;
    root["scannedAtMs"] = static_cast<qint64>(report.scannedAtMs);
    root["indexHash"]   = QString::fromUtf8(report.indexHash);
    root["issues"]      = issues;

    if (report.version != kIntegrityFormatVersion)
        return false;

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (out.write(bytes) != bytes.size()) {
        out.cancelWriting();
        return false;
    }
    return out.commit();
}

QString IntegrityScanner::summarize(const Report& report)
{
    if (report.issues.empty()) {
        return QStringLiteral("Integrity scan: OK (0 broken references)");
    }
    return QStringLiteral("Integrity scan: %1 broken reference(s) — see .cache/project_index/integrity.json")
        .arg(report.issues.size());
}

}
