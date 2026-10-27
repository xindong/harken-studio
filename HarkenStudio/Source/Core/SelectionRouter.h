
#pragma once

#include "../MainWindow.h"
#include "CurveSelection.h"

#include <QObject>
#include <QString>

namespace VNX {

class PlacementContext;

class SelectionRouter : public QObject
{
    Q_OBJECT

public:
    explicit SelectionRouter(MainWindow* host);

    struct Views
    {
        BlueprintCanvasWidget* blueprintCanvas = nullptr;
        ProjectExplorer* projectExplorer = nullptr;
        ads::CDockWidget* blueprintDock = nullptr;
    };
    void setViews(const Views& v);
    ProjectItem* activeItem() const;

    void routeSelection(ProjectItem* item, bool reveal, const QObject* source = nullptr);
    void selectItem(ProjectItem* item, bool reveal, const QObject* source = nullptr);
    void selectItem(ProjectItem* item, bool reveal,
                    const PlacementContext& placement);
    void revealItemSourceInExplorer(ProjectItem* item);
    bool revealProjectObject(harkenstudio::ObjectId id);

    void selectCurve(const CurveSelection& selection);

public slots:
    void onProjectItemSelected(ProjectItem* item);
    void onBlueprintNodeSelected(uint32_t anchorId);
    void onBlueprintNodeDoubleClicked(uint32_t anchorId);
    void onBlueprintSelectionCleared();
    void onProjectItemDoubleClicked(ProjectItem* item);
    void onAudioFileSelected(const QString& path);
    void onActionSelected(const QString& name);

signals:
    void statusMessage(const QString& text);
    void selectionChanged(ProjectItem* item);
    void auditionTargetRequested(ProjectItem* item);
    void curveSelectionChanged(const CurveSelection& selection);

private:
    void activateItem(ProjectItem* item, const PlacementContext* placement = nullptr);
    void routeSelection(ProjectItem* item, bool reveal,
                        const PlacementContext* placement,
                        const QList<ProjectItem*>& items, const QObject* source);
    void syncSelectionViews(const QList<ProjectItem*>& items,
                            const PlacementContext* placement, const QObject* source);
    void clearActiveSelection();
    void reconcileCurveSelection(ProjectItem* item);
    void republishCurveSelection();
    QString resolveAudioPathHintToAbsolute(const QString& pathHint) const;
    QString resolveItemAudioAbsolutePath(const ProjectItem* item) const;

    MainWindow* host_ = nullptr;
    CurveSelection curveSelection_;
    QUuid activeItemGuid_;

    BlueprintCanvasWidget* blueprintCanvas_ = nullptr;
    ProjectExplorer* projectExplorer_ = nullptr;
    ads::CDockWidget* blueprintDock_ = nullptr;
};

}
