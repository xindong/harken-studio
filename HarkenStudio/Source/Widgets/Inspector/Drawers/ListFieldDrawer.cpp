
#include "ListFieldDrawer.h"
#include "Widgets/Inspector/InspectorLayout.h"

#include "Core/ThemeManager.h"
#include "Core/QtControlSkin.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/LayoutDebugTint.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ParameterValueEditor.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/RefDomainAdapter.h"
#include "Widgets/Selector/SelectorWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include "Widgets/Common/DecimalSpinBox.h"
#include "Widgets/Common/NumericToolTip.h"
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QValidator>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace VNX {

namespace {

constexpr int kFloatEditorWidth = 74 + 7 + 64;
constexpr int kFloatReadOnlyWidth = 64;

constexpr int kLeadColWeight = 220;
constexpr int kTailColWeight = 200;

class ListElementValidator : public QValidator
{
public:
    ListElementValidator(std::function<QString(const QString&)> validate, QObject* parent)
        : QValidator(parent), validate_(std::move(validate)) {}

    State validate(QString& input, int&) const override
    {
        return validate_(input).isEmpty() ? Acceptable : Intermediate;
    }

private:
    std::function<QString(const QString&)> validate_;
};

// Column tint by editor kind, mirroring the Steps palette: stretch column
// yellow (Flex), numeric columns cyan (ColDelay), choice columns indigo
// (ColType), anything else pink (ColToggle).
DebugTint::Role columnTintRole(const PropertyDescriptor& ed, bool stretch)
{
    if (stretch)
        return DebugTint::Role::Flex;
    switch (ed.type) {
        case PropType::Float:
        case PropType::Int:  return DebugTint::Role::ColDelay;
        case PropType::Enum:
        case PropType::Bool: return DebugTint::Role::ColType;
        default:             return DebugTint::Role::ColToggle;
    }
}

// A list that declares a FieldDomain is an anchored table: its integer column
// is an anchor over that domain, and the domain is a window rather than a
// fence. Authoring stays inside the window — the anchor editor is capped to it
// and a new row takes the lowest free anchor in it — while a row the window has
// moved off keeps its place in the value and simply stops being drawn, so
// widening the domain brings it back untouched.
class ListFieldWidget : public QWidget
{
public:
    ListFieldWidget(const PropertyDescriptor& d, QWidget* parent)
        : QWidget(parent), desc_(d)
    {
        hasRowOrder_ = !desc_.elementSchema.empty()
            && desc_.elementSchema.front().type == PropType::Int
            && desc_.elementSchema.front().flags.state;
        for (size_t ci = 0; ci < desc_.elementSchema.size(); ++ci)
            if (desc_.elementSchema[ci].type == PropType::Int
                && desc_.elementSchema[ci].field != QLatin1String("order"))
            {
                anchorCol_ = static_cast<int>(ci);
                break;
            }
        computeDefaultShares();

        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        box_ = new TableWidget(desc_.listPresentation == ListPresentation::Display
            ? TableOptions::display(TableBodySizing::Content) : TableOptions{}, this);
        box_->setSelectionClearHandler([this] { selectRow(-1); });
        box_->setRowSelectionHandler([this](QWidget* row) {
            selectRow(row->property("rowIndex").toInt());
        });
        v->addWidget(box_);
        buildHeader();
        rows_ = box_->rowsWidget();
        if (hasRowOrder_)
            box_->setRowMoveHandler([this](int from, int to) {
                finishPreview();
                if (anchored() && anchorCol_ == 0 && hasRowOrder_) {
                    QList<int> positions;
                    QVariantList entries;
                    for (int i = 0; i < value_.size(); ++i)
                        if (inWindow(value_[i].toMap())) {
                            positions.append(i);
                            entries.append(value_[i]);
                        }
                    const int source = positions.indexOf(from);
                    const int target = positions.indexOf(to);
                    if (source < 0 || target < 0) return;
                    entries.move(source, target);
                    for (int i = 0; i < positions.size(); ++i) {
                        auto entry = entries[i].toMap();
                        entry[anchorField()] = value_[positions[i]].toMap().value(anchorField());
                        value_[positions[i]] = entry;
                    }
                } else {
                    value_.move(from, to);
                }
                rebuildRows();
                const QPointer<ListFieldWidget> self(this);
                notify();
                if (self) self->selectRow(to);
            }, [this](int, int) {
                return canEdit();
            });

        mixedLabel_ = box_->bodyMessage();
        mixedLabel_->setText(tr("Mixed table\nSelect a single platform to edit"));
        mixedLabel_->setObjectName(QStringLiteral("listMixedValue"));

        if (!d.flags.readOnly) {
            footer_ = buildFooter();
        }

        qApp->installEventFilter(this);
    }

    ~ListFieldWidget() override
    {
        finishPreview();
        SelectorWindow::dismiss(this);
    }

    void setBoundItem(ProjectItem* item)
    {
        const uint32_t objectID = item ? item->objectID : 0;
        if (boundObjectID_ != objectID) {
            boundObjectID_ = objectID;
            cancelEdits();
        }
    }

    void setReadOnly(bool readOnly)
    {
        if (runtimeReadOnly_ == readOnly)
            return;
        runtimeReadOnly_ = readOnly;
        if (footer_)
            footer_->setVisible(!isReadOnly());
        cancelEdits();
    }

    bool isReadOnly() const
    {
        return desc_.flags.readOnly || runtimeReadOnly_ || mixed_;
    }

    bool canEdit() const
    {
        return isEnabled() && !isReadOnly() && (!editGuard_ || editGuard_());
    }

    void setEditGuard(std::function<bool()> guard)
    {
        editGuard_ = std::move(guard);
        cancelEdits(true);
    }

    void cancelEdits(bool preserveRows = false)
    {
        if (!preserveRows) box_->clearSelection();
        finishPreview();
        ++selectionGeneration_;
        SelectorWindow::dismiss(this);
        if (preserveRows)
            cancelRowEdits();
        else
            rebuildRows();
    }

    void setMixed(bool mixed)
    {
        if (mixed_ == mixed)
            return;
        mixed_ = mixed;
        setProperty("mixed", mixed);
        mixedLabel_->setVisible(mixed);
        rows_->setVisible(!mixed);
        if (footer_)
            footer_->setVisible(!isReadOnly());
        cancelEdits();
    }

    void setValue(const QVariant& v)
    {
        const bool orderedAnchors = hasRowOrder_ && anchorCol_ == 0;
        const QVariant selectedId = orderedAnchors && selected_ >= 0 && selected_ < value_.size()
            ? value_[selected_].toMap().value(QStringLiteral("id")) : QVariant();
        auto next = v.toList();
        if (hasRowOrder_ && anchorCol_ == 0)
            std::stable_sort(next.begin(), next.end(), [this](const auto& a, const auto& b) {
                return a.toMap().value(anchorField()).toInt() < b.toMap().value(anchorField()).toInt();
            });
        bool sameRows = value_.size() == next.size();
        for (int row = 0; sameRows && row < next.size(); ++row) {
            if (orderedAnchors && value_[row].toMap().value(QStringLiteral("id"))
                != next[row].toMap().value(QStringLiteral("id")))
                sameRows = false;
            for (const auto& field : desc_.elementSchema)
                if (field.type == PropType::ObjectRef
                    && value_[row].toMap().value(field.field) != next[row].toMap().value(field.field))
                    sameRows = false;
        }
        if (value_ != next) {
            cancelAnchorEdit();
            ++selectionGeneration_;
            SelectorWindow::dismiss(this);
        }
        value_ = next;
        if (desc_.flags.readOnly)
            setVisible(!value_.isEmpty());
        if (!sameRows || !refreshRows())
            rebuildRows();
        if (selectedId.isValid()) {
            int selectedRow = -1;
            for (int row = 0; row < value_.size(); ++row)
                if (value_[row].toMap().value(QStringLiteral("id")) == selectedId
                    && inWindow(value_[row].toMap())) {
                    selectedRow = row;
                    break;
                }
            selectRow(selectedRow);
        }
    }

    void setDomain(const FieldDomain& domain)
    {
        const bool sameWindow = domain_
            && qFuzzyCompare(domain_->min + 1.0, domain.min + 1.0)
            && qFuzzyCompare(domain_->max + 1.0, domain.max + 1.0);
        domain_ = domain;
        if (!sameWindow) {
            box_->clearSelection();
            rebuildRows();
        }
    }

    void setOnChange(std::function<void(const QVariant&)> cb) { onChange_ = std::move(cb); }

    void setPreviewCallbacks(std::function<void(const QVariant&)> preview, std::function<void()> finished)
    {
        onPreview_ = std::move(preview);
        onPreviewFinished_ = std::move(finished);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (anchorEdit_ && watched == anchorEdit_) {
            if (event->type() == QEvent::FocusOut
                && anchorEdit_->text() == static_cast<QLabel*>(anchorEdit_->parentWidget())->text()) {
                cancelAnchorEdit();
            } else if (event->type() == QEvent::KeyPress
                && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
                const QPointer<QWidget> number(anchorEdit_->parentWidget());
                cancelAnchorEdit();
                if (number) number->setFocus(Qt::OtherFocusReason);
                return true;
            }
        } else if (anchorEdit_ && watched == anchorEdit_->parentWidget()
                   && event->type() == QEvent::Resize) {
            anchorEdit_->setGeometry(anchorEdit_->parentWidget()->contentsRect());
        }
        if (event->type() == QEvent::MouseButtonDblClick
            && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton
            && anchored() && anchorCol_ == 0 && hasRowOrder_ && canEdit()) {
            auto* number = qobject_cast<QLabel*>(watched);
            if (number && number->property("tableRowNumber").toBool()
                && std::find(rowWidgets_.begin(), rowWidgets_.end(), number->parentWidget())
                    != rowWidgets_.end()) {
                beginAnchorEdit(number);
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonPress && isEnabled() && !mixed_
            && watched->isWidgetType())
        {
            auto* w = static_cast<QWidget*>(watched);
            QWidget* rowHost = w;
            while (rowHost && rowHost->parentWidget() != rows_)
                rowHost = rowHost->parentWidget();
            if (rowHost && rowHost->property("rowIndex").isValid())
            {
                selectRow(rowHost->property("rowIndex").toInt());
            }
            else if (selected_ >= 0 && w != this && !isAncestorOf(w)
                     && w->window() == window())
            {
                selectRow(-1);
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    QWidget* buildHeader()
    {
        QList<TableColumn> columns;
        for (size_t i = 0; i < desc_.elementSchema.size(); ++i) {
            const auto& field = desc_.elementSchema[i];
            columns.append({field.field, field.label.isEmpty() ? field.field : field.label,
                            defaultShares_[i]});
        }
        box_->setColumns(desc_.field, columns);
        applyInspectorTableDescriptor(box_, desc_);
        return box_->header();
    }

    void computeDefaultShares()
    {
        const int last = static_cast<int>(desc_.elementSchema.size()) - 1;
        defaultShares_.assign(desc_.elementSchema.size(), 0);
        int sum = 0;
        for (int ci = 0; ci <= last; ++ci)
        {
            if (hasRowOrder_ && ci == 0) continue;
            const auto& ed = desc_.elementSchema[ci];
            int weight = 0;
            if (ci == last)
                weight = kTailColWeight;
            else if (ci == (hasRowOrder_ ? 1 : 0))
                weight = kLeadColWeight;
            else if (ed.type == PropType::Float)
                weight = ((ed.flags.readOnly || desc_.flags.readOnly)
                              ? kFloatReadOnlyWidth : kFloatEditorWidth)
                    + 2 * ThemeManager::instance().metrics().table.cellPaddingX;
            defaultShares_[ci] = weight;
            sum += weight;
        }
        for (int& s : defaultShares_)
            s = s * 1000 / sum;
    }

    QWidget* buildFooter()
    {
        auto* foot = box_->footer();
        DebugTint::tint(foot, DebugTint::Role::Foot);
        addBtn_ = foot->addButton();
        addBtn_->setObjectName(QStringLiteral("listAdd_") + desc_.field);
        removeBtn_ = foot->removeButton();
        removeBtn_->setObjectName(QStringLiteral("listRemoveSelected_") + desc_.field);

        QObject::connect(addBtn_, &QToolButton::clicked, this, [this]() { addElement(); });
        QObject::connect(removeBtn_, &QToolButton::clicked, this, [this]() {
            if (!canEdit() || selected_ < 0 || selected_ >= value_.size()) return;
            box_->removeSelectedRow([this] {
                const auto previousSize = value_.size();
                value_.removeAt(selected_);
                rebuildRows();
                const QPointer<ListFieldWidget> self(this);
                notify();
                return self && self->value_.size() < previousSize;
            });
        });

        return foot;
    }

    void selectRow(int index)
    {
        if (selected_ == index)
            return;
        selected_ = index;
        for (QWidget* row : rowWidgets_)
        {
            row->setProperty("selected", row->property("rowIndex").toInt() == index);
            row->style()->unpolish(row);
            row->style()->polish(row);
        }
        if (removeBtn_)
            removeBtn_->setEnabled(canEdit() && index >= 0 && index < value_.size());
    }

    bool anchored() const { return domain_.has_value() && anchorCol_ >= 0; }

    QString anchorField() const { return desc_.elementSchema[anchorCol_].field; }

    QString anchorError(int row, const QString& text) const
    {
        bool ok = false;
        const int anchor = text.toInt(&ok);
        if (!ok || text != text.trimmed())
            return tr("Index must be an integer");
        const double low = std::max(0.0, std::ceil(domain_->min));
        const double high = std::min(16777216.0, std::floor(domain_->max));
        if (anchor < low || anchor > high)
            return tr("Index must be within the current range (%1 to %2)").arg(low).arg(high);
        for (int i = 0; i < value_.size(); ++i)
            if (i != row && value_[i].toMap().value(anchorField()).toInt() == anchor)
                return tr("Index %1 is already in use").arg(anchor);
        return {};
    }

    void cancelAnchorEdit()
    {
        if (!anchorEdit_) return;
        auto* edit = anchorEdit_.data();
        anchorEdit_.clear();
        const QSignalBlocker blocker(edit);
        edit->hide();
        edit->deleteLater();
        box_->setColumnMinimumWidth(0, 0);
    }

    void beginAnchorEdit(QLabel* number)
    {
        cancelAnchorEdit();
        const int row = number->parentWidget()->property("rowIndex").toInt();
        selectRow(row);
        auto* edit = new QLineEdit(number->text(), number);
        anchorEdit_ = edit;
        edit->setObjectName(QStringLiteral("listAnchorEditor"));
        edit->setAlignment(Qt::AlignCenter);
        edit->ensurePolished();
        const int digits = QString::number(static_cast<int>(
            std::clamp(std::floor(domain_->max), 0.0, 16777216.0))).size();
        const auto margins = number->contentsMargins();
        box_->setColumnMinimumWidth(0, edit->minimumSizeHint().width()
            + edit->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('8')))
            + margins.left() + margins.right());
        box_->refreshLayout();
        edit->setGeometry(number->contentsRect());
        edit->setValidator(new ListElementValidator([this, row](const QString& text) {
            return anchorError(row, text);
        }, edit));
        QObject::connect(edit, &QLineEdit::textChanged, this, [this, edit, row] {
            const QString error = anchorError(row, edit->text());
            edit->setToolTip(error);
            QtControlSkin::setSemanticProperty(edit, "inputInvalid", !error.isEmpty());
        });
        QObject::connect(edit, &QLineEdit::editingFinished, this,
            [this, edit, row, generation = rowGeneration_,
             id = value_[row].toMap().value(QStringLiteral("id"))] {
                if (anchorEdit_ != edit || generation != rowGeneration_ || !canEdit()
                    || row >= value_.size()
                    || value_[row].toMap().value(QStringLiteral("id")) != id
                    || !anchorError(row, edit->text()).isEmpty()) return;
                const int anchor = edit->text().toInt();
                const bool restoreFocus = edit->hasFocus();
                selectRow(row);
                cancelAnchorEdit();
                auto entry = value_[row].toMap();
                const bool changed = entry.value(anchorField()).toInt() != anchor;
                if (changed) {
                    entry[anchorField()] = anchor;
                    auto next = value_;
                    next[row] = entry;
                    setValue(next);
                }
                if (restoreFocus)
                    for (size_t i = 0; i < rowWidgets_.size(); ++i)
                        if (rowWidgets_[i]->property("selected").toBool()) {
                            rowEditors_[i][0]->setFocus(Qt::OtherFocusReason);
                            break;
                        }
                if (changed) notify();
            });
        edit->show();
        edit->setFocus(Qt::MouseFocusReason);
        edit->selectAll();
    }

    bool inWindow(const QVariantMap& elem) const
    {
        if (!anchored())
            return true;
        const double anchor = elem.value(anchorField()).toDouble();
        return anchor >= domain_->min && anchor <= domain_->max;
    }

    std::optional<int> freeAnchor() const
    {
        if (!anchored())
            return std::nullopt;
        QSet<int> used;
        for (const QVariant& e : value_)
            used.insert(e.toMap().value(anchorField()).toInt());
        const bool labelIndex = anchorCol_ == 0 && hasRowOrder_;
        const double low = labelIndex ? std::max(0.0, std::ceil(domain_->min)) : std::ceil(domain_->min);
        const double upper = labelIndex ? std::min(16777216.0, std::floor(domain_->max)) : std::floor(domain_->max);
        if (low > upper) return std::nullopt;
        const int high = static_cast<int>(upper);
        for (int anchor = static_cast<int>(low); anchor <= high; ++anchor)
            if (!used.contains(anchor))
                return anchor;
        return std::nullopt;
    }

    QVariantMap defaultElement() const
    {
        QVariantMap elem;
        for (const auto& ed : desc_.elementSchema)
            if (!ed.flags.state) elem[ed.field] = ed.def;
        if (const std::optional<int> anchor = freeAnchor())
            elem[anchorField()] = *anchor;
        if (hasRowOrder_ && anchorCol_ == 0)
            elem[QStringLiteral("id")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        return elem;
    }

    void addElement()
    {
        if (!canEdit() || (anchored() && !freeAnchor()))
            return;
        const PropertyDescriptor* refEd = nullptr;
        for (const auto& ed : desc_.elementSchema)
            if (ed.type == PropType::ObjectRef && ed.refFilter != 0) { refEd = &ed; break; }

        if (!refEd)
        {
            value_.append(defaultElement());
            if (anchored() && anchorCol_ == 0 && hasRowOrder_)
                std::stable_sort(value_.begin(), value_.end(), [this](const auto& a, const auto& b) {
                    return a.toMap().value(anchorField()).toInt() < b.toMap().value(anchorField()).toInt();
                });
            rebuildRows();
            notify();
            return;
        }

        QPointer<ListFieldWidget> self(this);
        SelectorRequest req;
        req.title = QObject::tr("Add %1").arg(desc_.label.isEmpty() ? desc_.field : desc_.label);
        req.filterType = static_cast<ProjectItem::Type>(refEd->refFilter);
        if (refEd->refSelectable || desc_.validateElement)
            req.isSelectable = [self, pred = refEd->refSelectable, field = refEd->field](uint32_t id) {
                return self && (!pred || (self->boundObjectID_ != 0 && pred(self->boundObjectID_, id)))
                    && self->elementError(self->value_.size(), field, id).isEmpty();
            };
        const uint64_t generation = ++selectionGeneration_;
        req.onSelected = [self, generation, ownerAtOpen = boundObjectID_,
                          filter = req.filterType, predicate = req.isSelectable,
                          field = refEd->field](uint32_t id) {
            if (!self || self->boundObjectID_ != ownerAtOpen
                || self->selectionGeneration_ != generation || !self->canEdit())
                return;
            const auto& adapter = refDomainAdapter(RefDomain::ProjectObject);
            if (id == 0 || !adapter.exists(id) || !adapter.matchesFilter(id, {filter, {}})
                || (predicate && !predicate(id)))
                return;
            QVariantMap elem = self->defaultElement();
            elem[field] = id;
            self->value_.append(elem);
            self->rebuildRows();
            self->notify();
        };
        SelectorWindow::open(req, this);
    }

    void notify()
    {
        if (!canEdit())
            return;
        const auto onChange = onChange_;
        if (onChange)
            onChange(QVariant(value_));
    }

    void applyElementChange(uint64_t generation, int row, const QString& field,
                            const QVariant& v)
    {
        if (generation != rowGeneration_ || !canEdit()
            || row < 0 || row >= value_.size()) return;
        selectRow(row);
        QVariantMap elem = value_[row].toMap();
        elem[field] = v;
        value_[row] = elem;
        notify();
    }

    QString elementError(int row, const QString& field, const QVariant& value) const
    {
        if (!desc_.validateElement || row < 0 || row > value_.size()) return {};
        auto candidate = value_;
        auto element = row == candidate.size() ? defaultElement() : candidate[row].toMap();
        element[field] = value;
        if (row == candidate.size())
            candidate.append(element);
        else
            candidate[row] = element;
        return desc_.validateElement(candidate, row, field);
    }

    void refreshInputValidation(QLineEdit* edit, int row, const QString& field)
    {
        const QString error = elementError(row, field, edit->text());
        edit->setToolTip(error);
        QtControlSkin::setSemanticProperty(edit, "inputInvalid", !error.isEmpty());
    }

    void previewElementChange(uint64_t generation, int row, const QString& field, double value)
    {
        if (!onPreview_ || generation != rowGeneration_ || !canEdit()
            || row < 0 || row >= value_.size()) return;
        auto preview = value_;
        auto entry = preview[row].toMap();
        entry[field] = value;
        preview[row] = entry;
        previewActive_ = true;
        const auto callback = onPreview_;
        callback(preview);
    }

    void finishPreview()
    {
        if (!previewActive_) return;
        previewActive_ = false;
        const auto callback = onPreviewFinished_;
        if (callback) callback();
    }

    bool refreshRows()
    {
        std::vector<int> visibleRows;
        for (int i = 0; i < value_.size(); ++i)
            if (inWindow(value_[i].toMap()))
                visibleRows.push_back(i);
        if (visibleRows.size() != rowWidgets_.size())
            return false;
        for (size_t i = 0; i < visibleRows.size(); ++i)
            if (rowWidgets_[i]->property("rowIndex").toInt() != visibleRows[i])
                return false;

        for (size_t i = 0; i < visibleRows.size(); ++i)
        {
            const QVariantMap elem = value_[visibleRows[i]].toMap();
            for (size_t ci = 0; ci < desc_.elementSchema.size(); ++ci)
            {
                if (hasRowOrder_ && ci == 0) {
                    if (anchorCol_ == 0)
                        static_cast<QLabel*>(rowEditors_[i][ci])->setText(elem.value(anchorField()).toString());
                    continue;
                }
                const auto& ed = desc_.elementSchema[ci];
                const QVariant v = elem.value(ed.field, ed.def);
                QWidget* editor = rowEditors_[i][ci];
                const QSignalBlocker blocker(editor);
                IFieldDrawer* drawer = ed.type != PropType::Float && ed.type != PropType::List
                    ? InspectorCustomizationRegistry::instance().fieldDrawer(ed.type) : nullptr;
                if (drawer)
                    drawer->bind(editor, v, nullptr);
                else if (auto* slider = qobject_cast<ParameterSlider*>(editor)) {
                    if (!qFuzzyCompare(slider->value() + 1.0, v.toDouble() + 1.0))
                        slider->setValue(v.toDouble());
                } else if (auto* doubleSpin = qobject_cast<QDoubleSpinBox*>(editor))
                    doubleSpin->setValue(v.toDouble());
                else if (auto* intSpin = qobject_cast<QSpinBox*>(editor))
                    intSpin->setValue(v.toInt());
                else if (auto* check = qobject_cast<QCheckBox*>(editor))
                    check->setChecked(v.toBool());
                else if (auto* combo = qobject_cast<QComboBox*>(editor))
                    combo->setCurrentIndex(combo->findData(v));
                else if (auto* edit = qobject_cast<QLineEdit*>(editor)) {
                    if (edit->text() != v.toString()
                        && !(edit->hasFocus() && edit->property("inputInvalid").toBool()))
                        edit->setText(v.toString());
                    if (desc_.validateElement)
                        refreshInputValidation(edit, visibleRows[i], ed.field);
                } else if (auto* label = qobject_cast<QLabel*>(editor))
                    label->setText(ed.type == PropType::Float
                        ? formatDecimal(v.toDouble(), ed.decimals)
                        : v.toString());
            }
        }
        if (addBtn_)
            addBtn_->setEnabled(canEdit() && (!anchored() || freeAnchor().has_value()));
        return true;
    }

    void cancelRowEdits()
    {
        cancelAnchorEdit();
        for (QWidget* row : rowWidgets_) {
            for (auto* reference : row->findChildren<ObjectRefWidget*>())
                reference->cancelPendingSelection();
            for (auto* editor : row->findChildren<ParameterValueEditor*>())
                editor->cancelPendingInput();
            for (auto* slider : row->findChildren<ParameterSlider*>())
                slider->cancelPendingInput();
        }
    }

    void rebuildRows()
    {
        finishPreview();
        ++rowGeneration_;
        cancelRowEdits();
        for (QWidget* row : rowWidgets_) {
            if (auto* focus = QApplication::focusWidget(); focus && row->isAncestorOf(focus))
                focus->clearFocus();
        }
        auto* rl = static_cast<QVBoxLayout*>(rows_->layout());
        while (QLayoutItem* it = rl->takeAt(0))
        {
            if (QWidget* w = it->widget())
            {
                w->hide();
                w->deleteLater();
            }
            delete it;
        }
        rowWidgets_.clear();
        rowEditors_.clear();
        selected_ = -1;
        if (removeBtn_)
            removeBtn_->setEnabled(false);
        if (addBtn_)
            addBtn_->setEnabled(canEdit() && (!anchored() || freeAnchor().has_value()));

        if (mixed_)
            return;

        for (int i = 0; i < value_.size(); ++i)
        {
            const QVariantMap elem = value_[i].toMap();
            if (!inWindow(elem))
                continue;

            auto* row = new QWidget(rows_);
            row->setObjectName(QStringLiteral("listRow"));
            row->setAttribute(Qt::WA_StyledBackground, true);
            DebugTint::tint(row, DebugTint::Role::Row);
            row->setProperty("rowIndex", i);
            row->setProperty("selected", false);
            row->setProperty("lastRow", false);

            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(0);

            const int last = static_cast<int>(desc_.elementSchema.size()) - 1;
            const auto& shares = defaultShares_;
            QList<QWidget*> cells;
            std::vector<QWidget*> editors;
            for (int ci = 0; ci <= last; ++ci)
            {
                if (hasRowOrder_ && ci == 0) {
                    auto* number = box_->createRowNumber(row, i, anchorCol_ == 0
                        ? std::optional<int>(elem.value(anchorField()).toInt()) : std::nullopt);
                    number->setEnabled(canEdit() || desc_.flags.readOnly);
                    if (anchored() && anchorCol_ == 0 && canEdit())
                        number->setToolTip(tr("Double-click to edit index; drag to move row"));
                    h->addWidget(number);
                    cells.append(number);
                    editors.push_back(number);
                    continue;
                }
                const auto& ed = desc_.elementSchema[ci];
                const bool stretch = ci == last;
                auto* cell = box_->createCell(row);
                QWidget* editor =
                    buildElementEditor(ed, i, elem.value(ed.field, ed.def), cell, stretch);
                editors.push_back(editor);
                // A width-capped editor in the stretch column pins to the right
                // edge; expanding editors fill the cell instead.
                DebugTint::tintContainer(cell, columnTintRole(ed, stretch));
                const bool alignRight = stretch && editor->maximumWidth() < QWIDGETSIZE_MAX;
                cell->addWidget(editor, alignRight ? 0 : 1,
                                alignRight ? Qt::AlignRight : Qt::Alignment{});
                if (auto* slider = qobject_cast<ParameterSlider*>(editor)) {
                    auto* valueEditor = new ParameterValueEditor(
                        slider->binding(), &ThemeManager::instance(), cell);
                    valueEditor->setCompact(true);
                    valueEditor->setEnabled(slider->isEnabled());
                    cell->addWidget(valueEditor);
                }
                if (shares[ci] > 0)
                {
                    // Proportional cells must stay shrinkable below their
                    // content's minimum size hint, or one wide column inflates
                    // the row's minimum width and pushes the table past the
                    // panel edge.
                    cell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                }
                h->addWidget(cell);
                cells.append(cell);
            }

            box_->registerRow(row, cells);
            rl->addWidget(row);
            rowWidgets_.push_back(row);
            rowEditors_.push_back(std::move(editors));
        }

        if (!rowWidgets_.empty())
            rowWidgets_.back()->setProperty("lastRow", true);
    }

    QWidget* buildElementEditor(const PropertyDescriptor& edIn, int row, const QVariant& v,
                                QWidget* parent, bool stretch)
    {
        PropertyDescriptor ed = edIn;
        ed.flags.readOnly = ed.flags.readOnly || isReadOnly();
        const uint64_t generation = rowGeneration_;

        if (ed.type == PropType::Float)
            return buildFloatEditor(ed, row, v, parent, stretch);

        if (ed.type != PropType::List)
        {
            if (IFieldDrawer* drawer = InspectorCustomizationRegistry::instance().fieldDrawer(ed.type))
            {
                QWidget* w = drawer->build(ed, parent);
                drawer->bind(w, v, nullptr);
                drawer->connectChanged(w, [this, generation, row, field = ed.field](const QVariant& nv) {
                    applyElementChange(generation, row, field, nv);
                });
                if (auto* orw = qobject_cast<ObjectRefWidget*>(w))
                {
                    orw->setCompact(true);
                    orw->setCompactPickable(true);
                    QPointer<ListFieldWidget> self(this);
                    orw->setEditGuard([self, generation] {
                        return self && self->rowGeneration_ == generation && self->canEdit();
                    });
                    if (ed.refSelectable || desc_.validateElement)
                        orw->setIsSelectable([self, pred = ed.refSelectable, row, field = ed.field](uint32_t id) {
                            return self && (!pred || (self->boundObjectID_ != 0 && pred(self->boundObjectID_, id)))
                                && self->elementError(row, field, id).isEmpty();
                        });
                }
                return w;
            }
        }

        auto finalize = [&ed](QWidget* w) {
            if (ed.flags.readOnly) w->setEnabled(false);
            return w;
        };

        switch (ed.type)
        {
            case PropType::Int:
            {
                auto* spin = new QSpinBox(parent);
                if (anchored() && ed.field == anchorField())
                    spin->setRange(static_cast<int>(std::ceil(domain_->min)),
                                   static_cast<int>(std::floor(domain_->max)));
                else
                    spin->setRange(ed.min.isValid() ? ed.min.toInt() : std::numeric_limits<int>::lowest(),
                                   ed.max.isValid() ? ed.max.toInt() : std::numeric_limits<int>::max());
                new NumericToolTip(spin, ed.unit);
                spin->setValue(v.toInt());
                QObject::connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                    [this, generation, row, field = ed.field](int nv) {
                        applyElementChange(generation, row, field, nv);
                    });
                return finalize(spin);
            }
            case PropType::Bool:
            {
                auto* check = new QCheckBox(parent);
                check->setChecked(v.toBool());
                QObject::connect(check, &QCheckBox::toggled, this,
                    [this, generation, row, field = ed.field](bool nv) {
                        applyElementChange(generation, row, field, nv);
                    });
                return finalize(check);
            }
            case PropType::Enum:
            {
                auto* combo = new QComboBox(parent);
                for (const auto& o : ed.options)
                    combo->addItem(o.text, o.value);
                {
                    QSignalBlocker block(combo);
                    const int idx = combo->findData(v);
                    if (idx >= 0) combo->setCurrentIndex(idx);
                }
                QObject::connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [this, generation, row, field = ed.field, combo](int idx) {
                        if (idx < 0) return;
                        applyElementChange(generation, row, field, combo->itemData(idx));
                    });
                return finalize(combo);
            }
            case PropType::String:
            {
                auto* edit = new QLineEdit(v.toString(), parent);
                if (desc_.validateElement) {
                    edit->setValidator(new ListElementValidator(
                        [this, row, field = ed.field](const QString& text) {
                            return elementError(row, field, text);
                        }, edit));
                    QObject::connect(edit, &QLineEdit::textChanged, this,
                        [this, row, field = ed.field, edit] {
                            refreshInputValidation(edit, row, field);
                        });
                    refreshInputValidation(edit, row, ed.field);
                }
                QObject::connect(edit, &QLineEdit::editingFinished, this,
                    [this, generation, row, field = ed.field, edit]() {
                        if (!edit->hasAcceptableInput()) return;
                        applyElementChange(generation, row, field, edit->text());
                    });
                return finalize(edit);
            }
            default:
                return new QLabel(v.toString(), parent);
        }
    }

    QWidget* buildFloatEditor(const PropertyDescriptor& ed, int row, const QVariant& v,
                              QWidget* parent, bool stretch)
    {
        const uint64_t generation = rowGeneration_;
        if (ed.flags.readOnly)
        {
            auto* label = new QLabel(formatDecimal(v.toDouble(), ed.decimals), parent);
            label->setToolTip(QStringLiteral("%1 %2").arg(label->text(), ed.unit.trimmed()).trimmed());
            label->setProperty("roValue", true);
            label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            return label;
        }

        const bool ranged = ed.min.isValid() && ed.max.isValid();
        if (!ranged)
        {
            auto* spin = new DecimalSpinBox(parent);
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setRange(ed.min.isValid() ? ed.min.toDouble() : std::numeric_limits<double>::lowest(),
                           ed.max.isValid() ? ed.max.toDouble() : std::numeric_limits<double>::max());
            spin->setDecimals(ed.decimals);
            spin->setKeyboardTracking(false);
            new NumericToolTip(spin, ed.unit);
            spin->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            if (stretch)
                spin->setFixedWidth(kFloatReadOnlyWidth);
            QtControlSkin::setSemanticProperty(spin, "fieldRole",
                                              QStringLiteral("inspectorValue"));
            spin->setValue(v.toDouble());
            QObject::connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, generation, row, field = ed.field](double nv) {
                    applyElementChange(generation, row, field, nv);
                });
            return spin;
        }

        auto* slider = new ParameterSlider(QString(), parent);
        slider->setPresenterOverride(ParameterPresenterId::HorizontalSlider);
        slider->setRange(ed.min.toDouble(), ed.max.toDouble(), ed.decimals);
        if (!ed.unit.isEmpty()) slider->setSuffix(ed.unit);
        slider->setValue(v.toDouble());

        QObject::connect(slider, &ParameterSlider::valueChanged, this,
            [this, generation, row, field = ed.field](double nv) {
                applyElementChange(generation, row, field, nv);
            });
        QObject::connect(slider, &ParameterSlider::interactivePreview, this,
            [this, generation, row, field = ed.field](double value) {
                previewElementChange(generation, row, field, value);
            });
        QObject::connect(slider, &ParameterSlider::interactiveEditEnd, this,
            [this, generation, row, field = ed.field](double, double end, bool committed) {
                if (generation != rowGeneration_) return;
                const auto self = QPointer<ListFieldWidget>(this);
                if (committed)
                    applyElementChange(generation, row, field, end);
                if (self && generation == self->rowGeneration_) self->finishPreview();
            });

        return slider;
    }

    PropertyDescriptor desc_;
    TableWidget* box_ = nullptr;
    QWidget* rows_ = nullptr;
    QLabel* mixedLabel_ = nullptr;
    QWidget* footer_ = nullptr;
    QToolButton* addBtn_ = nullptr;
    QToolButton* removeBtn_ = nullptr;
    std::vector<QWidget*> rowWidgets_;
    std::vector<std::vector<QWidget*>> rowEditors_;
    std::vector<int> defaultShares_;
    std::optional<FieldDomain> domain_;
    QPointer<QLineEdit> anchorEdit_;
    int anchorCol_ = -1;
    int selected_ = -1;
    bool hasRowOrder_ = false;
    QVariantList value_;
    uint32_t boundObjectID_ = 0;
    uint64_t selectionGeneration_ = 0;
    uint64_t rowGeneration_ = 0;
    bool runtimeReadOnly_ = false;
    bool mixed_ = false;
    bool previewActive_ = false;
    std::function<bool()> editGuard_;
    std::function<void(const QVariant&)> onChange_;
    std::function<void(const QVariant&)> onPreview_;
    std::function<void()> onPreviewFinished_;
};

}

