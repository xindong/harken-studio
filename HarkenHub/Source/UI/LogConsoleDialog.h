#pragma once

#include "Maintenance/LogTailer.h"

#include <QDialog>

class QPlainTextEdit;

namespace VNH {

class LogConsoleDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LogConsoleDialog(QWidget* parent = nullptr);
    static QString localizeTimestamp(const QString& line);
    static QString localizeLine(const QString& line);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void appendText(const QString& text);

    LogTailer tailer_;
    QPlainTextEdit* view_ = nullptr;
};

}
