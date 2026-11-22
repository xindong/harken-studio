#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Core/Component.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectTypes.h"

#include <gtest/gtest.h>

using harken::studio::ComponentType;
using harken::studio::ProjectItem;

TEST(ResolveComponentTyped, EnumTableHitsExactlyAttachedComponents)
{
    for (ProjectItem::Type type : harken::studio::ItemTypeCatalog::allTypes())
    {
        ProjectItem item(QStringLiteral("Item"), type);
        item.attachDefaultComponents();

        for (size_t i = 0; i < harken::studio::kMaxComponents; ++i)
        {
            const auto ct = static_cast<ComponentType>(i);
            harken::studio::IComponent* comp =
                harken::studio::PropertyEditorEngine::resolveComponentTyped(&item, ct);
            EXPECT_EQ(comp != nullptr, item.components.has(ct))
                << harken::studio::ItemTypeCatalog::canonicalName(type).toStdString()
                << " component " << static_cast<int>(ct);
            if (comp)
                EXPECT_EQ(comp->getType(), ct);
        }
    }
}
