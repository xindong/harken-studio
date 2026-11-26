
#pragma once

#include "ThemeStyleTokens.h"

#include <QObject>
#include <QFont>
#include <QHash>
#include <QReadWriteLock>
#include <QString>

namespace harken::studio {

enum class FontEmphasis {
    Normal,
    Strong
};

struct ResolvedFontFamily
{
    QString family;
    ThemeFontSource source;
};

class FontManager : public QObject
{
    Q_OBJECT

public:
    static FontManager& instance();

    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;


    QFont getFont(FontRole role, FontEmphasis emphasis = FontEmphasis::Normal) const;

    QFont getFont(FontRole role, int sizePtOverride) const;

    QFont getFont(const ThemeSnapshotPtr& snapshot, FontRole role,
                  FontEmphasis emphasis = FontEmphasis::Normal) const;
    ResolvedFontFamily familyFor(const ThemeSnapshotPtr& snapshot, FontRole role) const;
    QStringList themeFamilies(const ThemeSnapshotPtr& snapshot) const;
    QStringList systemFamilies() const;
    QString styleSheet(const ThemeSnapshotPtr& snapshot, FontRole role,
                       FontEmphasis emphasis = FontEmphasis::Normal) const;

    void applyTheme(ThemeSnapshotPtr snapshot);


    const QString& sansFamily() const { return sansFamily_; }

    const QString& monoFamily() const { return monoFamily_; }


    qreal scaleFactor() const { const auto snapshot = snapshot_.lock(); return snapshot ? snapshot->uiScale : 1.0; }

signals:
    void fontChanged();

private:
    explicit FontManager(QObject* parent = nullptr);
    ~FontManager() override = default;

    void refreshSystemFamilies();

private:
    QString sansFamily_;
    QString monoFamily_;
    QString systemSansFamily_;
    QString systemMonoFamily_;
    QString defaultSansFamily_;
    mutable QReadWriteLock familyLock_;
    QHash<QString, QString> installedNames_;
    std::weak_ptr<const ThemeSnapshot> snapshot_;
};

}
