#include "ConversionEditorPanel.h"

#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/BaseDialog.h"
#include "Core/ConversionEditing.h"
#include "Core/PlatformContextService.h"
#include "Core/PreviewMediaModeService.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeLayout.h"
#include "Core/ThemeManager.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"
#include "Widgets/Common/DialogFormLayout.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/SegmentedControl.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Selector/ObjectRefNavigation.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QAction>
#include <QButtonGroup>
#include <QChildEvent>
#include <QComboBox>
#include <QDebug>
#include <QDialogButtonBox>
#include <QFocusEvent>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QRegularExpressionValidator>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>

namespace VNX {
namespace {

uint64_t unitScale(int unit)
{
    return uint64_t(1) << (10 * (unit + 1));
}

QString sizeValue(uint64_t bytes, int unit)
{
    const auto scale = unitScale(unit);
    QString value = QString::number(bytes / scale);
    auto remainder = bytes % scale;
    if (remainder) value += QChar('.');
    while (remainder) {
        remainder *= 10;
        value += QChar(char('0' + remainder / scale));
        remainder %= scale;
    }
    return value;
}

std::optional<uint64_t> sizeBytes(const QString& text, int unit)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9]+(?:\\.[0-9]+)?$"));
    if (!pattern.match(text).hasMatch()) return std::nullopt;
    const auto parts = text.split(QChar('.'));
    bool valid = false;
    const auto whole = parts.front().toULongLong(&valid);
    if (!valid) return std::nullopt;
    const auto scale = unitScale(unit);
    uint64_t fraction = 0;
    if (parts.size() == 2) {
        for (qsizetype i = parts.back().size(); i > 0; --i) {
            const auto product = uint64_t(parts.back()[i - 1].digitValue()) * scale + fraction;
            if (product % 10 != 0) return std::nullopt;
            fraction = product / 10;
        }
    }
    const auto maximum = uint64_t(std::numeric_limits<qint64>::max());
    if (whole > (maximum - fraction) / scale) return std::nullopt;
    return whole * scale + fraction;
}

QComboBox* sampleRates(QWidget* parent, uint32_t selected)
{
    auto* rate = new QComboBox(parent);
    for (const auto value : kConversionSampleRates)
        rate->addItem(value == 0 ? QObject::tr("Original") : QObject::tr("%1 Hz").arg(value), value);
    rate->setCurrentIndex(rate->findData(selected));
    return rate;
}

