
#include "../ActionManager.h"
#include "../AttenuationStore.h"
#include "../AudioEngine.h"
#include "../NameValidator.h"
#include "../PlayablePropertyAuthoringV1.h"
#include "../ProjectManager.h"
#include "ActionSelectors.h"
#include "../Components/VSpatial.h"
#include <QFile>
#include <QJsonArray>
#include <algorithm>
#include <vector>

namespace VNX { ProjectItem* findItemByPathInProject(ProjectData* project, const QString& path); }
namespace VNX { ProjectItem* findItemByObjectIDInProject(ProjectData* project, uint32_t objectID); }
namespace VNX { uint32_t itemShortID(const ProjectItem* item); }

namespace VNX {


static ActionResult engineNotAvailable()
{
    return {false, {}, "ENGINE_NOT_AVAILABLE", "AudioEngine not available"};
}

static ProjectData* currentProjectFrom(ProjectManager* projectManager)
{
    if (projectManager && projectManager->isProjectOpen())
        return projectManager->currentProject();
    return ProjectManager::instance().currentProject();
}

static QString firstStringArg(const QJsonObject& args, std::initializer_list<const char*> keys)
{
    for (const char* key : keys)
    {
        const QString qkey = QString::fromLatin1(key);
        if (args.contains(qkey))
        {
            const QString value = args.value(qkey).toString();
            if (!value.trimmed().isEmpty())
                return value;
        }
    }
    return {};
}

static QString attenuationGuidString(const QUuid& guid)
{
    return AttenuationStore::guidString(guid);
}

static void ensureAttenuationGuid(AttenuationSnapshot& def)
{
    AttenuationStore::ensureGuid(def);
}

static QUuid firstGuidArg(const QJsonObject& args, std::initializer_list<const char*> keys)
{
    for (const char* key : keys)
    {
        const QString qkey = QString::fromLatin1(key);
        if (args.contains(qkey))
        {
            const QUuid value = QUuid::fromString(args.value(qkey).toString().trimmed());
            if (!value.isNull())
                return value;
        }
    }
    return {};
}

static VAttenuation* findAttenuationByGuid(ProjectData* project, const QUuid& guid)
{
    return AttenuationStore::findByGuid(project, guid);
}

static void eraseAttenuationByIdentity(ProjectData* project, const QUuid& guid, const QString& name,
                                               ProjectManager* projectManager = nullptr)
{
    AttenuationStore::erase(project, guid, name, projectManager);
}

static void upsertAttenuation(ProjectData* project, const QString& name,
                                      const QJsonObject& json,
                                      ProjectManager* projectManager = nullptr)
{
    AttenuationStore::upsertJson(project, name, json, projectManager);
}

static QJsonArray attenuationReferences(
    ProjectData* project, const VAttenuation& attenuation, QString* error)
{
    return AttenuationStore::referencesJson(project, *attenuation.hostItem(), error);
}

static void applyAttenuationDistanceScaling(ProjectManager* projectManager,
                                            uint32_t objectID, float scaling)
{
    auto* project = currentProjectFrom(projectManager);
    if (!project) return;
    auto* item = findItemByObjectIDInProject(project, objectID);
    if (!item) return;
    auto* spatialComp = item->comp<VSpatial>();
    if (!spatialComp) return;
    spatialComp->attenuationDistanceScaling = scaling;
    item->markDirty();
    ProjectManager::resyncItemToRuntime(item);
    if (projectManager)
        emit projectManager->itemPropertyChanged(item, QStringLiteral("attenuationDistanceScaling"));
}

static QJsonObject attenuationSummary(const VAttenuation& def)
{
    return AttenuationStore::summaryJson(def);
}

static QVector<CurvePointDef> parseCurvePoints(const QJsonArray& arr,
                                                          QString* error)
{
    QVector<CurvePointDef> points;
    points.reserve(arr.size());

    for (int i = 0; i < arr.size(); ++i)
    {
        const QJsonValue v = arr.at(i);
        CurvePointDef p;
        if (v.isObject())
        {
            const QJsonObject obj = v.toObject();
            if (!obj.contains("distance") || !obj.contains("value"))
            {
                if (error) *error = QString("points[%1] requires distance and value").arg(i);
                return {};
            }
            p.x = static_cast<float>(obj.value("distance").toDouble());
            p.value = static_cast<float>(obj.value("value").toDouble());
        }
        else if (v.isArray())
        {
            const QJsonArray pair = v.toArray();
            if (pair.size() < 2)
            {
                if (error) *error = QString("points[%1] array requires [distance, value]").arg(i);
                return {};
            }
            p.x = static_cast<float>(pair.at(0).toDouble());
            p.value = static_cast<float>(pair.at(1).toDouble());
        }
        else
        {
            if (error) *error = QString("points[%1] must be an object or [distance, value]").arg(i);
            return {};
        }

        if (p.x < 0.0f)
        {
            if (error) *error = QString("points[%1].distance must be >= 0").arg(i);
            return {};
        }
        points.push_back(p);
    }

    return points;
}

static QVector<CurveHandlePairDef> parseCurveHandles(const QJsonValue& value)
{
    QVector<CurveHandlePairDef> handles;
    for (const QJsonValue& v : value.toArray())
    {
        const QJsonArray pair = v.toArray();
        if (pair.size() < 4)
            return {};
        handles.push_back({static_cast<float>(pair.at(0).toDouble()),
                           static_cast<float>(pair.at(1).toDouble()),
                           static_cast<float>(pair.at(2).toDouble()),
                           static_cast<float>(pair.at(3).toDouble())});
    }
    return handles;
}

void registerSpatialActions(ActionManager& mgr,
                            AudioEngine* engine,
                            ProjectManager* projectManager)
{


    {
        ActionDef def;
        def.id = "spatial_set_image_source";
        def.displayName = "Set Image Source";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"source_image_id", "uint32"}, {"source_id", "uint32"}, {"pos_x", "float"}, {"pos_y", "float"}, {"pos_z", "float"}, {"distance_scale", "float"}, {"level", "float"}, {"diffraction", "float"}, {"occlusion", "float"}, {"texture_id", "uint32"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint32_t srcID = static_cast<uint32_t>(args["source_image_id"].toInt(0));
            engine->setImageSource(srcID,
                static_cast<uint32_t>(args["source_id"].toInt(0)),
                static_cast<float>(args["pos_x"].toDouble(0)), static_cast<float>(args["pos_y"].toDouble(0)), static_cast<float>(args["pos_z"].toDouble(0)),
                static_cast<float>(args["distance_scale"].toDouble(1.0)),
                static_cast<float>(args["level"].toDouble(1.0)),
                static_cast<float>(args["diffraction"].toDouble(0.0)),
                static_cast<float>(args["occlusion"].toDouble(0.0)),
                static_cast<uint32_t>(args["texture_id"].toInt(0)));
            return {true, QJsonObject{{"source_image_id", static_cast<int>(srcID)}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "spatial_remove_image_source";
        def.displayName = "Remove Image Source";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"source_image_id", "uint32"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            engine->removeImageSource(static_cast<uint32_t>(args["source_image_id"].toInt(0)));
            return {true, QJsonObject{{"removed", true}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "spatial_clear_image_sources";
        def.displayName = "Clear Image Sources";
        def.category = "spatial";
        def.undoable = false;
        def.handler = [engine](const QJsonObject&) -> ActionResult {
            if (!engine) return engineNotAvailable();
            engine->clearImageSources();
            return {true, QJsonObject{{"cleared", true}}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "spatial_set_diffraction_order";
        def.displayName = "Set Diffraction Order";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"order", "uint32"}, {"update_paths", "bool"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint32_t order = static_cast<uint32_t>(args["order"].toInt(4));
            bool updatePaths = args["update_paths"].toBool(true);
            engine->setDiffractionOrder(order, updatePaths);
            return {true, QJsonObject{{"order", static_cast<int>(order)}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "spatial_set_transmission_operation";
        def.displayName = "Set Transmission Operation";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"operation", "int"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint8_t op = static_cast<uint8_t>(args["operation"].toInt(0));
            engine->setTransmissionOperation(op);
            return {true, QJsonObject{{"operation", op}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "spatial_set_load_balancing";
        def.displayName = "Set Load Balancing";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"num_frames", "uint32"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint32_t spread = static_cast<uint32_t>(args["num_frames"].toInt(1));
            engine->setLoadBalancingSpread(spread);
            return {true, QJsonObject{{"num_frames", static_cast<int>(spread)}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "spatial_set_smoothing";
        def.displayName = "Set Smoothing";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"smoothing_ms", "float"}, {"game_object_id", "uint64"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            float ms = static_cast<float>(args["smoothing_ms"].toDouble(100.0));
            uint64_t goID = static_cast<uint64_t>(args["game_object_id"].toInteger(0));
            engine->setSmoothingConstant(ms, goID);
            return {true, QJsonObject{{"smoothing_ms", ms}}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "spatial_init";
        def.displayName = "Initialize Spatial Audio";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"max_propagation_depth", "uint32"}, {"num_rays", "uint32"}, {"max_reflection_order", "uint32"}, {"max_diffraction_order", "uint32"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint32_t maxPropDepth = static_cast<uint32_t>(args["max_propagation_depth"].toInt(4));
            uint32_t numRays = static_cast<uint32_t>(args["num_rays"].toInt(100));
            uint32_t maxReflOrder = static_cast<uint32_t>(args["max_reflection_order"].toInt(2));
            uint32_t maxDiffOrder = static_cast<uint32_t>(args["max_diffraction_order"].toInt(4));
            bool ok = engine->initSpatialAudio(maxPropDepth, numRays, maxReflOrder, maxDiffOrder);
            if (!ok) return {false, {}, "INIT_FAILED", "Failed to initialize spatial audio"};
            return {true, QJsonObject{{"initialized", true}}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "spatial_load_hrtf";
        def.displayName = "Load HRTF";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"path", "string"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            QString path = args["path"].toString();
            if (path.isEmpty())
                return {false, {}, "MISSING_PARAM", "path is required (path to .vhrtf file)"};
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return {false, {}, "FILE_NOT_FOUND",
                    QString("Cannot open HRTF file: %1").arg(path)};
            QByteArray data = file.readAll();
            if (data.isEmpty())
                return {false, {}, "FILE_EMPTY", "HRTF file is empty"};
            bool ok = engine->loadHRTFDatabase(
                reinterpret_cast<const uint8_t*>(data.constData()),
                static_cast<uint32_t>(data.size()));
            if (!ok)
                return {false, {}, "INVALID_FORMAT",
                    "Failed to parse .vhrtf file (check magic/version/data)"};
            return {true, QJsonObject{{"loaded", true}, {"size_bytes", data.size()}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "spatial_unload_hrtf";
        def.displayName = "Unload HRTF";
        def.category = "spatial";
        def.undoable = false;
        def.handler = [engine](const QJsonObject&) -> ActionResult {
            if (!engine) return engineNotAvailable();
            engine->unloadHRTFDatabase();
            return {true, QJsonObject{{"unloaded", true}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "spatial_set_hrtf_enabled";
        def.displayName = "Set HRTF Enabled";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"enabled", "bool"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            bool enabled = args["enabled"].toBool(true);
            engine->setHRTFEnabled(enabled);
            return {true, QJsonObject{{"enabled", enabled}}};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "spatial_set_game_object_radius";
        def.displayName = "Set Game Object Radius";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"game_object_id", "uint64"}, {"outer_radius", "float"}, {"inner_radius", "float"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint64_t goID = static_cast<uint64_t>(args["game_object_id"].toInteger(0));
            float outer = static_cast<float>(args["outer_radius"].toDouble(1.0));
            float inner = static_cast<float>(args["inner_radius"].toDouble(0.0));
            engine->setGameObjectRadius(goID, outer, inner);
            return {true, QJsonObject{{"game_object_id", static_cast<qint64>(goID)}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_game_object_position";
        def.displayName = "Set Game Object Position";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"game_object_id", "uint64"}, {"x", "float"}, {"y", "float"}, {"z", "float"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint64_t goID = static_cast<uint64_t>(args["game_object_id"].toInteger(0));
            if (goID == 0) return {false, {}, "INVALID_PARAM", "game_object_id required"};
            float x = static_cast<float>(args["x"].toDouble(0.0));
            float y = static_cast<float>(args["y"].toDouble(0.0));
            float z = static_cast<float>(args["z"].toDouble(0.0));
            engine->setGameObjectPosition(goID, x, y, z);
            return {true, QJsonObject{
                {"game_object_id", static_cast<qint64>(goID)},
                {"x", x}, {"y", y}, {"z", z}
            }};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_game_object_orientation";
        def.displayName = "Set Game Object Orientation";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"game_object_id", "uint64"}, {"forward_x", "float"}, {"forward_y", "float"}, {"forward_z", "float"}};
        def.undoable = false;
        def.handler = [engine](const QJsonObject& args) -> ActionResult {
            if (!engine) return engineNotAvailable();
            uint64_t goID = static_cast<uint64_t>(args["game_object_id"].toInteger(0));
            if (goID == 0) return {false, {}, "INVALID_PARAM", "game_object_id required"};
            float fwdX = static_cast<float>(args["forward_x"].toDouble(0.0));
            float fwdY = static_cast<float>(args["forward_y"].toDouble(0.0));
            float fwdZ = static_cast<float>(args["forward_z"].toDouble(-1.0));
            engine->setGameObjectOrientation(goID, fwdX, fwdY, fwdZ);
            return {true, QJsonObject{
                {"game_object_id", static_cast<qint64>(goID)},
                {"forward_x", fwdX}, {"forward_y", fwdY}, {"forward_z", fwdZ}
            }};
        };
        mgr.registerAction(def);
    }


    {
        ActionDef def;
        def.id = "create_attenuation";
        def.displayName = "Create Attenuation";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"name", "string"}, {"max_distance", "float"}, {"definition", "object"}, {"guid", "string"}};
        def.undoable = true;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            auto* project = currentProjectFrom(projectManager);
            if (!project) return {false, {}, "NO_PROJECT", "No project open"};

            const QString name = firstStringArg(args, {"name", "attenuation"});
            if (name.isEmpty())
                return {false, {}, "INVALID_PARAM", "name required"};
            NameValidator::Result nameCheck;
            if (!NameValidator::validateItemName(name, ProjectItem::Type::Attenuation, &nameCheck))
                return {false, {}, "INVALID_NAME", NameValidator::resultMessage(nameCheck)};
            AttenuationSnapshot attenuation;
            const bool hasExplicitDefinition = args.value("definition").isObject();
            if (hasExplicitDefinition)
                attenuation = AttenuationSnapshot::fromJson(args.value("definition").toObject());
            const QUuid explicitGuid = firstGuidArg(args, {"guid", "attenuation_guid"});
            if (!explicitGuid.isNull())
                attenuation.guid = explicitGuid;
            ensureAttenuationGuid(attenuation);
            if (findAttenuationByGuid(project, attenuation.guid))
                return {false, {}, "ALREADY_EXISTS", "Attenuation guid already exists: " + attenuationGuidString(attenuation.guid)};
            attenuation.name = name;
            if (args.contains("max_distance"))
                attenuation.maxDistance = std::max(0.0f, static_cast<float>(args.value("max_distance").toDouble(attenuation.maxDistance)));
            if (args.contains("maxDistance"))
                attenuation.maxDistance = std::max(0.0f, static_cast<float>(args.value("maxDistance").toDouble(attenuation.maxDistance)));

            if (!hasExplicitDefinition)
            {
                auto& dry = attenuation.ensureDimension(AttenuationDimension::Volume);
                dry.curve = attenuationDefaultCurve(dry.type, attenuation.maxDistance);
                attenuationNormalizeCurve(dry.type, dry.curve, dry.handles);
            }

            const auto* created = AttenuationStore::create(project, attenuation, projectManager);
            if (!created)
                return {false, {}, "INVALID_PARAM", "Invalid Attenuation definition"};

            const QJsonObject createdJson = created->snapshot().toJson();
            const QString createdName = created->hostItem()->name;
            ActionResult r;
            r.success = true;
            r.data = QJsonObject{{"name", createdName}, {"guid", attenuationGuidString(attenuation.guid)}, {"definition", createdJson}};
            r.undoText = QStringLiteral("Create Attenuation");
            r.undoFn = [projectManager, name = createdName, guid = attenuation.guid]() {
                auto* proj = currentProjectFrom(projectManager);
                eraseAttenuationByIdentity(proj, guid, name, projectManager);
            };
            r.redoFn = [projectManager, name = createdName, createdJson]() {
                auto* proj = currentProjectFrom(projectManager);
                upsertAttenuation(proj, name, createdJson, projectManager);
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "rename_attenuation";
        def.displayName = "Rename Attenuation";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"old_name", "string"}, {"new_name", "string"}, {"guid", "string"}, {"path", "string"}};
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            auto* project = currentProjectFrom(projectManager);
            if (!project) return {false, {}, "NO_PROJECT", "No project open"};

            const QString oldName = firstStringArg(args, {"guid", "path", "old_name", "oldName", "from", "name", "attenuation"});
            const QString newName = firstStringArg(args, {"new_name", "newName", "to"});
            if (oldName.isEmpty())
                return {false, {}, "INVALID_PARAM", "old_name required"};
            if (newName.isEmpty())
                return {false, {}, "INVALID_PARAM", "new_name required"};
            const auto selected = resolveProjectSelector(project, oldName, ProjectItem::Type::Attenuation);
            if (!selected.ok)
                return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            const QString previousName = selected.item->name;
            if (previousName == newName)
                return {false, {}, "NO_CHANGE", "New Attenuation name matches the old name"};
            auto* attenuation = selected.item->comp<VAttenuation>();
            const QUuid guid = attenuation->hostItem()->guid;
            QString referenceError;
            const QJsonArray references = attenuationReferences(project, *attenuation, &referenceError);
            if (!referenceError.isEmpty())
                return {false, {}, "INVALID_PROJECT", referenceError};
            auto& pm = projectManager ? *projectManager : ProjectManager::instance();
            if (!pm.renameItem(attenuation->hostItem(), newName))
                return {false, {}, "RENAME_FAILED", "Could not rename Attenuation: " + oldName};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"old_name", previousName},
                {"new_name", attenuation->hostItem()->name},
                {"guid", attenuationGuidString(guid)},
                {"updated_reference_count", references.size()},
                {"references", references}
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "delete_attenuation";
        def.displayName = "Delete Attenuation";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"name", "string"}, {"guid", "string"}, {"path", "string"}};
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            auto* project = currentProjectFrom(projectManager);
            if (!project) return {false, {}, "NO_PROJECT", "No project open"};

            const QString name = firstStringArg(args, {"guid", "path", "name", "attenuation"});
            if (name.isEmpty())
                return {false, {}, "INVALID_PARAM", "name required"};
            const auto selected = resolveProjectSelector(project, name, ProjectItem::Type::Attenuation);
            if (!selected.ok)
                return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            auto* attenuation = selected.item->comp<VAttenuation>();

            QString referenceError;
            attenuationReferences(project, *attenuation, &referenceError);
            if (!referenceError.isEmpty())
                return {false, {}, "INVALID_PROJECT", referenceError};

            const QString resolvedName = attenuation->hostItem()->name;
            const QUuid resolvedGuid = attenuation->hostItem()->guid;
            const QJsonObject deletedJson = attenuation->snapshot().toJson();
            auto& manager = projectManager ? *projectManager : ProjectManager::instance();
            if (!manager.deleteItem(attenuation->hostItem()))
                return {false, {}, "DELETE_FAILED", "Could not delete Attenuation"};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{{"name", resolvedName}, {"guid", attenuationGuidString(resolvedGuid)}, {"definition", deletedJson}};
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "list_attenuations";
        def.displayName = "List Attenuations";
        def.category = "spatial";
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject&) -> ActionResult {
            auto* project = currentProjectFrom(projectManager);
            if (!project) return {false, {}, "NO_PROJECT", "No project open"};

            QJsonArray items;
            for (const auto* attenuation : AttenuationStore::all(*project))
                items.append(attenuationSummary(*attenuation));
            return {true, QJsonObject{{"count", items.size()}, {"attenuations", items}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "get_attenuation";
        def.displayName = "Get Attenuation";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"name", "string"}, {"guid", "string"}};
        def.undoable = false;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            auto* project = currentProjectFrom(projectManager);
            if (!project) return {false, {}, "NO_PROJECT", "No project open"};

            const QString name = firstStringArg(args, {"name", "attenuation"});
            const QUuid guid = firstGuidArg(args, {"guid", "attenuation_guid"});
            if (name.isEmpty() && guid.isNull())
                return {false, {}, "INVALID_PARAM", "name or guid required"};
            const auto selected = resolveProjectSelector(project,
                guid.isNull() ? name : attenuationGuidString(guid), ProjectItem::Type::Attenuation);
            if (!selected.ok)
                return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            auto* attenuation = selected.item->comp<VAttenuation>();
            return {true, QJsonObject{{"definition", attenuation->snapshot().toJson()}}};
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_attenuation_curve";
        def.displayName = "Set Attenuation Curve";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"name", "string"}, {"guid", "string"}, {"curve", "string"}, {"points", "array"}, {"handles", "array"}, {"create_if_missing", "bool"}};
        def.undoable = true;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            auto* project = currentProjectFrom(projectManager);
            if (!project) return {false, {}, "NO_PROJECT", "No project open"};

            const QString name = firstStringArg(args, {"name", "attenuation"});
            const QUuid guid = firstGuidArg(args, {"guid", "attenuation_guid"});
            if (name.isEmpty() && guid.isNull())
                return {false, {}, "INVALID_PARAM", "name or guid required"};
            const QString curveName = firstStringArg(args, {"curve", "curve_type", "type"});
            if (curveName.isEmpty())
                return {false, {}, "INVALID_PARAM", "curve required (volume)"};
            if (!args.value("points").isArray())
                return {false, {}, "INVALID_PARAM", "points array required"};

            const auto selected = resolveProjectSelector(project,
                guid.isNull() ? name : attenuationGuidString(guid), ProjectItem::Type::Attenuation);
            if (!selected.ok && selected.errorCode == QStringLiteral("AMBIGUOUS_SELECTOR"))
                return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            auto* attenuation = selected.item ? selected.item->comp<VAttenuation>() : nullptr;
            const bool existedBefore = (attenuation != nullptr);
            if (!attenuation)
            {
                if (!args.value("create_if_missing").toBool(false))
                    return {false, {}, "NOT_FOUND", "Attenuation not found: " + (name.isEmpty() ? attenuationGuidString(guid) : name)};
                if (name.isEmpty())
                    return {false, {}, "INVALID_PARAM", "name required when create_if_missing is true"};
            }

            AttenuationDimension dimensionType = AttenuationDimension::Volume;
            if (!attenuationDimensionFromName(curveName, &dimensionType))
                return {false, {}, "INVALID_PARAM", "Unknown curve: " + curveName};

            QString error;
            QVector<CurvePointDef> parsed = parseCurvePoints(args.value("points").toArray(), &error);
            if (!error.isEmpty())
                return {false, {}, "INVALID_PARAM", error};
            QVector<CurveHandlePairDef> parsedHandles =
                parseCurveHandles(args.value("handles"));
            attenuationNormalizeCurve(dimensionType, parsed, parsedHandles);

            auto candidate = attenuation ? attenuation->snapshot() : AttenuationSnapshot{};
            candidate.name = attenuation ? attenuation->hostItem()->name : name;
            candidate.guid = attenuation ? attenuation->hostItem()->guid : guid;
            auto& dimension = candidate.ensureDimension(dimensionType);
            dimension.curve = parsed;
            dimension.handles = parsedHandles;

            if (args.contains("max_distance"))
                candidate.maxDistance = std::max(0.0f, static_cast<float>(args.value("max_distance").toDouble(candidate.maxDistance)));
            if (args.contains("maxDistance"))
                candidate.maxDistance = std::max(0.0f, static_cast<float>(args.value("maxDistance").toDouble(candidate.maxDistance)));

            const QString undoText = args.value(QStringLiteral("undo_text")).toString().trimmed();
            const QString text = undoText.isEmpty()
                ? QStringLiteral("Set Attenuation Curve")
                : undoText;
            const auto data = candidate.toJson();
            if (!attenuation) {
                VAttenuation validated;
                if (!validated.setField("maxDistance", candidate.maxDistance)
                    || !validated.setDimensions(data.value("dimensions").toArray()))
                    return {false, {}, "INVALID_PARAM", "Invalid Attenuation fields"};
                attenuation = AttenuationStore::create(project, candidate, projectManager);
                if (!attenuation)
                    return {false, {}, "INVALID_PARAM", "Invalid Attenuation definition"};
            }
            auto* manager = projectManager ? projectManager : &ProjectManager::instance();
            ActionResult r = AttenuationStore::edit(manager, *attenuation,
                {{"dimensions", data.value("dimensions")}, {"maxDistance", candidate.maxDistance}},
                text, ActionManager::instance().executeOriginator());
            if (!r.success) return r;
            if (!existedBefore) {
                const auto created = attenuation->snapshot();
                r.undoText = text;
                r.undoFn = [projectManager, guid = created.guid, name = created.name] {
                    eraseAttenuationByIdentity(currentProjectFrom(projectManager), guid, name, projectManager);
                };
                r.redoFn = [projectManager, created] {
                    AttenuationStore::upsertJson(currentProjectFrom(projectManager), created.name,
                                                created.toJson(), projectManager);
                };
            }
            auto resultData = r.data;
            resultData.insert("name", attenuation->hostItem()->name);
            resultData.insert("curve", QLatin1String(attenuationDimensionName(dimensionType)));
            resultData.insert("point_count", parsed.size());
            r.data = resultData;
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "add_attenuation_dimension";
        def.displayName = "Add Attenuation Dimension";
        def.category = "spatial";
        def.undoable = true;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            auto* project = currentProjectFrom(projectManager);
            if (!project) return {false, {}, "NO_PROJECT", "No project open"};

            const QString name = firstStringArg(args, {"name", "attenuation"});
            const QUuid guid = firstGuidArg(args, {"guid", "attenuation_guid"});
            if (name.isEmpty() && guid.isNull())
                return {false, {}, "INVALID_PARAM", "name or guid required"};
            const QString dimensionName = firstStringArg(args, {"dimension", "curve", "type"});
            AttenuationDimension dimensionType = AttenuationDimension::Volume;
            if (dimensionName.isEmpty() || !attenuationDimensionFromName(dimensionName, &dimensionType))
                return {false, {}, "INVALID_PARAM", "dimension required (volume or pitch)"};

            const auto selected = resolveProjectSelector(project,
                guid.isNull() ? name : attenuationGuidString(guid), ProjectItem::Type::Attenuation);
            if (!selected.ok)
                return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            auto* attenuation = selected.item->comp<VAttenuation>();
            if (attenuation->dimension(dimensionType))
                return {false, {}, "ALREADY_EXISTS",
                        QString("Dimension already exists: %1").arg(QLatin1String(attenuationDimensionName(dimensionType)))};

            auto candidate = attenuation->snapshot();
            auto& dimension = candidate.ensureDimension(dimensionType);
            dimension.curve = attenuationDefaultCurve(dimensionType, attenuation->maxDistance);
            attenuationNormalizeCurve(dimensionType, dimension.curve, dimension.handles);
            auto* manager = projectManager ? projectManager : &ProjectManager::instance();
            ActionResult r = AttenuationStore::edit(manager, *attenuation,
                {{"dimensions", candidate.toJson().value("dimensions")}},
                QStringLiteral("Add Attenuation Dimension"), ActionManager::instance().executeOriginator());
            if (!r.success) return r;
            auto data = r.data;
            data.insert("name", attenuation->hostItem()->name);
            data.insert("dimension", QLatin1String(attenuationDimensionName(dimensionType)));
            r.data = data;
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "remove_attenuation_dimension";
        def.displayName = "Remove Attenuation Dimension";
        def.category = "spatial";
        def.undoable = true;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            auto* project = currentProjectFrom(projectManager);
            if (!project) return {false, {}, "NO_PROJECT", "No project open"};

            const QString name = firstStringArg(args, {"name", "attenuation"});
            const QUuid guid = firstGuidArg(args, {"guid", "attenuation_guid"});
            if (name.isEmpty() && guid.isNull())
                return {false, {}, "INVALID_PARAM", "name or guid required"};
            const QString dimensionName = firstStringArg(args, {"dimension", "curve", "type"});
            AttenuationDimension dimensionType = AttenuationDimension::Volume;
            if (dimensionName.isEmpty() || !attenuationDimensionFromName(dimensionName, &dimensionType))
                return {false, {}, "INVALID_PARAM", "dimension required (volume or pitch)"};

            const auto selected = resolveProjectSelector(project,
                guid.isNull() ? name : attenuationGuidString(guid), ProjectItem::Type::Attenuation);
            if (!selected.ok)
                return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            auto* attenuation = selected.item->comp<VAttenuation>();
            if (!attenuation->dimension(dimensionType))
                return {false, {}, "NOT_FOUND",
                        QString("Dimension not found: %1").arg(QLatin1String(attenuationDimensionName(dimensionType)))};

            auto candidate = attenuation->snapshot();
            candidate.removeDimension(dimensionType);
            auto* manager = projectManager ? projectManager : &ProjectManager::instance();
            ActionResult r = AttenuationStore::edit(manager, *attenuation,
                {{"dimensions", candidate.toJson().value("dimensions")}},
                QStringLiteral("Remove Attenuation Dimension"), ActionManager::instance().executeOriginator());
            if (!r.success) return r;
            auto data = r.data;
            data.insert("name", attenuation->hostItem()->name);
            data.insert("dimension", QLatin1String(attenuationDimensionName(dimensionType)));
            r.data = data;
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "bind_attenuation_to_object";
        def.displayName = "Bind Attenuation to Object";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"path", "string"}, {"object_id", "uint32"}, {"attenuation", "string"}, {"attenuation_guid", "string"}};
        def.undoable = true;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            auto* project = currentProjectFrom(projectManager);
            if (!project) return {false, {}, "NO_PROJECT", "No project open"};

            const QString path = firstStringArg(args, {"path", "object_path", "target_object"});
            const uint32_t objectID = static_cast<uint32_t>(args.value("object_id").toInteger(
                static_cast<qint64>(args.value("object_id").toDouble(0))));
            if (path.isEmpty() && objectID == 0)
                return {false, {}, "INVALID_PARAM", "path or object_id required"};
            auto* item = objectID != 0
                ? findItemByObjectIDInProject(project, objectID)
                : findItemByPathInProject(project, path);
            if (!item)
                return {false, {}, "NOT_FOUND",
                    objectID != 0
                        ? QString("Item not found: object_id=%1").arg(objectID)
                        : QString("Item not found: %1").arg(path)};
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type))
                return {false, {}, "FIELD_GROUP_COMMAND_REQUIRED",
                        "Use field group commands to edit Spatial Rating"};

            const QString name = firstStringArg(args, {"attenuation", "name"});
            const QUuid guid = firstGuidArg(args, {"guid", "attenuation_guid"});
            if (name.isEmpty() && guid.isNull())
                return {false, {}, "INVALID_PARAM", "attenuation or attenuation_guid required"};
            const auto selected = resolveProjectSelector(project,
                guid.isNull() ? name : attenuationGuidString(guid), ProjectItem::Type::Attenuation);
            if (!selected.ok)
                return {false, {{"candidates", selected.candidates}}, selected.errorCode, selected.errorMessage};
            auto* attenuation = selected.item->comp<VAttenuation>();
            const QString resolvedName = attenuation->hostItem()->name;

            auto* spatialComp = item->comp<VSpatial>();
            if (!spatialComp)
                return {false, {}, "NO_COMPONENT", "Item has no VSpatial"};
            constexpr auto propertyId = static_cast<uint8_t>(
                harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
            const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
            const auto previous = locator ? PlayablePropertyAuthoringV1::readProperty(
                *item, *locator, propertyId, std::nullopt) : std::nullopt;
            const harkenstudio::ObjectId stableId = AttenuationStore::runtimeId(*project, *attenuation);
            if (!previous || harkenstudio::IsNull(stableId))
                return {false, {}, "INVALID_STATE", "Attenuation binding has no canonical identity"};
            const uint32_t capObjectID = item->objectID;
            const PlayablePropertyStateV1 bound{true, objectIdString(stableId)};
            if (!AttenuationStore::applyBinding(projectManager, capObjectID, bound))
                return {false, {}, "INVALID_STATE", "Attenuation override could not be written"};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"path", path},
                {"object_id", static_cast<qint64>(itemShortID(item))},
                {"authoring_handle", static_cast<qint64>(capObjectID)},
                {"attenuation", resolvedName},
                {"attenuation_guid", attenuationGuidString(attenuation->hostItem()->guid)}
            };
            r.undoText = QStringLiteral("Bind Attenuation");
            r.undoFn = [projectManager, capObjectID, previous = *previous]() {
                AttenuationStore::applyBinding(projectManager, capObjectID, previous);
            };
            r.redoFn = [projectManager, capObjectID, bound]() {
                AttenuationStore::applyBinding(projectManager, capObjectID, bound);
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "unbind_attenuation_from_object";
        def.displayName = "Unbind Attenuation from Object";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"path", "string"}, {"object_id", "uint32"}};
        def.undoable = true;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            auto* project = currentProjectFrom(projectManager);
            if (!project) return {false, {}, "NO_PROJECT", "No project open"};

            const QString path = firstStringArg(args, {"path", "object_path", "target_object"});
            const uint32_t objectID = static_cast<uint32_t>(args.value("object_id").toInteger(
                static_cast<qint64>(args.value("object_id").toDouble(0))));
            if (path.isEmpty() && objectID == 0)
                return {false, {}, "INVALID_PARAM", "path or object_id required"};

            auto* item = objectID != 0
                ? findItemByObjectIDInProject(project, objectID)
                : findItemByPathInProject(project, path);
            if (!item)
                return {false, {}, "NOT_FOUND",
                    objectID != 0
                        ? QString("Item not found: object_id=%1").arg(objectID)
                        : QString("Item not found: %1").arg(path)};
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type))
                return {false, {}, "FIELD_GROUP_COMMAND_REQUIRED",
                        "Use field group commands to edit Spatial Rating"};

            auto* spatialComp = item->comp<VSpatial>();
            if (!spatialComp)
                return {false, {}, "NO_COMPONENT", "Item has no VSpatial"};
            constexpr auto propertyId = static_cast<uint8_t>(
                harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
            const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
            const auto previous = locator ? PlayablePropertyAuthoringV1::readProperty(
                *item, *locator, propertyId, std::nullopt) : std::nullopt;
            if (!previous || !previous->present)
                return {false, {}, "NOT_FOUND", "No attenuation bound to object"};

            const auto* attenuation = AttenuationStore::findByStableId(
                project, spatialComp->attenuationRef);
            const QString oldName = attenuation ? attenuation->hostItem()->name : QString();
            const QUuid oldGuid = attenuation ? attenuation->hostItem()->guid : QUuid();
            const uint32_t capObjectID = item->objectID;
            if (!AttenuationStore::applyBinding(projectManager, capObjectID, {}))
                return {false, {}, "INVALID_STATE", "Attenuation override could not be cleared"};

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"path", path},
                {"object_id", static_cast<qint64>(itemShortID(item))},
                {"authoring_handle", static_cast<qint64>(capObjectID)},
                {"attenuation", oldName},
                {"attenuation_guid", attenuationGuidString(oldGuid)}
            };
            r.undoText = QStringLiteral("Unbind Attenuation");
            r.undoFn = [projectManager, capObjectID, previous = *previous]() {
                AttenuationStore::applyBinding(projectManager, capObjectID, previous);
            };
            r.redoFn = [projectManager, capObjectID]() {
                AttenuationStore::applyBinding(projectManager, capObjectID, {});
            };
            return r;
        };
        mgr.registerAction(def);
    }

    {
        ActionDef def;
        def.id = "set_attenuation_distance_scaling";
        def.displayName = "Set Attenuation Distance Scaling";
        def.category = "spatial";
        def.paramSchema = QJsonObject{{"path", "string"}, {"object_id", "uint32"}, {"scaling", "float"}};
        def.undoable = true;
        def.handler = [projectManager](const QJsonObject& args) -> ActionResult {
            auto* project = currentProjectFrom(projectManager);
            if (!project) return {false, {}, "NO_PROJECT", "No project open"};

            const QString path = firstStringArg(args, {"path", "object_path", "target_object"});
            const uint32_t objectID = static_cast<uint32_t>(args.value("object_id").toInteger(
                static_cast<qint64>(args.value("object_id").toDouble(0))));
            if (path.isEmpty() && objectID == 0)
                return {false, {}, "INVALID_PARAM", "path or object_id required"};
            if (!args.contains("scaling"))
                return {false, {}, "INVALID_PARAM", "scaling required (0.01-100, 1.0=100%)"};

            auto* item = objectID != 0
                ? findItemByObjectIDInProject(project, objectID)
                : findItemByPathInProject(project, path);
            if (!item)
                return {false, {}, "NOT_FOUND",
                    objectID != 0
                        ? QString("Item not found: object_id=%1").arg(objectID)
                        : QString("Item not found: %1").arg(path)};
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type))
                return {false, {}, "FIELD_GROUP_COMMAND_REQUIRED",
                        "Use field group commands to edit Spatial Rating"};

            auto* spatialComp = item->comp<VSpatial>();
            if (!spatialComp)
                return {false, {}, "NO_COMPONENT", "Item has no VSpatial"};
            const float clamped = std::clamp(static_cast<float>(args.value("scaling").toDouble(1.0)), 0.01f, 100.0f);
            const float oldScaling = spatialComp->attenuationDistanceScaling;
            const uint32_t capObjectID = item->objectID;
            spatialComp->attenuationDistanceScaling = clamped;
            item->markDirty();
            ProjectManager::resyncItemToRuntime(item);
            if (projectManager)
                emit projectManager->itemPropertyChanged(item, QStringLiteral("attenuationDistanceScaling"));

            ActionResult r;
            r.success = true;
            r.data = QJsonObject{
                {"path", path},
                {"object_id", static_cast<qint64>(itemShortID(item))},
                {"authoring_handle", static_cast<qint64>(capObjectID)},
                {"attenuation_distance_scaling", static_cast<double>(clamped)}
            };
            r.undoText = QStringLiteral("Set Attenuation Distance Scaling");
            r.undoFn = [projectManager, capObjectID, oldScaling]() {
                applyAttenuationDistanceScaling(projectManager, capObjectID, oldScaling);
            };
            r.redoFn = [projectManager, capObjectID, clamped]() {
                applyAttenuationDistanceScaling(projectManager, capObjectID, clamped);
            };
            return r;
        };
        mgr.registerAction(def);
    }
}

}
