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
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Selector/ObjectRefNavigation.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QAction>
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
#include <QShortcut>
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

QWidget* sizeInput(QWidget* parent, QLineEdit*& value, QComboBox*& unit, std::optional<uint64_t> bytes)
{
    auto* limit = new QWidget(parent);
    auto* layout = new QHBoxLayout(limit);
    layout->setContentsMargins(0, 0, 0, 0);
    ThemeLayout::spacing(layout, ThemeLayout::Space::Tight);
    const int unitIndex = bytes && *bytes >= unitScale(1) && *bytes % unitScale(1) == 0 ? 1 : 0;
    value = new QLineEdit(bytes ? sizeValue(*bytes, unitIndex) : QString(), limit);
    value->setMaxLength(64);
    value->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]+(?:\\.[0-9]+)?")), value));
    unit = new QComboBox(limit);
    unit->addItems({QStringLiteral("KB"), QStringLiteral("MB")});
    unit->setCurrentIndex(unitIndex);
    unit->setProperty("previousUnit", unitIndex);
    auto* text = value;
    auto* units = unit;
    QObject::connect(unit, &QComboBox::currentIndexChanged, limit, [text, units](int next) {
        const auto converted = sizeBytes(text->text(), units->property("previousUnit").toInt());
        units->setProperty("previousUnit", next);
        if (converted) text->setText(sizeValue(*converted, next));
    });
    layout->addWidget(value, 1);
    layout->addWidget(unit);
    return limit;
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
    platforms_ = new TableWidget(TableOptions{TableKind::Config, TableBodySizing::Expanding}, splitter);
    platforms_->setObjectName(QStringLiteral("conversionPlatforms"));
    platforms_->setColumns(QStringLiteral("ConversionPlatforms"), {
        {QStringLiteral("Format"), tr("Format"), 220},
        {QStringLiteral("Quality"), tr("Quality"), 180},
        {QStringLiteral("UpperLimit"), tr("Upper limit"), 320},
        {QStringLiteral("SampleRate"), tr("Sample rate"), 280}
    });
    platforms_->setSelectionClearHandler([this] {
        selectedPlatform_ = QUuid{};
        selectedTier_ = -1;
        refreshSelection();
    });
    platforms_->setRowSelectionHandler([this](QWidget* row) {
        selectedPlatform_ = row->property("platformId").toUuid();
        selectedTier_ = row->property("tierIndex").toInt();
        refreshSelection();
    });
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
        this, &ConversionEditorPanel::refreshPreset);
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
        selectedPlatform_ = QUuid{};
        selectedTier_ = -1;
    }
    itemGuid_ = guid;
    refreshPreset();
}

