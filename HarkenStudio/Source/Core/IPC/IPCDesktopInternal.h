#pragma once

#include "IPCServerInternal.h"
#include "../../MainWindow.h"
#include "../../Widgets/Common/ParameterSlider.h"
#include "../../Widgets/Common/ToggleSwitch.h"
#include "../../Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "../../Widgets/Blueprint/SignalChainSource.h"
#include "../../Widgets/Blueprint/NodeItem.h"
#include "../../Widgets/ProjectExplorer/ProjectExplorer.h"
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
#include <QPixmap>
#include <DockWidget.h>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QCheckBox>
#include <QTableWidget>
#include <QListWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include "Core/UndoHistory.h"

namespace VNX {

QJsonObject serializeWidget(QWidget* widget, int depth, int maxDepth);
QString widgetPath(QWidget* widget);
QString widgetText(QWidget* widget);

}
