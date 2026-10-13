#pragma once

#include <QWidget>

class QToolButton;

namespace VNX {

class ExplorerSection : public QWidget
{
    Q_OBJECT

public:
    explicit ExplorerSection(const QString& title, QWidget* parent = nullptr);
    QWidget* content() const { return content_; }
    QWidget* header() const { return header_; }
    bool isExpanded() const;
    void setExpanded(bool expanded);

signals:
    void expandedChanged(bool expanded);
    void headerContextMenuRequested(const QPoint& globalPosition);

private:
    void updateHeader();

    QWidget* header_ = nullptr;
    QToolButton* toggle_ = nullptr;
    QWidget* content_ = nullptr;
};

}
