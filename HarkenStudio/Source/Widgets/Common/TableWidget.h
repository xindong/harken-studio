#pragma once

#include "TableColumnLayout.h"

#include <QFrame>
#include <QModelIndex>
#include <QPointer>

#include <cstdint>
#include <functional>
#include <optional>

class QAbstractItemModel;
class QAbstractItemView;
class QAbstractScrollArea;
class QHeaderView;
class QHBoxLayout;
class QLabel;
class QMimeData;
class QScrollArea;
class QTableView;
class QVBoxLayout;

namespace VNX {

class ListFooter;
class ProportionalTreeWidget;
class ThemeManager;

class TableCell : public QWidget
{
    Q_OBJECT

public:
    void addWidget(QWidget* widget, int stretch = 1, Qt::Alignment alignment = {});

private:
    friend class TableWidget;
    explicit TableCell(QWidget* parent, ThemeManager* themeManager);
    void applyTheme();

    ThemeManager* themeManager_;
    QHBoxLayout* layout_;
};

enum class TableKind { Config, Display };
enum class TableBodySizing { Content, Expanding };
enum class TableColumnSizing { Fit, Scrollable };

struct TableOptions
{
    TableKind kind = TableKind::Config;
    TableBodySizing bodySizing = TableBodySizing::Content;
    bool columnsMovable = false;
    TableColumnSizing columnSizing = TableColumnSizing::Fit;

    static TableOptions display(TableBodySizing sizing = TableBodySizing::Expanding)
    {
        return {TableKind::Display, sizing, true};
    }
};

struct TableColumn
{
    QString id;
    QString title;
    int weight = 1;
    int minimumWidth = 0;
    QString objectName = {};
};

class TableWidget : public QFrame
{
    Q_OBJECT

public:
    explicit TableWidget(QWidget* parent = nullptr, ThemeManager* themeManager = nullptr);
    explicit TableWidget(const TableOptions& options, QWidget* parent = nullptr,
                         ThemeManager* themeManager = nullptr);
    ~TableWidget() override;
    const TableOptions& options() const { return options_; }
    void setBodySizing(TableBodySizing sizing);
    void setColumnSizing(TableColumnSizing sizing);
    void setColumnsMovable(bool movable);
    void moveColumn(int logicalColumn, int visualIndex);
    QList<int> columnOrder() const;
    void setColumns(const QString& key, const QList<TableColumn>& columns);
    void setColumnWeights(const QString& key, const QList<int>& weights);
    int columnCount() const { return static_cast<int>(columns_.size()); }
    QLabel* headerCell(int column) const;
    void setColumnMinimumWidth(int column, int width);
    void setColumnVisible(int column, bool visible);
    QWidget* header() const { return header_; }
    void setHeaderVisible(bool visible);
    QWidget* rowsWidget();
    QVBoxLayout* rowsLayout();
    QLabel* bodyMessage();
    ListFooter* footer(bool standardButtons = true);
    QTableView* createModelView(const QString& name, QAbstractItemModel* model);
    ProportionalTreeWidget* createItemView(const QString& name, const QStringList& headers,
                                         const QList<int>& columnWeights);
    template<typename Factory>
    auto createCustomView(const QString& name, Factory factory)
    {
        Q_ASSERT(!rows_ && !view_);
        auto* view = factory(this, themeManager_);
        Q_ASSERT(view);
        view->setObjectName(name);
        attachView(view);
        return view;
    }
    void setModel(QAbstractItemModel* model);
    void setViewColumns(const QString& key, const QList<TableColumn>& columns);
    void setRowNumberColumn(int column);
    TableCell* createCell(QWidget* parent);
    void registerRow(QWidget* row, const QList<QWidget*>& cells, QWidget* details = nullptr);
    QWidget* rowForWidget(QWidget* widget) const;
    void registerGroupRow(QWidget* row);
    QLabel* createRowNumber(QWidget* row, int index, std::optional<int> displayNumber = {});
    void setRowMoveHandler(std::function<void(int, int)> handler,
                           std::function<bool(int, int)> guard = {});
    void setDropHandler(std::function<bool(const QMimeData*)> handler,
                        std::function<bool(const QMimeData*)> guard);
    void setSelectionClearHandler(std::function<void()> handler);
    void setRowSelectionHandler(std::function<void(QWidget*)> handler);
    bool removeSelectedRow(const std::function<bool()>& remove, const QModelIndex& removedIndex = {});
    bool canMoveRow(int from, int to) const;
    void setBodyExpanding(bool expanding);
    QScrollArea* scrollArea() const { return scroll_; }
    QSize minimumSizeHint() const override;

public slots:
    void refreshLayout();
    void moveRow(int from, int to);
    virtual void clearSelection();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    virtual void applyTheme();
    virtual bool hasSelection() const;

private:
    struct Row {
        QPointer<QWidget> widget;
        QList<QPointer<QWidget>> cells;
        QPointer<QWidget> number;
        QPointer<QWidget> details;
        QPointer<QFrame> detailsNumber;
        int detailsIndent = 0;
    };

