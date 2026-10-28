#include "RemoteConnectPanel.h"
#include "Core/BaseDialog.h"

#include "Core/QtControlSkin.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "RuntimeBridge/RemoteConnection.h"
#include "Widgets/Common/DialogFormLayout.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableWidget.h"

#include <QAction>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>

#include <algorithm>

namespace VNX {

namespace {

enum TargetRole { InstanceRole = Qt::UserRole, AddressRole, PortRole, CompatibleRole };

}

RemoteConnectPanel::RemoteConnectPanel(HarkenEditor::Bridge::RemoteConnection* connection,
                                       QWidget* parent)
    : QWidget(parent), connection_(connection)
{
    setObjectName(QStringLiteral("RemoteConnectPanel"));
    setWindowTitle(tr("Remote Connect"));
    setMinimumSize(420, 240);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);
    auto* table = new TableWidget(TableOptions::display(), this);
    targets_ = table->createItemView(QStringLiteral("remoteTargets"),
        {tr("Application"), tr("Machine / PID"), tr("Address"),
         tr("Engine"), tr("Protocol"), tr("Compatibility")},
        {220, 180, 190, 120, 100, 140});
    targets_->setSelectionMode(QAbstractItemView::SingleSelection);
    targets_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setMinimumHeight(160);
    auto* footer = table->footer();
    footer->removeButton()->hide();
    manual_ = footer->addButton();
    manual_->setObjectName(QStringLiteral("remoteAddConnection"));
    const auto manualTitle = HarkenEditor::Bridge::RemoteConnection::kLocalConnectionsOnly
        ? tr("Connect to Local Application") : tr("Connect by Address");
    manual_->setToolTip(manualTitle);
    manual_->setAccessibleName(manualTitle);
    auto* refreshAction = new QAction(tr("Refresh"), footer);
    refreshAction->setToolTip(tr("Refresh Applications"));
    auto* refresh = footer->setRefreshAction(refreshAction);
    refresh->setObjectName(QStringLiteral("remoteDiscover"));
    root->addWidget(table, 1);

    auto* connectionRow = new QHBoxLayout();
    connectionRow->setSpacing(8);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("remoteConnectionStatus"));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    connectionRow->addWidget(status_, 1);
    connect_ = new QPushButton(tr("Connect"), this);
    connect_->setObjectName(QStringLiteral("remoteConnect"));
    disconnect_ = new QPushButton(tr("Disconnect"), this);
    disconnect_->setObjectName(QStringLiteral("remoteDisconnect"));
    connectionRow->addWidget(connect_);
    connectionRow->addWidget(disconnect_);
    root->addLayout(connectionRow);

    identity_ = new QLabel(this);
    identity_->setObjectName(QStringLiteral("remoteIdentity"));
    identity_->setWordWrap(true);
    identity_->setTextFormat(Qt::PlainText);
    identity_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QtControlSkin::setSemanticProperty(identity_, "textRole", QStringLiteral("secondary"));
    root->addWidget(identity_);

    connect(refreshAction, &QAction::triggered, this, [this] { connection_->discover(); });
    connect(manual_, &QToolButton::clicked, this, &RemoteConnectPanel::openManualConnection);
    connect(targets_, &QTreeWidget::itemSelectionChanged, this, &RemoteConnectPanel::selectTarget);
    connect(targets_, &QTreeWidget::itemActivated, this, [this] { connectSelectedTarget(); });
    connect(connect_, &QPushButton::clicked, this, &RemoteConnectPanel::connectSelectedTarget);
    connect(disconnect_, &QPushButton::clicked, connection_, &HarkenEditor::Bridge::RemoteConnection::disconnectTarget);
    connect(connection_, &HarkenEditor::Bridge::RemoteConnection::stateChanged, this, [this] {
        if (connection_->state() == HarkenEditor::Bridge::RemoteConnectionState::Connected)
            selectedTargetKey_ = connection_->target().instanceId;
        refreshTargets();
    });
    connect(connection_, &HarkenEditor::Bridge::RemoteConnection::targetsChanged, this, &RemoteConnectPanel::refreshTargets);
    connect(connection_, &HarkenEditor::Bridge::RemoteConnection::discoveryFailed,
            this, [this](const QString& error) {
        status_->setText(error);
        QtControlSkin::setSemanticProperty(status_, "textRole", QStringLiteral("error"));
    });
    refreshTargets();
}

QSize RemoteConnectPanel::sizeHint() const
{
    return QSize(860, 380);
}

