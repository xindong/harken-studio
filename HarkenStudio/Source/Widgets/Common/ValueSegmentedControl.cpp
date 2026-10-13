#include "ValueSegmentedControl.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QToolButton>

#include <cmath>

namespace VNX {

namespace {

// Segment values arrive from the schema as plain ints while the field may hand
// back a qulonglong, so identity is compared numerically rather than by variant
// type -- the same widening the combo and segmented rows already do.
bool sameValue(const QVariant& a, const QVariant& b)
{
    if (a == b) return true;
    bool okA = false;
    bool okB = false;
    const double da = a.toDouble(&okA);
    const double db = b.toDouble(&okB);
    return okA && okB && qFuzzyCompare(da + 1.0, db + 1.0);
}

QHash<QString, QVariant>& sessionMemory()
{
    static QHash<QString, QVariant> memory;
    return memory;
}

}

ValueSegmentedControl::ValueSegmentedControl(QWidget* parent)
    : QWidget(parent)
{
    setProperty("segmentedControl", true);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void ValueSegmentedControl::setSegments(const std::vector<SegmentSpec>& segments)
{
    numericSelection_.clear();
    auto* layout = static_cast<QHBoxLayout*>(this->layout());
    editor_ = nullptr;
    for (Segment& segment : segments_)
        delete segment.button;
    segments_.clear();
    while (QLayoutItem* item = layout->takeAt(0))
        delete item;

    const int count = static_cast<int>(segments.size());
    for (int i = 0; i < count; ++i)
    {
        auto* button = new QToolButton(this);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setCursor(Qt::PointingHandCursor);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        button->setProperty("segmentedControl", true);
        const QString position = count == 1
            ? QStringLiteral("only")
            : (i == 0 ? QStringLiteral("first")
                      : (i == count - 1 ? QStringLiteral("last")
                                        : QStringLiteral("joined")));
        button->setProperty("segmentPosition", position);
        if (!segments[static_cast<size_t>(i)].numeric)
            button->setText(segments[static_cast<size_t>(i)].text);
        connect(button, &QToolButton::clicked, this, [this, i] { onSegmentClicked(i); });
        layout->addWidget(button, 1);
        segments_.push_back({segments[static_cast<size_t>(i)], button});
    }

    const int numeric = numericIndex();
    if (numeric >= 0)
    {
        QToolButton* host = segments_[static_cast<size_t>(numeric)].button;
        host->installEventFilter(this);
        host->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(host, &QToolButton::customContextMenuRequested,
                this, [this, host](const QPoint& pos) {
            QMenu menu(this);
            menu.addAction(tr("Edit value…"), this, [this] { beginNumericEdit(); });
            menu.exec(host->mapToGlobal(pos));
        });

        auto* hostLayout = new QHBoxLayout(host);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        editor_ = new QLineEdit(host);
        editor_->setProperty("fieldRole", QStringLiteral("segmentInline"));
        editor_->setAlignment(Qt::AlignCenter);
        // The editor lives inside the segment, so it must not be what decides
        // how wide the segment is -- the segments stay evenly split.
        editor_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        editor_->hide();
        editor_->installEventFilter(this);
        connect(editor_, &QLineEdit::textEdited, this, [this](const QString& text) {
            if (editing_)
                emit numericPreviewed(clampNumeric(text));
        });
        hostLayout->addWidget(editor_);
    }

    refreshSegments();
}

void ValueSegmentedControl::setContext(const QString& context)
{
    if (context_ == context)
        return;
    context_ = context;
    numericSelection_.clear();
    refreshSegments();
}

void ValueSegmentedControl::setValue(const QVariant& value)
{
    value_ = value;
    // Live previews flow back through here while the inline editor is open.
    // Recording them would let an abandoned edit outlive its own Escape, so the
    // count is only remembered once it is committed or arrives from outside.
    if (!editing_ && holdsNumeric(value))
        rememberNumeric(value);
    refreshSegments();
}

void ValueSegmentedControl::clearSessionMemory()
{
    sessionMemory().clear();
}

int ValueSegmentedControl::numericIndex() const
{
    for (size_t i = 0; i < segments_.size(); ++i)
        if (segments_[i].spec.numeric)
            return static_cast<int>(i);
    return -1;
}

bool ValueSegmentedControl::holdsNumeric(const QVariant& value) const
{
    const int index = numericIndex();
    if (index < 0)
        return false;
    if (!editing_ && !sameValue(numericSelection_, value))
        for (const Segment& segment : segments_)
            if (!segment.spec.numeric && sameValue(segment.spec.value, value))
                return false;

    const SegmentSpec& spec = segments_[static_cast<size_t>(index)].spec;
    bool ok = false;
    const double raw = value.toDouble(&ok);
    if (!ok)
        return false;
    if (spec.numericMin.isValid() && raw < spec.numericMin.toDouble())
        return false;
    if (spec.numericMax.isValid() && raw > spec.numericMax.toDouble())
        return false;
    return true;
}

QVariant ValueSegmentedControl::numericFallback() const
{
    const int index = numericIndex();
    if (index < 0)
        return {};
    if (!context_.isEmpty())
    {
        const auto it = sessionMemory().constFind(context_);
        if (it != sessionMemory().constEnd())
            return *it;
    }
    return segments_[static_cast<size_t>(index)].spec.value;
}

QVariant ValueSegmentedControl::clampNumeric(const QVariant& value) const
{
    const int index = numericIndex();
    if (index < 0)
        return value;
    const SegmentSpec& spec = segments_[static_cast<size_t>(index)].spec;
    const double floorValue = spec.numericMin.isValid() ? spec.numericMin.toDouble()
                                                        : spec.value.toDouble();
    bool ok = false;
    double raw = value.toDouble(&ok);
    if (!ok)
        return QVariant(static_cast<qlonglong>(floorValue));

    raw = std::floor(raw);
    if (raw < floorValue)
        raw = floorValue;
    if (spec.numericMax.isValid() && raw > spec.numericMax.toDouble())
        raw = spec.numericMax.toDouble();
    return QVariant(static_cast<qlonglong>(raw));
}

void ValueSegmentedControl::rememberNumeric(const QVariant& value)
{
    if (!context_.isEmpty())
        sessionMemory().insert(context_, value);
}

void ValueSegmentedControl::refreshSegments()
{
    const bool onNumeric = holdsNumeric(value_);
    for (const Segment& segment : segments_)
    {
        const bool checked = segment.spec.numeric
            ? onNumeric
            : !onNumeric && sameValue(segment.spec.value, value_);
        segment.button->blockSignals(true);
        segment.button->setChecked(checked);
        segment.button->blockSignals(false);
    }

    const int index = numericIndex();
    if (index < 0 || editing_)
        return;
    const QVariant shown = onNumeric ? value_ : numericFallback();
    segments_[static_cast<size_t>(index)].button->setText(
        QString::number(shown.toLongLong()));
}

void ValueSegmentedControl::onSegmentClicked(int index)
{
    if (editing_ || index < 0 || index >= static_cast<int>(segments_.size()))
        return;
    const SegmentSpec& spec = segments_[static_cast<size_t>(index)].spec;

    // Clicking the segment the field already sits on is not an edit. For the
    // numeric segment that also keeps a stray single click from overwriting the
    // count on the way to a double-click.
    if (spec.numeric)
    {
        if (holdsNumeric(value_))
        {
            refreshSegments();
            return;
        }
        numericSelection_ = clampNumeric(numericFallback());
        refreshSegments();
        emit segmentPicked(numericSelection_);
        return;
    }

    numericSelection_.clear();
    refreshSegments();
    if (sameValue(spec.value, value_))
        return;
    emit segmentPicked(spec.value);
}

void ValueSegmentedControl::beginNumericEdit()
{
    const int index = numericIndex();
    if (editing_ || index < 0 || !editor_)
        return;

    QToolButton* host = segments_[static_cast<size_t>(index)].button;
    const QVariant shown = holdsNumeric(value_) ? value_ : numericFallback();
    editing_ = true;
    host->setText(QString());
    editor_->setText(QString::number(shown.toLongLong()));
    editor_->show();
    // The session must exist before focus moves: taking focus can end another
    // row's edit, which refreshes fields straight back through setValue().
    emit numericEditBegan();
    editor_->setFocus(Qt::MouseFocusReason);
    editor_->selectAll();
}

void ValueSegmentedControl::cancelNumericEdit()
{
    if (editing_)
        endNumericEdit(false);
}

void ValueSegmentedControl::commitNumericEdit()
{
    endNumericEdit(true);
}

void ValueSegmentedControl::endNumericEdit(bool committed)
{
    if (!editing_ || !editor_)
        return;

    const QVariant typed = clampNumeric(editor_->text());
    editing_ = false;
    editor_->hide();
    if (committed)
    {
        value_ = typed;
        numericSelection_ = typed;
        rememberNumeric(typed);
        // The session commits whatever it last previewed. Opening the editor
        // from a fixed segment and confirming the shown count never types, so
        // that count has to enter the session here.
        emit numericPreviewed(typed);
    }
    refreshSegments();
    emit numericEditEnded(committed);
}

bool ValueSegmentedControl::eventFilter(QObject* watched, QEvent* event)
{
    const int index = numericIndex();
    if (index >= 0 && watched == segments_[static_cast<size_t>(index)].button
        && event->type() == QEvent::MouseButtonDblClick)
    {
        beginNumericEdit();
        return true;
    }

    if (editor_ && watched == editor_ && editing_)
    {
        if (event->type() == QEvent::KeyPress)
        {
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter)
            {
                endNumericEdit(true);
                return true;
            }
            if (key == Qt::Key_Escape)
            {
                endNumericEdit(false);
                return true;
            }
        }
        else if (event->type() == QEvent::FocusOut)
        {
            endNumericEdit(true);
        }
    }

    return QWidget::eventFilter(watched, event);
}

}
