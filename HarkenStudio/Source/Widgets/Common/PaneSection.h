#pragma once

#include <QWidget>

class QToolButton;

namespace VNX {

class PaneSection : public QWidget
{
    Q_OBJECT

public:
    PaneSection(const QString& title, const QString& settingsId, QWidget* parent = nullptr);
    QWidget* content() const { return content_; }
    QWidget* header() const { return header_; }
    QString settingsId() const { return settingsId_; }
    bool isExpanded() const;
    void setExpanded(bool expanded);

signals:
    void expandedChanged(bool expanded);
    void headerContextMenuRequested(const QPoint& globalPosition);

private:
    void updateHeader();

    QString settingsId_;
    QWidget* header_ = nullptr;
    QToolButton* toggle_ = nullptr;
    QWidget* content_ = nullptr;
};

}
