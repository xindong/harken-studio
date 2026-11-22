#include "ConversionEditorPanel.h"
#include "ConversionSizeSplits.h"

#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/BaseDialog.h"
#include "Core/ConversionEditing.h"
#include "Core/NumericFormat.h"
#include "Core/PlatformContextService.h"
#include "Core/PreviewMediaModeService.h"
#include "Core/ProjectManager.h"
#include "Core/ThemeLayout.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"
#include "Widgets/Common/DialogFormLayout.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/PaneSection.h"
#include "Widgets/Common/PaneSectionStack.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/SourceRangeThumb.h"
#include "Widgets/Common/TableItemDelegate.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Selector/ObjectRefNavigation.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QChildEvent>
#include <QComboBox>
#include <QDebug>
#include <QDialogButtonBox>
#include <QFocusEvent>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <limits>

namespace VNX {
namespace {

constexpr int kPrimaryFieldStretch = 7;
constexpr int kSecondaryFieldStretch = 3;

enum RuleColumn { RuleNumber, SizeRange, SampleRate, Format, Quality };
constexpr int kRuleLimitRole = Qt::UserRole;

void setRangeText(QTreeWidgetItem* row, std::optional<uint64_t> previous, std::optional<uint64_t> maximum)
{
    const auto minimum = conversionSizeText(previous.value_or(0));
    const auto text = minimum + QStringLiteral(" - ") + (maximum ? conversionSizeText(*maximum) : kPositiveInfinityText);
    row->setText(SizeRange, text);
    row->setToolTip(SizeRange, text);
}

QComboBox* sampleRates(QWidget* parent, uint32_t selected)
{
    auto* rate = new QComboBox(parent);
    for (const auto value : kConversionSampleRates)
        rate->addItem(value == 0 ? QObject::tr("Original") : QObject::tr("%1 Hz").arg(value), value);
    rate->setCurrentIndex(rate->findData(selected));
    return rate;
}

QComboBox* formats(QWidget* parent, ConversionFormat selected)
{
    auto* format = new QComboBox(parent);
    format->setObjectName(QStringLiteral("conversionFormat"));
    format->addItem(QStringLiteral("PCM"), static_cast<int>(ConversionFormat::PCM));
    format->addItem(QStringLiteral("Vorbis"), static_cast<int>(ConversionFormat::Vorbis));
    format->setCurrentIndex(format->findData(static_cast<int>(selected)));
    return format;
}

ParameterControlHost* qualityEditor(QWidget* parent, int level)
{
    auto* quality = new ParameterControlHost(QObject::tr("Quality"), ParameterPresenterContext::InspectorFloat,
                                             ParameterPresenterId::SliderWithValue, parent);
    quality->setObjectName(QStringLiteral("conversionQuality"));
    auto* binding = quality->binding();
    binding->setRange(0, kMaxVorbisQuality);
    binding->setDecimals(0);
    binding->setQuantum(1);
    binding->setDefaultValue(kDefaultVorbisQuality);
    binding->setValue(level);
    ThemeManager::instance().bindMetrics(quality, [quality](const ThemeMetrics& metrics) {
        quality->setCompactValueColumn(metrics.inspector.valueColumnWidth);
        quality->setValueColumnStretch(kPrimaryFieldStretch, kSecondaryFieldStretch);
    });
    return quality;
}

QString sizeText(uint64_t bytes)
{
    return QLocale().formattedDataSize(static_cast<qint64>(bytes));
}

}

ConversionEditorPanel::ConversionEditorPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("ConversionEditorPanel"));
    setFocusPolicy(Qt::StrongFocus);
    auto* layout = new QVBoxLayout(this);
    ThemeLayout::margins(layout, ThemeLayout::Space::Panel);
    layout->setSpacing(0);
    sections_ = new PaneSectionStack(QStringLiteral("ConversionEditor"), this);
    sections_->setObjectName(QStringLiteral("conversionEditorPage"));
    layout->addWidget(sections_, 1);

    auto* configuration = sections_->addSection(tr("Configuration"), QStringLiteral("configuration"), 300);
    configuration->setObjectName(QStringLiteral("conversionConfigurationSection"));
    configuration->layout()->setSizeConstraint(QLayout::SetMinimumSize);
    auto* body = new QWidget(configuration->content());
    configuration->setContentWidget(body);
    auto* configLayout = new QVBoxLayout(body);
    configLayout->setContentsMargins(0, 0, 0, 0);
    ThemeLayout::spacing(configLayout, ThemeLayout::Space::Control);
    splits_ = new ConversionSizeSplits(body);
    configLayout->addWidget(splits_);
    rulesTable_ = new TableWidget(TableOptions{TableKind::Config, TableBodySizing::Expanding}, body);
    configLayout->addWidget(rulesTable_, 1);
    rules_ = rulesTable_->createItemView(QStringLiteral("conversionRules"),
        {QStringLiteral("#"), tr("Size range"), tr("Sample rate"), tr("Format"), tr("Quality")}, {0, 1, 1, 1, 1});
    rulesTable_->setRowNumberColumn(RuleNumber);
    rules_->headerItem()->setToolTip(RuleNumber, tr("Rule numbers match the size ranges above."));
    rules_->setRootIsDecorated(false);
    rules_->setSelectionMode(QAbstractItemView::SingleSelection);
    rules_->setTextElideMode(Qt::ElideNone);
    rules_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto* ruleFooter = rulesTable_->footer();
    ThemeManager::instance().bindMetrics(rulesTable_, [this, ruleFooter](const ThemeMetrics& metrics) {
        rulesTable_->setMinimumHeight(rules_->header()->height() + metrics.table.rowHeight
                                      + ruleFooter->height() + 2 * metrics.borderWidth);
    });
    ruleFooter->addButton()->setToolTip(tr("Split the selected size range"));
    connect(ruleFooter->addButton(), &QToolButton::clicked, this, [this] { splits_->requestSplit(selectedRule().row()); });
    connect(ruleFooter->removeButton(), &QToolButton::clicked, this, &ConversionEditorPanel::removeRule);
    connect(rules_, &QTreeWidget::itemSelectionChanged, this, &ConversionEditorPanel::updateRuleSelection);
    connect(splits_, &ConversionSizeSplits::rangeSelected, this, &ConversionEditorPanel::selectRange);
    connect(splits_, &ConversionSizeSplits::splitRequested, this, [this](int index, quint64 bytes) { editSplit(index, bytes, true); });
    connect(splits_, &ConversionSizeSplits::editRequested, this, [this](int index, quint64 bytes) { editSplit(index, bytes, false); });
    connect(splits_, &ConversionSizeSplits::boundaryPreviewed, this, &ConversionEditorPanel::previewBoundary);
    connect(splits_, &ConversionSizeSplits::boundaryMoved, this, [this](int index, quint64 bytes) {
        auto next = *displayed_;
        next.rules[index].maxBytes = bytes;
        commitConfig(next);
    });

    convert_ = new QPushButton(tr("Convert"), body);
    convert_->setToolTip(tr("Convert all active sources for the current platform. Press Esc to cancel."));
    configLayout->addWidget(convert_, 0, Qt::AlignRight);
    connect(convert_, &QPushButton::clicked, this, &ConversionEditorPanel::startConversion);

    auto* result = sections_->addSection(tr("Result"), QStringLiteral("result"), 200);
    result->setObjectName(QStringLiteral("conversionResultSection"));
    auto* resultsTable = new TableWidget(TableOptions::display(), result->content());
    result->setContentWidget(resultsTable);
    results_ = resultsTable->createItemView(QStringLiteral("conversionResults"),
        {tr("Sound"), tr("Active Source"), tr("Language"), tr("Original format"), tr("Original rate"),
         tr("Original size"), tr("Converted format"), tr("Converted rate"), tr("Converted size"),
         tr("Ratio")}, {170, 195, 70, 100, 85, 85, 110, 95, 95, 55});
    auto* header = results_->headerItem();
    for (int column = 0; column < results_->columnCount(); ++column)
        header->setToolTip(column, header->text(column));
    results_->setRootIsDecorated(false);
    results_->setSelectionMode(QAbstractItemView::NoSelection);
    results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    resultsRefresh_.setSingleShot(true);
    resultsRefresh_.setInterval(0);
    connect(&resultsRefresh_, &QTimer::timeout, this, &ConversionEditorPanel::refreshResults);
    cancelShortcut_ = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    cancelShortcut_->setContext(Qt::WidgetWithChildrenShortcut);
    connect(cancelShortcut_, &QShortcut::activated, this, &ConversionEditorPanel::cancelConversion);
    auto* navigation = new ObjectRefNavigation(this);
    const auto navigate = [this](RefDomain domain, uint32_t id) {
        if (domain != RefDomain::ProjectObject) return;
        if (const auto* item = ProjectManager::instance().findItemByObjectID(id))
            emit navigateRequested(item->guid);
    };
    connect(navigation, &ObjectRefNavigation::navigateRequested, this, navigate);
    connect(navigation, &ObjectRefNavigation::locateRequested, this, navigate);
    auto& projects = ProjectManager::instance();
    connect(&projects, &ProjectManager::projectClosed, this, &ConversionEditorPanel::clearEditor);
    connect(&projects, &ProjectManager::itemPropertyChanged, this,
        [this](ProjectItem* item, const QString& property, const QObject*) {
            if (item && item->guid == itemGuid_)
                QMetaObject::invokeMethod(this, &ConversionEditorPanel::refreshPreset, Qt::QueuedConnection);
            if (property == QStringLiteral("fieldGroups") || property == QStringLiteral("sources")
                || property == QStringLiteral("distinguishLanguage") || (item && item->type == ProjectItem::Type::Conversion))
                resultsRefresh_.start();
        });
    connect(&projects, &ProjectManager::itemDeleted, this, [this] {
        if (!boundItem()) clearEditor();
        else resultsRefresh_.start();
    });
    connect(&projects, &ProjectManager::itemCreated, this, [this] { resultsRefresh_.start(); });
    connect(&projects, &ProjectManager::itemRenamed, this, [this] { refreshPreset(); });
    connect(&projects, &ProjectManager::itemMoved, this, [this] { resultsRefresh_.start(); });
    connect(&projects, &ProjectManager::targetPlatformsChanged, this, &ConversionEditorPanel::refreshPreset);
    connect(&projects, &ProjectManager::projectLanguagesChanged, this, [this] { resultsRefresh_.start(); });
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
        this, &ConversionEditorPanel::updateConversionState);
    connect(&PreviewMediaModeService::instance(), &PreviewMediaModeService::modeChanged,
        this, [this] { resultsRefresh_.start(); });
    auto& assets = AssetRegistry::instance();
    connect(&assets, &AssetRegistry::assetRegistered, this, [this] { resultsRefresh_.start(); });
    connect(&assets, &AssetRegistry::assetModified, this, [this] { resultsRefresh_.start(); });
    connect(&assets, &AssetRegistry::assetUnregistered, this, [this] { resultsRefresh_.start(); });
    connect(&SoundSourceDerivative::MaterializationEvents::instance(),
        &SoundSourceDerivative::MaterializationEvents::artifactReady, this,
        [this] { resultsRefresh_.start(); }, Qt::QueuedConnection);
    refreshPreset();
    installFocusFilter(this);
}