void RemoteConnectPanel::refreshTargets()
{
    QSignalBlocker blocker(targets_);
    auto targets = connection_->discoveredTargets();
    if (connection_->state() == HarkenEditor::Bridge::RemoteConnectionState::Connected) {
        const auto connected = connection_->target();
        const auto found = std::find_if(targets.begin(), targets.end(), [&](const auto& target) {
            return target.instanceId == connected.instanceId;
        });
        if (found == targets.end())
            targets.prepend(connected);
        else
            *found = connected;
    }
    targets_->clear();
    for (const auto& target : targets) {
        const QStringList values {target.application,
            tr("%1 / %2").arg(target.machine).arg(target.processId),
            tr("%1:%2").arg(target.address).arg(target.port), target.engineVersion,
            tr("%1.%2").arg(target.protocolMajor).arg(target.protocolMinor),
            target.compatible ? tr("Compatible") : tr("Incompatible")};
        auto* row = new QTreeWidgetItem(targets_, values);
        for (int column = 0; column < values.size(); ++column)
            row->setToolTip(column, values[column]);
        row->setData(0, InstanceRole, target.instanceId);
        row->setData(0, AddressRole, target.address);
        row->setData(0, PortRole, target.port);
        row->setData(0, CompatibleRole, target.compatible);
        if (target.instanceId == selectedTargetKey_)
            targets_->setCurrentItem(row);
    }
    refreshState();
}

void RemoteConnectPanel::selectTarget()
{
    const auto* row = targets_->currentItem();
    selectedTargetKey_ = row && row->isSelected() ? row->data(0, InstanceRole).toString() : QString();
    refreshState();
}

void RemoteConnectPanel::connectSelectedTarget()
{
    if (!connect_->isEnabled())
        return;
    const auto* row = targets_->currentItem();
    connection_->connectTarget(row->data(0, AddressRole).toString(),
                               static_cast<quint16>(row->data(0, PortRole).toUInt()));
}

void RemoteConnectPanel::openManualConnection()
{
    BaseDialog dialog(this);
    dialog.setObjectName(QStringLiteral("remoteManualConnectDialog"));
    dialog.setWindowTitle(manual_->toolTip());
    dialog.setMinimumWidth(360);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    auto* form = new DialogFormLayout();
    auto* address = new QLineEdit(&dialog);
    address->setObjectName(QStringLiteral("remoteAddress"));
    QSettings settings;
    if constexpr (HarkenEditor::Bridge::RemoteConnection::kLocalConnectionsOnly) {
        address->setText(QStringLiteral("127.0.0.1"));
        address->setReadOnly(true);
    } else {
        address->setPlaceholderText(tr("Host name or IP address"));
        address->setText(settings.value(QStringLiteral("remote/host"), QStringLiteral("127.0.0.1")).toString());
    }
    auto* port = new QSpinBox(&dialog);
    port->setObjectName(QStringLiteral("remotePort"));
    port->setRange(0, 65535);
    port->setSpecialValueText(tr("Not set"));
    const int savedPort = settings.value(QStringLiteral("remote/port"), 0).toInt();
    port->setValue(savedPort >= 1 && savedPort <= 65535 ? savedPort : 0);
    form->addRow(tr("Host"), address);
    form->addRow(tr("Port"), port);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(&dialog);
    auto* submit = buttons->addButton(tr("Connect"), QDialogButtonBox::AcceptRole);
    submit->setObjectName(QStringLiteral("remoteManualConnect"));
    submit->setDefault(true);
    layout->addWidget(buttons);
    const auto validate = [this, address, port, submit] {
        using HarkenEditor::Bridge::RemoteConnectionState;
        const auto state = connection_->state();
        submit->setEnabled((state == RemoteConnectionState::Disconnected || state == RemoteConnectionState::Failed)
                           && !address->text().trimmed().isEmpty() && port->value() != 0);
    };
    connect(address, &QLineEdit::textChanged, &dialog, validate);
    connect(port, &QSpinBox::valueChanged, &dialog, validate);
    connect(connection_, &HarkenEditor::Bridge::RemoteConnection::stateChanged, &dialog, validate);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    validate();
    if constexpr (HarkenEditor::Bridge::RemoteConnection::kLocalConnectionsOnly)
        port->setFocus();
    else
        address->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return;
    settings.setValue(QStringLiteral("remote/host"), address->text().trimmed());
    settings.setValue(QStringLiteral("remote/port"), port->value());
    connection_->connectTarget(address->text().trimmed(), static_cast<quint16>(port->value()));
}

void RemoteConnectPanel::refreshState()
{
    using HarkenEditor::Bridge::RemoteConnectionState;
    const auto state = connection_->state();
    const bool connected = state == RemoteConnectionState::Connected;
    const bool idle = state == RemoteConnectionState::Disconnected || state == RemoteConnectionState::Failed;
    const auto* row = targets_->currentItem();
    const bool selected = row && row->isSelected();
    const bool compatible = selected && row->data(0, CompatibleRole).toBool();
    connect_->setEnabled(idle && compatible && row->data(0, PortRole).toUInt() != 0);
    connect_->setToolTip(selected && !compatible ? tr("Incompatible remote protocol") : QString());
    disconnect_->setEnabled(!idle);
    manual_->setEnabled(idle);
    status_->setText(connection_->statusText());
    QtControlSkin::setSemanticProperty(status_, "textRole", state == RemoteConnectionState::Failed
        ? QStringLiteral("error") : connected ? QStringLiteral("accent") : QStringLiteral("secondary"));
    const auto target = connection_->target();
    identity_->setText(connected
        ? tr("%1 | Project %2\nBuild %3 | Instance %4")
            .arg(target.platform, target.project, target.buildId, target.instanceId)
        : QString());
    identity_->setVisible(connected);
}

}