    void ensureRows();
    void attachView(QAbstractItemView* view);
    void initializeScrollArea(QAbstractScrollArea* area);
    void applySizing();
    int bodyHeightForContent(int contentHeight) const;
    bool columnsFitToWidth() const;
    void bindViewModel();
    void scheduleLayout();
    void refreshViewLayout();
    void refreshViewColumns();
    void resizeViewColumn(int column, int width);
    QHeaderView* viewHeader() const;
    void applyColumnOrder();
    int resizeColumnAt(int x) const;
    void updateCellMetrics(QWidget* cell);
    void refreshRowState(const Row& row);
    void refreshRowDetails(const Row& row);
    QList<QWidget*> movableRows() const;
    QModelIndexList movableViewRows() const;
    QList<QWidget*> selectableWidgetRows() const;
    QModelIndexList selectableViewRows() const;
    int rowMoveTarget(const QPoint& globalPosition, int* indicatorY = nullptr) const;
    void cancelRowMove();
    bool canAcceptDrop(const QDropEvent* event) const;

    ThemeManager* themeManager_;
    TableOptions options_;
    QVBoxLayout* layout_;
    QWidget* headerClip_ = nullptr;
    QWidget* header_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* rows_ = nullptr;
    QLabel* message_ = nullptr;
    ListFooter* footer_ = nullptr;
    QAbstractItemView* view_ = nullptr;
    QList<QMetaObject::Connection> modelConnections_;
    QList<TableColumn> columns_;
    QList<QLabel*> labels_;
    QList<bool> hiddenColumns_;
    QList<Row> rowBindings_;
    QList<QPointer<QWidget>> groups_;
    TableColumnLayout columnLayout_;
    QString viewColumnLayoutKey_;
    QList<TableColumn> viewColumns_;
    QHash<int, int> scrollableViewWidths_;
    QList<int> widths_;
    QList<int> minimums_;
    QList<int> dragWidths_;
    QList<int> visualOrder_;
    int dragColumn_ = -1;
    int movingColumn_ = -1;
    int dragX_ = 0;
    bool layoutPending_ = false;
    bool applyingLayout_ = false;
    std::function<void(int, int)> rowMoveHandler_;
    std::function<bool(int, int)> rowMoveGuard_;
    std::function<bool(const QMimeData*)> dropHandler_;
    std::function<bool(const QMimeData*)> dropGuard_;
    std::function<void()> selectionClearHandler_;
    std::function<void(QWidget*)> rowSelectionHandler_;
    uint64_t selectionGeneration_ = 0;
    QPointer<QWidget> draggedNumber_;
    QPersistentModelIndex draggedIndex_;
    QPoint rowDragStart_;
    QFrame* rowMoveIndicator_ = nullptr;
};

}
