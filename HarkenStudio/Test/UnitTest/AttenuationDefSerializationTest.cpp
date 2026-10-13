#include "ProjectTypes.h"
#include "Components/VAttenuation.h"
#include "ProjectSerializer.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

namespace {

QJsonObject pointJson(double distance, double value)
{
    return QJsonObject{{"distance", distance}, {"value", value}};
}

// QJsonArray{QJsonArray{...}} with a single element hits the copy
// constructor and flattens; append keeps the nesting.
QJsonArray singleHandleJson(double h1x, double h1y, double h2x, double h2y)
{
    QJsonArray pairs;
    pairs.append(QJsonArray{h1x, h1y, h2x, h2y});
    return pairs;
}

void expectHandle(const VNX::CurveHandlePairDef& handle,
                  float h1x, float h1y, float h2x, float h2y)
{
    EXPECT_FLOAT_EQ(handle.h1x, h1x);
    EXPECT_FLOAT_EQ(handle.h1y, h1y);
    EXPECT_FLOAT_EQ(handle.h2x, h2x);
    EXPECT_FLOAT_EQ(handle.h2y, h2y);
}

} // namespace

TEST(AttenuationDefSerialization, PointsAndHandlesRoundTripExactly)
{
    VNX::AttenuationSnapshot def;
    def.name = QStringLiteral("Bezier");
    def.maxDistance = 100.0f;

    auto& volume = def.ensureDimension(VNX::AttenuationDimension::Volume);
    volume.curve.push_back({0.0f, 0.0f});
    volume.curve.push_back({40.0f, -12.0f});
    volume.curve.push_back({100.0f, -96.0f});
    volume.handles.push_back({0.0f, 0.0f, 40.0f, -12.0f});
    volume.handles.push_back({55.0f, -12.0f, 80.0f, -70.0f});

    auto& pitch = def.ensureDimension(VNX::AttenuationDimension::Pitch);
    pitch.curve.push_back({0.0f, 0.0f});
    pitch.curve.push_back({100.0f, 1200.0f});
    pitch.handles.push_back({25.0f, 0.0f, 75.0f, 1200.0f});

    const QJsonObject json = def.toJson();
    const VNX::AttenuationSnapshot loaded = VNX::AttenuationSnapshot::fromJson(json);

    ASSERT_EQ(loaded.dimensions.size(), 2);
    const auto* loadedVolume = loaded.dimension(VNX::AttenuationDimension::Volume);
    ASSERT_NE(loadedVolume, nullptr);
    ASSERT_EQ(loadedVolume->curve.size(), 3);
    ASSERT_EQ(loadedVolume->handles.size(), 2);
    EXPECT_FLOAT_EQ(loadedVolume->curve[1].x, 40.0f);
    EXPECT_FLOAT_EQ(loadedVolume->curve[1].value, -12.0f);
    expectHandle(loadedVolume->handles[0], 0.0f, 0.0f, 40.0f, -12.0f);
    expectHandle(loadedVolume->handles[1], 55.0f, -12.0f, 80.0f, -70.0f);

    const auto* loadedPitch = loaded.dimension(VNX::AttenuationDimension::Pitch);
    ASSERT_NE(loadedPitch, nullptr);
    ASSERT_EQ(loadedPitch->handles.size(), 1);
    expectHandle(loadedPitch->handles[0], 25.0f, 0.0f, 75.0f, 1200.0f);
}

