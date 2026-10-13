
#include "MemoryDataSource.h"
#include "VQLFieldCatalog.h"
#include "../Core/ItemTypeCatalog.h"
#include "../Core/ProjectTypes.h"
#include <QRegularExpression>

namespace VNX {


namespace {

bool wildcardMatch(const QString& pattern, const QString& text)
{
    QString regexStr;
    regexStr.reserve(pattern.size() * 2);
    for (const QChar& ch : pattern) {
        if      (ch == '*') regexStr += ".*";
        else if (ch == '?') regexStr += ".";
        else                regexStr += QRegularExpression::escape(QString(ch));
    }
    QRegularExpression re("^" + regexStr + "$",
                          QRegularExpression::CaseInsensitiveOption);
    return re.match(text).hasMatch();
}

}


void MemoryDataSource::collectRoots(const QString& rootSelector,
                                     std::vector<ProjectItem*>& out)
{
    if (!project_) return;

    if (rootSelector.isEmpty()) {
        for (const auto* binding : project_->allFileBindings()) {
            if (!binding || !binding->rootItem) continue;
            collectAllDescendants(binding->rootItem.get(), out);
        }
        return;
    }

    for (const auto* binding : project_->allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        if (binding->rootItem->name.compare(rootSelector, Qt::CaseInsensitive) == 0 ||
            wildcardMatch(rootSelector, binding->rootItem->name))
        {
            collectAllDescendants(binding->rootItem.get(), out);
        }
    }
}


void MemoryDataSource::collectAllDescendants(ProjectItem* root,
                                              std::vector<ProjectItem*>& out)
{
    if (!root) return;
    out.push_back(root);
    for (const auto& child : root->children)
        collectAllDescendants(child.get(), out);
}


VQLValue MemoryDataSource::getProperty(const ProjectItem* item, const QString& fieldName)
{
    VQLValue value;
    VQLFieldCatalog::project(item, fieldName, nullptr, &value, project_);
    return value;
}


bool MemoryDataSource::matchesType(const ProjectItem* item, const QString& typeName)
{
    if (!item) return false;
    const auto type = ItemTypeCatalog::parseTypeName(typeName, Qt::CaseInsensitive, false);
    return type && item->type == *type;
}

}
