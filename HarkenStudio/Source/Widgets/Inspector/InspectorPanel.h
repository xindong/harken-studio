#pragma once

#include <QWidget>
#include <memory>
#include <QJsonObject>
#include <QString>
#include <QUuid>
#include "Core/AppSettings.h"
#include "Core/CurveSelection.h"
#include "Core/ExternalEditorSelection.h"
#include "Core/PlacementContext.h"

#include <optional>

namespace VNX {

struct ProjectItem;
class InspectorPanelPrivate;
class PropertyEditorEngine;

class InspectorPanel : public QWidget
{
    Q_OBJECT

public:
    explicit InspectorPanel(QWidget* parent = nullptr);
    ~InspectorPanel() override;

    void showItemInspector(ProjectItem* item);
    void showItemInspector(ProjectItem* item, const PlacementContext& placement);
    void selectCurveDimension(AttenuationDimension dimension);
    void requestEditorLaunch(const QString& slot);
    void showEmpty();
    ProjectItem* currentContextItem() const;
    QString contextType() const;
    QString contextName() const;
    QString titleObjectName() const;
    void requestCurveSelection(const CurveSelection& selection);
    void requestExternalEditor(const ExternalEditorSelection& selection, bool addBinding = false);
    void showBusInspector(uint32_t busID);
    void showCanvasNode(uint32_t anchorId);
    QJsonObject getInspectorSchema(const QString& schemaId) const;
    void refreshBusInspector();
    void reapplySearchFilter();

    // True when origin is one of this panel's PropertyEditorEngines — i.e. a
    // property-changed broadcast is this panel's own edit echoing back.
    bool ownsEditorOrigin(const QObject* origin) const;

    int panelId() const { return panelId_; }
    void setPanelId(int id) { panelId_ = id; }

    bool isPinned() const { return pinned_; }
    void setPinned(bool pinned);

    QString pinnedObjectGuid() const;
    const PlacementContext* placementContext() const;

signals:
    void propertyChanged();
    void navigateToAction(uint32_t eventObjectID);
    void navigateToAttenuation(const QString& attenuationName);
    void navigateToAttenuationByGuid(const QUuid& attenuationGuid, const QString& fallbackName);
    void curveSelectionRequested(const CurveSelection& selection);
    void externalEditorRequested(const ExternalEditorSelection& selection, bool addBinding);

    void panelFocused(int panelId);

    void pinToggled(int panelId, bool pinned);

    void titleContextChanged(int panelId, const QString& type, const QString& name);

    void editorLaunchRequested(int panelId, const QString& slot, ProjectItem* item,
                               const PlacementContext* placement);

    void searchFilterChanged(const QString& text);

private slots:
    void onSearchTextChanged(const QString& text);
    void onFontSettingsChanged(PanelId panelId, int size);
    void applyTheme();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    void focusInEvent(QFocusEvent* event) override;

private:
    friend class InspectorPanelPrivate;
    std::unique_ptr<InspectorPanelPrivate> d;

    void installFocusFilterRecursively(QObject* root);

    void wireEngineLaunch(PropertyEditorEngine* engine);
    void showItemInspectorImpl(ProjectItem* item);

    int panelId_ = 0;
    bool pinned_ = false;
    std::optional<PlacementContext> placementContext_;
};

}
