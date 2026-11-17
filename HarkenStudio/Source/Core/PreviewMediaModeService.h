#pragma once

#include "ISettingsStorage.h"

#include <QObject>
#include <memory>

namespace VNX {

enum class PreviewMediaMode { Original, Converted };

class PreviewMediaModeService : public QObject
{
    Q_OBJECT

public:
    static PreviewMediaModeService& instance();
    explicit PreviewMediaModeService(std::unique_ptr<ISettingsStorage> storage,
                                     QObject* parent = nullptr);
    PreviewMediaMode mode();
    void setMode(PreviewMediaMode mode);
    void setStorageForTesting(std::unique_ptr<ISettingsStorage> storage);

signals:
    void modeChanged(PreviewMediaMode mode);

private:
    PreviewMediaModeService();
    QString storageGroup() const;
    void restore();

    std::unique_ptr<ISettingsStorage> storage_;
    QString restoredGroup_;
    PreviewMediaMode mode_ = PreviewMediaMode::Original;
};

}
