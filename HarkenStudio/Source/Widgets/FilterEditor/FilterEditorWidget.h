#pragma once

#include "Core/ExternalEditorSelection.h"
#include "Core/FilterSemantics.h"

#include <QWidget>
#include <memory>


namespace harken::studio {

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
    FilterState gestureStart_;
    FilterState gestureBase_;
    std::unique_ptr<PropertyEditSession> gesture_;
    bool gestureActive_ = false;
};

}
