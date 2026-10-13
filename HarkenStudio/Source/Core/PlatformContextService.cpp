#include "PlatformContextService.h"

#include "ProjectManager.h"
#include "PropertyEditSession.h"
#include "LanguageContextService.h"

#include <utility>

namespace VNX {
namespace {

QStringList selectedIds(const QVector<ProjectPlatformId>& selected)
{
    QStringList names;
    names.reserve(selected.size());
    for (ProjectPlatformId platform : selected)
        names.append(ProjectPlatformSemantics::idString(platform));
    return names;
}

}

PlatformContextService& PlatformContextService::instance()
{
    static PlatformContextService instance;
    return instance;
}

PlatformContextService::PlatformContextService()
    : PlatformContextService(
          SettingsStorageFactory::create(
              SettingsStorageFactory::StorageType::QSettings,
              QStringLiteral("HarkenSound"),
              QStringLiteral("HarkenStudio")))
{
}

PlatformContextService::PlatformContextService(
    std::unique_ptr<ISettingsStorage> storage,
    QObject* parent)
    : QObject(parent)
    , storage_(std::move(storage))
{
    auto& pm = ProjectManager::instance();
    connect(&pm, &ProjectManager::projectOpened, this,
            [this](ProjectData*) { recompute(); });
    connect(&pm, &ProjectManager::projectClosed, this, &PlatformContextService::recompute);
    connect(&pm, &ProjectManager::targetPlatformsChanged, this,
            [this](QStringList) { recompute(); });
    recompute();
}

void PlatformContextService::setAll(bool all)
{
    if (!ProjectManager::instance().isProjectOpen())
        return;
    setSelected(context_.selected, context_.primary, all);
}

void PlatformContextService::setSelected(const QVector<ProjectPlatformId>& selected, ProjectPlatformId primary)
{
    setSelected(selected, primary, context_.all);
}

void PlatformContextService::setSelected(const QVector<ProjectPlatformId>& selected,
                                         ProjectPlatformId primary,
                                         bool all)
{
    const QStringList targets = ProjectManager::instance().getTargetPlatforms();
    PlatformContext next = resolvePlatformContext(
        selectedIds(selected),
        ProjectPlatformSemantics::idString(primary),
        targets,
        all);
    if (!next.selected.isEmpty() && next.selected.contains(primary))
        next.primary = primary;
    applyContext(next);
}

QString PlatformContextService::storageGroup() const
{
    const auto* project = ProjectManager::instance().currentProject();
    return project ? QStringLiteral("PlatformContext/%1").arg(project->projectId) : QString();
}

void PlatformContextService::persistContext(const PlatformContext& context)
{
    if (!storage_ || storageGroup().isEmpty())
        return;
    storage_->beginGroup(storageGroup());
    storage_->setValue(QStringLiteral("selectedIds"), selectedIds(context.selected));
    storage_->setValue(QStringLiteral("primaryId"), ProjectPlatformSemantics::idString(context.primary));
    storage_->setValue(QStringLiteral("all"), context.all);
    storage_->endGroup();
    storage_->sync();
}

QStringList PlatformContextService::persistedSelectedIds() const
{
    if (!storage_ || storageGroup().isEmpty())
        return {};
    storage_->beginGroup(storageGroup());
    const QStringList selected =
        storage_->value(QStringLiteral("selectedIds")).toStringList();
    storage_->endGroup();
    return selected;
}

QString PlatformContextService::persistedPrimaryId() const
{
    if (!storage_ || storageGroup().isEmpty())
        return {};
    storage_->beginGroup(storageGroup());
    const QString primary =
        storage_->value(QStringLiteral("primaryId")).toString();
    storage_->endGroup();
    return primary;
}

std::optional<bool> PlatformContextService::persistedAll() const
{
    if (!storage_ || storageGroup().isEmpty())
        return std::nullopt;
    storage_->beginGroup(storageGroup());
    std::optional<bool> all;
    if (storage_->contains(QStringLiteral("all")))
        all = storage_->value(QStringLiteral("all")).toBool();
    storage_->endGroup();
    return all;
}

void PlatformContextService::recompute()
{
    const PlatformContext next = resolvePlatformContext(
        persistedSelectedIds(),
        persistedPrimaryId(),
        ProjectManager::instance().getTargetPlatforms(),
        persistedAll());
    applyContext(next);
}

void PlatformContextService::applyContext(const PlatformContext& next)
{
    if (next == context_)
        return;
    PropertyEditSession::finishAllActive(true);
    const bool primaryChanged = next.primary != context_.primary;
    if (!next.selected.isEmpty())
        persistContext(next);
    context_ = next;
    if (primaryChanged)
        ProjectManager::instance().refreshSoundSourceProjections(
            context_.primary, LanguageContextService::instance().language());
    emit contextChanged(context_);
}

void PlatformContextService::setStorage(std::unique_ptr<ISettingsStorage> storage)
{
    storage_ = std::move(storage);
    recompute();
}

void PlatformContextService::setStorageForTesting(std::unique_ptr<ISettingsStorage> storage)
{
    setStorage(std::move(storage));
}

}
