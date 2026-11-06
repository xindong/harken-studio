#include "IPCDesktopInternal.h"
#include "PackGenerationRecord.h"
#include <QApplication>
#include <QPlainTextEdit>
#include <QDateTime>
#include <QMenu>
#include <QToolButton>
#include <QAction>
#include <QInputDialog>
#include <QFileDialog>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QDialog>
#include <QItemSelectionModel>
#include <QAbstractScrollArea>
#include <cmath>
#include <climits>

namespace VNX {

namespace Bridge = HarkenEditor::Bridge;

void IPCServer::registerDesktopAdvancedHandlers()
{
    registerDesktopHandler("get_ui_tree", [this](const QJsonObject& args) { return handleGetUiTree(args); });
    registerDesktopHandler("find_widget", [this](const QJsonObject& args) { return handleFindWidget(args); });
    registerDesktopHandler("grab_widget", [this](const QJsonObject& args) { return handleGrabWidget(args); });
    registerDesktopHandler("get_focused_widget", [this](const QJsonObject& args) { return handleGetFocusedWidget(args); });
    registerDesktopHandler("get_last_interacted_widget", [this](const QJsonObject& args) { return handleGetLastInteractedWidget(args); });
    registerDesktopHandler("get_widget_value", [this](const QJsonObject& args) { return handleGetWidgetValue(args); });
    registerDesktopHandler("get_widget_children", [this](const QJsonObject& args) { return handleGetWidgetChildren(args); });
    registerDesktopHandler("get_table_data", [this](const QJsonObject& args) { return handleGetTableData(args); });
    registerDesktopHandler("get_list_items", [this](const QJsonObject& args) { return handleGetListItems(args); });
    registerDesktopHandler("get_tree_data", [this](const QJsonObject& args) { return handleGetTreeData(args); });
    registerDesktopHandler("click_widget", [this](const QJsonObject& args) { return handleClickWidget(args); });
    registerDesktopHandler("key_press", [this](const QJsonObject& args) { return handleKeyPress(args); });
    registerDesktopHandler("set_widget_value", [this](const QJsonObject& args) { return handleSetWidgetValue(args); });
    registerDesktopHandler("select_table_row", [this](const QJsonObject& args) { return handleSelectTableRow(args); });
    registerDesktopHandler("trigger_menu_action", [this](const QJsonObject& args) { return handleTriggerMenuAction(args); });
    registerDesktopHandler("answer_dialog", [this](const QJsonObject& args) { return handleAnswerDialog(args); });
    registerDesktopHandler("flush_ui_events", [this](const QJsonObject& args) { return handleFlushUIEvents(args); });
}

QJsonObject IPCServer::handleGetFocusedWidget(const QJsonObject& )
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    QWidget* focused = QApplication::focusWidget();
    if (!focused)
        return successResponse(QJsonObject{
            {"has_focus", false},
            {"hint", "Click on a widget first, then call get_focused_widget"}
        });

    QWidget* named = focused;
    while (named && named->objectName().isEmpty())
        named = named->parentWidget();

    const QString panel = ipcFocusedWidgetKnownPanelName(focused);

    QWidget* target = named ? named : focused;
    QJsonObject result = serializeWidget(target, 0, 0);
    result["has_focus"] = true;
    result["path"] = widgetPath(target);

    if (!panel.isEmpty())
        result["panel"] = panel;

    if (named && named != focused) {
        result["raw_focus_type"] = QString::fromLatin1(focused->metaObject()->className());
    }

    return successResponse(result);
}

QJsonObject IPCServer::handleGetLastInteractedWidget(const QJsonObject& )
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    QWidget* widget = mainWindow_->lastInteractedWidget();
    if (!widget)
        return successResponse(QJsonObject{
            {"has_interaction", false},
            {"hint", "Interact with a widget (click/scroll) first, then call get_last_interacted_widget"}
        });

    const QString panel = ipcLastInteractedWidgetKnownPanelName(widget);

    QJsonObject result = serializeWidget(widget, 0, 0);
    result["has_interaction"] = true;
    result["path"] = widgetPath(widget);
    result["interaction_type"] = mainWindow_->lastInteractionType();
    result["interaction_time_ms"] = mainWindow_->lastInteractionTimeMs();

    qint64 age = QDateTime::currentMSecsSinceEpoch() - mainWindow_->lastInteractionTimeMs();
    result["age_ms"] = age;

    if (!panel.isEmpty())
        result["panel"] = panel;

    return successResponse(result);
}