TEST(AttenuationDefSerialization, LegacyShapeKeysAreDroppedUnread)
{
    QJsonArray dims;
    dims.append(QJsonObject{
        {"type", "volume"},
        {"points", QJsonArray{
            QJsonObject{{"distance", 0.0}, {"value", 0.0}, {"shape", "s"}},
            QJsonObject{{"distance", 50.0}, {"value", -20.0}, {"shape", "exponential"}},
            QJsonObject{{"distance", 100.0}, {"value", -96.0}, {"shape", 7}}
        }}
    });
    const QJsonObject json{
        {"name", "LegacyShapes"},
        {"maxDistance", 100.0},
        {"dimensions", dims}
    };

    const VNX::AttenuationSnapshot loaded = VNX::AttenuationSnapshot::fromJson(json);
    const auto* volume = loaded.dimension(VNX::AttenuationDimension::Volume);
    ASSERT_NE(volume, nullptr);
    ASSERT_EQ(volume->curve.size(), 3);
    ASSERT_EQ(volume->handles.size(), 2);
    expectHandle(volume->handles[0], 0.0f, 0.0f, 50.0f, -20.0f);
    expectHandle(volume->handles[1], 50.0f, -20.0f, 100.0f, -96.0f);

    const QJsonArray savedPoints = loaded.toJson().value("dimensions").toArray()[0]
        .toObject().value("points").toArray();
    ASSERT_EQ(savedPoints.size(), 3);
    for (const auto& v : savedPoints)
        EXPECT_FALSE(v.toObject().contains(QStringLiteral("shape")));
}

TEST(AttenuationDefSerialization, MissingHandlesRebuildAsEndpointLines)
{
    QJsonArray dims;
    dims.append(QJsonObject{
        {"type", "volume"},
        {"points", QJsonArray{pointJson(0.0, 0.0), pointJson(60.0, -30.0),
                              pointJson(100.0, -96.0)}}
    });
    const QJsonObject json{
        {"name", "NoHandles"},
        {"maxDistance", 100.0},
        {"dimensions", dims}
    };

    const VNX::AttenuationSnapshot loaded = VNX::AttenuationSnapshot::fromJson(json);
    const auto* volume = loaded.dimension(VNX::AttenuationDimension::Volume);
    ASSERT_NE(volume, nullptr);
    ASSERT_EQ(volume->handles.size(), 2);
    expectHandle(volume->handles[0], 0.0f, 0.0f, 60.0f, -30.0f);
    expectHandle(volume->handles[1], 60.0f, -30.0f, 100.0f, -96.0f);
}

TEST(AttenuationDefSerialization, MispairedHandlesRebuildAsEndpointLines)
{
    QJsonArray dims;
    dims.append(QJsonObject{
        {"type", "volume"},
        {"points", QJsonArray{pointJson(0.0, 0.0), pointJson(50.0, -20.0),
                              pointJson(100.0, -96.0)}},
        {"handles", singleHandleJson(10.0, -5.0, 40.0, -15.0)}
    });
    const QJsonObject json{
        {"name", "Mispaired"},
        {"maxDistance", 100.0},
        {"dimensions", dims}
    };

    const VNX::AttenuationSnapshot loaded = VNX::AttenuationSnapshot::fromJson(json);
    const auto* volume = loaded.dimension(VNX::AttenuationDimension::Volume);
    ASSERT_NE(volume, nullptr);
    ASSERT_EQ(volume->handles.size(), 2);
    expectHandle(volume->handles[0], 0.0f, 0.0f, 50.0f, -20.0f);
    expectHandle(volume->handles[1], 50.0f, -20.0f, 100.0f, -96.0f);
}

TEST(AttenuationDefSerialization, PairedHandlesClampIntoSegmentAndDomain)
{
    QJsonArray dims;
    dims.append(QJsonObject{
        {"type", "volume"},
        {"points", QJsonArray{pointJson(0.0, 0.0), pointJson(100.0, -96.0)}},
        {"handles", singleHandleJson(-20.0, 5.0, 140.0, -200.0)}
    });
    const QJsonObject json{
        {"name", "OutOfBoundsHandles"},
        {"maxDistance", 100.0},
        {"dimensions", dims}
    };

    const VNX::AttenuationSnapshot loaded = VNX::AttenuationSnapshot::fromJson(json);
    const auto* volume = loaded.dimension(VNX::AttenuationDimension::Volume);
    ASSERT_NE(volume, nullptr);
    ASSERT_EQ(volume->handles.size(), 1);
    expectHandle(volume->handles[0], 0.0f, 0.0f, 100.0f, -96.0f);
}