QWidget* ListFieldDrawer::build(const PropertyDescriptor& d, QWidget* parent)
{
    auto* w = new ListFieldWidget(d, parent);
    w->setObjectName(QStringLiteral("prop_") + d.field);
    return w;
}

void ListFieldDrawer::bind(QWidget* widget, const QVariant& value, ProjectItem* item)
{
    if (auto* w = dynamic_cast<ListFieldWidget*>(widget))
    {
        w->setBoundItem(item);
        w->setValue(value);
    }
}

void ListFieldDrawer::connectChanged(QWidget* widget, std::function<void(const QVariant&)> onChange)
{
    if (auto* w = dynamic_cast<ListFieldWidget*>(widget))
        w->setOnChange(std::move(onChange));
}

void ListFieldDrawer::applyDomain(QWidget* widget, const FieldDomain& domain)
{
    if (auto* w = dynamic_cast<ListFieldWidget*>(widget))
        w->setDomain(domain);
}

void ListFieldDrawer::setWidgetReadOnly(QWidget* widget, bool readOnly)
{
    if (auto* w = dynamic_cast<ListFieldWidget*>(widget))
        w->setReadOnly(readOnly);
}

void ListFieldDrawer::setWidgetEditGuard(QWidget* widget, std::function<bool()> guard)
{
    if (auto* w = dynamic_cast<ListFieldWidget*>(widget))
        w->setEditGuard(std::move(guard));
}

void ListFieldDrawer::setWidgetPreviewCallbacks(QWidget* widget,
    std::function<void(const QVariant&)> preview, std::function<void()> finished)
{
    if (auto* w = dynamic_cast<ListFieldWidget*>(widget))
        w->setPreviewCallbacks(std::move(preview), std::move(finished));
}

void ListFieldDrawer::cancelWidgetEdits(QWidget* widget, bool preserveRows)
{
    if (auto* w = dynamic_cast<ListFieldWidget*>(widget))
        w->cancelEdits(preserveRows);
}

void ListFieldDrawer::setWidgetMixed(QWidget* widget, bool mixed)
{
    if (auto* w = dynamic_cast<ListFieldWidget*>(widget))
        w->setMixed(mixed);
}

}
