#pragma once

#include "TransitionRuleEditing.h"

#include <QWidget>

class QDoubleSpinBox;
class QScrollBar;
class QToolButton;

namespace VNX {

class TransitionFadeTimeline;

class TransitionFadeEditor final : public QWidget
{
    Q_OBJECT

public:
    explicit TransitionFadeEditor(QWidget* parent = nullptr);

    void bindRule(const QUuid& containerGuid, const TransitionEndpoints& endpoints);
    void clear();
    bool hasRule() const;

private:
    ProjectItem* container() const;
    void refresh();
    void refreshFields(const TransitionFadeDef& fadeOut, const TransitionFadeDef& fadeIn);
    void refreshScroll();
    void refreshTheme();
    void commit(const TransitionFadeDef& fadeOut, const TransitionFadeDef& fadeIn);
    QDoubleSpinBox* createSpin(const QString& name, double minimum);

    QUuid containerGuid_;
    TransitionEndpoints endpoints_;
    TransitionFadeTimeline* timeline_ = nullptr;
    QScrollBar* scroll_ = nullptr;
    QToolButton* zoomIn_ = nullptr;
    QToolButton* zoomOut_ = nullptr;
    QDoubleSpinBox* outStart_ = nullptr;
    QDoubleSpinBox* outDuration_ = nullptr;
    QDoubleSpinBox* inStart_ = nullptr;
    QDoubleSpinBox* inDuration_ = nullptr;
};

}
