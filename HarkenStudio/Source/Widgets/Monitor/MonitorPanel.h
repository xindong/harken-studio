#pragma once

#include "Monitor/MonitorCatalogModel.h"

#include <QString>
#include <QVariantMap>
#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QToolButton;
class QTextBrowser;

namespace VNX {

class MonitorCatalogFilter;
class TableWidget;

using MonitorJumpLocator = std::function<void(harkenstudio::ObjectId stableId)>;

class MonitorPanel : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorPanel(QWidget* parent = nullptr);

    QTableView* table() const { return table_; }
    void setCaptureController(Monitor::EventCaptureController* capture);
    void copySelected();
    QString gapMessage() const;

    QVariantMap snapshotParams() const;
    void restoreParams(const QVariantMap& params);
    int visibleRowCount() const;

    QPushButton* jumpButton() const { return jumpButton_; }
    void setJumpLocator(MonitorJumpLocator locator);
    bool jumpCurrentRow();

private:
    void rebuildVisibleRows();
    void refreshJumpButton();
    void onJumpClicked();
    void refreshCaptureView();
    void refreshDetails();

    QLabel* gapLabel_ = nullptr;
    QComboBox* categoryFilter_ = nullptr;
    QLineEdit* textFilter_ = nullptr;
    QPushButton* jumpButton_ = nullptr;
    TableWidget* tableFrame_ = nullptr;
    QTableView* table_ = nullptr;
    QPointer<Monitor::EventCaptureController> capture_;
    MonitorCatalogModel* model_ = nullptr;
    MonitorCatalogFilter* filter_ = nullptr;
    QComboBox* levelFilter_ = nullptr;
    QToolButton* autoScroll_ = nullptr;
    QToolButton* copy_ = nullptr;
    QToolButton* clear_ = nullptr;
    QLabel* status_ = nullptr;
    QTextBrowser* details_ = nullptr;
    MonitorJumpLocator locator_;
};

} // namespace VNX
