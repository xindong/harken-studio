#pragma once

#include <QVariant>
#include <QString>
#include <functional>
#include <memory>
#include "AIPermissionModel.h"

namespace VNX {

class ISettingsStorage;

struct AIAgentConfig
{
    QString baseUrl;
    QString model;
    AIPermissionSetting permissionProfile = AIPermissionMode::Ask;
    int contextWindowTokens = 128000;
    int maxTokens = 1024;
    int requestTimeoutMs = 30000;

    static AIAgentConfig defaults();
    AIAgentConfig effectiveSnapshot() const;
    bool operator==(const AIAgentConfig& other) const;
    bool operator!=(const AIAgentConfig& other) const { return !(*this == other); }
};

class AIAgentConfigStore
{
public:
    using ValueLoader = std::function<QVariant(const QString&, const QVariant&)>;

    AIAgentConfigStore();

    AIAgentConfig load() const;
    AIAgentConfig effectiveSnapshot() const;
    bool save(const AIAgentConfig& config) const;
    void setValueLoaderForTesting(ValueLoader loader);

    // Test seam: route every store's persistence through the given storage
    // (for example a MemorySettingsStorage) so tests never read or write the
    // real user settings. Pass nullptr to restore the default QSettings-backed
    // storage. Covers stores constructed internally (e.g. by the settings pages),
    // which instance injection cannot reach.
    static void setStorageOverrideForTesting(std::shared_ptr<ISettingsStorage> storage);

private:
    ValueLoader valueLoader_;
    std::shared_ptr<ISettingsStorage> storage_;
};

}