void sortTiers(std::vector<SampleRateTier>& tiers)
{
    std::sort(tiers.begin(), tiers.end(), [](const auto& first, const auto& second) {
        return first.maxBytes && (!second.maxBytes || *first.maxBytes < *second.maxBytes);
    });
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
    ThemeLayout::spacing(layout, ThemeLayout::Space::Section);
    auto* splitter = new QSplitter(Qt::Vertical, this);
    layout->addWidget(splitter);
    auto* upper = new QWidget(splitter);
    auto* upperLayout = new QVBoxLayout(upper);
    upperLayout->setContentsMargins(0, 0, 0, 0);
    ThemeLayout::spacing(upperLayout, ThemeLayout::Space::Control);
    platforms_ = new SegmentedControl(upper);
    platforms_->setObjectName(QStringLiteral("conversionPlatforms"));
    upperLayout->addWidget(platforms_);
    auto* scroll = new QScrollArea(upper);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    fields_ = new QWidget(scroll);
    auto* fieldsLayout = new QVBoxLayout(fields_);
    fieldsLayout->setContentsMargins(0, 0, 0, 0);
    ThemeLayout::spacing(fieldsLayout, ThemeLayout::Space::Control);
    auto* form = new DialogFormLayout;
    format_ = new QComboBox(fields_);
    format_->setObjectName(QStringLiteral("conversionFormat"));
    format_->addItem(QStringLiteral("PCM"), static_cast<int>(ConversionFormat::PCM));
    format_->addItem(QStringLiteral("Vorbis"), static_cast<int>(ConversionFormat::Vorbis));
    form->addRow(tr("Format:"), format_);
    quality_ = new QSpinBox(fields_);
    quality_->setObjectName(QStringLiteral("conversionQuality"));
    quality_->setRange(0, 100);
    quality_->setSuffix(QStringLiteral("%"));
    quality_->setKeyboardTracking(false);
    form->addRow(tr("Quality:"), quality_);
    fieldsLayout->addLayout(form);
    tiersTable_ = new TableWidget(fields_);
    tiers_ = tiersTable_->createItemView(QStringLiteral("conversionSampleRateTiers"),
        {tr("Upper limit"), tr("Sample rate")}, {600, 400});
    tiers_->setRootIsDecorated(false);
    tiers_->setSelectionMode(QAbstractItemView::SingleSelection);
    tiers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fieldsLayout->addWidget(tiersTable_);
    fieldsLayout->addStretch();
    scroll->setWidget(fields_);
    upperLayout->addWidget(scroll);
    auto* resultsTable = new TableWidget(TableOptions::display(), splitter);
    results_ = resultsTable->createItemView(QStringLiteral("conversionResults"),
        {tr("Sound"), tr("Source"), tr("Language"), tr("Original format"), tr("Original rate"),
         tr("Original channels"), tr("Original size"), tr("Converted format"), tr("Converted rate"),
         tr("Converted size"), tr("Ratio")}, {180, 220, 70, 90, 90, 90, 90, 90, 90, 90, 70});
    results_->setRootIsDecorated(false);
    results_->setSelectionMode(QAbstractItemView::NoSelection);
    results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    convert_ = new QAction(tr("Convert"), this);
    convert_->setToolTip(tr("Convert all active sources for the current platform. Press Esc to cancel."));
    resultsTable->footer(false)->addActionButton(convert_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    resultsRefresh_.setSingleShot(true);
    resultsRefresh_.setInterval(0);
    connect(&resultsRefresh_, &QTimer::timeout, this, &ConversionEditorPanel::refreshResults);
    connect(convert_, &QAction::triggered, this, &ConversionEditorPanel::startConversion);
    auto* cancel = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    cancel->setContext(Qt::WidgetWithChildrenShortcut);
    connect(cancel, &QShortcut::activated, this, &ConversionEditorPanel::cancelConversion);
    connect(platforms_->buttonGroup(), &QButtonGroup::idClicked, this, [this](int index) {
        if (index < 0 || index >= platformOrder_.size()) return;
        selectedPlatform_ = platformOrder_[index];
        refreshConfig();
    });
    connect(format_, &QComboBox::currentIndexChanged, this, [this] {
        const auto* current = displayedConfig();
        if (!current) return;
        auto next = *current;
        next.format = static_cast<ConversionFormat>(format_->currentData().toInt());
        commitConfig(next);
    });
    connect(quality_, &QSpinBox::valueChanged, this, [this] {
        const auto* current = displayedConfig();
        if (!current) return;
        auto next = *current;
        next.vorbisQuality = static_cast<float>(quality_->value()) / 100.0f;
        commitConfig(next);
    });
    connect(tiersTable_->footer()->addButton(), &QToolButton::clicked, this, &ConversionEditorPanel::addTier);
    connect(tiersTable_->footer()->removeButton(), &QToolButton::clicked, this, &ConversionEditorPanel::removeTier);
    connect(tiers_, &QTreeWidget::itemSelectionChanged, this, [this] {
        const auto* config = displayedConfig();
        const int index = tiers_->indexOfTopLevelItem(tiers_->currentItem());
        tiersTable_->footer()->removeButton()->setEnabled(config && index >= 0
            && static_cast<size_t>(index) < config->tiers.size() && config->tiers[index].maxBytes.has_value());
    });
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
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged, this, [this] {
        selectedPlatform_ = PlatformContextService::instance().context().primary;
        refreshPreset();
    });
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
    if (guid != itemGuid_) cancelConversion();
    itemGuid_ = guid;
    selectedPlatform_ = PlatformContextService::instance().context().primary;
    refreshPreset();
}

