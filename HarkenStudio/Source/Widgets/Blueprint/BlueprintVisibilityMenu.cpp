#include "BlueprintVisibilityMenu.h"

#include "Core/FontManager.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>

namespace VNX {
namespace {

constexpr int RowHeight = 24;
constexpr int TitleInset = 8;
constexpr int FieldInset = 24;
constexpr int CheckInset = 8;
constexpr int MaximumHeight = 240;
constexpr NodeContentFields PlaybackFields = NodeContentField::Volume | NodeContentField::Pitch
    | NodeContentField::OffsetOfStart | NodeContentField::Loop;

struct RowDefinition
{
    const char* id;
    const char* label;
    NodeContentFields fields;
    bool group;
};

constexpr std::array<RowDefinition, 7> Rows{{
    {"Assist", QT_TR_NOOP("Assist"), NodeContentField::Waveform, true},
    {"Waveform", QT_TR_NOOP("Waveform"), NodeContentField::Waveform, false},
    {"Playback", QT_TR_NOOP("Playback"), PlaybackFields, true},
    {"Volume", QT_TR_NOOP("Volume"), NodeContentField::Volume, false},
    {"Pitch", QT_TR_NOOP("Pitch"), NodeContentField::Pitch, false},
    {"OffsetOfStart", QT_TR_NOOP("Offset of Start"), NodeContentField::OffsetOfStart, false},
    {"Loop", QT_TR_NOOP("Loop"), NodeContentField::Loop, false}
}};

}

BlueprintVisibilityMenu::BlueprintVisibilityMenu(QWidget* parent)
    : QMenu(parent)
{
    setObjectName(QStringLiteral("BlueprintViewportVisibilityMenu"));
    setTitle(tr("Viewport Visibility"));
    setFont(FontManager::instance().getFont(FontRole::Caption, 9));

    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("BlueprintVisibilityScrollArea"));
    scroll_->setAttribute(Qt::WA_NoMousePropagation);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setWidgetResizable(true);
    scroll_->viewport()->setAutoFillBackground(false);
    contents_ = new QWidget;
    contents_->setObjectName(QStringLiteral("BlueprintVisibilityContent"));
    scroll_->setWidget(contents_);
    contents_->setAutoFillBackground(false);

    auto* layout = new QVBoxLayout(contents_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    for (size_t index = 0; index < Rows.size(); ++index) {
        const auto& definition = Rows[index];
        const QString label = tr(definition.label);
        const QString id = QString::fromLatin1(definition.id);
        auto* row = new QWidget(contents_);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(definition.group ? TitleInset : FieldInset, 0, CheckInset, 0);
        auto* check = new QCheckBox(row);
        check->setObjectName(QStringLiteral("BlueprintVisibilityField_") + id);
        check->setAccessibleName(label);
        check->setToolTip(label);
        check->setTristate(definition.group);
        check->installEventFilter(this);
        checks_[index] = check;
        auto* labelView = new QLabel(label, row);
        labelView->setObjectName(QStringLiteral("BlueprintVisibilityLabel_") + id);
        labelView->setToolTip(label);
        labelView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        if (definition.group) {
            labelView->setProperty("textRole", "secondary");
            QFont font;
            font.setWeight(QFont::DemiBold);
            labelView->setFont(font);
        }
        labels_[index] = labelView;
        rowLayout->addWidget(labelView, 1);
        rowLayout->addWidget(check);
        layout->addWidget(row);
        connect(check, &QCheckBox::clicked, this, [this, fields = definition.fields]() {
            emit fieldsToggled(fields);
        });
    }
    scroll_->setFocusProxy(checks_.front());
    auto* action = new QWidgetAction(this);
    action->setDefaultWidget(scroll_);
    addAction(action);
    setFields(kAllNodeContentFields);
    relayout_();
    connect(this, &QMenu::aboutToShow, this, &BlueprintVisibilityMenu::relayout_);
    connect(&FontManager::instance(), &FontManager::fontChanged, this, [this]() {
        setFont(FontManager::instance().getFont(FontRole::Caption, 9));
        relayout_();
    });
}

void BlueprintVisibilityMenu::setFields(NodeContentFields fields)
{
    for (size_t index = 0; index < Rows.size(); ++index) {
        auto* check = checks_[index];
        const QSignalBlocker blocker(check);
        const auto shown = fields & Rows[index].fields;
        check->setCheckState(shown == Rows[index].fields ? Qt::Checked
            : shown ? Qt::PartiallyChecked : Qt::Unchecked);
    }
}

void BlueprintVisibilityMenu::relayout_()
{
    int rowHeight = std::max(RowHeight, fontMetrics().height() + 4);
    int contentWidth = 0;
    for (size_t index = 0; index < Rows.size(); ++index) {
        auto* check = checks_[index];
        auto* label = labels_[index];
        rowHeight = std::max({rowHeight, label->fontMetrics().height() + 4, check->sizeHint().height()});
        const int inset = Rows[index].group ? TitleInset : FieldInset;
        contentWidth = std::max(contentWidth, label->fontMetrics().horizontalAdvance(label->text())
            + inset + check->sizeHint().width() + CheckInset + 8);
    }
    for (auto* check : checks_)
        check->parentWidget()->setFixedHeight(rowHeight);
    const int contentHeight = rowHeight * static_cast<int>(Rows.size());
    const bool scrolls = contentHeight > MaximumHeight;
    scroll_->setVerticalScrollBarPolicy(scrolls ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
    contents_->setFixedHeight(contentHeight);
    scroll_->setFixedSize(contentWidth + (scrolls ? style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0),
        std::min(MaximumHeight, contentHeight));
    updateGeometry();
    if (isVisible()) adjustSize();
}

bool BlueprintVisibilityMenu::eventFilter(QObject* watched, QEvent* event)
{
    const auto current = std::find(checks_.begin(), checks_.end(), watched);
    if (current != checks_.end() && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            close();
            return true;
        }
        const bool forward = key->key() == Qt::Key_Down
            || (key->key() == Qt::Key_Tab && !key->modifiers().testFlag(Qt::ShiftModifier));
        const bool backward = key->key() == Qt::Key_Up || key->key() == Qt::Key_Backtab
            || (key->key() == Qt::Key_Tab && key->modifiers().testFlag(Qt::ShiftModifier));
        if (forward || backward) {
            const int count = static_cast<int>(checks_.size());
            const int index = (static_cast<int>(current - checks_.begin()) + (forward ? 1 : count - 1)) % count;
            checks_[index]->setFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
            scroll_->ensureWidgetVisible(checks_[index]);
            return true;
        }
    }
    return QMenu::eventFilter(watched, event);
}

void BlueprintVisibilityMenu::showEvent(QShowEvent* event)
{
    QMenu::showEvent(event);
    checks_.front()->setFocus(Qt::PopupFocusReason);
}

}