TEST(AttenuationDefSerialization, SinglePointCurveOmitsHandlesKey)
{
    VNX::AttenuationSnapshot def;
    def.name = QStringLiteral("Constant");
    auto& volume = def.ensureDimension(VNX::AttenuationDimension::Volume);
    volume.curve.push_back({30.0f, -12.0f});

    const QJsonObject dimJson =
        def.toJson().value("dimensions").toArray()[0].toObject();
    EXPECT_FALSE(dimJson.contains(QStringLiteral("handles")));

    const VNX::AttenuationSnapshot loaded = VNX::AttenuationSnapshot::fromJson(def.toJson());
    const auto* loadedVolume = loaded.dimension(VNX::AttenuationDimension::Volume);
    ASSERT_NE(loadedVolume, nullptr);
    ASSERT_EQ(loadedVolume->curve.size(), 1);
    EXPECT_TRUE(loadedVolume->handles.isEmpty());
}

TEST(AttenuationDefSerialization, LoadNormalizationIsIdempotent)
{
    QJsonArray dims;
    dims.append(QJsonObject{
        {"type", "volume"},
        {"points", QJsonArray{pointJson(0.0, 5.0), pointJson(100.0, -200.0)}},
        {"handles", singleHandleJson(130.0, 10.0, -30.0, -300.0)}
    });
    const QJsonObject json{
        {"name", "Converge"},
        {"maxDistance", 100.0},
        {"dimensions", dims}
    };

    const VNX::AttenuationSnapshot once = VNX::AttenuationSnapshot::fromJson(json);
    const VNX::AttenuationSnapshot twice = VNX::AttenuationSnapshot::fromJson(once.toJson());

    const auto* volumeOnce = once.dimension(VNX::AttenuationDimension::Volume);
    const auto* volumeTwice = twice.dimension(VNX::AttenuationDimension::Volume);
    ASSERT_NE(volumeOnce, nullptr);
    ASSERT_NE(volumeTwice, nullptr);
    EXPECT_FLOAT_EQ(volumeOnce->curve[0].value, 0.0f);
    EXPECT_FLOAT_EQ(volumeOnce->curve[1].value, -96.0f);
    ASSERT_EQ(volumeOnce->handles.size(), 1);
    ASSERT_EQ(volumeTwice->handles.size(), 1);
    expectHandle(volumeOnce->handles[0], 100.0f, 0.0f, 0.0f, -96.0f);
    expectHandle(volumeTwice->handles[0], 100.0f, 0.0f, 0.0f, -96.0f);
    EXPECT_FLOAT_EQ(volumeTwice->curve[0].value, volumeOnce->curve[0].value);
    EXPECT_FLOAT_EQ(volumeTwice->curve[1].value, volumeOnce->curve[1].value);
}

TEST(AttenuationDefSerialization, MissingDimensionStaysMissingAcrossRoundTrip)
{
    VNX::AttenuationSnapshot def;
    def.name = QStringLiteral("VolumeOnly");
    auto& volume = def.ensureDimension(VNX::AttenuationDimension::Volume);
    volume.curve.push_back({0.0f, 0.0f});
    volume.curve.push_back({100.0f, -96.0f});

    const QJsonObject json = def.toJson();
    ASSERT_TRUE(json.contains(QStringLiteral("dimensions")));
    EXPECT_EQ(json.value(QStringLiteral("dimensions")).toArray().size(), 1);

    const VNX::AttenuationSnapshot loaded = VNX::AttenuationSnapshot::fromJson(json);
    EXPECT_EQ(loaded.dimensions.size(), 1);
    EXPECT_NE(loaded.dimension(VNX::AttenuationDimension::Volume), nullptr);
    EXPECT_EQ(loaded.dimension(VNX::AttenuationDimension::Pitch), nullptr);
}

