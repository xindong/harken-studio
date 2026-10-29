
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QPointF>
#include <QHash>
#include <memory>
#include <vector>
#include <unordered_map>

#include "ProjectTypes.h"
#include "ProjectUpgradeManager.h"
#include "RuntimeBridge/PackagingBridge.h"

class QSaveFile;

namespace VNX {

class VBus;

class ProjectSerializer : public QObject
{
    Q_OBJECT

public:
    enum class LoadMode { RecoverTransactions, ReadOnly };

    explicit ProjectSerializer(QObject* parent = nullptr);
    ~ProjectSerializer() override;

    static const QString fileExtension() { return ".hsp"; }
    static const QString legacyPackExtension() { return HarkenEditor::Bridge::packArtifactExtension(); }
    static const QString packExtension() { return ".vp"; }

    bool saveProject(const QString& filePath, ProjectData& project);
    bool saveProject(const QString& filePath, const ProjectData& project);
    bool prepareReferenceWrite(const ProjectData& project);
    bool prepareProjectSave(const QString& filePath, ProjectData& project,
                            const QString& stagingFilePath = {});
    bool commitProjectSave(QJsonObject* savedHeader = nullptr);
    void cancelProjectSave();
    bool loadProject(const QString& filePath, ProjectData& project,
                     bool allowUpgrade = false,
                     ProjectUpgradeProgressCallback upgradeProgress = {},
                     ProjectUpgradeApprovalCallback upgradeApproval = {},
                     ProjectContractReadContext* context = nullptr,
                     LoadMode mode = LoadMode::RecoverTransactions);

    const ProjectUpgradeResult& upgradeResult() const { return upgradeResult_; }


    QJsonObject projectItemToJson(const ProjectItem& item, bool includeChildren = true) const;
    bool projectItemFromJson(const QJsonObject& json, ProjectItem& item) const;
    bool prepareStandaloneReferenceRead(const QJsonArray& itemRoots);
    uint32_t transientObjectIdForGuid(const QUuid& guid) const;
    harkenstudio::ObjectId transientRuntimeStableIdForGuid(const QUuid& guid) const;

    QJsonObject audioItemToJson(const ProjectItem& layer) const;
    bool        audioItemFromJson(const QJsonObject& json, ProjectItem& layer) const;
    bool        audioPackItemFromJson(const QJsonObject& json, ProjectItem& layer) const;
    QJsonObject audioBranchToJson(const ProjectItem& node) const;
    bool        audioBranchFromJson(const QJsonObject& json,
                                    ProjectItem& node) const;

    QJsonObject actionPackToJson(const ProjectItem& event) const;
    bool        actionPackFromJson(const QJsonObject& json, ProjectItem& event) const;

    QJsonObject attenuationPackToJson(const ProjectItem& item) const;
    bool        attenuationPackFromJson(const QJsonObject& json, ProjectItem& item) const;

    QJsonObject variablePackToJson(const ProjectItem& item) const;
    bool        variablePackFromJson(const QJsonObject& json, ProjectItem& item) const;

    QJsonObject busPackToJson(const ProjectItem& busItem) const;

    ProjectItem* busPackFromJson(const QJsonObject& json,
                                      ProjectItem& bucketParent,
                                      std::unordered_map<uint32_t, uint32_t>& outParentEdges) const;

    QString lastError() const { return lastError_; }
    QStringList warnings() const { return warnings_; }

signals:
    void progressChanged(int percent, const QString& message);
    void errorOccurred(const QString& error);
    void warningOccurred(const QString& warning);

private:
    struct CachedFieldGroups
    {
        DefinitionFieldGroups input;
        QJsonObject json;
    };

    bool writeFieldGroups(const ProjectItem& item, QJsonObject& json) const;

    struct ReferenceTarget
    {
        uint32_t objectId = 0;
        ProjectItem::Type type = ProjectItem::Type::None;
        QUuid guid;
        harkenstudio::ObjectId runtimeId() const { return runtimeObjectId(guid); }
        QUuid documentId;
        QString name;
    };

    bool prepareReferenceRead(const QString& projectFilePath);
    void resetReferenceReadState();
    bool registerReferenceReadTree(const QJsonObject& node,
                                   const QUuid& documentId,
                                   const QString& sourceLabel,
                                   bool documentPerObject = false);
    bool registerWriteItem(const ProjectItem& item, const QUuid& documentId);
    bool registerSdkName(const QString& name, ProjectItem::Type type, const QUuid& guid) const;
    bool registerReadGuid(const QUuid& guid, ProjectItem::Type type,
                          const QUuid& documentId, const QString& name) const;
    uint32_t transientObjectId(const QUuid& guid) const;
    QJsonObject persistentReference(uint32_t objectId,
                                    std::optional<ProjectItem::Type> expectedType = std::nullopt) const;
    QJsonObject persistentRuntimeReference(
        harkenstudio::ObjectId runtimeStableId, ProjectItem::Type expectedType) const;
    bool transientReference(const QJsonValue& value,
                            std::optional<ProjectItem::Type> expectedType,
                            uint32_t* objectId, QString* targetName = nullptr) const;
    bool transientRuntimeReference(const QJsonValue& value,
                                   ProjectItem::Type expectedType,
                                   harkenstudio::ObjectId* runtimeStableId) const;
    QJsonObject persistentParentReference(const ProjectItem& item) const;
    bool loadParentReference(const QJsonObject& json, ParentRef* ref) const;
    bool encodeComponentReferences(QJsonObject* components) const;
    bool decodeComponentReferences(QJsonObject* components) const;
    bool removeRetiredPlayableProperties(QJsonObject* properties) const;
    bool rewritePlayableReferenceFields(QJsonObject* properties, bool encode) const;
    bool encodePlayableReferences(QJsonObject* properties,
                                   bool allowFieldGroupFields = false) const;
    bool decodePlayableReferences(QJsonObject* properties,
                                   bool allowFieldGroupFields = false) const;
    bool encodeActionReferences(QJsonArray* actions) const;
    bool decodeActionReferences(QJsonArray* actions) const;

    mutable std::optional<CachedFieldGroups> cachedFieldGroups_;
    mutable QJsonObject validatedOccurrenceProperties_;
    mutable QString lastError_;
    mutable QStringList warnings_;
    std::unique_ptr<QSaveFile> pendingProjectSave_;
    QString pendingSaveFilePath_;
    QByteArray pendingSaveBytes_;
    QJsonObject pendingSaveHeader_;
    ProjectUpgradeResult upgradeResult_;
    mutable QHash<uint32_t, ReferenceTarget> referencesByObjectId_;
    mutable QHash<harkenstudio::ObjectId, ReferenceTarget> referencesByRuntimeStableId_;
    mutable QHash<QUuid, ReferenceTarget> referencesByGuid_;
    mutable QHash<QString, QUuid> sdkNames_;
    mutable QHash<QUuid, ParentRef> parentOverrides_;
    mutable uint32_t nextTransientObjectId_ = 1;
    mutable bool strictReferenceRead_ = false;
};

}
