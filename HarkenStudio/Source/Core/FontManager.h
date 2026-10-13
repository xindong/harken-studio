
#pragma once

#include "ThemeSnapshot.h"

#include <QObject>
#include <QFont>
#include <QString>

namespace VNX {

enum class FontRole {
    Heading,
    Title,
    Body,
    Caption,
    Axis,
    Mono,
    Label
};

class FontManager : public QObject
{
    Q_OBJECT

public:
    static FontManager& instance();

    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;


    QFont getFont(FontRole role) const;

    QFont getFont(FontRole role, int sizePtOverride) const;

    QFont getFont(FontRole role, int sizePtOverride, QFont::Weight weightOverride) const;
    QFont getFont(const ThemeSnapshotPtr& snapshot, FontRole role) const;

    void applyTheme(ThemeSnapshotPtr snapshot);


    const QString& sansFamily() const { return sansFamily_; }

    const QString& monoFamily() const { return monoFamily_; }


    float scaleFactor() const { return scaleFactor_; }
    void setScaleFactor(float factor);

signals:
    void fontChanged();

private:
    explicit FontManager(QObject* parent = nullptr);
    ~FontManager() override = default;

    void resolveFontFamilies();
    int scaledSize(int basePt) const;

    static int defaultSize(FontRole role);
    static QFont::Weight defaultWeight(FontRole role);
    const ThemeFontStyle* themeStyle(FontRole role) const;
    static const ThemeFontStyle* themeStyle(const ThemeSnapshotPtr& snapshot,
                                            FontRole role);

private:
    QString sansFamily_;
    QString monoFamily_;
    QString systemSansFamily_;
    QString systemMonoFamily_;
    ThemeSnapshotPtr snapshot_;
    float scaleFactor_ = 1.0f;
};

}