ConversionEditorPanel::~ConversionEditorPanel()
{
    cancelConversion();
}

bool ConversionEditorPanel::acceptsItem(const ProjectItem* item)
{
    return item && item->type == ProjectItem::Type::Conversion && item->comp<VConversion>();
}

ProjectItem* ConversionEditorPanel::boundItem() const
{
    auto* item = ProjectManager::instance().findItemByGuid(itemGuid_);
    return acceptsItem(item) ? item : nullptr;
}

void ConversionEditorPanel::bindItem(ProjectItem* item)
{
    const auto guid = acceptsItem(item) ? item->guid : QUuid{};
    if (guid != itemGuid_) {
        cancelConversion();
        rulesTable_->clearSelection();
    }
    itemGuid_ = guid;
    refreshPreset();
}

void ConversionEditorPanel::clearEditor()
{
    bindItem(nullptr);
}

void ConversionEditorPanel::setPinned(bool pinned)
{
    if (pinned_ == pinned) return;
    pinned_ = pinned;
    emit pinToggled(pinned_);
}

QVariantMap ConversionEditorPanel::snapshotState() const
{
    return {{QStringLiteral("itemGuid"), itemGuid_.toString(QUuid::WithoutBraces)}};
}

void ConversionEditorPanel::restoreState(const QVariantMap& state)
{
    bindItem(ProjectManager::instance().findItemByGuid(QUuid(state.value(QStringLiteral("itemGuid")).toString())));
}