void ConversionEditorPanel::clearEditor()
{
    cancelConversion();
    itemGuid_ = QUuid{};
    displayed_.clear();
    selectedPlatform_ = QUuid{};
    selectedTier_ = -1;
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

const ConversionPlatformConfig* ConversionEditorPanel::displayedConfig(const ProjectPlatformId& platform) const
{
    const auto found = std::find_if(displayed_.begin(), displayed_.end(),
        [&](const auto& config) { return config.platformId == platform; });
    return found == displayed_.end() ? nullptr : &*found;
}

void ConversionEditorPanel::refreshPreset()
{
    const auto* item = boundItem();
    setWindowTitle(item ? tr("Conversion Editor - %1").arg(item->name) : tr("Conversion Editor"));
    displayed_ = item ? item->comp<VConversion>()->platforms : std::vector<ConversionPlatformConfig>{};
    rebuildPlatforms();
    resultsRefresh_.start();
}

void ConversionEditorPanel::rebuildPlatforms()
{
    const QScopedValueRollback refreshing(refreshing_, true);
    auto* rows = platforms_->rowsLayout();
    while (QLayoutItem* entry = rows->takeAt(0)) {
        if (QWidget* widget = entry->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete entry;
    }
    tierRows_.clear();
    groupFooters_.clear();
    const auto* project = ProjectManager::instance().currentProject();
    if (project && boundItem()) {
        QVector<const ProjectPlatform*> ordered;
        const auto primary = PlatformContextService::instance().context().primary;
        if (const auto* platform = ProjectPlatformSemantics::find(project->platforms, primary))
            ordered.push_back(platform);
        for (const auto& platform : project->platforms)
            if (platform.id != primary) ordered.push_back(&platform);
        for (const auto* platform : ordered) {
            const auto* config = displayedConfig(platform->id);
            if (!config) continue;
            auto* header = platforms_->createGroupHeader(platform->name);
            header->setObjectName(QStringLiteral("conversionPlatformGroup"));
            for (size_t index = 0; index < config->tiers.size(); ++index)
                rows->addWidget(buildTierRow(*config, index));
            auto* footer = platforms_->createGroupFooter();
            const auto id = platform->id;
            connect(footer->addButton(), &QToolButton::clicked, this, [this, id] { addTier(id); });
            connect(footer->removeButton(), &QToolButton::clicked, this, [this, id] { removeTier(id); });
            groupFooters_.insert(id, footer);
        }
    }
    const auto* selected = displayedConfig(selectedPlatform_);
    if (!selected || selectedTier_ < 0 || static_cast<size_t>(selectedTier_) >= selected->tiers.size()) {
        selectedPlatform_ = QUuid{};
        selectedTier_ = -1;
    }
    refreshSelection();
    platforms_->refreshLayout();
}

QWidget* ConversionEditorPanel::buildTierRow(const ConversionPlatformConfig& config, size_t index)
{
    const auto tier = config.tiers[index];
    const auto platform = config.platformId;
    const auto rendered = config;
    auto* row = new QWidget(platforms_->rowsWidget());
    row->setObjectName(QStringLiteral("listRow"));
    row->setProperty("platformId", platform);
    row->setProperty("tierIndex", static_cast<int>(index));
    row->setProperty("selected", false);
    row->setProperty("lastRow", index + 1 == config.tiers.size());
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    const auto current = [this, platform, rendered]() -> const ConversionPlatformConfig* {
        const auto* value = displayedConfig(platform);
        return refreshing_ || !value || *value != rendered ? nullptr : value;
    };

    auto* formatCell = platforms_->createCell(row);
    formatCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* qualityCell = platforms_->createCell(row);
    qualityCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (index == 0) {
        auto* format = new QComboBox(formatCell);
        format->setObjectName(QStringLiteral("conversionFormat"));
        format->addItem(QStringLiteral("PCM"), static_cast<int>(ConversionFormat::PCM));
        format->addItem(QStringLiteral("Vorbis"), static_cast<int>(ConversionFormat::Vorbis));
        format->setCurrentIndex(format->findData(static_cast<int>(config.format)));
        formatCell->addWidget(format);
        connect(format, &QComboBox::currentIndexChanged, this, [this, format, current] {
            const auto* value = current();
            if (!value) return;
            auto next = *value;
            next.format = static_cast<ConversionFormat>(format->currentData().toInt());
            commitConfig(next);
        });
        auto* quality = new QSpinBox(qualityCell);
        quality->setObjectName(QStringLiteral("conversionQuality"));
        quality->setRange(0, 100);
        quality->setSuffix(QStringLiteral("%"));
        quality->setKeyboardTracking(false);
        quality->setValue(static_cast<int>(std::lround(config.vorbisQuality * 100.0f)));
        quality->setEnabled(config.format == ConversionFormat::Vorbis);
        qualityCell->addWidget(quality);
        connect(quality, &QSpinBox::valueChanged, this, [this, quality, current] {
            const auto* value = current();
            if (!value) return;
            auto next = *value;
            next.vorbisQuality = static_cast<float>(quality->value()) / 100.0f;
            commitConfig(next);
        });
    }
    layout->addWidget(formatCell);
    layout->addWidget(qualityCell);

    auto* limitCell = platforms_->createCell(row);
    limitCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (tier.maxBytes) {
        QLineEdit* value = nullptr;
        QComboBox* unit = nullptr;
        limitCell->addWidget(sizeInput(limitCell, value, unit, tier.maxBytes));
        connect(value, &QLineEdit::editingFinished, this, [this, index, value, unit, current] {
            const auto* config = current();
            if (!config) return;
            const auto bytes = sizeBytes(value->text(), unit->currentIndex());
            auto next = *config;
            if (bytes) next.tiers[index].maxBytes = *bytes;
            sortTiers(next.tiers);
            QString error;
            const bool valid = bytes && validateSampleRateTiers(next.tiers, &error);
            QtControlSkin::setSemanticProperty(value, "inputInvalid", !valid);
            value->setToolTip(valid ? QString() : error.isEmpty() ? tr("Enter a whole-byte size within range.") : error);
            if (valid) commitConfig(next);
        });
    } else {
        limitCell->addWidget(new QLabel(tr("Infinite"), limitCell));
    }
    layout->addWidget(limitCell);

    auto* rateCell = platforms_->createCell(row);
    rateCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* rate = sampleRates(rateCell, tier.sampleRate);
    rateCell->addWidget(rate);
    connect(rate, &QComboBox::currentIndexChanged, this, [this, index, rate, current] {
        const auto* config = current();
        if (!config) return;
        auto next = *config;
        next.tiers[index].sampleRate = rate->currentData().toUInt();
        commitConfig(next);
    });
    layout->addWidget(rateCell);

    platforms_->registerRow(row, {formatCell, qualityCell, limitCell, rateCell}, nullptr,
                            platform.toString(QUuid::WithoutBraces));
    tierRows_.append(row);
    return row;
}

void ConversionEditorPanel::refreshSelection()
{
    for (auto* row : tierRows_) {
        const bool selected = row->property("platformId").toUuid() == selectedPlatform_
            && row->property("tierIndex").toInt() == selectedTier_;
        QtControlSkin::setSemanticProperty(row, "selected", selected);
    }
    const auto* selected = displayedConfig(selectedPlatform_);
    const bool removable = selected && selectedTier_ >= 0
        && static_cast<size_t>(selectedTier_) < selected->tiers.size()
        && selected->tiers[selectedTier_].maxBytes.has_value();
    for (auto it = groupFooters_.cbegin(); it != groupFooters_.cend(); ++it)
        it.value()->removeButton()->setEnabled(removable && it.key() == selectedPlatform_);
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

void ConversionEditorPanel::addTier(const ProjectPlatformId& platform)
{
    const auto* config = displayedConfig(platform);
    if (!config) return;
    const auto before = displayed_;
    const auto preset = itemGuid_;
    const auto project = ProjectManager::instance().currentProjectLease();
    auto next = *config;
    BaseDialog dialog(this);
    dialog.setWindowTitle(tr("Add Range"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new DialogFormLayout;
    QLineEdit* value = nullptr;
    QComboBox* unit = nullptr;
    form->addRow(tr("Upper limit:"), sizeInput(&dialog, value, unit, std::nullopt));
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

void ConversionEditorPanel::removeTier(const ProjectPlatformId& platform)
{
    const auto* config = displayedConfig(platform);
    if (!config || platform != selectedPlatform_ || selectedTier_ < 0
        || static_cast<size_t>(selectedTier_) >= config->tiers.size()
        || !config->tiers[selectedTier_].maxBytes) return;
    auto next = *config;
    next.tiers.erase(next.tiers.begin() + selectedTier_);
    selectedPlatform_ = QUuid{};
    selectedTier_ = -1;
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
