
#pragma once

#include "ThemeSnapshot.h"

#include <QObject>
#include <QFont>
#include <QString>

namespace VNX {

enum class FontRole {
    Title,
    Body,
    Caption,
    Mono
};

enum class FontEmphasis {
    Normal,
    Strong
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
    QString styleSheet(const ThemeSnapshotPtr& snapshot, FontRole role,
                       FontEmphasis emphasis = FontEmphasis::Normal) const;

    void applyTheme(ThemeSnapshotPtr snapshot);


    const QString& sansFamily() const { return sansFamily_; }

    const QString& monoFamily() const { return monoFamily_; }


    qreal scaleFactor() const { return snapshot_ ? snapshot_->uiScale : 1.0; }

signals:
    void fontChanged();

private:
    explicit FontManager(QObject* parent = nullptr);
    ~FontManager() override = default;

    void resolveFontFamilies();

private:
    QString sansFamily_;
    QString monoFamily_;
    QString systemSansFamily_;
    QString systemMonoFamily_;
    ThemeSnapshotPtr snapshot_;
};

}