QJsonObject IPCServer::handleGetUiTree(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    QString root = args["root"].toString();
    int maxDepth = args.contains("depth") ? args["depth"].toInt() : 3;
    if (maxDepth < 0) maxDepth = 0;
    if (maxDepth > 10) maxDepth = 10;

    QWidget* rootWidget = nullptr;
    if (root.isEmpty() || root == "MainWindow")
        rootWidget = mainWindow_;
    else
        rootWidget = mainWindow_->findChild<QWidget*>(root);

    if (!rootWidget)
        return errorResponse("WIDGET_NOT_FOUND",
                             QString("Widget '%1' not found").arg(root),
                             "Use find_widget to discover available widget names");

    QJsonObject tree = serializeWidget(rootWidget, 0, maxDepth);
    return successResponse(tree);
}

QJsonObject IPCServer::handleFindWidget(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    QString nameFilter = args["name"].toString();
    QString textFilter = args["text"].toString();
    QString typeFilter = args["type"].toString();
    bool visibleOnly = !args.contains("visible_only") || args["visible_only"].toBool(true);

    if (nameFilter.isEmpty() && textFilter.isEmpty() && typeFilter.isEmpty())
        return errorResponse("MISSING_FILTER",
                             "At least one of name, text, or type is required");

    QList<QWidget*> allWidgets = mainWindow_->findChildren<QWidget*>();
    QJsonArray matches;
    int limit = 50;

    for (auto* w : allWidgets) {
        if (visibleOnly && !w->isVisible())
            continue;

        bool match = true;
        if (!nameFilter.isEmpty() && w->objectName() != nameFilter)
            match = false;
        if (match && !typeFilter.isEmpty()) {
            if (QString::fromLatin1(w->metaObject()->className()) != typeFilter)
                match = false;
        }
        if (match && !textFilter.isEmpty()) {
            QString txt = widgetText(w);
            if (!txt.contains(textFilter, Qt::CaseInsensitive))
                match = false;
        }

        if (match) {
            QJsonObject item = serializeWidget(w, 0, 0);
            item["path"] = widgetPath(w);
            matches.append(item);
            if (--limit <= 0) break;
        }
    }

    QJsonObject result;
    result["count"] = matches.size();
    result["widgets"] = matches;
    return successResponse(result);
}

QJsonObject IPCServer::handleGrabWidget(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    QString name = args["name"].toString();
    if (name.isEmpty())
        return errorResponse("MISSING_PARAM", "name is required",
                             "Provide widget objectName");

    QWidget* target = nullptr;
    if (name == "MainWindow")
        target = mainWindow_;
    else
        target = mainWindow_->findChild<QWidget*>(name);

    if (!target)
        return errorResponse("WIDGET_NOT_FOUND",
                             QString("Widget '%1' not found").arg(name));

    QPixmap pixmap = target->grab();
    if (pixmap.isNull())
        return errorResponse("GRAB_FAILED", "QWidget::grab() returned null pixmap");

    int maxWidth = args.contains("max_width") ? args["max_width"].toInt() : 1920;
    if (pixmap.width() > maxWidth)
        pixmap = pixmap.scaledToWidth(maxWidth, Qt::SmoothTransformation);

    QByteArray pngData;
    QBuffer buffer(&pngData);
    buffer.open(QIODevice::WriteOnly);
    pixmap.save(&buffer, "PNG");
    buffer.close();

    QJsonObject result;
    result["image_base64"] = QString::fromLatin1(pngData.toBase64());
    result["format"] = QStringLiteral("png");
    result["width"] = pixmap.width();
    result["height"] = pixmap.height();
    result["widget_name"] = name;
    return successResponse(result);
}