void ConversionEditorPanel::refreshPreset()
{
    const QScopedValueRollback refreshing(refreshing_, true);
    const auto* item = boundItem();
    const auto* project = ProjectManager::instance().currentProject();
    setWindowTitle(item ? tr("Conversion Editor - %1").arg(item->name) : tr("Conversion Editor"));
    displayed_ = item && project ? std::optional{item->comp<VConversion>()->config} : std::nullopt;
    refreshRules();
    sections_->setVisible(displayed_.has_value());
    updateConversionState();
}

void ConversionEditorPanel::updateConversionState()
{
    const auto platform = PlatformContextService::instance().context().primary;
    convert_->setEnabled(displayed_.has_value() && !platform.isNull() && !batch_);
    cancelShortcut_->setEnabled(bool(batch_));
    resultsRefresh_.start();
}

QModelIndex ConversionEditorPanel::selectedRule() const
{
    return rules_->selectionModel()->selectedRows(RuleNumber).value(0);
}

void ConversionEditorPanel::selectRange(int index)
{
    auto* row = rules_->topLevelItem(index);
    if (!row) {
        rulesTable_->clearSelection();
        return;
    }
    rules_->setCurrentItem(row, RuleNumber, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    rules_->scrollToItem(row);
}

void ConversionEditorPanel::updateRuleSelection()
{
    const auto selected = selectedRule();
    splits_->setSelectedRange(selected.row());
    auto* footer = rulesTable_->footer();
    footer->addButton()->setEnabled(displayed_.has_value() && splits_->canSplit(selected.row()));
    footer->removeButton()->setEnabled(selected.data(kRuleLimitRole).isValid());
}

void ConversionEditorPanel::previewBoundary(int index, quint64 bytes)
{
    const auto& rules = displayed_->rules;
    setRangeText(rules_->topLevelItem(index), index ? rules[index - 1].maxBytes : std::nullopt, bytes);
    setRangeText(rules_->topLevelItem(index + 1), bytes, rules[index + 1].maxBytes);
}

bool ConversionEditorPanel::commitConfig(const ConversionConfig& config)
{
    if (refreshing_ || !displayed_ || *displayed_ == config) return false;
    QString error;
    if (!ConversionEditing::setConfig(ProjectManager::instance(), itemGuid_, config, displayed_, this, &error)) {
        if (!error.isEmpty()) qWarning().noquote() << error;
        QMetaObject::invokeMethod(this, &ConversionEditorPanel::refreshPreset, Qt::QueuedConnection);
        return false;
    }
    displayed_ = config;
    return true;
}

void ConversionEditorPanel::refreshRules()
{
    const int oldCount = rules_->topLevelItemCount();
    const int selectedIndex = selectedRule().row();
    const auto selectedLimit = selectedRule().data(kRuleLimitRole);
    QVector<quint64> limits;
    if (displayed_) {
        for (const auto& rule : displayed_->rules)
            if (rule.maxBytes) limits.append(*rule.maxBytes);
    }
    splits_->setLimits(std::move(limits));
    rules_->clear();
    updateRuleSelection();
    if (!displayed_) return;
    int restoreIndex = -1;
    const auto rendered = std::make_shared<const ConversionConfig>(*displayed_);
    const auto isCurrent = [this, rendered, guid = itemGuid_] {
        return !refreshing_ && itemGuid_ == guid && displayed_ && *displayed_ == *rendered;
    };
    for (size_t index = 0; index < rendered->rules.size(); ++index) {
        const auto& rule = rendered->rules[index];
        const auto editRule = [this, isCurrent, index](auto edit) {
            if (!isCurrent()) return;
            auto next = *displayed_;
            edit(next.rules[index]);
            commitConfig(next);
        };
        auto* row = new QTreeWidgetItem(rules_);
        row->setText(RuleNumber, QString::number(index + 1));
        row->setTextAlignment(RuleNumber, Qt::AlignCenter);
        row->setData(RuleNumber, TableTextRole, static_cast<int>(TableTextState::Muted));
        if (rule.maxBytes) row->setData(RuleNumber, kRuleLimitRole, QVariant::fromValue(*rule.maxBytes));
        setRangeText(row, index ? rendered->rules[index - 1].maxBytes : std::nullopt, rule.maxBytes);
        auto* rate = sampleRates(rules_, rule.sampleRate);
        rules_->setItemWidget(row, SampleRate, rate);
        connect(rate, &QComboBox::currentIndexChanged, this, [editRule, rate] {
            editRule([&](ConversionRule& edited) { edited.sampleRate = rate->currentData().toUInt(); });
        });
        auto* format = formats(rules_, rule.format);
        rules_->setItemWidget(row, Format, format);
        connect(format, &QComboBox::currentIndexChanged, this, [editRule, format] {
            editRule([&](ConversionRule& edited) {
                edited.setFormat(static_cast<ConversionFormat>(format->currentData().toInt()));
            });
        });
        if (rule.vorbisQuality) {
            auto* quality = qualityEditor(rules_, *rule.vorbisQuality);
            rules_->setItemWidget(row, Quality, quality);
            const auto commitQuality = [editRule](double level) {
                editRule([&](ConversionRule& edited) { edited.vorbisQuality = static_cast<int>(level); });
            };
            connect(quality->binding(), &ParameterBinding::valueCommitted, this, commitQuality);
            connect(quality->binding(), &ParameterBinding::editEnded, this, [commitQuality](double, double level, bool committed) {
                if (committed) commitQuality(level);
            });
        }
        if (selectedIndex >= 0 && row->data(RuleNumber, kRuleLimitRole) == selectedLimit)
            restoreIndex = static_cast<int>(index);
    }
    if (restoreIndex < 0 && rules_->topLevelItemCount() == oldCount) restoreIndex = selectedIndex;
    if (restoreIndex >= 0) selectRange(restoreIndex);
}

void ConversionEditorPanel::editSplit(int index, quint64 bytes, bool insert)
{
    if (!displayed_) return;
    const auto before = *displayed_;
    const auto preset = itemGuid_;
    const auto project = ProjectManager::instance().currentProjectLease();
    const auto& initial = before.rules[index];
    const quint64 first = index ? *before.rules[index - 1].maxBytes + 1 : 0;
    const auto upper = insert ? initial.maxBytes : before.rules[index + 1].maxBytes;
    const quint64 last = upper ? *upper - 1 : std::numeric_limits<qint64>::max();
    auto rule = initial;
    BaseDialog dialog(this);
    dialog.setWindowTitle(insert ? tr("Split Range %1").arg(index + 1) : tr("Edit Split %1").arg(index + 1));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new DialogFormLayout;
    auto* field = new ConversionSizeField(bytes, &dialog);
    form->addRow(tr("Split at:"), field);
    if (insert) {
        auto* rate = sampleRates(&dialog, rule.sampleRate);
        form->addRow(tr("Sample rate:"), rate);
        auto* format = formats(&dialog, rule.format);
        form->addRow(tr("Format:"), format);
        auto* quality = qualityEditor(&dialog, rule.vorbisQuality.value_or(kDefaultVorbisQuality));
        auto* qualityLabel = new QLabel(tr("Quality:"), &dialog);
        form->addRow(qualityLabel, quality);
        const auto setFormat = [&, format, qualityLabel, quality] {
            rule.setFormat(static_cast<ConversionFormat>(format->currentData().toInt()));
            qualityLabel->setVisible(rule.vorbisQuality.has_value());
            quality->setVisible(rule.vorbisQuality.has_value());
            if (rule.vorbisQuality) quality->binding()->setValue(*rule.vorbisQuality);
        };
        setFormat();
        connect(format, &QComboBox::currentIndexChanged, &dialog, setFormat);
        connect(rate, &QComboBox::currentIndexChanged, &dialog, [&, rate] { rule.sampleRate = rate->currentData().toUInt(); });
        connect(quality->binding(), &ParameterBinding::valueUpdated, &dialog, [&](double level) {
            if (rule.format == ConversionFormat::Vorbis) rule.vorbisQuality = static_cast<int>(level);
        });
    }
    layout->addLayout(form);
    auto* error = new QLabel(&dialog);
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto value = field->bytes();
        if (!value || *value < first || *value > last) {
            error->setText(tr("Enter a whole-byte size from %1 to %2.").arg(conversionSizeText(first), conversionSizeText(last)));
            error->show();
            return;
        }
        rule.maxBytes = *value;
        dialog.accept();
    });
    field->setFocus();
    if (dialog.exec() != QDialog::Accepted || ProjectManager::instance().currentProjectLease() != project
        || itemGuid_ != preset || displayed_ != before) return;
    auto next = before;
    if (insert) next.rules.insert(next.rules.begin() + index, rule);
    else next.rules[index] = rule;
    if (commitConfig(next)) {
        if (insert) selectRange(index);
        rules_->topLevelItem(index)->setData(RuleNumber, kRuleLimitRole, QVariant::fromValue(*rule.maxBytes));
    }
}

