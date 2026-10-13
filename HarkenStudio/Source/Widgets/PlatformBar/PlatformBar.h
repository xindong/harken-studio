#pragma once

#include "../../Core/PlatformContext.h"

#include <QStringList>
#include <QToolBar>

class QToolButton;
class QAction;
class QComboBox;
class QWidget;

namespace VNX {

class PlatformContextService;
class SegmentedControl;

class PlatformBar : public QToolBar
{
    Q_OBJECT

public:
    explicit PlatformBar(QWidget* parent = nullptr);
    explicit PlatformBar(PlatformContextService& service, QWidget* parent = nullptr);
    QToolButton* addLeadingAction(QAction* action, const QString& objectName = {});
    void addCenterWidget(QWidget* widget);

private:
    void rebuild();
    void rebuildLanguages();
    void applyContext(const PlatformContext& context);
    void applyLanguage(const QString& language);
    void onAllToggled(bool on);
    void onPlatformChanged(int index);
    void onLanguageToggled(const QString& language, bool on);

    PlatformContextService* service_ = nullptr;
    QWidget* host_ = nullptr;
    QWidget* leading_ = nullptr;
    QWidget* center_ = nullptr;
    QToolButton* allButton_ = nullptr;
    QComboBox* platformCombo_ = nullptr;
    SegmentedControl* languageControl_ = nullptr;
    QStringList languages_;
    bool updating_ = false;
};

}
