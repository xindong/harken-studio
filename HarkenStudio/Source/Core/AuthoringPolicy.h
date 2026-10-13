#pragma once

#include "Timeline/TimelineTypes.h"
#include "TransitionSemantics.h"
#include "Components/VSelection.h"

namespace VNX::AuthoringPolicy {

using ObjectResolver = std::function<const ProjectItem*(uint32_t)>;

bool supportsObjectColorEditing();
bool supportsMusicalTime();
QString musicalTimeRestriction();
const std::vector<TransitionExitTiming>& allowedTransitionExitTimings();
bool allowsTransitionExitTiming(TransitionExitTiming timing);
TransitionRuleDef defaultTransitionRule();
TransitionTableDef defaultTransitionTable();
TimelineValidation validatePieceTime(const PieceTimeSpec& time);
TimelineValidation validatePieceTime(const QJsonObject& time);
TimelineValidation validateTemplate(const QJsonObject& templateData);
TimelineValidation validateTimeline(const ProjectItem& item,
                                    const TimelineItemResolver& resolve,
                                    const ObjectResolver& resolveObject = {});
TimelineValidation validateNewSubtree(const ProjectItem& item,
                                      const ProjectItem* destinationParent,
                                      const TimelineItemResolver& resolve,
                                      const ObjectResolver& resolveObject = {});
TimelineValidation validateTransitions(const TransitionTableDef& replacement,
                                       const TransitionTableDef* previous = nullptr);
TimelineValidation validateTransitionActivation(const ProjectItem& container,
                                                const VSelection& replacement,
                                                PlatformScope scope = std::nullopt);

}
