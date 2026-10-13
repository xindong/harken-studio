#pragma once

#include "IPCServer.h"
#include "ActionManager.h"
#include "AudioEngine.h"
#include "RuntimeBridge/IpcBridge.h"
#include "RuntimeBridge/DiagnosticsBridge.h"
#include "ProjectManager.h"
#include <cstring>
#include <QThread>
#include <QTimer>
#include <QPointer>
#include <QCoreApplication>
#include <QDir>
#include "ProjectPackExporter.h"
#include "UndoCommands.h"
#include "../../MainWindow.h"
#include "AuditionManager.h"
#include "MigrationManager.h"
#include "../../Widgets/Common/ParameterSlider.h"
#include "../../Widgets/Common/ToggleSwitch.h"
#include "../../Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "../../Widgets/Blueprint/SignalChainSource.h"
#include "../../Widgets/Blueprint/NodeItem.h"
#include "../../Widgets/ProjectExplorer/ProjectExplorer.h"
#include "HarkenMath.h"

#include "AssetPipeline/AssetTypes.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/MediaPool.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/PlatformProfile.h"
#include "AssetPipeline/AssetGarbageCollector.h"
#include "AssetPipeline/ImportAssetsCommand.h"
#include "AssetPipeline/PathUtils.h"

#ifdef GetObject
#undef GetObject
#endif

#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VAction.h"
#include "Components/VBus.h"

#include "../../Query/VQLEngine.h"
#include "../../Query/VQLObserver.h"

#include <QDebug>
#include <QJsonArray>
#include <QLabel>
#include <QAbstractButton>
#include <QPushButton>
#include <QTabWidget>
#include <QStackedWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QProgressBar>
#include <QBuffer>
#include <QPixmap>
#include <DockWidget.h>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QCheckBox>
#include <QTableWidget>
#include <QListWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include <QHostAddress>
#include <QUndoStack>
#include <QFileInfo>
#include <QDateTime>
#include <QSet>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace VNX {

inline uint32_t fnv1a_hash(const char* str) { return HarkenEditor::hashString(str); }
inline uint32_t fnv1a_hash(const QString& str) { return HarkenEditor::hashString(str.toUtf8().constData()); }

ProjectItem* findItemByPathInProject(ProjectData* project, const QString& path);

ProjectItem* findItemByPath(ProjectItem* root, const QString& path);
ProjectItem* findItemByNameRecursive(ProjectItem* item, const QString& name);
ProjectFileBinding* findBindingByFolder(ProjectData* project, const QString& folder);
ProjectItem* findItemByObjectID(ProjectItem* root, uint32_t objectID);
ProjectItem* findItemByObjectIDInProject(ProjectData* project, uint32_t objectID);
ProjectItem* findItemByAuthoringHandle(ProjectItem* root, uint32_t authoringHandle);
ProjectItem* findItemByAuthoringHandleInProject(ProjectData* project, uint32_t authoringHandle);
uint32_t itemShortID(const ProjectItem* item);
ProjectItem* findItemByShortID(ProjectItem* root, uint32_t shortID);
ProjectItem* findItemByShortIDInProject(ProjectData* project, uint32_t shortID);
bool containsProjectItemPointer(const ProjectItem* root, const ProjectItem* target);
ProjectFileBinding* findOwnerBindingForItem(ProjectData* project, ProjectItem* item);
QString buildItemFullPath(ProjectData* project, ProjectItem* item);
ProjectItem* resolveItemByPathOrObjectID(ProjectData* project,
                                          const QString& path, uint32_t objectID);
QString itemTypeToString(ProjectItem::Type type);
std::optional<ProjectItem::Type> stringToItemType(const QString& typeStr);


QJsonObject serializeWidget(QWidget* w, int depth, int maxDepth);
QString widgetPath(QWidget* w);
QString widgetText(QWidget* w);

}
