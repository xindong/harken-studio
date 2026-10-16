#include "Widgets/Inspector/Providers/SourcesSectionProvider.h"
#include "Widgets/Inspector/InspectorLayout.h"
#include "Widgets/Inspector/SoundInspectorSchema.h"

#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/AssetPipeline/AudioFormatCatalog.h"
#include "Core/AssetPipeline/AudioImportGate.h"
#include "Core/AssetPipeline/ImportSoundSourcesCommand.h"
#include "Core/AssetPipeline/MediaPool.h"
#include "Core/AssetPipeline/SourceFileSystem.h"
#include "Core/ToastManager.h"
#include "Core/Components/VSound.h"
#include "Core/LanguageContextService.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectLanguageSemantics.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/SoundSourceSemantics.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/LayoutDebugTint.h"
#include "Widgets/Common/SourceRangeThumb.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Shared/InspectorFormRow.h"
#include "Widgets/Inspector/InspectorPanel.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDynamicPropertyChangeEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <vector>

namespace VNX {
namespace {


constexpr int kActiveShare = 120;
constexpr int kRangeShare = 360;
constexpr int kSourceShare = 1000 - kActiveShare - kRangeShare;

struct SourceRow
{
    SoundSourceDef entry;
    QString language;
};

class SourcesListWidget : public QWidget
{
public:
    explicit SourcesListWidget(QWidget* parent)
        : QWidget(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setAcceptDrops(true);
        setObjectName(QStringLiteral("sourcesListWidget"));

        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, ThemeManager::instance().metrics().inspector.columnPadding, 0);
        v->setSpacing(2);
        reserveInspectorContentSlots(this);

        box_ = new TableWidget(this);
        box_->setSelectionClearHandler([this] { selectRow(-1); });
        box_->setRowSelectionHandler([this](QWidget* row) {
            selectRow(row->property("rowIndex").toInt());
        });
        v->addWidget(box_);
        buildHeader();
        rows_ = box_->rowsWidget();
        buildFooter();
        box_->setRowMoveHandler([this](int from, int to) { moveRow(from, to); },
            [this](int from, int to) {
                return boundItem_ && missingLanguageEntries().isEmpty()
                    && from < static_cast<int>(sources_.size()) && to < static_cast<int>(sources_.size())
                    && sources_[from].language == sources_[to].language;
            });

        QObject::connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged, this,
                         [this](ProjectItem* item, const QString& property, const QObject*) {
                             if (item == boundItem_
                                 && (property == QStringLiteral("sources")
                                     || property == QStringLiteral("distinguishLanguage")))
                                 reloadFromItem();
                         }, Qt::QueuedConnection);
        QObject::connect(&AssetRegistry::instance(), &AssetRegistry::assetModified, this,
                         [this](uint32_t) { reloadFromItem(); });
        QObject::connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
                         this, [this](const PlatformContext&) { refreshActiveColumn(); });
        QObject::connect(&ProjectManager::instance(), &ProjectManager::targetPlatformsChanged,
                         this, [this](const QStringList&) { refreshActiveColumn(); });
        QObject::connect(&LanguageContextService::instance(), &LanguageContextService::languageChanged,
                         this, [this](const QString&) {
                             refreshSelectionChrome();
                             refreshActiveColumn();
                         });
        QObject::connect(&ProjectManager::instance(), &ProjectManager::projectLanguagesChanged,
                         this, [this](QStringList) { reloadFromItem(); });
        QObject::connect(&ProjectManager::instance(), &ProjectManager::itemDeleted, this,
                         [this](ProjectItem* item) {
                             if (item == boundItem_)
                                 setBoundItem(nullptr);
                         });
        QObject::connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this,
                         [this]() { setBoundItem(nullptr); });

        qApp->installEventFilter(this);
    }

    ~SourcesListWidget() override
    {
        qApp->removeEventFilter(this);
    }

    QWidget* buildLanguageToggle(QWidget* parent)
    {
        toggle_ = new QCheckBox(QObject::tr("Language"), parent);
        toggle_->setObjectName(QStringLiteral("sourceDistinguishLanguage"));
        toggle_->setProperty("inspectorField", QStringLiteral("distinguishLanguage"));
        toggle_->setEnabled(false);
        QObject::connect(toggle_, &QCheckBox::clicked, this, [this](bool checked) {
            commitDistinguishLanguage(checked);
        });
        return toggle_;
    }

    void setBoundItem(ProjectItem* item)
    {
        if (boundItem_ != item) {
            box_->clearSelection();
            selectedGroupLanguage_.clear();
        }
        boundItem_ = item;
        reloadFromItem();
    }

    bool appendExisting(uint32_t sourceId)
    {
        if (!canAppendSources() || !AssetRegistry::instance().contains(sourceId))
            return false;
        auto* sound = boundItem_->comp<VSound>();
        if (!sound)
            return false;
        SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = sourceId;
        const QString language = distinguishLanguage() ? currentGroupLanguage() : QString();
        if (distinguishLanguage() && language.isEmpty())
            return false;
        if (const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceId)) {
            source.rangeContentHash = asset->contentHash;
            if (const auto seed = SoundSourceSemantics::fileLoopRegion(asset)) {
                source.loopRegion = seed;
                source.loopProvenance = SoundLoopProvenance::File;
            }
        }
        auto sources = sound->sources;
        auto& set = distinguishLanguage() ? sources.localized[language] : sources.plain;
        set.entries.push_back(source);
        return commit(sources);
    }

