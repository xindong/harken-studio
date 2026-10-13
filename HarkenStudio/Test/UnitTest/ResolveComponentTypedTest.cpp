#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Core/Component.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectTypes.h"

#include <gtest/gtest.h>

using VNX::ComponentType;
using VNX::ProjectItem;

TEST(ResolveComponentTyped, EnumTableHitsExactlyAttachedComponents)
{
    for (ProjectItem::Type type : VNX::ItemTypeCatalog::allTypes())
    {
        ProjectItem item(QStringLiteral("Item"), type);
        item.attachDefaultComponents();

        for (size_t i = 0; i < VNX::kMaxComponents; ++i)
        {
            const auto ct = static_cast<ComponentType>(i);
            VNX::IComponent* comp =
                VNX::PropertyEditorEngine::resolveComponentTyped(&item, ct);
            EXPECT_EQ(comp != nullptr, item.components.has(ct))
                << VNX::ItemTypeCatalog::canonicalName(type).toStdString()
                << " component " << static_cast<int>(ct);
            if (comp)
                EXPECT_EQ(comp->getType(), ct);
        }
    }
}

TEST(ResolveComponentTyped, SentinelAndNullItemResolveToNull)
{
    ProjectItem sound(QStringLiteral("Item"), ProjectItem::Type::Sound);
    sound.attachDefaultComponents();
    EXPECT_EQ(VNX::PropertyEditorEngine::resolveComponentTyped(&sound, ComponentType::_Count),
              nullptr);
    EXPECT_EQ(VNX::PropertyEditorEngine::resolveComponentTyped(nullptr, ComponentType::MixNode),
              nullptr);
}
