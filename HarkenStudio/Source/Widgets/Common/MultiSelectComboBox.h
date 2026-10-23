#pragma once

#include <QComboBox>
#include <QPersistentModelIndex>
#include <QVariant>

class QStandardItemModel;

namespace VNX {

class MultiSelectComboBox final : public QComboBox
{
    Q_OBJECT

public:
    explicit MultiSelectComboBox(QWidget* parent = nullptr);

    void setOptions(const QList<QPair<QString, QVariant>>& options);
    void setSelectedData(const QVariantList& values);
    QVariantList selectedData() const { return selectedData_; }
    QString selectedText() const { return selectedText_; }

    QSize minimumSizeHint() const override;
    void showPopup() override;

signals:
    void selectedDataChanged(const QVariantList& values);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void toggleItem(const QModelIndex& index);
    void updateSelection();

    QStandardItemModel* items_;
    QPersistentModelIndex pressedIndex_;
    QVariantList selectedData_;
    QString selectedText_;
    bool selectionUpdating_ = false;
};

}