TEST(AttenuationDefSerialization, LegacyScalingKeyIsIgnored)
{
    QJsonArray dims;
    dims.append(QJsonObject{
        {"type", "volume"},
        {"scaling", 1},
        {"points", QJsonArray{pointJson(0.0, 0.0), pointJson(100.0, -200.0)}}
    });
    const QJsonObject json{
        {"name", "Legacy"},
        {"maxDistance", 100.0},
        {"dimensions", dims}
    };

    const VNX::AttenuationSnapshot loaded = VNX::AttenuationSnapshot::fromJson(json);
    ASSERT_EQ(loaded.dimensions.size(), 1);
    const auto* volume = loaded.dimension(VNX::AttenuationDimension::Volume);
    ASSERT_NE(volume, nullptr);
    ASSERT_EQ(volume->curve.size(), 2);
    EXPECT_FLOAT_EQ(volume->curve[1].value, -96.0f);

    const QJsonArray savedDims = loaded.toJson().value("dimensions").toArray();
    ASSERT_EQ(savedDims.size(), 1);
    EXPECT_FALSE(savedDims[0].toObject().contains(QStringLiteral("scaling")));
}

TEST(AttenuationDefSerialization, LoadClampsValuesToDimensionRange)
{
    QJsonArray dims;
    dims.append(QJsonObject{
        {"type", "volume"},
        {"points", QJsonArray{pointJson(0.0, 5.0), pointJson(100.0, -200.0)}}
    });
    dims.append(QJsonObject{
        {"type", "pitch"},
        {"points", QJsonArray{pointJson(0.0, -1500.0), pointJson(100.0, 1500.0)}}
    });
    const QJsonObject json{
        {"name", "OutOfRange"},
        {"maxDistance", 100.0},
        {"dimensions", dims}
    };

    const VNX::AttenuationSnapshot loaded = VNX::AttenuationSnapshot::fromJson(json);
    const auto* volume = loaded.dimension(VNX::AttenuationDimension::Volume);
    ASSERT_NE(volume, nullptr);
    ASSERT_EQ(volume->curve.size(), 2);
    EXPECT_FLOAT_EQ(volume->curve[0].value, 0.0f);
    EXPECT_FLOAT_EQ(volume->curve[1].value, -96.0f);

    const auto* pitch = loaded.dimension(VNX::AttenuationDimension::Pitch);
    ASSERT_NE(pitch, nullptr);
    ASSERT_EQ(pitch->curve.size(), 2);
    EXPECT_FLOAT_EQ(pitch->curve[0].value, -1200.0f);
    EXPECT_FLOAT_EQ(pitch->curve[1].value, 1200.0f);

    const VNX::AttenuationSnapshot reloaded = VNX::AttenuationSnapshot::fromJson(loaded.toJson());
    const auto* reloadedVolume = reloaded.dimension(VNX::AttenuationDimension::Volume);
    ASSERT_NE(reloadedVolume, nullptr);
    EXPECT_FLOAT_EQ(reloadedVolume->curve[1].value, -96.0f);
}

TEST(AttenuationDefSerialization, DuplicateAndUnknownDimensionTypesAreDropped)
{
    QJsonArray dims;
    dims.append(QJsonObject{
        {"type", "volume"},
        {"points", QJsonArray{pointJson(0.0, 0.0)}}
    });
    dims.append(QJsonObject{
        {"type", "volume"},
        {"points", QJsonArray{pointJson(5.0, -10.0)}}
    });
    dims.append(QJsonObject{
        {"type", "lowpass"},
        {"points", QJsonArray{}}
    });
    const QJsonObject json{
        {"name", "Dupes"},
        {"maxDistance", 50.0},
        {"dimensions", dims}
    };

    const VNX::AttenuationSnapshot loaded = VNX::AttenuationSnapshot::fromJson(json);
    ASSERT_EQ(loaded.dimensions.size(), 1);
    const auto* volume = loaded.dimension(VNX::AttenuationDimension::Volume);
    ASSERT_NE(volume, nullptr);
    ASSERT_EQ(volume->curve.size(), 1);
    EXPECT_FLOAT_EQ(volume->curve[0].x, 0.0f);
}

