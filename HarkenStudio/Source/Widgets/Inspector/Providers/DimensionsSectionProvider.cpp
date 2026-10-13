
#include "Widgets/Inspector/Providers/DimensionsSectionProvider.h"
#include "Widgets/Inspector/InspectorLayout.h"
#include "Widgets/Inspector/AttenuationInspectorSchema.h"

#include "Core/ActionManager.h"
#include "Core/Components/VAttenuation.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/CurveSemantics.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/LayoutDebugTint.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Shared/InspectorFormRow.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>

#include <memory>
#include <optional>
#include <vector>

namespace VNX {

namespace {


constexpr int kCurveShare = 700;

constexpr int kThumbSamples = 32;

struct DimensionUiOption
{
    AttenuationDimension type;
    const char* label;
    float yMin;
    float yMax;
};

const DimensionUiOption kDimensionOptions[] = {
    { AttenuationDimension::Volume, "Volume",   -96.0f,    0.0f },
    { AttenuationDimension::Pitch,  "Pitch",  -1200.0f, 1200.0f },
};

QColor dimensionColor(AttenuationDimension type)
{
    const auto& colors = ThemeManager::instance().snapshot()->domains.curve.colors;
    return type == AttenuationDimension::Pitch
        ? colors.attenuationPitch
        : colors.attenuationVolume;
}

const DimensionUiOption& uiOptionFor(AttenuationDimension type)
{
    for (const auto& opt : kDimensionOptions)
        if (opt.type == type)
            return opt;
    return kDimensionOptions[0];
}

class CurveThumbnail : public QWidget
{
public:
    explicit CurveThumbnail(QWidget* parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("curveThumbnail"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QtControlSkin::setSemanticProperty(this, "fieldRole", QStringLiteral("preview"));
    }

    void setCurve(const QVector<CurvePointDef>& points,
                  const QVector<CurveHandlePairDef>& handles,
                  float maxDistance, const DimensionUiOption& ui)
    {
        points_ = points;
        handles_ = handles;
        domain_ = ui.type == AttenuationDimension::Volume
            ? CurveAxisScale::AmplitudeDb
            : CurveAxisScale::LinearValue;
        maxDistance_ = std::max(1.0f, maxDistance);
        if (points_.isEmpty())
            points_ = attenuationDefaultCurve(ui.type, maxDistance_);
        attenuationNormalizeCurve(ui.type, points_, handles_);
        yMin_ = ui.yMin;
        yMax_ = ui.yMax;
        color_ = dimensionColor(ui.type);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        QtControlSkin::paintFieldFrame(p, this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

        if (points_.size() < 2)
            return;

        const QRectF r = frame.adjusted(2.0, 2.5, -2.0, -2.5);
        // Same axis mapping as CurveCanvas::pointToScreen (shared
        // curveAxisNormalizedY), so the thumbnail silhouette matches the
        // editor for every dimension.
        const auto mapPoint = [&](float distance, float value) {
            const float nx = qBound(0.0f, distance / maxDistance_, 1.0f);
            const double ny = qBound(0.0, curveAxisNormalizedY(domain_, yMin_, yMax_, value), 1.0);
            return QPointF(r.left() + nx * r.width(), r.bottom() - ny * r.height());
        };

        // Sampling mirrors CurveCanvas::segmentScreenPolyline:
        // anchors always enter the polyline; every segment samples the Bezier
        // mirror evaluator (volume on the amplitude plane, same as runtime).
        const auto valueAt = [this](float d) {
            return curveBezierValueAt(points_, handles_, d, domain_);
        };
        QPolygonF poly;
        poly.append(mapPoint(points_.front().x, points_.front().value));
        for (int i = 0; i + 1 < points_.size(); ++i) {
            const CurvePointDef& left = points_[i];
            const CurvePointDef& right = points_[i + 1];
            for (int step = 1; step < kThumbSamples; ++step) {
                const float t = static_cast<float>(step) / kThumbSamples;
                const float d = left.x + (right.x - left.x) * t;
                poly.append(mapPoint(d, valueAt(d)));
            }
            poly.append(mapPoint(right.x, right.value));
        }
        p.setPen(QPen(color_, 1.2));
        p.drawPolyline(poly);
    }

private:
    QVector<CurvePointDef> points_;
    QVector<CurveHandlePairDef> handles_;
    CurveAxisScale domain_ = CurveAxisScale::AmplitudeDb;
    float maxDistance_ = 1.0f;
    float yMin_ = 0.0f;
    float yMax_ = 1.0f;
    QColor color_;
};

class DimensionsListWidget : public QWidget
{
public:
    explicit DimensionsListWidget(QWidget* parent)
        : QWidget(parent)
    {
        setFocusPolicy(Qt::StrongFocus);

        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, ThemeManager::instance().metrics().inspector.columnPadding, 0);
        v->setSpacing(0);
        reserveInspectorContentSlots(this);

        box_ = new TableWidget(this);
        box_->setSelectionClearHandler([this] { selectRow(-1); });
        box_->setRowSelectionHandler([this](QWidget* row) {
            const int index = row->property("rowIndex").toInt();
            selectRow(index);
            notifyRowActivated(index);
        });
        v->addWidget(box_);
        buildHeader();
        rows_ = box_->rowsWidget();
        buildFooter();
        box_->setRowMoveHandler([this](int from, int to) {
            auto dimensions = def_.dimensions;
            dimensions.move(from, to);
            QJsonArray value;
            for (const auto& dimension : dimensions) value.append(dimension.toJson());
            const auto result = ActionManager::instance().execute(QStringLiteral("set_item_property"),
                {{"authoring_handle", static_cast<qint64>(boundItem_->objectID)},
                 {"property", QStringLiteral("dimensions")}, {"value", value}}, ActionSource::GUI, this);
            if (result.success) {
                reloadFromItem();
                selectRow(to);
            }
        }, [this](int, int) { return boundItem_ != nullptr; });

        QObject::connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged, this,
                         [this](ProjectItem* item, const QString&) {
                             if (item == boundItem_)
                                 reloadFromItem();
                         }, Qt::QueuedConnection);
        QObject::connect(&ProjectManager::instance(), &ProjectManager::itemDeleted, this,
                         [this](ProjectItem* item) {
                             if (item == boundItem_)
                                 setBoundItem(nullptr);
                         });
        QObject::connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this,
                         [this]() { setBoundItem(nullptr); });

