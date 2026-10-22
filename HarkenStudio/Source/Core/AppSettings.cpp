
#include "AppSettings.h"
#include "QSettingsStorage.h"
#include <QApplication>
#include <QDebug>

namespace VNX {

AppSettings& AppSettings::instance()
{
    static AppSettings instance;
    return instance;
}

AppSettings::AppSettings(QObject* parent)
    : QObject(parent)
    , storage_(SettingsStorageFactory::create(
          SettingsStorageFactory::StorageType::QSettings,
          "HarkenSound", "HarkenStudio"))
    , globalUIScale_(DEFAULT_GLOBAL_SCALE)
{
    panelFontSizes_.fill(DEFAULT_FONT_SIZE);
    load();
}

void AppSettings::setStorage(std::unique_ptr<ISettingsStorage> storage)
{
    storage_ = std::move(storage);
    load();
}

void AppSettings::load()
{
    if (!storage_) return;
    
    storage_->beginGroup("Appearance");
    
    globalUIScale_ = storage_->value("GlobalUIScale", DEFAULT_GLOBAL_SCALE).toInt();
    
    PanelRegistry::forEach([this](const PanelInfo& panel) {
        QString key = QString("PanelFontSize_%1").arg(panel.key);
        if (panel.id == PanelId::Assistant && !storage_->contains(key))
            key = QStringLiteral("PanelFontSize_ai_assistant");
        int fontSize = storage_->value(key, panel.defaultFontSize).toInt();
        panelFontSizes_[static_cast<size_t>(panel.id)] = fontSize;
    });

    language_ = storage_->value("Language", QStringLiteral("en")).toString();

    storage_->endGroup();

    storage_->beginGroup("Developer");
    developerMode_ = storage_->value("DeveloperMode", false).toBool();
    storage_->endGroup();

    qDebug() << "[AppSettings] Loaded settings - UI Scale:" << globalUIScale_ << "%";
}

void AppSettings::save()
{
    if (!storage_) return;
    
    storage_->beginGroup("Appearance");
    
    storage_->setValue("GlobalUIScale", globalUIScale_);
    
    PanelRegistry::forEach([this](const PanelInfo& panel) {
        QString key = QString("PanelFontSize_%1").arg(panel.key);
        int fontSize = panelFontSizes_[static_cast<size_t>(panel.id)];
        storage_->setValue(key, fontSize);
    });

    storage_->setValue("Language", language_);

    storage_->endGroup();

    storage_->beginGroup("Developer");
    storage_->setValue("DeveloperMode", developerMode_);
    storage_->endGroup();

    storage_->sync();

    qDebug() << "[AppSettings] Settings saved";
}

int AppSettings::globalUIScale() const
{
    return globalUIScale_;
}

void AppSettings::setGlobalUIScale(int percent)
{
    percent = qBound(100, percent, 200);
    
    if (globalUIScale_ != percent)
    {
        globalUIScale_ = percent;
        emit globalUIScaleChanged(percent);
        emit settingsChanged();
    }
}

bool AppSettings::developerMode() const
{
    return developerMode_;
}

void AppSettings::setDeveloperMode(bool enabled)
{
    if (developerMode_ != enabled)
    {
        developerMode_ = enabled;
        emit developerModeChanged(enabled);
        emit settingsChanged();
        save();
    }
}

QString AppSettings::language() const
{
    return language_;
}

void AppSettings::setLanguage(const QString& code)
{
    if (language_ != code)
    {
        language_ = code;
        emit languageChanged(code);
        emit settingsChanged();
        save();
    }
}

int AppSettings::panelFontSize(PanelId panelId) const
{
    if (panelId == PanelId::Count) {
        return DEFAULT_FONT_SIZE;
    }
    return panelFontSizes_[static_cast<size_t>(panelId)];
}

void AppSettings::setPanelFontSize(PanelId panelId, int size)
{
    if (panelId == PanelId::Count) {
        return;
    }
    
    size = qBound(MIN_FONT_SIZE, size, MAX_FONT_SIZE);
    
    size_t index = static_cast<size_t>(panelId);
    if (panelFontSizes_[index] != size)
    {
        panelFontSizes_[index] = size;
        emit fontSizeChanged(panelId, size);
        emit fontSizeChangedLegacy(PanelRegistry::key(panelId), size);
        emit settingsChanged();
    }
}

int AppSettings::effectiveFontSize(PanelId panelId) const
{
    int baseSize = panelFontSize(panelId);
    
    if (globalUIScale_ != 100)
    {
        baseSize = static_cast<int>(baseSize * globalUIScale_ / 100.0);
    }
    
    return qBound(MIN_FONT_SIZE, baseSize, MAX_FONT_SIZE);
}

int AppSettings::panelFontSize(const QString& panelKey) const
{
    PanelId id = PanelRegistry::findByKey(panelKey);
    if (id == PanelId::Count) {
        return DEFAULT_FONT_SIZE;
    }
    return panelFontSize(id);
}

void AppSettings::setPanelFontSize(const QString& panelKey, int size)
{
    PanelId id = PanelRegistry::findByKey(panelKey);
    if (id != PanelId::Count) {
        setPanelFontSize(id, size);
    }
}

int AppSettings::effectiveFontSize(const QString& panelKey) const
{
    PanelId id = PanelRegistry::findByKey(panelKey);
    if (id == PanelId::Count) {
        return DEFAULT_FONT_SIZE;
    }
    return effectiveFontSize(id);
}

int AppSettings::defaultFontSize(PanelId panelId)
{
    return PanelRegistry::defaultFontSize(panelId);
}

int AppSettings::defaultFontSize(const QString& panelKey)
{
    PanelId id = PanelRegistry::findByKey(panelKey);
    if (id == PanelId::Count) {
        return DEFAULT_FONT_SIZE;
    }
    return PanelRegistry::defaultFontSize(id);
}

void AppSettings::resetToDefaults()
{
    globalUIScale_ = DEFAULT_GLOBAL_SCALE;
    developerMode_ = false;
    language_ = QStringLiteral("en");

    PanelRegistry::forEach([this](const PanelInfo& panel) {
        panelFontSizes_[static_cast<size_t>(panel.id)] = panel.defaultFontSize;
    });

    emit globalUIScaleChanged(globalUIScale_);
    emit developerModeChanged(developerMode_);
    emit languageChanged(language_);
    
    PanelRegistry::forEach([this](const PanelInfo& panel) {
        int fontSize = panelFontSizes_[static_cast<size_t>(panel.id)];
        emit fontSizeChanged(panel.id, fontSize);
        emit fontSizeChangedLegacy(panel.key, fontSize);
    });
    
    emit settingsChanged();
    
    qDebug() << "[AppSettings] Reset to defaults";
}

}
