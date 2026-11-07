
#include "AppSettings.h"
#include "QSettingsStorage.h"
#include <QDebug>
#include <algorithm>
#include <cmath>

namespace VNX {
namespace {

int zoomLevelForScale(qreal percent)
{
    if (!qIsFinite(percent) || percent <= 0.0)
        return 0;
    const auto& presets = AppSettings::GLOBAL_SCALE_PRESETS;
    const auto nearest = std::min_element(presets.begin(), presets.end(), [percent](int left, int right) {
        return std::abs(left - percent) < std::abs(right - percent);
    });
    return int(nearest - presets.begin()) + AppSettings::MIN_GLOBAL_ZOOM_LEVEL;
}

}

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
    
    globalUIZoomLevel_ = zoomLevelForScale(
        storage_->value("GlobalUIScale", DEFAULT_GLOBAL_SCALE).toDouble());
    
    PanelRegistry::forEach([this](const PanelInfo& panel) {
        const QString key = QStringLiteral("PanelFontSizePt_%1").arg(panel.key);
        int fontSize = storage_->value(key, 0).toInt();
        if (!storage_->contains(key)) {
            const std::array<int, PanelRegistry::PANEL_COUNT> previousDefaults{13, 12, 12, 11, 11, 12, 12};
            QString previousKey = QStringLiteral("PanelFontSize_%1").arg(panel.key);
            if (panel.id == PanelId::Assistant && !storage_->contains(previousKey))
                previousKey = QStringLiteral("PanelFontSize_ai_assistant");
            const int previousDefault = previousDefaults[static_cast<size_t>(panel.id)];
            fontSize = storage_->value(previousKey, previousDefault).toInt();
            if (fontSize == previousDefault) fontSize = 0;
            else if (panel.id == PanelId::Assistant) fontSize = qRound(fontSize * 0.75);
        }
        if (fontSize != 0) fontSize = qBound(MIN_FONT_SIZE, fontSize, MAX_FONT_SIZE);
        panelFontSizes_[static_cast<size_t>(panel.id)] = fontSize;
    });

    language_ = storage_->value("Language", QStringLiteral("en")).toString();

    storage_->endGroup();

    storage_->beginGroup("Developer");
    developerMode_ = storage_->value("DeveloperMode", false).toBool();
    storage_->endGroup();

    qDebug() << "[AppSettings] Loaded settings - UI Scale:" << globalUIScale() << "%";
}

void AppSettings::save()
{
    if (!storage_) return;
    
    storage_->beginGroup("Appearance");
    
    storage_->setValue("GlobalUIScale", globalUIScale());
    
    PanelRegistry::forEach([this](const PanelInfo& panel) {
        QString key = QStringLiteral("PanelFontSizePt_%1").arg(panel.key);
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

qreal AppSettings::globalUIScale() const
{
    return uiScaleForZoomLevel(globalUIZoomLevel_);
}

qreal AppSettings::uiScaleForZoomLevel(int level)
{
    return GLOBAL_SCALE_PRESETS[qBound(MIN_GLOBAL_ZOOM_LEVEL, level, MAX_GLOBAL_ZOOM_LEVEL)
                                - MIN_GLOBAL_ZOOM_LEVEL];
}

int AppSettings::globalUIZoomLevel() const
{
    return globalUIZoomLevel_;
}

void AppSettings::setGlobalUIScale(qreal percent)
{
    setGlobalUIZoomLevel(zoomLevelForScale(percent));
}

void AppSettings::setGlobalUIZoomLevel(int level)
{
    level = qBound(MIN_GLOBAL_ZOOM_LEVEL, level, MAX_GLOBAL_ZOOM_LEVEL);
    if (globalUIZoomLevel_ != level)
    {
        globalUIZoomLevel_ = level;
        emit globalUIScaleChanged(globalUIScale());
        emit settingsChanged();
        save();
    }
}

void AppSettings::increaseGlobalUIScale()
{
    setGlobalUIZoomLevel(globalUIZoomLevel_ + 1);
}

void AppSettings::decreaseGlobalUIScale()
{
    setGlobalUIZoomLevel(globalUIZoomLevel_ - 1);
}

bool AppSettings::canIncreaseGlobalUIScale() const
{
    return globalUIZoomLevel_ < MAX_GLOBAL_ZOOM_LEVEL;
}

bool AppSettings::canDecreaseGlobalUIScale() const
{
    return globalUIZoomLevel_ > MIN_GLOBAL_ZOOM_LEVEL;
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
    
    if (size != 0) size = qBound(MIN_FONT_SIZE, size, MAX_FONT_SIZE);
    
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
    if (baseSize == 0) return 0;
    
    return qRound(baseSize * globalUIScale() / 100.0);
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

int AppSettings::defaultFontSize(PanelId)
{
    return DEFAULT_FONT_SIZE;
}

int AppSettings::defaultFontSize(const QString&)
{
    return DEFAULT_FONT_SIZE;
}

void AppSettings::resetToDefaults()
{
    globalUIZoomLevel_ = 0;
    developerMode_ = false;
    language_ = QStringLiteral("en");

    PanelRegistry::forEach([this](const PanelInfo& panel) {
        panelFontSizes_[static_cast<size_t>(panel.id)] = DEFAULT_FONT_SIZE;
    });

    emit globalUIScaleChanged(globalUIScale());
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