void ConversionEditorPanel::removeRule()
{
    const auto limit = selectedRule().data(kRuleLimitRole);
    if (!displayed_ || !limit.isValid()) return;
    auto next = *displayed_;
    if (std::erase_if(next.rules, [bytes = limit.toULongLong()](const ConversionRule& rule) {
            return rule.maxBytes == bytes;
        }))
        rulesTable_->removeSelectedRow([this, &next] { return commitConfig(next); });
}

void ConversionEditorPanel::refreshResults()
{
    results_->clear();
    const auto* project = ProjectManager::instance().currentProject();
    const auto platform = PlatformContextService::instance().context().primary;
    if (!project || !boundItem() || platform.isNull()) return;
    QString error;
    const auto rows = ConversionResults::query(*project, itemGuid_, platform, &error);
    if (!error.isEmpty()) { qWarning().noquote() << error; return; }
    for (const auto& row : rows) {
        auto* item = new QTreeWidgetItem(results_, {QString(), QString(), row.language,
            row.original.format, tr("%1 Hz").arg(row.original.sampleRate),
            sizeText(row.original.bytes), row.converted ? row.converted->format : QString(),
            row.converted ? tr("%1 Hz").arg(row.converted->sampleRate) : QString(),
            row.converted ? sizeText(row.converted->bytes) : QString(),
            row.converted && row.original.bytes != 0
                ? QString::number(100.0 * row.converted->bytes / row.original.bytes, 'f', 1) + QChar('%') : QString()});
        auto* reference = new ObjectRefWidget(results_);
        reference->setRefDomain(RefDomain::ProjectObject);
        reference->setFilterType(ProjectItem::Type::Sound);
        reference->setReadOnly(true);
        reference->setSelectorEnabled(false);
        reference->setClearSupport(false);
        if (const auto* sound = ProjectManager::instance().findItemByGuid(row.soundGuid))
            reference->setCurrentObjectID(sound->objectID);
        results_->setItemWidget(item, 0, reference);
        auto* preview = new SourceRangeThumb(results_, SourceRangeThumb::Mode::ReadOnly);
        preview->setSource(row.source, AssetRegistry::instance().getAsset(row.source.sourceId));
        preview->setAccessibleName(row.sourcePath);
        item->setToolTip(1, row.sourcePath);
        results_->setItemWidget(item, 1, preview);
    }
}

