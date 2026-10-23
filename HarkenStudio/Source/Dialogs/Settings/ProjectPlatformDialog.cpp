#include "ProjectPlatformDialog.h"

#include "ProjectPlatformEditor.h"
#include "Core/QtControlSkin.h"
#include "Widgets/Common/TableWidget.h"

#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>

namespace VNX {

ProjectPlatformDialog::ProjectPlatformDialog(QWidget* parent)
    : BaseDialog(parent)
{
    setObjectName("projectPlatformDialog");
    setWindowTitle(tr("Add Platform"));
    resize(600, 360);
    auto* layout = new QVBoxLayout(this);
    auto* content = new QHBoxLayout();
    content->setSpacing(0);
    auto* platformTabs = new TableWidget(
        {TableKind::Config, TableBodySizing::Expanding}, this);
    platformTabs->setFixedWidth(150);
    auto* platforms = platformTabs->createNavigationView(QStringLiteral("supportedPlatformList"));
    for (const auto base : {BasePlatform::Windows, BasePlatform::Android, BasePlatform::Linux,
                            BasePlatform::macOS, BasePlatform::iOS}) {
        auto* item = new QListWidgetItem(ProjectPlatformSemantics::basePlatformName(base), platforms);
        item->setData(Qt::UserRole, static_cast<int>(base));
    }
    content->addWidget(platformTabs);
    auto* preview = new ProjectPlatformEditor(this);
    QtControlSkin::setSemanticProperty(preview, "joinedEdge", QStringLiteral("left"));
    preview->setReadOnly(true);
    content->addWidget(preview, 1);
    layout->addLayout(content, 1);
    connect(platforms, &QListWidget::currentRowChanged, this, [this, platforms, preview](int row) {
        if (row < 0) return;
        platform_ = {};
        platform_.basePlatform = static_cast<BasePlatform>(platforms->item(row)->data(Qt::UserRole).toInt());
        platform_.name = ProjectPlatformSemantics::basePlatformName(platform_.basePlatform);
        platform_.profile = ProjectPlatformProfile::fromBasePlatform(platform_.basePlatform);
        preview->setPlatform(platform_);
    });
    platforms->setCurrentRow(0);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->setObjectName("supportedPlatformDialogButtons");
    auto* add = buttons->addButton(tr("Add"), QDialogButtonBox::AcceptRole);
    add->setObjectName("addProjectPlatformButton");
    add->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

}