        qApp->installEventFilter(this);
    }

    void setBoundItem(ProjectItem* item)
    {
        if (boundItem_ != item) box_->clearSelection();
        boundItem_ = item;
        reloadFromItem();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!watched->isWidgetType())
            return QWidget::eventFilter(watched, event);

        auto* w = static_cast<QWidget*>(watched);
        const QEvent::Type t = event->type();

        if (t == QEvent::MouseButtonPress) {
            QWidget* rowHost = w;
            while (rowHost && rowHost->parentWidget() != rows_)
                rowHost = rowHost->parentWidget();
            if (rowHost && rowHost->property("rowIndex").isValid()) {
                const int index = rowHost->property("rowIndex").toInt();
                selectRow(index);
                notifyRowActivated(index);
            } else if (selected_ >= 0 && w != this && !isAncestorOf(w)
                       && w->window() == window()) {
                selectRow(-1);
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    QWidget* buildHeader()
    {
        box_->setColumns(QStringLiteral("Dimensions"), {
            {QStringLiteral("Number"), {}, 0},
            {QStringLiteral("Dimension"), {}, 1000 - kCurveShare},
            {QStringLiteral("Curve"), {}, kCurveShare}
        });
        applyInspectorTableDescriptor(box_, attenuationInspectorSchema(), QStringLiteral("dimensions"));
        return box_->header();
    }

    QWidget* buildFooter()
    {
        auto* foot = box_->footer();
        addBtn_ = foot->addButton();
        addBtn_->setObjectName(QStringLiteral("listAdd_dimensions"));
        removeBtn_ = foot->removeButton();
        removeBtn_->setObjectName(QStringLiteral("listRemoveSelected_dimensions"));

        QObject::connect(addBtn_, &QToolButton::clicked, this, [this]() { showAddMenu(); });
        QObject::connect(removeBtn_, &QToolButton::clicked, this, [this]() { removeSelected(); });

        return foot;
    }

    void selectRow(int index)
    {
        if (selected_ == index)
            return;
        selected_ = index;
        for (int r = 0; r < static_cast<int>(rowWidgets_.size()); ++r) {
            QWidget* row = rowWidgets_[r];
            row->setProperty("selected", r == index);
            row->style()->unpolish(row);
            row->style()->polish(row);
        }
        if (removeBtn_)
            removeBtn_->setEnabled(index >= 0 && index < static_cast<int>(def_.dimensions.size()));
    }

    void showAddMenu()
    {
        QMenu menu(this);
        for (const auto& opt : kDimensionOptions) {
            if (def_.dimension(opt.type))
                continue;
            const AttenuationDimension type = opt.type;
            QAction* act = menu.addAction(QObject::tr(opt.label));
            QObject::connect(act, &QAction::triggered, this, [this, type]() {
                executeDimensionCommand("add_attenuation_dimension", type);
            });
        }
        if (!menu.isEmpty())
            menu.exec(addBtn_->mapToGlobal(QPoint(0, addBtn_->height())));
    }

    void notifyRowActivated(int index)
    {
        if (index < 0 || index >= static_cast<int>(def_.dimensions.size()))
            return;
        for (QWidget* p = parentWidget(); p; p = p->parentWidget()) {
            if (auto* panel = qobject_cast<InspectorPanel*>(p)) {
                panel->selectCurveDimension(def_.dimensions[index].type);
                return;
            }
        }
    }

    void removeSelected()
    {
        if (selected_ < 0 || selected_ >= static_cast<int>(def_.dimensions.size()))
            return;
        box_->removeSelectedRow([this] {
            return executeDimensionCommand("remove_attenuation_dimension",
                                           def_.dimensions[selected_].type);
        });
    }

    bool executeDimensionCommand(const char* command, AttenuationDimension type)
    {
        if (!boundItem_)
            return false;
        const ActionResult result = ActionManager::instance().execute(
            QString::fromLatin1(command),
            QJsonObject{
                {"name", def_.name},
                {"attenuation_guid", def_.guid.toString(QUuid::WithoutBraces)},
                {"dimension", QLatin1String(attenuationDimensionName(type))}},
            ActionSource::GUI);
        if (result.success)
            reloadFromItem();
        return result.success;
    }

    bool hasAvailableTypes() const
    {
        for (const auto& opt : kDimensionOptions)
            if (!def_.dimension(opt.type))
                return true;
        return false;
    }

    void reloadFromItem()
    {
        const auto selectedType = selected_ >= 0 && selected_ < def_.dimensions.size()
            ? std::optional(def_.dimensions[selected_].type) : std::nullopt;
        const auto* comp = boundItem_ ? boundItem_->comp<VAttenuation>() : nullptr;
        def_ = comp ? comp->snapshot() : AttenuationSnapshot{};
        rebuildRows();
        for (int index = 0; selectedType && index < def_.dimensions.size(); ++index)
            if (def_.dimensions[index].type == *selectedType) selectRow(index);
    }

    QWidget* buildRow(int index)
    {
        const AttenuationDimensionDef& dim = def_.dimensions[index];
        const DimensionUiOption& ui = uiOptionFor(dim.type);

        auto* row = new QWidget(rows_);
        row->setObjectName(QStringLiteral("listRow"));
        row->setAttribute(Qt::WA_StyledBackground, true);
        DebugTint::tint(row, DebugTint::Role::Row);
        row->setProperty("rowIndex", index);
        row->setProperty("selected", false);
        row->setProperty("lastRow", index == static_cast<int>(def_.dimensions.size()) - 1);

        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);

        auto* number = box_->createRowNumber(row, index);
        h->addWidget(number);

        auto* nameLabel = new QLabel(QObject::tr(ui.label), row);
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        DebugTint::tint(nameLabel, DebugTint::Role::Flex);
        h->addWidget(nameLabel);

        auto* thumbCell = box_->createCell(row);
        DebugTint::tintContainer(thumbCell, DebugTint::Role::ColToggle);
        thumbCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto* thumb = new CurveThumbnail(thumbCell);
        thumb->setCurve(dim.curve, dim.handles, def_.maxDistance, ui);
        thumbCell->addWidget(thumb);
        h->addWidget(thumbCell);

        box_->registerRow(row, {number, nameLabel, thumbCell});
        rowWidgets_.push_back(row);
        return row;
    }

    void rebuildRows()
    {
        auto* rl = static_cast<QVBoxLayout*>(rows_->layout());
        while (QLayoutItem* it = rl->takeAt(0)) {
            if (QWidget* w = it->widget()) {
                w->hide();
                w->deleteLater();
            }
            delete it;
        }
        rowWidgets_.clear();
        selected_ = -1;
        if (removeBtn_)
            removeBtn_->setEnabled(false);
        if (addBtn_)
            addBtn_->setEnabled(boundItem_ && hasAvailableTypes());

        for (int i = 0; i < static_cast<int>(def_.dimensions.size()); ++i)
            rl->addWidget(buildRow(i));
    }

    TableWidget* box_ = nullptr;
    QWidget* rows_ = nullptr;
    QToolButton* addBtn_ = nullptr;
    QToolButton* removeBtn_ = nullptr;
    std::vector<QWidget*> rowWidgets_;
    int selected_ = -1;
    AttenuationSnapshot def_;
    ProjectItem* boundItem_ = nullptr;
};

}

QWidget* DimensionsSectionProvider::build(QWidget* parent)
{
    auto* content = new QWidget(parent);
    auto* cl = new QVBoxLayout(content);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(2);

    auto* list = new DimensionsListWidget(content);
    list->setObjectName(QStringLiteral("dimensionsListWidget"));
    cl->addWidget(list);

    return content;
}

void DimensionsSectionProvider::bind(QWidget* widget, ProjectItem* item)
{
    if (!widget) return;
    widget->setProperty("boundItemPtr",
                        QVariant::fromValue(reinterpret_cast<quintptr>(item)));

    if (QWidget* found = widget->findChild<QWidget*>(QStringLiteral("dimensionsListWidget")))
        static_cast<DimensionsListWidget*>(found)->setBoundItem(item);
}

void registerDimensionsSectionProvider()
{
    InspectorCustomizationRegistry::instance().registerSectionProvider(
        ProjectItem::Type::Attenuation,
        QStringLiteral("dimensions"),
        std::make_unique<DimensionsSectionProvider>());
}

}
