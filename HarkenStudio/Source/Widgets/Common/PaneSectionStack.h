#pragma once

#include <QFrame>
#include <QList>

class QSplitter;

namespace VNX {

class PaneSection;

class PaneSectionStack : public QFrame
{
    Q_OBJECT

public:
    explicit PaneSectionStack(const QString& settingsGroup, QWidget* parent = nullptr);

    PaneSection* addSection(const QString& title, const QString& settingsId, int defaultSize);
    void setSectionVisible(PaneSection* section, bool visible);
    QSplitter* splitter() const { return splitter_; }

private:
    bool isShown(const PaneSection* section) const;
    void updateLayout();
    void save() const;

    QString settingsGroup_;
    QSplitter* splitter_ = nullptr;
    QWidget* collapsedSpace_ = nullptr;
    QList<PaneSection*> sections_;
    QList<int> expandedSizes_;
};

}