void ConversionEditorPanel::startConversion()
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!project || !boundItem() || batch_) return;
    QString error;
    batch_ = ConversionResults::Batch::create(*project, itemGuid_,
        PlatformContextService::instance().context().primary, &error);
    if (!batch_) { if (!error.isEmpty()) qWarning().noquote() << error; return; }
    updateConversionState();
    auto* watcher = new QFutureWatcher<ConversionResults::BatchResult>(this);
    const auto batch = batch_;
    connect(watcher, &QFutureWatcher<ConversionResults::BatchResult>::finished, this, [this, watcher, batch] {
        watcher->deleteLater();
        if (batch_ == batch) batch_.reset();
        updateConversionState();
    });
    watcher->setFuture(QtConcurrent::run([batch] { return batch->run(); }));
}

void ConversionEditorPanel::cancelConversion()
{
    if (batch_) batch_->cancel();
    batch_.reset();
    updateConversionState();
}

void ConversionEditorPanel::installFocusFilter(QObject* object)
{
    object->installEventFilter(this);
    for (auto* child : object->children()) installFocusFilter(child);
}

bool ConversionEditorPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusIn && watched != this)
        emit panelFocused(panelId_);
    if (event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        auto* widget = qobject_cast<QWidget*>(watched);
        auto* viewport = rules_->viewport();
        if (mouse->button() == Qt::LeftButton && widget && widget != viewport && viewport->isAncestorOf(widget)) {
            const auto index = rules_->indexAt(widget->mapTo(viewport, mouse->position().toPoint()));
            if (index.isValid())
                rules_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
    }
    if (event->type() == QEvent::ChildAdded)
        installFocusFilter(static_cast<QChildEvent*>(event)->child());
    return QWidget::eventFilter(watched, event);
}

void ConversionEditorPanel::focusInEvent(QFocusEvent* event)
{
    emit panelFocused(panelId_);
    QWidget::focusInEvent(event);
}

}
