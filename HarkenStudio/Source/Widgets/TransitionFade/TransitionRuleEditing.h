#pragma once

#include "Core/AssetPipeline/SourceWaveformView.h"
#include "Core/TransitionSemantics.h"

#include <QColor>
#include <QIcon>
#include <QString>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

class QObject;

namespace VNX {

struct ProjectItem;

using TransitionEndpoints = std::pair<std::optional<QUuid>, std::optional<QUuid>>;

namespace TransitionRuleEditing {

const TransitionTableDef* table(const ProjectItem* container);
const TransitionRuleDef* find(const ProjectItem* container, const TransitionEndpoints& endpoints);
bool commit(ProjectItem* container, const TransitionTableDef& candidate,
            const QString& undoText, QObject* source);
bool update(ProjectItem* container, const TransitionEndpoints& endpoints,
            const std::function<void(TransitionRuleDef&)>& edit,
            const QString& undoText, QObject* source);

}

struct TransitionPreviewClip
{
    QString name;
    QIcon icon;
    QColor color;
    bool known = false;
    double start = 0.0;
    double end = 0.0;
    double playedStart = 0.0;
    double playedEnd = 0.0;
    QString waveformPath;
    std::optional<SourceWaveformView> waveformView;
};

struct TransitionPreview
{
    TransitionPreviewClip source;
    TransitionPreviewClip target;
    TransitionFadeDef fadeOut;
    TransitionFadeDef fadeIn;

    double sourceTailSeconds() const { return std::max(0.0, source.playedEnd); }
    double targetLeadInSeconds() const { return std::max(0.0, -target.playedStart); }
};

TransitionPreview buildTransitionPreview(const ProjectItem& container, const TransitionRuleDef& rule);

}
