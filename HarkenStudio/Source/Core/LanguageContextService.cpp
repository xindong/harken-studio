#include "LanguageContextService.h"

#include "ProjectLanguageSemantics.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"

#include <utility>

namespace VNX {

LanguageContextService& LanguageContextService::instance()
{
    static LanguageContextService instance;
    return instance;
}

LanguageContextService::LanguageContextService()
    : LanguageContextService(
          SettingsStorageFactory::create(
              SettingsStorageFactory::StorageType::QSettings,
              QStringLiteral("HarkenSound"),
              QStringLiteral("HarkenStudio")))
{
}

LanguageContextService::LanguageContextService(
    std::unique_ptr<ISettingsStorage> storage,
    QObject* parent)
    : QObject(parent)
    , storage_(std::move(storage))
{
    auto& pm = ProjectManager::instance();
    connect(&pm, &ProjectManager::projectOpened, this,
            [this](ProjectData*) { recompute(); });
    connect(&pm, &ProjectManager::projectClosed, this, &LanguageContextService::recompute);
    connect(&pm, &ProjectManager::projectLanguagesChanged, this,
            [this](QStringList) { recompute(); });
    recompute();
}

QStringList LanguageContextService::availableLanguages() const
{
    const ProjectData* project = ProjectManager::instance().currentProject();
    if (!project)
        return {};
    return ProjectLanguageSemantics::buildLanguages(*project);
}

void LanguageContextService::setLanguage(const QString& language)
{
    const QStringList options = availableLanguages();
    QString next = language.trimmed();
    if (!options.contains(next))
        next = options.isEmpty() ? QString() : options.front();
    if (next == language_)
        return;
    persistLanguage(next);
    language_ = next;
    emit languageChanged(language_);
}

QString LanguageContextService::persistedLanguage() const
{
    if (!storage_)
        return {};
    storage_->beginGroup(QStringLiteral("LanguageContext"));
    const QString language = storage_->value(QStringLiteral("language")).toString();
    storage_->endGroup();
    return language;
}

void LanguageContextService::persistLanguage(const QString& language)
{
    if (!storage_)
        return;
    storage_->beginGroup(QStringLiteral("LanguageContext"));
    storage_->setValue(QStringLiteral("language"), language);
    storage_->endGroup();
    storage_->sync();
}

void LanguageContextService::recompute()
{
    const QStringList options = availableLanguages();
    QString next = persistedLanguage().trimmed();
    if (!options.contains(next))
        next = options.isEmpty() ? QString() : options.front();
    if (next == language_)
        return;
    persistLanguage(next);
    language_ = next;
    emit languageChanged(language_);
}

void LanguageContextService::setStorage(std::unique_ptr<ISettingsStorage> storage)
{
    storage_ = std::move(storage);
    recompute();
}

void LanguageContextService::setStorageForTesting(std::unique_ptr<ISettingsStorage> storage)
{
    setStorage(std::move(storage));
}

}
