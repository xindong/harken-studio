
#pragma once

#include "../MainWindow.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <limits>

namespace VNX {

class BusController : public QObject
{
    Q_OBJECT

public:
    explicit BusController(MainWindow* host);

    struct Views
    {
        BlueprintCanvasWidget* blueprintCanvas = nullptr;
        ProjectExplorer* projectExplorer = nullptr;
        ads::CDockWidget* blueprintDock = nullptr;
    };
    void setViews(const Views& v);

    void selectBus(uint32_t busID);
    void loadBusSignalChain(uint32_t busID);
    void focusBusInBlueprint(uint32_t busID);
    void handleBusParameterChanged(uint32_t busID, const QString& parameterName,
                                   const QObject* origin = nullptr);
    void scheduleBusRefresh();
    void clearSelection();

    bool isSelectionSyncing() const { return selectionSyncGuard_; }

private:
    MainWindow* host_ = nullptr;

    BlueprintCanvasWidget* blueprintCanvas_ = nullptr;
    ProjectExplorer* projectExplorer_ = nullptr;
    ads::CDockWidget* blueprintDock_ = nullptr;

    static constexpr uint32_t kInvalidBusSelection = std::numeric_limits<uint32_t>::max();
    uint32_t selectedBusId_ = kInvalidBusSelection;
    bool selectionSyncGuard_ = false;
    bool busRefreshPending_ = false;
};

}