protected:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (!canAppendSources() || !AudioImportGate::mimeHasAudio(event->mimeData())) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        if (!canAppendSources() || !AudioImportGate::mimeHasAudio(event->mimeData())) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }

    void dropEvent(QDropEvent* event) override
    {
        const QStringList paths = AudioImportGate::extractAudioFiles(event->mimeData());
        if (!canAppendSources() || paths.isEmpty()) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
        importExternalPaths(paths);
    }

    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::DynamicPropertyChange) {
            const auto* change = static_cast<QDynamicPropertyChangeEvent*>(event);
            if (change->propertyName() == "selectedRow")
                selectRow(property("selectedRow").toInt());
            else if (change->propertyName() == "selectedGroup")
                selectGroup(property("selectedGroup").toString());
            else if (change->propertyName() == "appendSourceId")
                appendExisting(property("appendSourceId").toUInt());
        }
        return QWidget::event(event);
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!watched->isWidgetType())
            return QWidget::eventFilter(watched, event);

        auto* w = static_cast<QWidget*>(watched);
        if (w == this || isAncestorOf(w)) {
            const QEvent::Type type = event->type();
            QWidget* row = w;
            while (row && row->parentWidget() != rows_)
                row = row->parentWidget();
            const QUuid targetEntry = row ? QUuid(row->property("entryId").toString()) : QUuid();
            if (type == QEvent::DragEnter || type == QEvent::DragMove) {
                auto* drag = static_cast<QDropEvent*>(event);
                if (canAppendSources(targetEntry) && AudioImportGate::mimeHasAudio(drag->mimeData())) {
                    drag->setDropAction(Qt::CopyAction);
                    drag->accept();
                    return true;
                }
            } else if (type == QEvent::Drop) {
                auto* drop = static_cast<QDropEvent*>(event);
                const QStringList paths = AudioImportGate::extractAudioFiles(drop->mimeData());
                if (canAppendSources(targetEntry) && !paths.isEmpty()) {
                    drop->setDropAction(Qt::CopyAction);
                    drop->accept();
                    importExternalPaths(paths, targetEntry);
                    return true;
                }
            }
        }
        if (event->type() == QEvent::MouseButtonPress) {
            QWidget* rowHost = w;
            while (rowHost && rowHost->parentWidget() != rows_)
                rowHost = rowHost->parentWidget();
            if (rowHost && rowHost->property("rowIndex").isValid()) {
                selectRow(rowHost->property("rowIndex").toInt());
            } else if (rowHost && !rowHost->property("groupLanguage").toString().isEmpty()) {
                const QString language = rowHost->property("groupLanguage").toString();
                if (buildLanguages().contains(language))
                    selectGroup(language);
            } else if ((selected_ >= 0 || !selectedGroupLanguage_.isEmpty())
                       && w != this && !isAncestorOf(w)
                       && !w->isAncestorOf(this)
                       && w->window() == window()) {
                selectRow(-1);
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    QWidget* buildHeader()
    {
        box_->setColumns(QStringLiteral("Sources"), {
            {QStringLiteral("Number"), {}, 0},
            {QStringLiteral("Source"), {}, kSourceShare},
            {QStringLiteral("Range"), {}, kRangeShare},
            {QStringLiteral("Active"), {}, kActiveShare}
        });
        applyInspectorTableDescriptor(box_, soundInspectorSchema(), QStringLiteral("sources"));
        return box_->header();
    }

    QWidget* buildFooter()
    {
        auto* foot = box_->footer();
        addBtn_ = foot->addButton();
        addBtn_->setObjectName(QStringLiteral("listAdd_sources"));
        removeBtn_ = foot->removeButton();
        removeBtn_->setObjectName(QStringLiteral("listRemoveSelected_sources"));

        QObject::connect(addBtn_, &QToolButton::clicked, this, [this]() { showAddMenu(); });
        QObject::connect(removeBtn_, &QToolButton::clicked, this, [this]() { removeSelected(); });

        return foot;
    }

    void selectRow(int index)
    {
        QString group;
        if (index >= 0 && index < static_cast<int>(sources_.size())) {
            const QString language = sources_[index].language;
            if (buildLanguages().contains(language))
                group = language;
        }
        if (selected_ == index && selectedGroupLanguage_ == group)
            return;
        selected_ = index;
        selectedGroupLanguage_ = group;
        refreshSelectionChrome();
    }

    void selectGroup(const QString& language)
    {
        const QString group = buildLanguages().contains(language) ? language : QString();
        box_->clearSelection();
        selectedGroupLanguage_ = group;
        if (!group.isEmpty())
            LanguageContextService::instance().setLanguage(group);
        refreshSelectionChrome();
    }

    void refreshSelectionChrome()
    {
        for (QWidget* row : rowWidgets_) {
            row->setProperty("selected", row->property("rowIndex").toInt() == selected_);
            row->style()->unpolish(row);
            row->style()->polish(row);
        }
        for (QWidget* header : groupHeaders_) {
            header->setProperty(
                "selected",
                selected_ < 0 && header->property("groupLanguage").toString() == selectedGroupLanguage_
                    && selectedGroupLanguage_ == LanguageContextService::instance().language()
                    && !selectedGroupLanguage_.isEmpty());
            header->style()->unpolish(header);
            header->style()->polish(header);
        }
        const QSet<QUuid> missing = missingLanguageEntries();
        const bool hasSelected = selected_ >= 0 && selected_ < static_cast<int>(sources_.size());
        if (removeBtn_) {
            const bool canRemove = hasSelected && (missing.isEmpty()
                || (missing.size() == 1 && missing.contains(sources_[selected_].entry.entryId)));
            removeBtn_->setEnabled(canRemove);
            removeBtn_->setToolTip(!missing.isEmpty() && !canRemove ? missingLanguagesHint() : QString());
        }
        if (addBtn_) {
            addBtn_->setEnabled(canAppendSources());
            addBtn_->setToolTip(missing.isEmpty() ? QString() : missingLanguagesHint());
        }
    }

    void showAddMenu()
    {
        QMenu menu(this);
        QAction* importAction = menu.addAction(QObject::tr("Import from file…"));
        QObject::connect(importAction, &QAction::triggered, this, [this]() { importFiles(); });
        QAction* existingAction = menu.addAction(QObject::tr("Add existing…"));
        QObject::connect(existingAction, &QAction::triggered, this, [this]() { chooseExisting(); });
        menu.exec(addBtn_->mapToGlobal(QPoint(0, addBtn_->height())));
    }

    void chooseExisting()
    {
        if (!boundItem_)
            return;
        QPointer<SourcesListWidget> self(this);
        const uint32_t ownerAtOpen = boundItem_->objectID;
        SelectorRequest req;
        req.title = QObject::tr("Add Existing Source");
        req.domain = RefDomain::AudioAsset;
        req.onSelected = [self, ownerAtOpen](uint32_t id) {
            if (!self || !self->boundItem_ || self->boundItem_->objectID != ownerAtOpen)
                return;
            self->appendExisting(id);
        };
        SelectorWindow::open(req, addBtn_);
    }

    void importFiles()
    {
        if (!boundItem_)
            return;
        const QStringList paths = QFileDialog::getOpenFileNames(
            this, QObject::tr("Import Audio Sources"), {},
            QObject::tr("Audio Files (%1)").arg(AudioFormatCatalog::filePatterns().join(' ')));
        if (paths.isEmpty())
            return;
        importExternalPaths(paths);
    }

    void importExternalPaths(const QStringList& paths, const QUuid& targetEntry = {})
    {
        if (!canAppendSources(targetEntry) || paths.isEmpty())
            return;
        const QString language = distinguishLanguage() ? currentGroupLanguage(targetEntry) : QString();
        if (distinguishLanguage() && language.isEmpty())
            return;
        ProjectData* project = ProjectManager::instance().currentProject();
        ProjectFileBinding* binding = project
            ? findFileBindingForItem(*project, boundItem_) : nullptr;
        if (binding)
            ImportSoundSourcesCommand::pushImport(
                paths, binding, boundItem_, language, targetEntry);
    }

    void relink(uint32_t sourceId)
    {
        const QString projectRoot = AssetRegistry::instance().projectRoot();
        const QString path = QFileDialog::getOpenFileName(
            this, QObject::tr("Relink Audio Source"), {},
            QObject::tr("Audio Files (%1)").arg(AudioFormatCatalog::filePatterns().join(' ')));
        if (path.isEmpty() || projectRoot != AssetRegistry::instance().projectRoot())
            return;
        const auto* asset = AssetRegistry::instance().getAsset(sourceId);
        if (!asset || QFileInfo(path).canonicalFilePath() == QFileInfo(asset->absolutePath).canonicalFilePath())
            return;
        QString error;
        auto command = SourceFileSystem::replaceFiles({{asset->relativePath, path}}, &error);
        if (command)
            UndoRedoManager::instance().push(command.release());
        else
            ToastManager::instance().showToast(error, 5000, ToastType::Error);
    }

    int moveTarget(int index, int delta) const
    {
        if (index < 0 || index >= static_cast<int>(sources_.size()) || delta == 0)
            return -1;
        if (!distinguishLanguage()) {
            const int target = index + delta;
            if (target < 0 || target >= static_cast<int>(sources_.size()))
                return -1;
            return target;
        }
        const QString language = sources_[index].language;
        if (delta < 0) {
            for (int i = index - 1; i >= 0; --i) {
                if (sources_[i].language == language)
                    return i;
            }
        } else {
            for (int i = index + 1; i < static_cast<int>(sources_.size()); ++i) {
                if (sources_[i].language == language)
                    return i;
            }
        }
        return -1;
    }

    void moveRow(int index, int target)
    {
        auto copy = sources_;
        SourceRow row = copy[index];
        copy.erase(copy.begin() + index);
        copy.insert(copy.begin() + target, std::move(row));
        selectRow(index);
        commit(copy);
    }

    void moveToLanguage(int index, const QString& language)
    {
        if (!boundItem_ || index < 0 || index >= static_cast<int>(sources_.size()))
            return;
        moveSourcesToLanguage({sources_[index].entry.entryId}, language);
    }

    void moveSourcesToLanguage(const QSet<QUuid>& entryIds, const QString& language)
    {
        if (!distinguishLanguage() || !buildLanguages().contains(language))
            return;
        auto* current = boundItem_->comp<VSound>();
        if (!current)
            return;
        auto sources = current->sources;
        SoundSourceSemantics::moveToLanguage(sources, entryIds, language);
        commit(sources);
    }

    QSet<QUuid> missingLanguageEntries() const
    {
        const auto* sound = boundItem_ ? boundItem_->comp<VSound>() : nullptr;
        if (!sound || !sound->distinguishLanguage)
            return {};
        const QStringList languages = buildLanguages();
        QSet<QUuid> missing;
        for (auto it = sound->sources.localized.cbegin(); it != sound->sources.localized.cend(); ++it) {
            if (!languages.contains(it.key()))
                for (const auto& source : it->entries)
                    missing.insert(source.entryId);
        }
        return missing;
    }

    static QString missingLanguagesHint()
    {
        return QObject::tr("Move or remove sources with missing languages before editing. "
                           "Right-click an affected source for repair actions.");
    }

    bool canAppendSources(const QUuid& targetEntry = {}) const
    {
        const auto* sound = boundItem_ ? boundItem_->comp<VSound>() : nullptr;
        return sound && (!sound->distinguishLanguage
            || (!currentGroupLanguage(targetEntry).isEmpty() && missingLanguageEntries().isEmpty()));
    }

    void removeMissingLanguageSources()
    {
        const QSet<QUuid> missing = missingLanguageEntries();
        if (missing.isEmpty())
            return;
        const auto remove = [this] {
            const auto* sound = boundItem_->comp<VSound>();
            auto sources = sound->sources;
            const QStringList languages = buildLanguages();
            sources.localized.removeIf([&](QMap<QString, SoundSourceSet>::iterator it) { return !languages.contains(it.key()); });
            return commit(sources);
        };
        if (selected_ >= 0 && selected_ < static_cast<int>(sources_.size())
            && missing.contains(sources_[selected_].entry.entryId))
            box_->removeSelectedRow(remove);
        else
            remove();
    }

    void commitDistinguishLanguage(bool enabled)
    {
        if (!boundItem_)
            return;
        const ActionResult result = ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(boundItem_->objectID)},
             {QStringLiteral("property"), QStringLiteral("distinguishLanguage")},
             {QStringLiteral("value"), enabled}},
            ActionSource::GUI, this);
        if (!result.success)
            reloadFromItem();
    }

    void removeSelected()
    {
        if (!boundItem_ || selected_ < 0 || selected_ >= static_cast<int>(sources_.size()))
            return;
        auto* current = boundItem_->comp<VSound>();
        if (!current)
            return;
        box_->removeSelectedRow([this, current] {
            auto copy = current->sources;
            SoundSourceSemantics::removeEntry(copy, sources_[selected_].entry.entryId);
            const QString language = sources_[selected_].language;
            if (distinguishLanguage() && copy.localized[language].entries.empty())
                copy.localized.remove(language);
            return commit(copy);
        });
    }

    void relinkRow(int index, uint32_t sourceId)
    {
        if (!boundItem_ || index < 0 || index >= static_cast<int>(sources_.size()))
            return;
        if (!AssetRegistry::instance().contains(sourceId))
            return;
        auto copy = sources_;
        SoundSourceDef& source = copy[index].entry;
        if (source.sourceId == sourceId)
            return;
        source.sourceId = sourceId;
        if (const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceId)) {
            source.rangeContentHash = asset->contentHash;
            if (!source.loopRegion) {
                if (const auto seed = SoundSourceSemantics::fileLoopRegion(asset)) {
                    source.loopRegion = seed;
                    source.loopProvenance = SoundLoopProvenance::File;
                }
            }
        }
        commit(copy);
    }

    bool commit(const std::vector<SourceRow>& rows)
    {
        const auto* current = boundItem_ ? boundItem_->comp<VSound>() : nullptr;
        if (!current) return false;
        auto sources = current->sources;
        if (current->distinguishLanguage) {
            for (auto& set : sources.localized) set.entries.clear();
            for (const auto& row : rows)
                sources.localized[row.language].entries.push_back(row.entry);
        } else {
            sources.plain.entries.clear();
            for (const auto& row : rows) sources.plain.entries.push_back(row.entry);
        }
        return commit(sources);
    }

    bool commit(const SoundSources& sources)
    {
        if (!boundItem_) return false;
        const ActionResult result = ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(boundItem_->objectID)},
             {QStringLiteral("property"), QStringLiteral("sources")},
             {QStringLiteral("value"), SoundSourceSemantics::toJson(sources)}},
            ActionSource::GUI, this);
        if (!result.success)
            reloadFromItem();
        return result.success;
    }

    bool distinguishLanguage() const
    {
        const auto* sound = boundItem_ ? boundItem_->comp<VSound>() : nullptr;
        return sound && sound->distinguishLanguage;
    }

    QStringList buildLanguages() const
    {
        const ProjectData* project = ProjectManager::instance().currentProject();
        return project ? ProjectLanguageSemantics::buildLanguages(*project)
                       : QStringList();
    }

    QString currentGroupLanguage(const QUuid& targetEntry = {}) const
    {
        const QStringList languages = buildLanguages();
        if (!targetEntry.isNull()) {
            for (const auto& source : sources_)
                if (source.entry.entryId == targetEntry && languages.contains(source.language))
                    return source.language;
            return {};
        }
        if (selected_ >= 0 && selected_ < static_cast<int>(sources_.size())) {
            const QString selected = sources_[selected_].language;
            if (languages.contains(selected))
                return selected;
        }
        const QString bar = LanguageContextService::instance().language();
        return languages.contains(bar) ? bar : QString();
    }

    QString mixedActiveTooltip(const QString& language) const
    {
        const auto* sound = boundItem_ ? boundItem_->comp<VSound>() : nullptr;
        if (!sound)
            return {};
        const PlatformContext& context = PlatformContextService::instance().context();
        if (!context.all)
            return {};
        QStringList lines;
        QSet<QUuid> distinct;
        const auto* project = ProjectManager::instance().currentProject();
        if (!project) return {};
        for (ProjectPlatformId platform : context.selected) {
            const auto* definition = ProjectPlatformSemantics::find(project->platforms, platform);
            if (!definition) continue;
            const auto resolved = SoundSourceSemantics::resolveSourceSelection(
                *sound, ProjectPlatformSemantics::idString(platform), language);
            if (resolved.source) {
                distinct.insert(resolved.source->entryId);
                lines.append(QStringLiteral("%1: %2")
                                 .arg(definition->name, rowDisplayName(*resolved.source)));
            } else {
                distinct.insert(QUuid());
                lines.append(QStringLiteral("%1: %2")
                                 .arg(definition->name, QObject::tr("None")));
            }
        }
        if (distinct.size() <= 1)
            return {};
        return lines.join(QLatin1Char('\n'));
    }

    QUuid resolvedActiveEntry(const QString& language) const
    {
        const auto* sound = boundItem_ ? boundItem_->comp<VSound>() : nullptr;
        if (!sound)
            return {};
        const PlatformContext& context = PlatformContextService::instance().context();
        if (!context.all) {
            const auto resolved = SoundSourceSemantics::resolveSourceSelection(
                *sound, ProjectPlatformSemantics::idString(context.primary), language);
            return resolved.source ? resolved.source->entryId : QUuid{};
        }
        QUuid shared;
        for (ProjectPlatformId platform : context.selected) {
            const auto resolved = SoundSourceSemantics::resolveSourceSelection(
                *sound, ProjectPlatformSemantics::idString(platform), language);
            if (!resolved.source)
                return {};
            if (shared.isNull())
                shared = resolved.source->entryId;
            else if (shared != resolved.source->entryId)
                return {};
        }
        return shared;
    }

    QString rowDisplayName(const SoundSourceDef& source) const
    {
        if (const AssetInfo* asset = AssetRegistry::instance().getAsset(source.sourceId)) {
            if (!asset->relativePath.isEmpty())
                return asset->relativePath;
        }
        return QString::number(source.sourceId);
    }

    void openRangeEditor(int index)
    {
        if (!boundItem_ || index < 0 || index >= static_cast<int>(sources_.size()))
            return;
        if (!AssetRegistry::instance().sourceFileExists(sources_[index].entry.sourceId))
            return;
        for (QWidget* parent = parentWidget(); parent; parent = parent->parentWidget()) {
            auto* inspector = qobject_cast<InspectorPanel*>(parent);
            if (!inspector) continue;
            inspector->requestExternalEditor(
                ExternalEditorSelection::source(boundItem_->guid, sources_[index].entry.entryId));
            return;
        }
    }

    void assignActive(int index)
    {
        if (!boundItem_ || index < 0 || index >= static_cast<int>(sources_.size()))
            return;
        auto* sound = boundItem_->comp<VSound>();
        if (!sound)
            return;
        const PlatformContext& context = PlatformContextService::instance().context();
        if (context.selected.isEmpty())
            return;
        if (distinguishLanguage() && !buildLanguages().contains(sources_[index].language))
            return;
        const QString language = distinguishLanguage()
            ? sources_[index].language : QString();
        const QUuid entryId = sources_[index].entry.entryId;
        auto sources = sound->sources;
        auto& set = distinguishLanguage() ? sources.localized[language] : sources.plain;
        SoundSourceSemantics::assignActive(set, entryId, context.all ? QString()
            : ProjectPlatformSemantics::idString(context.primary));
        commit(sources);
    }

    void refreshActiveColumn()
    {
        const auto* sound = boundItem_ ? boundItem_->comp<VSound>() : nullptr;
        if (!sound)
            return;
        const bool canEdit = missingLanguageEntries().isEmpty();
        for (QWidget* row : rowWidgets_) {
            auto* check = row->findChild<QCheckBox*>(QStringLiteral("sourceActive"));
            if (!check)
                continue;
            const int index = row->property("rowIndex").toInt();
            if (index < 0 || index >= static_cast<int>(sources_.size()))
                continue;
            const bool grouped = distinguishLanguage();
            const QString language = grouped
                ? sources_[index].language : QString();
            const bool ungrouped = grouped && !buildLanguages().contains(sources_[index].language);
            const QSignalBlocker blocker(check);
            check->setEnabled(canEdit);
            if (ungrouped) {
                check->setTristate(false);
                check->setCheckState(Qt::Unchecked);
                check->setToolTip(missingLanguagesHint());
                continue;
            }
            const QUuid active = resolvedActiveEntry(language);
            const QString mixed = mixedActiveTooltip(language);
            const QUuid entryId = QUuid::fromString(row->property("entryId").toString());
            check->setTristate(!mixed.isEmpty());
            check->setCheckState(!mixed.isEmpty() ? Qt::PartiallyChecked
                : (!active.isNull() && entryId == active ? Qt::Checked : Qt::Unchecked));
            check->setToolTip(canEdit ? mixed : missingLanguagesHint());
        }
    }

    void reloadFromItem()
    {
        const QUuid selectedEntry = selected_ >= 0 && selected_ < static_cast<int>(sources_.size())
            ? sources_[selected_].entry.entryId : QUuid();
        const auto* sound = boundItem_ ? boundItem_->comp<VSound>() : nullptr;
        sources_.clear();
        if (sound) {
            if (sound->distinguishLanguage) {
                for (auto it = sound->sources.localized.cbegin(); it != sound->sources.localized.cend(); ++it)
                    for (const auto& entry : it->entries)
                        sources_.push_back({entry, it.key()});
            } else {
                for (const auto& entry : sound->sources.plain.entries)
                    sources_.push_back({entry, {}});
            }
        }
        selected_ = -1;
        for (int index = 0; index < static_cast<int>(sources_.size()); ++index) {
            if (sources_[index].entry.entryId == selectedEntry) {
                selected_ = index;
                selectedGroupLanguage_ = sources_[index].language;
                break;
            }
        }
        if (selected_ < 0 && !selectedEntry.isNull())
            selectedGroupLanguage_.clear();
        if (toggle_) {
            QSignalBlocker blocker(toggle_);
            toggle_->setChecked(sound && sound->distinguishLanguage);
            toggle_->setEnabled(sound && (!buildLanguages().isEmpty() || sound->distinguishLanguage));
        }
        rebuildRows();
    }

    QWidget* buildRow(int index)
    {
        const SoundSourceDef& source = sources_[index].entry;
        const QSet<QUuid> missing = missingLanguageEntries();
        const bool canEdit = missing.isEmpty();

        auto* row = new QWidget(rows_);
        row->setObjectName(QStringLiteral("listRow"));
        row->setAttribute(Qt::WA_StyledBackground, true);
        DebugTint::tint(row, DebugTint::Role::Row);
        row->setProperty("rowIndex", index);
        row->setProperty("entryId", source.entryId.toString(QUuid::WithoutBraces));
        row->setProperty("selected", false);
        row->setProperty("lastRow", false);
        row->setContextMenuPolicy(Qt::CustomContextMenu);
        if (!canEdit)
            row->setToolTip(missingLanguagesHint());
        const bool canMoveUp = canEdit && moveTarget(index, -1) >= 0;
        const bool canMoveDown = canEdit && moveTarget(index, 1) >= 0;
        auto* moveUp = new QAction(QObject::tr("Move Up"), row);
        moveUp->setObjectName(QStringLiteral("sourceMoveUp"));
        moveUp->setEnabled(canMoveUp);
        QObject::connect(moveUp, &QAction::triggered, this, [this, index]() { box_->moveRow(index, moveTarget(index, -1)); });
        auto* moveDown = new QAction(QObject::tr("Move Down"), row);
        moveDown->setObjectName(QStringLiteral("sourceMoveDown"));
        moveDown->setEnabled(canMoveDown);
        QObject::connect(moveDown, &QAction::triggered, this, [this, index]() { box_->moveRow(index, moveTarget(index, 1)); });
        auto* relinkAction = new QAction(QObject::tr("Relink…"), row);
        relinkAction->setObjectName(QStringLiteral("sourceRelink"));
        QObject::connect(relinkAction, &QAction::triggered, this, [this, index]() {
            relink(sources_[index].entry.sourceId);
        });
        row->addAction(moveUp);
        row->addAction(moveDown);
        if (distinguishLanguage()) {
            const QStringList languages = buildLanguages();
            auto* moveTo = new QAction(QObject::tr("Move to"), row);
            moveTo->setObjectName(QStringLiteral("sourceMoveTo"));
            auto* moveMenu = new QMenu(row);
            moveMenu->setObjectName(QStringLiteral("sourceMoveToMenu"));
            for (const QString& language : languages) {
                if (language == sources_[index].language)
                    continue;
                auto* destination = moveMenu->addAction(language);
                destination->setObjectName(QStringLiteral("sourceMoveTo_") + language);
                QObject::connect(destination, &QAction::triggered, this,
                                 [this, index, language]() { moveToLanguage(index, language); });
            }
            moveTo->setMenu(moveMenu);
            moveTo->setEnabled(!moveMenu->isEmpty() && (canEdit
                || (missing.size() == 1 && missing.contains(source.entryId))));
            if (!canEdit && !moveTo->isEnabled())
                moveTo->setToolTip(missingLanguagesHint());
            row->addAction(moveTo);
            if (missing.contains(source.entryId) && missing.size() > 1) {
                if (!languages.isEmpty()) {
                    auto* moveAll = new QAction(
                        QObject::tr("Move all sources with missing languages to"), row);
                    moveAll->setObjectName(QStringLiteral("sourceMoveAllMissingLanguagesTo"));
                    auto* destinations = new QMenu(row);
                    destinations->setObjectName(QStringLiteral("sourceMoveAllMissingLanguagesMenu"));
                    for (const QString& language : languages) {
                        auto* destination = destinations->addAction(language);
                        destination->setObjectName(
                            QStringLiteral("sourceMoveAllMissingLanguagesTo_") + language);
                        QObject::connect(destination, &QAction::triggered, this, [this, language]() {
                            moveSourcesToLanguage(missingLanguageEntries(), language);
                        });
                    }
                    moveAll->setMenu(destinations);
                    row->addAction(moveAll);
                }
                auto* removeAll = new QAction(
                    QObject::tr("Remove all sources with missing languages"), row);
                removeAll->setObjectName(QStringLiteral("sourceRemoveAllMissingLanguages"));
                QObject::connect(removeAll, &QAction::triggered, this,
                                 [this]() { removeMissingLanguageSources(); });
                row->addAction(removeAll);
            }
        }
        row->addAction(relinkAction);
        QObject::connect(row, &QWidget::customContextMenuRequested, row,
                         [row](const QPoint& pos) {
                             QMenu menu(row);
                             menu.setToolTipsVisible(true);
                             for (QAction* action : row->actions()) {
                                 if (QMenu* nested = action->menu()) {
                                     QMenu* submenu = menu.addMenu(action->text());
                                     submenu->setObjectName(nested->objectName());
                                     submenu->menuAction()->setEnabled(action->isEnabled());
                                     submenu->menuAction()->setToolTip(action->toolTip());
                                     submenu->addActions(nested->actions());
                                 } else {
                                     menu.addAction(action);
                                 }
                             }
                             menu.exec(row->mapToGlobal(pos));
                         });

        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);

        auto* number = box_->createRowNumber(row, index);
        number->setEnabled(canEdit);
        h->addWidget(number);

        auto* sourceCell = box_->createCell(row);
        DebugTint::tintContainer(sourceCell, DebugTint::Role::Flex);
        sourceCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto* reference = new ObjectRefWidget(sourceCell);
        reference->setObjectName(QStringLiteral("sourceRef"));
        reference->setRefDomain(RefDomain::AudioAsset);
        reference->setSelectorTitle(QObject::tr("Select Source"));
        reference->setCompact(true);
        reference->setCompactPickable(true);
        reference->setClearSupport(false);
        reference->setCurrentObjectID(source.sourceId);
        reference->setReadOnly(!canEdit);
        QObject::connect(reference, &ObjectRefWidget::objectChanged, this,
                         [this, index](uint32_t id) { relinkRow(index, id); });
        sourceCell->addWidget(reference);
        h->addWidget(sourceCell);

        auto* rangeCell = box_->createCell(row);
        DebugTint::tintContainer(rangeCell, DebugTint::Role::ColDelay);
        rangeCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        rangeCell->setEnabled(canEdit);
        auto* thumb = new SourceRangeThumb(rangeCell);
        thumb->setSource(source, AssetRegistry::instance().getAsset(source.sourceId));
        QObject::connect(thumb, &SourceRangeThumb::clicked, this, [this, index]() {
            selectRow(index);
            openRangeEditor(index);
        });
        rangeCell->addWidget(thumb);
        h->addWidget(rangeCell);

        auto* activeCell = box_->createCell(row);
        DebugTint::tintContainer(activeCell, DebugTint::Role::ColToggle);
        activeCell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto* active = new QCheckBox(activeCell);
        active->setObjectName(QStringLiteral("sourceActive"));
        QObject::connect(active, &QCheckBox::clicked, this, [this, index](bool) {
            assignActive(index);
            refreshActiveColumn();
        });
        activeCell->addWidget(active, 0, Qt::AlignCenter);
        h->addWidget(activeCell);

        box_->registerRow(row, {number, sourceCell, rangeCell, activeCell});
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
        groupHeaders_.clear();
        if (!buildLanguages().contains(selectedGroupLanguage_))
            selectedGroupLanguage_.clear();
        if (removeBtn_)
            removeBtn_->setEnabled(false);

        if (!distinguishLanguage()) {
            for (int i = 0; i < static_cast<int>(sources_.size()); ++i)
                rl->addWidget(buildRow(i));
            markLastRow();
            refreshSelectionChrome();
            refreshActiveColumn();
            return;
        }

        const QStringList languages = buildLanguages();
        QSet<int> placed;
        auto addGroup = [&](const QString& language) {
            rl->addWidget(buildGroupHeader(language));
            for (int i = 0; i < static_cast<int>(sources_.size()); ++i) {
                if (sources_[i].language != language)
                    continue;
                rl->addWidget(buildRow(i));
                placed.insert(i);
            }
        };
        for (const QString& language : languages)
            addGroup(language);
        for (int i = 0; i < static_cast<int>(sources_.size()); ++i) {
            if (!placed.contains(i))
                rl->addWidget(buildRow(i));
        }
        markLastRow();
        refreshSelectionChrome();
        refreshActiveColumn();
    }

    void markLastRow()
    {
        if (rowWidgets_.empty())
            return;
        QWidget* row = rowWidgets_.back();
        row->setProperty("lastRow", true);
        row->style()->unpolish(row);
        row->style()->polish(row);
    }

    QWidget* buildGroupHeader(const QString& language)
    {
        auto* header = new QWidget(rows_);
        header->setObjectName(QStringLiteral("sourceLanguageGroup"));
        header->setAttribute(Qt::WA_StyledBackground, true);
        header->setProperty("groupLanguage", language);
        header->setProperty("selected", false);
        header->setCursor(Qt::PointingHandCursor);
        groupHeaders_.push_back(header);
        auto* h = new QHBoxLayout(header);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        auto* label = new QLabel(language, header);
        label->setObjectName(QStringLiteral("sourceLanguageGroupLabel"));
        h->addWidget(label);
        box_->registerGroupRow(header);
        return header;
    }

    QCheckBox* toggle_ = nullptr;
    TableWidget* box_ = nullptr;
    QWidget* rows_ = nullptr;
    QToolButton* addBtn_ = nullptr;
    QToolButton* removeBtn_ = nullptr;
    std::vector<QWidget*> rowWidgets_;
    std::vector<QWidget*> groupHeaders_;
    std::vector<SourceRow> sources_;
    int selected_ = -1;
    QString selectedGroupLanguage_;
    ProjectItem* boundItem_ = nullptr;
};

}

QWidget* SourcesSectionProvider::build(QWidget* parent)
{
    auto* content = new QWidget(parent);
    auto* cl = new QVBoxLayout(content);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(2);

    auto* list = new SourcesListWidget(content);
    cl->addWidget(list);

    return content;
}

QWidget* SourcesSectionProvider::buildHeader(QWidget* widget, QWidget* parent)
{
    auto* list = static_cast<SourcesListWidget*>(
        widget->findChild<QWidget*>(QStringLiteral("sourcesListWidget")));
    return list->buildLanguageToggle(parent);
}

void SourcesSectionProvider::bind(QWidget* widget, ProjectItem* item)
{
    if (!widget)
        return;
    widget->setProperty("boundItemPtr",
                        QVariant::fromValue(reinterpret_cast<quintptr>(item)));
    if (QWidget* found = widget->findChild<QWidget*>(QStringLiteral("sourcesListWidget")))
        static_cast<SourcesListWidget*>(found)->setBoundItem(item);
}

void registerSourcesSectionProvider()
{
    InspectorCustomizationRegistry::instance().registerSectionProvider(
        ProjectItem::Type::Sound, QStringLiteral("sources"),
        std::make_unique<SourcesSectionProvider>());
}

}
