
#pragma once

#include "InspectorPanel.h"

#include <QScrollArea>
#include <QStackedWidget>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QListWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>
#include <QGroupBox>
#include <QDoubleSpinBox>
#include <QMap>
#include <QVariant>
#include <QColor>
#include <QListWidgetItem>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <array>
#include <functional>

#include "Core/AppSettings.h"
#include "Core/ProjectTypes.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ToggleSwitch.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/InspectorCustomization.h"

namespace VNX {

class InspectorPanelPrivate {
public:
    explicit InspectorPanelPrivate(InspectorPanel* q);
    ~InspectorPanelPrivate();

    InspectorPanel* q_ptr;

    enum Page {
        PageEmpty = 0,
        PageAction,
        PageAttenuation,
        PageSound,
        PageLayer,
        PageBus,
        PageContainer,
        PageTrack,
        PagePiece,
        PageVariable,
        PageCount
    };

    struct PageSlot {
        std::function<void()> builder;
        bool built = false;
    };
    std::array<PageSlot, PageCount> pages_;

    void registerPages();
    int  ensurePageBuilt(Page p);
    void invalidateBuiltPages();
    void dropEnginePageBindings();

    void setupUI();
    void setupActionPage();
    void setupAttenuationPage();
    void setupSoundPageEngine();
    void setupLayerPage();
    void setupContainerPage();
    void setupTrackPage();
    void setupPiecePage();
    void setupVariablePage();
    void setupBusPage();
    void wireAuthoringCommit(PropertyEditorEngine* engine);
    void setupEmptyPage();

    void refreshBoundPages(ProjectItem* item, const QString& prop);
    void rebindHierarchyPages(ProjectItem* createdChild);
    void clearItemBindings();
    bool currentContextContains(ProjectItem* item) const;

    void updateContext(const QString& type, const QString& name);


    QString titleType_;
    QString titleName_;

    QLineEdit* searchBox_;
    QStackedWidget* stack_;
    QScrollArea* scrollArea_;

    ProjectItem* currentSoundItem_ = nullptr;
    PropertyEditorEngine* soundEngine_ = nullptr;
    InspectorCustomization soundCustomization_;

    PropertyEditorEngine* layerEngine_ = nullptr;
    InspectorCustomization layerCustomization_;
    ProjectItem* currentLayerItem_ = nullptr;

    PropertyEditorEngine* containerEngine_ = nullptr;
    InspectorCustomization containerCustomization_;
    ProjectItem* currentContainerItem_ = nullptr;

    PropertyEditorEngine* trackEngine_ = nullptr;
    InspectorCustomization trackCustomization_;
    ProjectItem* currentTrackItem_ = nullptr;

    PropertyEditorEngine* pieceEngine_ = nullptr;
    InspectorCustomization pieceCustomization_;
    ProjectItem* currentPieceItem_ = nullptr;

    PropertyEditorEngine* variableEngine_ = nullptr;
    InspectorCustomization variableCustomization_;
    ProjectItem* currentVariableItem_ = nullptr;

    PropertyEditorEngine* busEngine_ = nullptr;
    InspectorCustomization busCustomization_;
    ProjectItem* currentBusItem_ = nullptr;

    ProjectItem* currentActionItem_ = nullptr;
    PropertyEditorEngine* actionEngine_ = nullptr;
    InspectorCustomization actionCustomization_;

    ProjectItem* currentAttenuationItem_ = nullptr;
    PropertyEditorEngine* attenuationEngine_ = nullptr;
    InspectorCustomization attenuationCustomization_;

    bool ownsEditorEngine(const QObject* origin) const
    {
        return origin == soundEngine_ || origin == layerEngine_
            || origin == containerEngine_ || origin == trackEngine_
            || origin == pieceEngine_ || origin == variableEngine_
            || origin == busEngine_
            || origin == actionEngine_ || origin == attenuationEngine_;
    }
};

InspectorPanelPrivate::Page pageForItemType(ProjectItem::Type type);

}