QJsonObject IPCServer::handleClickWidget(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    QString name = args["name"].toString();
    if (name.isEmpty())
        return errorResponse("MISSING_PARAM", "name is required",
                             "Provide widget objectName to click");

    QWidget* target = nullptr;
    const auto matches = name == "MainWindow" ? QList<QWidget*>{mainWindow_}
        : mainWindow_->findChildren<QWidget*>(name);
    if (matches.isEmpty())
        return errorResponse("WIDGET_NOT_FOUND", QString("Widget '%1' not found").arg(name));
    for (auto* widget : matches) {
        if (!widget->isVisible()) continue;
        if (target)
            return errorResponse("AMBIGUOUS_WIDGET", QString("More than one visible widget is named '%1'").arg(name));
        target = widget;
    }
    if (!target)
        return errorResponse("WIDGET_NOT_VISIBLE", QString("Widget '%1' is not visible").arg(name));

    if (!target->isEnabled())
        return errorResponse("WIDGET_DISABLED", QString("Widget '%1' is disabled").arg(name));
    QString buttonStr = args["button"].toString("left").toLower();
    if (buttonStr != "left" && buttonStr != "right" && buttonStr != "middle")
        return errorResponse("INVALID_ARGS", "button must be left, right or middle");
    Qt::MouseButton button = Qt::LeftButton;
    if (buttonStr == "right")
        button = Qt::RightButton;
    else if (buttonStr == "middle")
        button = Qt::MiddleButton;

    bool doubleClick = args["double_click"].toBool(false);

    QPointer<QWidget> safeTarget = target;
    QMetaObject::invokeMethod(mainWindow_, [safeTarget, button, doubleClick]() {
        if (PackGenerationRecordStore::instance().isExporting()) return;
        if (!safeTarget)
            return;
        QWidget* w = safeTarget.data();
        QPoint center = w->rect().center();
        QPointF centerF(center);
        QPointF globalF = w->mapToGlobal(center);

        if (doubleClick) {
            QMouseEvent dblClick(QEvent::MouseButtonDblClick, centerF, globalF,
                                button, button, Qt::NoModifier);
            QCoreApplication::sendEvent(w, &dblClick);
        } else {
            QMouseEvent press(QEvent::MouseButtonPress, centerF, globalF,
                              button, button, Qt::NoModifier);
            QCoreApplication::sendEvent(w, &press);
            if (!safeTarget) return;

            QMouseEvent release(QEvent::MouseButtonRelease, centerF, globalF,
                                button, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(w, &release);
        }
    }, Qt::QueuedConnection);

    QJsonObject result;
    result["widget_name"] = name;
    result["button"] = buttonStr;
    result["double_click"] = doubleClick;
    result["queued"] = true;
    return successResponse(result);
}

QJsonObject IPCServer::handleKeyPress(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    if (!args.contains("key"))
        return errorResponse("MISSING_PARAM", "key is required",
                             "Provide Qt::Key enum value (e.g. 32=Space, 16777220=Return)");

    const auto keyValue = args.value("key");
    const qint64 keyNumber = keyValue.toInteger(-1);
    if (!keyValue.isDouble() || keyNumber <= 0 || keyNumber > INT_MAX || keyValue.toDouble() != keyNumber)
        return errorResponse("INVALID_ARGS", "key must be a positive Qt key integer");
    const int key = static_cast<int>(keyNumber);
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    if (args.contains("modifiers")) {
        const auto value = args.value("modifiers");
        const qint64 number = value.toInteger(-1);
        if (!value.isDouble() || number < 0 || number > INT_MAX || value.toDouble() != number
            || (number & ~static_cast<qint64>(Qt::KeyboardModifierMask)) != 0)
            return errorResponse("INVALID_ARGS", "modifiers must contain only Qt keyboard modifier flags");
        modifiers = static_cast<Qt::KeyboardModifiers>(static_cast<int>(number));
    }

    QString text;
    if (key >= 0x20 && key <= 0x7E)
        text = modifiers.testFlag(Qt::ShiftModifier) ? QChar(key) : QChar(key).toLower();

    QWidget* target = nullptr;
    QString widgetName = args["widget"].toString();
    if (!widgetName.isEmpty()) {
        if (widgetName == "MainWindow")
            target = mainWindow_;
        else
            target = mainWindow_->findChild<QWidget*>(widgetName);

        if (!target)
            return errorResponse("WIDGET_NOT_FOUND",
                                 QString("Widget '%1' not found").arg(widgetName));
    } else {
        target = mainWindow_;
    }

    QPointer<QWidget> safeTarget = target;
    QMetaObject::invokeMethod(mainWindow_, [safeTarget, key, modifiers, text]() {
        if (PackGenerationRecordStore::instance().isExporting()) return;
        if (!safeTarget) return;
        QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
        QCoreApplication::sendEvent(safeTarget.data(), &press);
        if (!safeTarget) return;
        QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
        QCoreApplication::sendEvent(safeTarget.data(), &release);
    }, Qt::QueuedConnection);

    QJsonObject result;
    result["key"] = key;
    result["modifiers"] = static_cast<int>(modifiers);
    result["target_widget"] = widgetName.isEmpty() ? QStringLiteral("MainWindow") : widgetName;
    result["queued"] = true;
    return successResponse(result);
}

QJsonObject IPCServer::handleSetWidgetValue(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");
    if (PackGenerationRecordStore::instance().isExporting())
        return errorResponse("BUSY", "Wait for the current export to finish");
    const QString name = args.value("name").toString();
    if (name.isEmpty())
        return errorResponse("MISSING_PARAM", "name is required");
    if (!args.contains("value"))
        return errorResponse("MISSING_PARAM", "value is required");
    QWidget* target = mainWindow_->findChild<QWidget*>(name);
    if (!target)
        return errorResponse("WIDGET_NOT_FOUND", QString("Widget '%1' not found").arg(name));
    if (!target->isEnabled())
        return errorResponse("WIDGET_DISABLED", QString("Widget '%1' is disabled").arg(name));
    const QPointer<QWidget> safeTarget = target;
    const QString typeName = QString::fromLatin1(target->metaObject()->className());
    const QJsonValue value = args.value("value");
    const auto validNumber = [&](double minimum, double maximum, bool integer = false) {
        const double number = value.toDouble();
        return value.isDouble() && std::isfinite(number) && number >= minimum && number <= maximum
            && (!integer || std::trunc(number) == number);
    };
    const auto invalidValue = [&]() {
        return errorResponse("INVALID_VALUE", "value must match the widget type and allowed range");
    };
    if (auto* slider = qobject_cast<QSlider*>(target)) {
        if (!validNumber(slider->minimum(), slider->maximum(), true)) return invalidValue();
        slider->setValue(value.toInt());
    } else if (auto* spin = qobject_cast<QSpinBox*>(target)) {
        if (spin->isReadOnly()) return errorResponse("WIDGET_READ_ONLY", "Widget is read-only");
        if (!validNumber(spin->minimum(), spin->maximum(), true)) return invalidValue();
        spin->setValue(value.toInt());
    } else if (auto* spin = qobject_cast<QDoubleSpinBox*>(target)) {
        if (spin->isReadOnly()) return errorResponse("WIDGET_READ_ONLY", "Widget is read-only");
        if (!validNumber(spin->minimum(), spin->maximum())) return invalidValue();
        spin->setValue(value.toDouble());
    } else if (auto* edit = qobject_cast<QLineEdit*>(target)) {
        if (edit->isReadOnly()) return errorResponse("WIDGET_READ_ONLY", "Widget is read-only");
        if (!value.isString() || value.toString().size() > edit->maxLength()) return invalidValue();
        edit->setText(value.toString());
    } else if (auto* edit = qobject_cast<QPlainTextEdit*>(target)) {
        if (edit->isReadOnly()) return errorResponse("WIDGET_READ_ONLY", "Widget is read-only");
        if (!value.isString()) return invalidValue();
        edit->setPlainText(value.toString());
    } else if (auto* combo = qobject_cast<QComboBox*>(target)) {
        if (value.isDouble()) {
            if (!validNumber(0, combo->count() - 1, true)) return invalidValue();
            combo->setCurrentIndex(value.toInt());
        } else {
            if (!value.isString()) return invalidValue();
            const int index = combo->findText(value.toString());
            if (index < 0 && !combo->isEditable()) return invalidValue();
            if (index >= 0) combo->setCurrentIndex(index);
            else combo->setEditText(value.toString());
        }
    } else if (auto* slider = qobject_cast<ParameterSlider*>(target)) {
        if (!validNumber(slider->minimum(), slider->maximum())) return invalidValue();
        slider->simulateValueChange(value.toDouble());
    } else if (auto* toggle = qobject_cast<ToggleSwitch*>(target)) {
        if (!value.isBool()) return invalidValue();
        toggle->setChecked(value.toBool());
    } else if (auto* list = qobject_cast<QListWidget*>(target)) {
        if (!validNumber(-1, list->count() - 1, true)) return invalidValue();
        list->setCurrentRow(value.toInt());
    } else if (auto* button = qobject_cast<QAbstractButton*>(target); button && button->isCheckable()) {
        if (!value.isBool()) return invalidValue();
        button->setChecked(value.toBool());
    } else {
        return errorResponse("UNSUPPORTED_WIDGET", "Widget does not expose a supported editable value");
    }
    QJsonObject result{{"widget_name", name}, {"widget_type", typeName}};
    if (safeTarget)
        result.insert("state", serializeWidget(safeTarget.data(), 0, 0));
    return successResponse(result);
}

QJsonObject IPCServer::handleSelectTableRow(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    QString name = args["name"].toString();
    if (name.isEmpty())
        return errorResponse("MISSING_PARAM", "name is required",
                             "Provide QTableWidget objectName");

    QWidget* widget = mainWindow_->findChild<QWidget*>(name);
    if (!widget)
        return errorResponse("WIDGET_NOT_FOUND",
                             QString("Widget '%1' not found").arg(name),
                             "Use find_widget or get_ui_tree to discover names");

    auto* table = qobject_cast<QTableWidget*>(widget);
    if (!table)
        return errorResponse("TYPE_MISMATCH",
                             QString("Widget '%1' is %2, not QTableWidget")
                                 .arg(name, QString::fromLatin1(widget->metaObject()->className())));

    const int rowCount = table->rowCount();
    if (rowCount == 0)
        return errorResponse("EMPTY_TABLE", QString("Table '%1' has no rows").arg(name));

    int targetRow = -1;
    if (args.contains("row")) {
        targetRow = args["row"].toInt();
        if (targetRow < 0 || targetRow >= rowCount)
            return errorResponse("ROW_OUT_OF_RANGE",
                                 QString("row %1 out of range [0,%2)").arg(targetRow).arg(rowCount));
    } else if (args.contains("match_text")) {
        const QString matchText = args["match_text"].toString();
        const bool contains = args.value("match_contains").toBool(false);
        int matchCol = -1;
        if (args.contains("match_column")) {
            const QJsonValue mc = args["match_column"];
            if (mc.isDouble()) {
                matchCol = mc.toInt();
            } else {
                const QString hdr = mc.toString();
                for (int c = 0; c < table->columnCount(); ++c) {
                    auto* hi = table->horizontalHeaderItem(c);
                    if (hi && hi->text().compare(hdr, Qt::CaseInsensitive) == 0) { matchCol = c; break; }
                }
                if (matchCol < 0)
                    return errorResponse("COLUMN_NOT_FOUND", QString("No column header '%1'").arg(hdr));
            }
        }
        for (int r = 0; r < rowCount && targetRow < 0; ++r) {
            const int cStart = matchCol >= 0 ? matchCol : 0;
            const int cEnd   = matchCol >= 0 ? matchCol + 1 : table->columnCount();
            for (int c = cStart; c < cEnd; ++c) {
                auto* it = table->item(r, c);
                if (!it) continue;
                const bool hit = contains ? it->text().contains(matchText, Qt::CaseInsensitive)
                                          : it->text().compare(matchText, Qt::CaseInsensitive) == 0;
                if (hit) { targetRow = r; break; }
            }
        }
        if (targetRow < 0)
            return errorResponse("NO_MATCH", QString("No row matches '%1'").arg(matchText));
    } else {
        return errorResponse("MISSING_PARAM", "row or match_text is required");
    }

    QPointer<QTableWidget> safeTable = table;
    const int row = targetRow;
    QMetaObject::invokeMethod(mainWindow_, [safeTable, row]() {
        if (!safeTable) return;
        QTableWidget* t = safeTable.data();
        t->setCurrentCell(row, 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        if (auto* it = t->item(row, 0))
            t->scrollToItem(it);
    }, Qt::DirectConnection);

    QJsonObject result;
    result["name"] = name;
    result["selected_row"] = targetRow;
    result["row_count"] = rowCount;
    return successResponse(result);
}

QJsonObject IPCServer::handleTriggerMenuAction(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    if (args.contains("emit_context")) {
        QString cn = args["emit_context"].toString();
        QWidget* cw = (cn == QStringLiteral("MainWindow")) ? mainWindow_
                                                           : mainWindow_->findChild<QWidget*>(cn);
        if (!cw)
            return errorResponse("WIDGET_NOT_FOUND", QString("Widget '%1' not found").arg(cn));
        QPoint pos;
        if (args.contains("pos_x") && args.contains("pos_y"))
            pos = QPoint(args["pos_x"].toInt(), args["pos_y"].toInt());
        else if (auto* sa = qobject_cast<QAbstractScrollArea*>(cw))
            pos = sa->viewport()->rect().center();
        else
            pos = cw->rect().center();
        QMetaObject::invokeMethod(cw, [cw, pos] {
            if (!PackGenerationRecordStore::instance().isExporting())
                emit cw->customContextMenuRequested(pos);
        }, Qt::QueuedConnection);
        QJsonObject result;
        result["emitted_context"] = cn;
        result["queued"] = true;
        result["pos_x"] = pos.x();
        result["pos_y"] = pos.y();
        return successResponse(result);
    }

    QString actionText = args["action_text"].toString();
    if (actionText.isEmpty())
        return errorResponse("MISSING_PARAM", "action_text is required",
                             "Provide the menu item text to trigger");

    auto stripAmp = [](QString s) { return s.remove(QChar('&')); };

    QMenu* menu = nullptr;
    QString buttonName = args["button"].toString();
    if (!buttonName.isEmpty()) {
        QWidget* bw = mainWindow_->findChild<QWidget*>(buttonName);
        if (!bw)
            return errorResponse("WIDGET_NOT_FOUND", QString("Widget '%1' not found").arg(buttonName));
        if (auto* tb = qobject_cast<QToolButton*>(bw))
            menu = tb->menu();
        else if (auto* pb = qobject_cast<QPushButton*>(bw))
            menu = pb->menu();
        if (!menu)
            return errorResponse("NO_MENU", QString("Widget '%1' has no attached menu").arg(buttonName));
    } else {
        menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu)
            return errorResponse("NO_ACTIVE_MENU",
                                 "No active popup menu",
                                 "Pass 'button' for a button menu, or emit_context first for a context menu");
    }

    QAction* target = nullptr;
    QStringList available;
    for (QAction* a : menu->actions()) {
        if (a->isSeparator()) continue;
        QString t = stripAmp(a->text());
        available << t;
        if (t.compare(actionText, Qt::CaseInsensitive) == 0) { target = a; break; }
    }
    if (!target)
        return errorResponse("ACTION_NOT_FOUND",
                             QString("No menu action '%1'").arg(actionText),
                             QString("Available: %1").arg(available.join(QStringLiteral(", "))));

    if (!target->isEnabled() || !target->isVisible())
        return errorResponse("ACTION_UNAVAILABLE", "The menu action is disabled or hidden");
    const bool isPopup = (menu == QApplication::activePopupWidget());
    QPointer<QMenu> safeMenu = menu;
    QPointer<QAction> safeAction = target;
    QMetaObject::invokeMethod(mainWindow_, [safeMenu, safeAction, isPopup]() {
        if (PackGenerationRecordStore::instance().isExporting()) return;
        if (!safeAction) return;
        if (isPopup && safeMenu) {
            safeMenu->setActiveAction(safeAction.data());
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::sendEvent(safeMenu.data(), &press);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::sendEvent(safeMenu.data(), &release);
        } else {
            safeAction->trigger();
        }
    }, Qt::QueuedConnection);

    QJsonObject result;
    result["action_text"] = stripAmp(target->text());
    result["queued"] = true;
    result["mode"] = isPopup ? QStringLiteral("popup") : QStringLiteral("button");
    if (!buttonName.isEmpty())
        result["button"] = buttonName;
    return successResponse(result);
}

QJsonObject IPCServer::handleAnswerDialog(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");
    QWidget* modal = QApplication::activeModalWidget();
    if (!modal)
        return errorResponse("NO_ACTIVE_DIALOG", "No active modal dialog");
    if (args.contains("response") && !args.value("response").isString())
        return errorResponse("INVALID_ARGS", "response must be a string");
    const QString response = args.value("response").toString(QStringLiteral("accept")).trimmed().toLower();
    const bool reject = response == "reject" || response == "cancel";
    const bool accept = response == "accept" || response == "ok";
    const QString dialogType = QString::fromLatin1(modal->metaObject()->className());
    const QPointer<QWidget> safe = modal;
    QString applied;
    if (auto* message = qobject_cast<QMessageBox*>(modal)) {
        QAbstractButton* button = nullptr;
        for (auto* candidate : message->buttons()) {
            QString text = candidate->text();
            text.remove(QChar('&'));
            if (text.compare(response, Qt::CaseInsensitive) == 0) {
                button = candidate;
                break;
            }
        }
        if (!button) {
            if (response == "yes") button = message->button(QMessageBox::Yes);
            else if (response == "no") button = message->button(QMessageBox::No);
            else if (response == "ok") button = message->button(QMessageBox::Ok);
            else if (reject) button = message->button(QMessageBox::Cancel);
            else if (response == "accept") button = message->defaultButton();
        }
        if (!button || !button->isEnabled())
            return errorResponse("INVALID_RESPONSE", "response must name an enabled button on the active message box");
        button->click();
        applied = "messagebox";
    } else {
        if (!accept && !reject)
            return errorResponse("INVALID_RESPONSE", "response must be accept, ok, reject or cancel");
        if (auto* input = qobject_cast<QInputDialog*>(modal)) {
            if (!reject && args.contains("text")) {
                if (!args.value("text").isString() || input->inputMode() != QInputDialog::TextInput)
                    return errorResponse("INVALID_ARGS", "text requires a text input dialog and a string value");
                input->setTextValue(args.value("text").toString());
            }
            reject ? input->reject() : input->accept();
            applied = "input";
        } else if (auto* file = qobject_cast<QFileDialog*>(modal)) {
            if (!reject && args.contains("path")) {
                if (!args.value("path").isString() || args.value("path").toString().isEmpty())
                    return errorResponse("INVALID_ARGS", "path must be a non-empty string");
                const QString path = args.value("path").toString();
                file->setDirectory(file->fileMode() == QFileDialog::Directory ? path : QFileInfo(path).absolutePath());
                file->selectFile(path);
            }
            QDialog* dialog = file;
            reject ? dialog->reject() : dialog->accept();
            applied = "file";
        } else if (auto* dialog = qobject_cast<QDialog*>(modal)) {
            reject ? dialog->reject() : dialog->accept();
            applied = "dialog";
        } else {
            return errorResponse("UNSUPPORTED_DIALOG", QString("Unsupported modal widget type: %1").arg(dialogType));
        }
    }
    if (safe && safe->isVisible())
        return errorResponse("DIALOG_NOT_CLOSED", "The dialog did not accept the supplied response or value");
    return successResponse(QJsonObject{{"dialog_type", dialogType},
        {"handled_as", applied}, {"response", response}});
}

QJsonObject IPCServer::handleGetWidgetValue(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    QString name = args["name"].toString();
    if (name.isEmpty())
        return errorResponse("MISSING_PARAM", "name is required");

    QWidget* target = mainWindow_->findChild<QWidget*>(name);
    if (!target)
        return errorResponse("WIDGET_NOT_FOUND",
                             QString("Widget '%1' not found").arg(name),
                             "Use find_widget or get_ui_tree to discover available widget names");

    QJsonObject widgetState = serializeWidget(target, 0, 0);
    widgetState["path"] = widgetPath(target);

    return successResponse(widgetState);
}

QJsonObject IPCServer::handleGetWidgetChildren(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    QString name = args["name"].toString();
    if (name.isEmpty())
        return errorResponse("MISSING_PARAM", "name is required");

    bool namedOnly = args.value("named_only").toBool(false);
    int maxDepth = qBound(0, args.value("depth").toInt(1), 5);

    QWidget* target = mainWindow_->findChild<QWidget*>(name);
    if (!target)
        return errorResponse("WIDGET_NOT_FOUND",
                             QString("Widget '%1' not found").arg(name),
                             "Use find_widget or get_ui_tree to discover available widget names");

    QJsonArray children;
    for (auto* child : target->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (namedOnly && child->objectName().isEmpty())
            continue;
        children.append(serializeWidget(child, 0, maxDepth));
    }

    QJsonObject result;
    result["parent"] = name;
    result["parent_type"] = QString::fromLatin1(target->metaObject()->className());
    result["count"] = children.size();
    result["children"] = children;
    return successResponse(result);
}

QJsonObject IPCServer::handleGetTableData(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    QString name = args["name"].toString();
    if (name.isEmpty())
        return errorResponse("MISSING_PARAM", "name is required");

    QWidget* widget = mainWindow_->findChild<QWidget*>(name);
    if (!widget)
        return errorResponse("WIDGET_NOT_FOUND",
                             QString("Widget '%1' not found").arg(name),
                             "Use find_widget or get_ui_tree to discover available widget names");

    auto* table = qobject_cast<QTableWidget*>(widget);
    if (!table)
        return errorResponse("TYPE_MISMATCH",
                             QString("Widget '%1' is %2, not QTableWidget")
                                 .arg(name, QString::fromLatin1(widget->metaObject()->className())),
                             "Use get_widget_value for non-table widgets");

    int maxRows = qBound(0, args.value("max_rows").toInt(500), 5000);

    QJsonArray headers;
    for (int c = 0; c < table->columnCount(); ++c) {
        auto* hdrItem = table->horizontalHeaderItem(c);
        headers.append(hdrItem ? hdrItem->text() : QString::number(c));
    }

    int rowCount = table->rowCount();
    int returnRows = qMin(rowCount, maxRows);
    QJsonArray rows;
    for (int r = 0; r < returnRows; ++r) {
        QJsonArray row;
        for (int c = 0; c < table->columnCount(); ++c) {
            auto* item = table->item(r, c);
            if (item) {
                QJsonObject cell;
                cell["text"] = item->text();
                if (item->checkState() != Qt::Unchecked)
                    cell["checked"] = (item->checkState() == Qt::Checked);
                if (item->flags() & Qt::ItemIsEditable)
                    cell["editable"] = true;
                row.append(cell);
            } else {
                row.append(QJsonValue());
            }
        }
        rows.append(row);
    }

    QSet<int> selectedRowSet;
    for (auto* selItem : table->selectedItems())
        selectedRowSet.insert(selItem->row());
    QJsonArray selectedRows;
    for (int r : selectedRowSet)
        selectedRows.append(r);

    QJsonObject result;
    result["name"] = name;
    result["headers"] = headers;
    result["rows"] = rows;
    result["rowCount"] = rowCount;
    result["columnCount"] = table->columnCount();
    result["returnedRows"] = returnRows;
    if (returnRows < rowCount)
        result["truncated"] = true;
    result["currentRow"] = table->currentRow();
    result["currentColumn"] = table->currentColumn();
    result["selectedRows"] = selectedRows;

    return successResponse(result);
}

QJsonObject IPCServer::handleGetListItems(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    QString name = args["name"].toString();
    if (name.isEmpty())
        return errorResponse("MISSING_PARAM", "name is required");

    QWidget* widget = mainWindow_->findChild<QWidget*>(name);
    if (!widget)
        return errorResponse("WIDGET_NOT_FOUND",
                             QString("Widget '%1' not found").arg(name),
                             "Use find_widget or get_ui_tree to discover available widget names");

    auto* list = qobject_cast<QListWidget*>(widget);
    if (!list)
        return errorResponse("TYPE_MISMATCH",
                             QString("Widget '%1' is %2, not QListWidget")
                                 .arg(name, QString::fromLatin1(widget->metaObject()->className())),
                             "Use get_widget_value for non-list widgets");

    int maxItems = qBound(0, args.value("max_items").toInt(500), 5000);
    int itemCount = list->count();
    int returnCount = qMin(itemCount, maxItems);

    QJsonArray items;
    for (int i = 0; i < returnCount; ++i) {
        auto* item = list->item(i);
        QJsonObject entry;
        entry["text"] = item->text();
        entry["selected"] = item->isSelected();
        if (item->checkState() != Qt::Unchecked)
            entry["checked"] = (item->checkState() == Qt::Checked);
        if (item->flags() & Qt::ItemIsEditable)
            entry["editable"] = true;
        items.append(entry);
    }

    QJsonObject result;
    result["name"] = name;
    result["items"] = items;
    result["count"] = itemCount;
    result["returnedCount"] = returnCount;
    if (returnCount < itemCount)
        result["truncated"] = true;
    result["currentRow"] = list->currentRow();

    return successResponse(result);
}

static QJsonObject serializeTreeItem(QTreeWidgetItem* item, int treeColumnCount, int depth, int maxDepth)
{
    QJsonObject obj;
    obj["text"] = item->text(0);

    if (treeColumnCount > 1) {
        QJsonArray cols;
        for (int c = 0; c < treeColumnCount; ++c)
            cols.append(item->text(c));
        obj["columns"] = cols;
    }

    obj["expanded"] = item->isExpanded();
    obj["selected"] = item->isSelected();

    if (item->checkState(0) != Qt::Unchecked)
        obj["checked"] = (item->checkState(0) == Qt::Checked);

    if (item->childCount() > 0 && depth < maxDepth) {
        QJsonArray children;
        for (int i = 0; i < item->childCount(); ++i)
            children.append(serializeTreeItem(item->child(i), treeColumnCount, depth + 1, maxDepth));
        obj["children"] = children;
        obj["childCount"] = item->childCount();
    } else if (item->childCount() > 0) {
        obj["childCount"] = item->childCount();
    }

    return obj;
}

QJsonObject IPCServer::handleGetTreeData(const QJsonObject& args)
{
    if (!mainWindow_)
        return errorResponse("NO_MAIN_WINDOW", "MainWindow not available");

    QString name = args["name"].toString();
    if (name.isEmpty())
        return errorResponse("MISSING_PARAM", "name is required");

    QWidget* widget = mainWindow_->findChild<QWidget*>(name);
    if (!widget)
        return errorResponse("WIDGET_NOT_FOUND",
                             QString("Widget '%1' not found").arg(name),
                             "Use find_widget or get_ui_tree to discover available widget names");

    auto* tree = qobject_cast<QTreeWidget*>(widget);
    if (!tree)
        return errorResponse("TYPE_MISMATCH",
                             QString("Widget '%1' is %2, not QTreeWidget")
                                 .arg(name, QString::fromLatin1(widget->metaObject()->className())),
                             "Use get_widget_value for non-tree widgets");

    int maxDepth = qBound(1, args.value("max_depth").toInt(10), 50);

    QJsonArray headers;
    for (int c = 0; c < tree->columnCount(); ++c) {
        auto* hdrItem = tree->headerItem();
        headers.append(hdrItem ? hdrItem->text(c) : QString::number(c));
    }

    QJsonArray items;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        items.append(serializeTreeItem(tree->topLevelItem(i), tree->columnCount(), 0, maxDepth));

    QJsonObject result;
    result["name"] = name;
    result["headers"] = headers;
    result["columnCount"] = tree->columnCount();
    result["topLevelItemCount"] = tree->topLevelItemCount();
    result["items"] = items;

    return successResponse(result);
}

QJsonObject IPCServer::handleFlushUIEvents(const QJsonObject& args)
{
    int maxIterations = qBound(1, args.value("max_iterations").toInt(3), 50);

    int processed = 0;
    for (int i = 0; i < maxIterations; ++i) {
        if (!QCoreApplication::instance())
            break;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50 );
        ++processed;
    }

    QJsonObject result;
    result["iterations"] = processed;
    return successResponse(result);
}

}