void ConversionEditorPanel::clearEditor()
{
    cancelConversion();
    itemGuid_ = QUuid{};
    displayed_.clear();
    refreshPreset();
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

const ConversionPlatformConfig* ConversionEditorPanel::displayedConfig() const
{
    const auto found = std::find_if(displayed_.begin(), displayed_.end(),
        [this](const auto& config) { return config.platformId == selectedPlatform_; });
    return found == displayed_.end() ? nullptr : &*found;
}

void ConversionEditorPanel::refreshPreset()
{
    const QScopedValueRollback refreshing(refreshing_, true);
    const auto* item = boundItem();
    const auto* project = ProjectManager::instance().currentProject();
    setWindowTitle(item ? tr("Conversion Editor - %1").arg(item->name) : tr("Conversion Editor"));
    fields_->setVisible(item && project);
    platforms_->setVisible(item && project);
    displayed_ = item ? item->comp<VConversion>()->platforms : std::vector<ConversionPlatformConfig>{};
    platformOrder_.clear();
    QList<QPair<QString, QVariant>> options;
    if (project && item) {
        const auto primary = PlatformContextService::instance().context().primary;
        const auto append = [&](const ProjectPlatform& platform) {
            platformOrder_.push_back(platform.id);
            options.push_back({platform.name, platform.id});
        };
        if (const auto* platform = ProjectPlatformSemantics::find(project->platforms, primary)) append(*platform);
        for (const auto& platform : project->platforms)
            if (platform.id != primary) append(platform);
    }
    platforms_->setOptions(options);
    if (!platformOrder_.contains(selectedPlatform_))
        selectedPlatform_ = platformOrder_.isEmpty() ? ProjectPlatformId{} : platformOrder_.front();
    const auto index = platformOrder_.indexOf(selectedPlatform_);
    if (index >= 0) platforms_->button(static_cast<int>(index))->setChecked(true);
    refreshConfig();
    resultsRefresh_.start();
}

void ConversionEditorPanel::refreshConfig()
{
    const QScopedValueRollback refreshing(refreshing_, true);
    const auto* config = displayedConfig();
    const QSignalBlocker formatBlocker(format_);
    const QSignalBlocker qualityBlocker(quality_);
    format_->setEnabled(config != nullptr);
    quality_->setEnabled(config && config->format == ConversionFormat::Vorbis);
    if (config) {
        format_->setCurrentIndex(format_->findData(static_cast<int>(config->format)));
        quality_->setValue(static_cast<int>(std::lround(config->vorbisQuality * 100.0f)));
    }
    refreshTiers();
}

void ConversionEditorPanel::commitConfig(const ConversionPlatformConfig& config)
{
    if (refreshing_) return;
    auto replacement = displayed_;
    const auto row = std::find_if(replacement.begin(), replacement.end(),
        [&](const auto& value) { return value.platformId == config.platformId; });
    if (row == replacement.end() || *row == config) return;
    *row = config;
    QString error;
    if (!ConversionEditing::setPlatforms(ProjectManager::instance(), itemGuid_, replacement, displayed_, this, &error)) {
        if (!error.isEmpty()) qWarning().noquote() << error;
        QMetaObject::invokeMethod(this, &ConversionEditorPanel::refreshPreset, Qt::QueuedConnection);
        return;
    }
    displayed_ = std::move(replacement);
}

void ConversionEditorPanel::refreshTiers()
{
    const QSignalBlocker blocker(tiers_);
    tiers_->clear();
    tiersTable_->footer()->removeButton()->setEnabled(false);
    const auto* config = displayedConfig();
    tiersTable_->footer()->addButton()->setEnabled(config != nullptr);
    if (!config) return;
    const auto rendered = *config;
    for (size_t index = 0; index < config->tiers.size(); ++index) {
        const auto tier = config->tiers[index];
        auto* row = new QTreeWidgetItem(tiers_);
        if (tier.maxBytes) {
            auto* limit = new QWidget(tiers_);
            auto* layout = new QHBoxLayout(limit);
            layout->setContentsMargins(0, 0, 0, 0);
            ThemeLayout::spacing(layout, ThemeLayout::Space::Tight);
            const int unitIndex = *tier.maxBytes >= unitScale(1) && *tier.maxBytes % unitScale(1) == 0 ? 1 : 0;
            auto* value = new QLineEdit(sizeValue(*tier.maxBytes, unitIndex), limit);
            value->setMaxLength(64);
            value->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]+(?:\\.[0-9]+)?")), value));
            auto* unit = new QComboBox(limit);
            unit->addItems({QStringLiteral("KB"), QStringLiteral("MB")});
            unit->setCurrentIndex(unitIndex);
            unit->setProperty("previousUnit", unitIndex);
            layout->addWidget(value, 1);
            layout->addWidget(unit);
            tiers_->setItemWidget(row, 0, limit);
            connect(unit, &QComboBox::currentIndexChanged, this, [value, unit](int next) {
                const auto bytes = sizeBytes(value->text(), unit->property("previousUnit").toInt());
                unit->setProperty("previousUnit", next);
                if (bytes) value->setText(sizeValue(*bytes, next));
            });
            connect(value, &QLineEdit::editingFinished, this, [this, index, value, unit, rendered] {
                const auto* current = displayedConfig();
                if (refreshing_ || !current || *current != rendered) return;
                const auto bytes = sizeBytes(value->text(), unit->currentIndex());
                auto next = *current;
                if (bytes) next.tiers[index].maxBytes = *bytes;
                sortTiers(next.tiers);
                QString error;
                const bool valid = bytes && validateSampleRateTiers(next.tiers, &error);
                QtControlSkin::setSemanticProperty(value, "inputInvalid", !valid);
                value->setToolTip(valid ? QString() : error.isEmpty() ? tr("Enter a whole-byte size within range.") : error);
                if (valid) commitConfig(next);
            });
        } else {
            row->setText(0, tr("Infinite"));
        }
        auto* rate = sampleRates(tiers_, tier.sampleRate);
        tiers_->setItemWidget(row, 1, rate);
        connect(rate, &QComboBox::currentIndexChanged, this, [this, index, rate, rendered] {
            const auto* current = displayedConfig();
            if (refreshing_ || !current || *current != rendered) return;
            auto next = *current;
            next.tiers[index].sampleRate = rate->currentData().toUInt();
            commitConfig(next);
        });
    }
}

