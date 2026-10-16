#pragma once

#include <QGridLayout>

class QLabel;

namespace VNX {

class DialogFormLayout : public QGridLayout
{
public:
    static constexpr int RowSpacing = 12;

    explicit DialogFormLayout(QWidget* parent = nullptr);

    void addRow(const QString& text, QWidget* field);
    void addRow(QLabel* label, QWidget* field);

private:
    int nextRow_ = 0;
};

}
