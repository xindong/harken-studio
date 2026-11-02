#pragma once

#include "Core/ExternalEditorSelection.h"
#include "Core/FilterSemantics.h"

#include <QWidget>
#include <memory>

class QStackedWidget;

namespace VNX {

class FilterResponseCanvas;
class PropertyEditorEngine;
class PropertyEditSession;

class FilterEditorWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit FilterEditorWidget(QWidget* parent = nullptr);
    ~FilterEditorWidget() override;
    void bindItem(ProjectItem* item);

signals:
    void externalEditorRequested(const ExternalEditorSelection& selection);

private:
    void refresh(const QObject* originator = nullptr);
    void refreshAppearance();
    void cancelGesture();

    QUuid ownerGuid_;
    PropertyEditorEngine* fields_ = nullptr;
    QWidget* fieldsPage_ = nullptr;
    FilterResponseCanvas* canvas_ = nullptr;
    QStackedWidget* graph_ = nullptr;
    FilterState gestureStart_;
    std::array<std::unique_ptr<PropertyEditSession>, 10> gestures_;
    bool gestureActive_ = false;
};

}