void ConversionEditorPanel::addTier()
{
    const auto* config = displayedConfig();
    if (!config) return;
    const auto before = displayed_;
    const auto preset = itemGuid_;
    const auto project = ProjectManager::instance().currentProjectLease();
    auto next = *config;
    BaseDialog dialog(this);
    dialog.setWindowTitle(tr("Add Range"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new DialogFormLayout;
    auto* limit = new QWidget(&dialog);
    auto* line = new QHBoxLayout(limit);
    line->setContentsMargins(0, 0, 0, 0);
    auto* value = new QLineEdit(limit);
    value->setMaxLength(64);
    value->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]+(?:\\.[0-9]+)?")), value));
    auto* unit = new QComboBox(limit);
    unit->addItems({QStringLiteral("KB"), QStringLiteral("MB")});
    unit->setProperty("previousUnit", 0);
    connect(unit, &QComboBox::currentIndexChanged, &dialog, [value, unit](int next) {
        const auto bytes = sizeBytes(value->text(), unit->property("previousUnit").toInt());
        unit->setProperty("previousUnit", next);
        if (bytes) value->setText(sizeValue(*bytes, next));
    });
    line->addWidget(value, 1);
    line->addWidget(unit);
    form->addRow(tr("Upper limit:"), limit);
    auto* rate = sampleRates(&dialog, 0);
    form->addRow(tr("Sample rate:"), rate);
    layout->addLayout(form);
    auto* error = new QLabel(&dialog);
    error->setWordWrap(true);
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto bytes = sizeBytes(value->text(), unit->currentIndex());
        if (!bytes) { error->setText(tr("Enter a whole-byte size within range.")); return; }
        auto candidate = next;
        candidate.tiers.push_back({bytes, rate->currentData().toUInt()});
        sortTiers(candidate.tiers);
        QString validation;
        if (!validateSampleRateTiers(candidate.tiers, &validation)) { error->setText(validation); return; }
        next = std::move(candidate);
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted
        && ProjectManager::instance().currentProjectLease() == project
        && itemGuid_ == preset && displayed_ == before)
        commitConfig(next);
}

void ConversionEditorPanel::removeTier()
{
    const auto* config = displayedConfig();
    const int index = tiers_->indexOfTopLevelItem(tiers_->currentItem());
    if (!config || index < 0 || static_cast<size_t>(index) >= config->tiers.size()
        || !config->tiers[index].maxBytes) return;
    auto next = *config;
    next.tiers.erase(next.tiers.begin() + index);
    commitConfig(next);
}

void ConversionEditorPanel::refreshResults()
{
    results_->clear();
    const auto* project = ProjectManager::instance().currentProject();
    const auto platform = PlatformContextService::instance().context().primary;
    convert_->setEnabled(project && boundItem() && !platform.isNull() && !batch_);
    if (!project || !boundItem() || platform.isNull()) return;
    QString error;
    const auto rows = ConversionResults::query(*project, itemGuid_, platform, &error);
    if (!error.isEmpty()) { qWarning().noquote() << error; return; }
    for (const auto& row : rows) {
        auto* item = new QTreeWidgetItem(results_, {QString(), row.sourcePath, row.language,
            row.original.format, tr("%1 Hz").arg(row.original.sampleRate), QString::number(row.original.channels),
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
    convert_->setEnabled(false);
    auto* watcher = new QFutureWatcher<ConversionResults::BatchResult>(this);
    const auto batch = batch_;
    connect(watcher, &QFutureWatcher<ConversionResults::BatchResult>::finished, this, [this, watcher, batch] {
        watcher->deleteLater();
        if (batch_ == batch) batch_.reset();
        resultsRefresh_.start();
    });
    watcher->setFuture(QtConcurrent::run([batch] { return batch->run(); }));
}

void ConversionEditorPanel::cancelConversion()
{
    if (batch_) batch_->cancel();
    batch_.reset();
    resultsRefresh_.start();
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