TEST(AttenuationDefSerialization, EnsureDimensionIsSingleton)
{
    VNX::AttenuationSnapshot def;
    auto& pitch = def.ensureDimension(VNX::AttenuationDimension::Pitch);
    pitch.curve.push_back({0.0f, 100.0f});

    auto& again = def.ensureDimension(VNX::AttenuationDimension::Pitch);
    EXPECT_EQ(def.dimensions.size(), 1);
    EXPECT_EQ(again.curve.size(), 1);

    def.ensureDimension(VNX::AttenuationDimension::Volume);
    EXPECT_EQ(def.dimensions.size(), 2);

    EXPECT_TRUE(def.removeDimension(VNX::AttenuationDimension::Pitch));
    EXPECT_EQ(def.dimension(VNX::AttenuationDimension::Pitch), nullptr);
    EXPECT_FALSE(def.removeDimension(VNX::AttenuationDimension::Pitch));
    EXPECT_EQ(def.dimensions.size(), 1);
}

TEST(AttenuationComponentSerialization, OwnsBusinessFieldsAndUsesProjectItemIdentity)
{
    VNX::ProjectItem item(QStringLiteral("Attenuation"), VNX::ProjectItem::Type::Attenuation);
    item.guid = QUuid::createUuid();
    item.attachDefaultComponents();
    auto* component = item.comp<VNX::VAttenuation>();
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(component->hostItem(), &item);
    const QJsonArray rows{QJsonObject{
        {"type", "volume"},
        {"points", QJsonArray{pointJson(0.0, 0.0), pointJson(120.0, -96.0)}},
        {"handles", singleHandleJson(30.0, -10.0, 100.0, -60.0)}}};
    ASSERT_TRUE(component->setField("dimensions", rows.toVariantList()));
    ASSERT_TRUE(component->setField("maxDistance", 120.0));
    EXPECT_EQ(QJsonValue::fromVariant(component->getField("dimensions")).toArray(), rows);
    EXPECT_EQ(component->getField("maxDistance").toFloat(), 120.0f);
    const auto body = component->toJson();
    EXPECT_EQ(body.keys(), (QStringList{"dimensions", "maxDistance"}));
    const auto originalGuid = item.guid;
    item.name = QStringLiteral("Renamed");
    EXPECT_EQ(component->snapshot().name, item.name);
    EXPECT_EQ(component->snapshot().guid, originalGuid);
    EXPECT_EQ(component->toJson(), body);

    VNX::ProjectSerializer serializer;
    const auto json = serializer.projectItemToJson(item);
    VNX::ProjectItem restored;
    ASSERT_TRUE(serializer.projectItemFromJson(json, restored)) << serializer.lastError().toStdString();
    ASSERT_NE(restored.comp<VNX::VAttenuation>(), nullptr);
    EXPECT_EQ(restored.comp<VNX::VAttenuation>()->toJson(), body);
    EXPECT_EQ(restored.comp<VNX::VAttenuation>()->snapshot().name, restored.name);
    EXPECT_EQ(restored.comp<VNX::VAttenuation>()->snapshot().guid, restored.guid);
}

TEST(AttenuationComponentSerialization, InvalidLoadAndFieldEditPreserveExistingCurves)
{
    VNX::VAttenuation component;
    const QJsonObject row{{"type", "volume"},
        {"points", QJsonArray{pointJson(0.0, 0.0), pointJson(100.0, -96.0)}}};
    ASSERT_TRUE(component.setDimensions(QJsonArray{row}));
    const auto before = component.toJson();
    EXPECT_FALSE(component.setField("dimensions", QStringLiteral("invalid")));
    EXPECT_FALSE(component.setDimensions(QJsonArray{row, row}));
    EXPECT_FALSE(component.setField("maxDistance", -1.0));
    EXPECT_EQ(component.toJson(), before);
    EXPECT_FALSE(component.fromJson(QJsonObject{{"maxDistance", 250.0},
                                               {"dimensions", QJsonArray{row, row}}}));
    EXPECT_EQ(component.toJson(), before);
}
