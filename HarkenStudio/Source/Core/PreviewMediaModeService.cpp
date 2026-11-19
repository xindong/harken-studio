#include "PreviewMediaModeService.h"

#include "ProjectManager.h"

namespace VNX {

PreviewMediaModeService& PreviewMediaModeService::instance()
{
    static PreviewMediaModeService service;
    return service;
}

PreviewMediaModeService::PreviewMediaModeService()
    : PreviewMediaModeService(SettingsStorageFactory::create(
          SettingsStorageFactory::StorageType::QSettings,
          QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")))
{
}

PreviewMediaModeService::PreviewMediaModeService(
    std::unique_ptr<ISettingsStorage> storage, QObject* parent)
    : QObject(parent), storage_(std::move(storage))
{
    auto& manager = ProjectManager::instance();
    connect(&manager, &ProjectManager::projectOpened, this, &PreviewMediaModeService::restore);
    connect(&manager, &ProjectManager::projectClosed, this, &PreviewMediaModeService::restore);
    restore();
}

QString PreviewMediaModeService::storageGroup() const
{
    const auto* project = ProjectManager::instance().currentProject();
    return project ? QStringLiteral("PreviewMedia/%1").arg(project->projectId) : QString{};
}

PreviewMediaMode PreviewMediaModeService::mode()
{
    if (storageGroup() != restoredGroup_) restore();
    return mode_;
}

void PreviewMediaModeService::restore()
{
    auto next = PreviewMediaMode::Original;
    const QString group = storageGroup();
    if (storage_ && !group.isEmpty()) {
        storage_->beginGroup(group);
        if (storage_->value(QStringLiteral("mode")).toString() == QStringLiteral("Converted"))
            next = PreviewMediaMode::Converted;
        storage_->endGroup();
    }
    restoredGroup_ = group;
    if (next == mode_) return;
    mode_ = next;
    emit modeChanged(mode_);
}

void PreviewMediaModeService::setMode(PreviewMediaMode mode)
{
    this->mode();
    const QString group = storageGroup();
    if (group.isEmpty() || mode == mode_) return;
    if (storage_) {
        storage_->beginGroup(group);
        storage_->setValue(QStringLiteral("mode"), mode == PreviewMediaMode::Converted
            ? QStringLiteral("Converted") : QStringLiteral("Original"));
        storage_->endGroup();
        storage_->sync();
    }
    mode_ = mode;
    emit modeChanged(mode_);
}

void PreviewMediaModeService::setStorageForTesting(std::unique_ptr<ISettingsStorage> storage)
{
    storage_ = std::move(storage);
    restore();
}

}
