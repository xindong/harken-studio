
#pragma once

#include "Dialogs/Settings/SettingsPage.h"

#include <QKeySequence>

class QKeySequenceEdit;
class QLineEdit;
class QTreeWidgetItem;

namespace VNX {

class ProportionalTreeWidget;

inline constexpr const char* kSettingsPageShortcuts = "shortcuts";

class ShortcutsPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit ShortcutsPage(QWidget* parent = nullptr);

    QString pageId() const override;
    QString categoryLabel() const override;
    void load() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void populateShortcutTree();
    void filterShortcutTree(const QString& filter);
    void refreshShortcutRow(QTreeWidgetItem* item);
    void beginShortcutCapture(QTreeWidgetItem* item);
    void cancelShortcutCapture();
    void commitShortcutCapture();
    void commitShortcut(const QString& id, const QKeySequence& sequence);
    void refreshShortcutRows();
    void resetShortcutsToDefaults();

    QLineEdit* shortcutSearchEdit_ = nullptr;
    ProportionalTreeWidget* shortcutTree_ = nullptr;
    QTreeWidgetItem* shortcutCaptureItem_ = nullptr;
    QKeySequenceEdit* shortcutCaptureEdit_ = nullptr;
};

}
