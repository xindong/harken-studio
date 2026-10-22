
#pragma once

#include <QObject>
#include <QString>
#include <array>
#include <functional>
#include <memory>

#include "ISettingsStorage.h"

namespace VNX {

// PanelId ordinals are not a stable persisted ABI. Appearance font
// sizes are stored as PanelFontSize_<key> strings. Inserting or deleting
// enumerators shifts later values; never serialize the integer.
enum class PanelId {
    Assistant,
    Explorer,
    Inspector,
    Log,
    NodeGraph,
    Hierarchy,
    Information,

    Count
};

struct PanelInfo {
    PanelId id;
    const char* key;
    const char* displayName;
    int defaultFontSize;
};

class PanelRegistry {
public:
    static constexpr size_t PANEL_COUNT = static_cast<size_t>(PanelId::Count);
    
    static constexpr std::array<PanelInfo, PANEL_COUNT> panels = {{
        { PanelId::Assistant,       "assistant",         "Assistant",         13 },
        { PanelId::Explorer,        "explorer",          "Explorer",          12 },
        { PanelId::Inspector,       "inspector",         "Inspector",         12 },
        { PanelId::Log,             "log",               "Log Panel",         11 },
        { PanelId::NodeGraph,       "node_graph",        "Node Graph",        11 },
        { PanelId::Hierarchy,       "hierarchy",         "Hierarchy",         12 },
        { PanelId::Information,     "information",       "Information",       12 },
    }};
    
    static const PanelInfo& get(PanelId id) {
        return panels[static_cast<size_t>(id)];
    }
    
    static const char* key(PanelId id) {
        return get(id).key;
    }
    
    static int defaultFontSize(PanelId id) {
        return get(id).defaultFontSize;
    }
    
    static const char* displayName(PanelId id) {
        return get(id).displayName;
    }
    
    template<typename Func>
    static void forEach(Func&& func) {
        for (const auto& panel : panels) {
            func(panel);
        }
    }
    
    static PanelId findByKey(const QString& key) {
        for (const auto& panel : panels) {
            if (key == panel.key) {
                return panel.id;
            }
        }
        return PanelId::Count;
    }
};

class AppSettings : public QObject
{
    Q_OBJECT

public:
    static AppSettings& instance();

    AppSettings(const AppSettings&) = delete;
    AppSettings& operator=(const AppSettings&) = delete;

    int globalUIScale() const;
    void setGlobalUIScale(int percent);

    int panelFontSize(PanelId panelId) const;
    void setPanelFontSize(PanelId panelId, int size);

    int effectiveFontSize(PanelId panelId) const;

    bool developerMode() const;
    void setDeveloperMode(bool enabled);

    QString language() const;
    void setLanguage(const QString& code);

    [[deprecated("Use PanelId overload instead")]]
    int panelFontSize(const QString& panelKey) const;
    [[deprecated("Use PanelId overload instead")]]
    void setPanelFontSize(const QString& panelKey, int size);
    [[deprecated("Use PanelId overload instead")]]
    int effectiveFontSize(const QString& panelKey) const;

    [[deprecated("Use PanelId::Assistant instead")]]
    static constexpr const char* PANEL_ASSISTANT = "assistant";
    [[deprecated("Use PanelId::Explorer instead")]]
    static constexpr const char* PANEL_EXPLORER = "explorer";
    [[deprecated("Use PanelId::Inspector instead")]]
    static constexpr const char* PANEL_INSPECTOR = "inspector";
    [[deprecated("Use PanelId::Log instead")]]
    static constexpr const char* PANEL_LOG = "log";
    [[deprecated("Use PanelId::NodeGraph instead")]]
    static constexpr const char* PANEL_NODE_GRAPH = "node_graph";

    static constexpr int DEFAULT_GLOBAL_SCALE = 100;
    static constexpr int DEFAULT_FONT_SIZE = 12;
    static constexpr int MIN_FONT_SIZE = 8;
    static constexpr int MAX_FONT_SIZE = 24;

    static int defaultFontSize(PanelId panelId);
    
    [[deprecated("Use PanelId overload instead")]]
    static int defaultFontSize(const QString& panelKey);

    void resetToDefaults();
    void save();

signals:
    void settingsChanged();
    void globalUIScaleChanged(int percent);
    void fontSizeChanged(PanelId panelId, int newSize);
    void developerModeChanged(bool enabled);
    void languageChanged(const QString& code);
    
    void fontSizeChangedLegacy(const QString& panelKey, int newSize);

private:
    explicit AppSettings(QObject* parent = nullptr);
    ~AppSettings() override = default;

    void load();
    
    void setStorage(std::unique_ptr<ISettingsStorage> storage);

private:
    std::unique_ptr<ISettingsStorage> storage_;
    int globalUIScale_;
    bool developerMode_ = false;
    QString language_ = QStringLiteral("en");
    std::array<int, PanelRegistry::PANEL_COUNT> panelFontSizes_;
    
    friend class AppSettingsTest;
    friend class AppSettingsLanguageTest;
    friend class SettingsApplyContractTest;
    friend class ThemeSettingsDialogTest;
    friend class AISettingsTest;
};

}
