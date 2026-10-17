#pragma once

#include "ISettingsStorage.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace VNX {

class LanguageContextService : public QObject
{
    Q_OBJECT

public:
    static LanguageContextService& instance();

    explicit LanguageContextService(std::unique_ptr<ISettingsStorage> storage,
                                    QObject* parent = nullptr);

    const QString& language() const { return language_; }
    QStringList availableLanguages() const;

    void setLanguage(const QString& language);
    void setStorageForTesting(std::unique_ptr<ISettingsStorage> storage);

signals:
    void languageChanged(const QString& language);

private:
    LanguageContextService();

    QString storageGroup() const;
    QString persistedLanguage() const;
    void persistLanguage(const QString& language);
    void recompute();
    void setStorage(std::unique_ptr<ISettingsStorage> storage);

    std::unique_ptr<ISettingsStorage> storage_;
    QString language_;
};

}
