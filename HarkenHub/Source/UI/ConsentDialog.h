#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

namespace VNH {

class ConsentDialog : public QDialog
{
    Q_OBJECT

public:
    static QString hubDocumentId();
    static QString studioDocumentId();
    static QString documentVersion();
    static bool requestConsent(QWidget* parent, const QString& documentId);
    static void showDocuments(QWidget* parent);

private:
    enum class Mode { RequestConsent, ViewDocuments };

    ConsentDialog(const QString& documentId, const QStringList& documentTexts,
                  Mode mode, QWidget* parent);
};

}
