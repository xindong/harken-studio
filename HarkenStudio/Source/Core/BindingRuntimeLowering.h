
#pragma once

#include "BindingSemantics.h"

#include <QString>

#include <cstdint>
#include <vector>

namespace VNX {

struct ProjectData;
struct ProjectItem;
class VVariable;

struct BindingRuntimeProgram
{
    harkenstudio::ObjectId bindingId = {};
    harkenstudio::ObjectId variableId = {};
    uint32_t targetScopeKind = 0;
    uint32_t targetId = 0;
    uint32_t transitionMs = 0;
    QVector<CurvePointDef> points;
    QVector<CurveHandlePairDef> handles;
};

struct BindingRuntimeAttachment
{
    harkenstudio::ObjectId bindingId = {};
    uint32_t scopeKind = 0;
    harkenstudio::ObjectId objectId = {};
};

enum class BindingLowerStatus : uint8_t
{
    Ok = 0,
    Skip = 1,
    Fail = 2,
};

struct BindingRuntimeLowering
{
    std::vector<BindingRuntimeProgram> programs;
    std::vector<BindingRuntimeAttachment> attachments;
    bool failed = false;
    QString error;
};

struct PreviewBindingSyncState
{
    std::vector<harkenstudio::ObjectId> ids;
    std::vector<BindingRuntimeAttachment> attachments;
};

namespace BindingRuntimeLoweringSemantics {

QVector<CurvePointDef> storedCurveToEngineX(
    const QVector<CurvePointDef>& points, float rangeMin, float rangeMax);
QVector<CurveHandlePairDef> storedHandlesToEngineX(
    const QVector<CurveHandlePairDef>& handles, float rangeMin, float rangeMax);

void collectAncestorBindings(
    const ProjectItem& leaf,
    BindingHostClass hostClass,
    std::vector<const ParameterBindingDef*>* out);
const VVariable* variableOf(const ProjectData& project, const QUuid& variableId);
harkenstudio::ObjectId attachmentObjectId(const ProjectItem& host);

BindingLowerStatus appendLoweredBinding(const ProjectData& project,
                                        const ProjectItem& host,
                                        const ParameterBindingDef& source,
                                        BindingRuntimeLowering* output);

BindingRuntimeLowering lowerProject(const ProjectData& project);

bool sameAttachment(const BindingRuntimeAttachment& a,
                    const BindingRuntimeAttachment& b);

PreviewBindingSyncState reconcilePreviewSync(
    const BindingRuntimeLowering& desired,
    const PreviewBindingSyncState& previous,
    const std::vector<harkenstudio::ObjectId>& registeredOk,
    const std::vector<BindingRuntimeAttachment>& attachedOk);

}

}
