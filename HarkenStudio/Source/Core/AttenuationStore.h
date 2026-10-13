#pragma once

#include "Components/VAttenuation.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <cstdint>
#include <vector>

namespace VNX {

class ProjectManager;
struct ActionResult;
struct PlayablePropertyStateV1;

struct AttenuationReference
{
    ProjectItem* item = nullptr;
    QString binding;
    QString attenuationName;
    QUuid attenuationGuid;
};

class AttenuationStore
{
public:
    static QString guidString(const QUuid& guid);
    static void ensureGuid(AttenuationSnapshot& def);

    static VAttenuation* find(ProjectData* project, const QString& name);
    static const VAttenuation* find(const ProjectData* project, const QString& name);
    static VAttenuation* findByGuid(ProjectData* project, const QUuid& guid);
    static const VAttenuation* findByGuid(const ProjectData* project, const QUuid& guid);
    static ProjectItem* findItemByStableId(ProjectData* project, harkenstudio::ObjectId stableId);
    static const ProjectItem* findItemByStableId(const ProjectData* project, harkenstudio::ObjectId stableId);
    static VAttenuation* findByStableId(ProjectData* project, harkenstudio::ObjectId stableId);
    static const VAttenuation* findByStableId(const ProjectData* project, harkenstudio::ObjectId stableId);
    static VAttenuation* find(ProjectData* project, const QUuid& guid, const QString& name);
    static const VAttenuation* find(const ProjectData* project, const QUuid& guid, const QString& name);
    static bool exists(const ProjectData* project, const QString& name);
    static std::vector<const VAttenuation*> all(const ProjectData& project);
    static harkenstudio::ObjectId runtimeId(const ProjectData& project, const VAttenuation& attenuation);
    static bool syncToRuntime(const ProjectData& project, const VAttenuation& attenuation);
    static bool syncToRuntime(const ProjectData& project, const AttenuationSnapshot& attenuation);

    static VAttenuation* create(ProjectData* project, const AttenuationSnapshot& input,
                                          ProjectManager* projectManager = nullptr);
    static bool erase(ProjectData* project, const QUuid& guid, const QString& name,
                      ProjectManager* projectManager = nullptr);
    static bool eraseByName(ProjectData* project, const QString& name,
                            ProjectManager* projectManager = nullptr);
    static VAttenuation* upsertJson(ProjectData* project, const QString& name, const QJsonObject& json,
                                              ProjectManager* projectManager = nullptr);

    static std::vector<AttenuationReference> references(ProjectData* project,
        const ProjectItem& attenuation, QString* error = nullptr);
    static QJsonArray referencesJson(ProjectData* project,
        const ProjectItem& attenuation, QString* error = nullptr);
    static int referenceCount(ProjectData* project,
        const ProjectItem& attenuation, QString* error = nullptr);

    static bool rename(ProjectData* project,
                       const QString& oldName,
                       const QString& newName,
                       ProjectManager* projectManager);
    static void notifyReferencesChanged(ProjectData* project,
                                        const QUuid& guid,
                                        ProjectManager* projectManager,
                                        const QStringList& propertyNames,
                                        const QObject* originator = nullptr);
    static bool applyBinding(ProjectManager* projectManager,
                             uint32_t objectID,
                             const PlayablePropertyStateV1& state);

    static QJsonObject summaryJson(const VAttenuation& attenuation);
    static ActionResult edit(ProjectManager* projectManager, VAttenuation& attenuation,
                             const QJsonObject& fields, const QString& undoText,
                             const QObject* originator = nullptr);
};

}
