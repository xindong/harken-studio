
#include "LanguageManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QLocale>

namespace VNX {

LanguageManager& LanguageManager::instance()
{
    static LanguageManager instance;
    return instance;
}

LanguageManager::LanguageManager()
    : currentLanguage_("en")
    , appTranslator_(std::make_unique<QTranslator>())
    , qtTranslator_(std::make_unique<QTranslator>())
{
    translationPath_ = QCoreApplication::applicationDirPath() + "/translations";
    
    setupDefaultLanguages();
}

LanguageManager::~LanguageManager() = default;

void LanguageManager::setupDefaultLanguages()
{
    LanguageInfo en;
    en.code = "en";
    en.name = "English";
    en.nativeName = "English";
    en.flag = QString::fromUtf8("🇺🇸");
    en.qmFile = "xdaudio_en.qm";
    registerLanguage(en);

    LanguageInfo zh_CN;
    zh_CN.code = "zh_CN";
    zh_CN.name = "Chinese (Simplified)";
    zh_CN.nativeName = QString::fromUtf8("简体中文");
    zh_CN.flag = QString::fromUtf8("🇨🇳");
    zh_CN.qmFile = "xdaudio_zh_CN.qm";
    registerLanguage(zh_CN);

    LanguageInfo zh_TW;
    zh_TW.code = "zh_TW";
    zh_TW.name = "Chinese (Traditional)";
    zh_TW.nativeName = QString::fromUtf8("繁體中文");
    zh_TW.flag = QString::fromUtf8("🇹🇼");
    zh_TW.qmFile = "xdaudio_zh_TW.qm";
    registerLanguage(zh_TW);

    LanguageInfo ja;
    ja.code = "ja";
    ja.name = "Japanese";
    ja.nativeName = QString::fromUtf8("日本語");
    ja.flag = QString::fromUtf8("🇯🇵");
    ja.qmFile = "xdaudio_ja.qm";
    registerLanguage(ja);

    LanguageInfo ko;
    ko.code = "ko";
    ko.name = "Korean";
    ko.nativeName = QString::fromUtf8("한국어");
    ko.flag = QString::fromUtf8("🇰🇷");
    ko.qmFile = "xdaudio_ko.qm";
    registerLanguage(ko);
}

void LanguageManager::registerLanguage(const LanguageInfo& info)
{
    languages_[info.code] = info;
}

QStringList LanguageManager::availableLanguages() const
{
    return languages_.keys();
}

LanguageInfo LanguageManager::languageInfo(const QString& code) const
{
    return languages_.value(code);
}

bool LanguageManager::setLanguage(const QString& code)
{
    if (!languages_.contains(code))
        return false;

    if (code == currentLanguage_)
        return true;

    applyTranslation(code);
    currentLanguage_ = code;

    QSettings settings;
    settings.setValue("language", code);

    emit languageChanged(code);
    return true;
}

QString LanguageManager::systemLanguage()
{
    QLocale locale = QLocale::system();
    QString name = locale.name();
    
    if (name.startsWith("zh_CN") || name.startsWith("zh_Hans"))
        return "zh_CN";
    if (name.startsWith("zh_TW") || name.startsWith("zh_Hant"))
        return "zh_TW";
    if (name.startsWith("ja"))
        return "ja";
    if (name.startsWith("ko"))
        return "ko";
    
    return "en";
}

QString LanguageManager::preferredLanguage()
{
    QSettings settings;
    QString saved = settings.value("language").toString();
    
    if (!saved.isEmpty())
        return saved;
    
    return systemLanguage();
}

bool LanguageManager::loadTranslationFile(const QString& path)
{
    bool success = appTranslator_->load(path);
    if (success)
    {
        QCoreApplication::installTranslator(appTranslator_.get());
    }
    emit translationLoaded(success);
    return success;
}

void LanguageManager::setTranslationPath(const QString& path)
{
    translationPath_ = path;
}

void LanguageManager::applyTranslation(const QString& code)
{
    QCoreApplication::removeTranslator(appTranslator_.get());
    QCoreApplication::removeTranslator(qtTranslator_.get());

    if (code == "en")
    {
        return;
    }

    LanguageInfo info = languages_.value(code);
    QString qmPath = translationPath_ + "/" + info.qmFile;

    if (appTranslator_->load(qmPath))
    {
        QCoreApplication::installTranslator(appTranslator_.get());
    }

    QString qtPath = translationPath_ + "/qt_" + code + ".qm";
    if (qtTranslator_->load(qtPath))
    {
        QCoreApplication::installTranslator(qtTranslator_.get());
    }
}

}
