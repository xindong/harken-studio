#pragma once

#include "Core/PropertyDescriptor.h"

#include <QString>
#include <vector>

namespace VNX {

class TableWidget;

inline constexpr int kInspectorCategoryCount = 4;

struct InspectorSectionInfo {
    PropertySection section;
    PropertyCategory category;
    QString key;
    QString title;
};

QString inspectorCategoryTitle(PropertyCategory category);
QString inspectorCategoryObjectName(PropertyCategory category);
const std::vector<InspectorSectionInfo>& inspectorSections();
const InspectorSectionInfo& inspectorSectionInfo(PropertySection section);
void validateInspectorSchema(const PropertySchema& schema);
PropertyDescriptor withInspectorRowOrder(PropertyDescriptor field);
void applyInspectorTableDescriptor(TableWidget* table, const PropertyDescriptor& field);
void applyInspectorTableDescriptor(TableWidget* table, const PropertySchema& schema,
                                   const QString& field);

}
