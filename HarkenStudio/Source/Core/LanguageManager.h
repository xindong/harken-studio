
#pragma once

#include <QObject>
#include <QTranslator>
#include <QString>
#include <QMap>
#include <QLocale>
#include <memory>

namespace VNX {

struct LanguageInfo
{
    QString code;
    QString name;
    QString nativeName;
    QString flag;
    QString qmFile;
};

class LanguageManager : public QObject
{
    Q_OBJECT

public:
    static LanguageManager& instance();

    void registerLanguage(const LanguageInfo& info);
    QStringList availableLanguages() const;
    LanguageInfo languageInfo(const QString& code) const;
    
    QString currentLanguage() const { return currentLanguage_; }
    bool setLanguage(const QString& code);
    
    static QString systemLanguage();
    static QString preferredLanguage();

    bool loadTranslationFile(const QString& path);
    void setTranslationPath(const QString& path);
    QString translationPath() const { return translationPath_; }

signals:
    void languageChanged(const QString& code);
    void translationLoaded(bool success);

private:
    LanguageManager();
    ~LanguageManager() override;
    Q_DISABLE_COPY(LanguageManager)

    void setupDefaultLanguages();
    void applyTranslation(const QString& code);

private:
    QString currentLanguage_;
    QString translationPath_;
    QMap<QString, LanguageInfo> languages_;
    std::unique_ptr<QTranslator> appTranslator_;
    std::unique_ptr<QTranslator> qtTranslator_;
};


}
