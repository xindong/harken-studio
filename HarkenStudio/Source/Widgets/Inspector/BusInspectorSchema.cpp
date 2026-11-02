
#include "BusInspectorSchema.h"
#include "SendsFieldSchema.h"
#include "ProjectItemInspectorRows.h"
#include "Core/Components/VBus.h"
#include "Core/Components/VMixNode.h"
#include "Core/ProjectManager.h"
#include "Shared/SchemaFieldPick.h"

namespace VNX {

namespace {

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
PropertyDescriptor inputsDescriptor()
{
    return {
        .field="inputs", .ownerComponent=ComponentType::Bus,
        .type=PropType::List, .label="Inputs", .section=PropertySection::Input,
        .flags={.readOnly=true, .serialize=false, .settableByName=false},
        .elementSchema={
            { .field="source", .type=PropType::ObjectRef, .label="Source" },
            { .field="level", .type=PropType::Float, .label="Volume", .unit=" dB",
              .min=-96.0, .max=24.0, .decimals=1 },
        },
        .listPresentation=ListPresentation::Display,
        .get=[](const IComponent* c) -> QVariant {
            const ProjectItem* host = c ? c->hostItem() : nullptr;
            if (!host) return {};
            QVariantList out;
            for (const auto& input : ProjectManager::instance().collectMainInputs(host->objectID))
            {
                out.push_back(QVariantMap{
                    { QStringLiteral("source"), input.sourceID },
                    { QStringLiteral("level"),  input.levelDb } });
            }
            return QVariant(out);
        },
    };
}

PropertyDescriptor receivesDescriptor()
{
    return {
        .field="receives", .ownerComponent=ComponentType::Bus,
        .type=PropType::List, .label="Receives", .section=PropertySection::Receive,
        .flags={.readOnly=true, .serialize=false, .settableByName=false},
        .elementSchema={
            { .field="source", .type=PropType::ObjectRef, .label="Source" },
            { .field="level", .type=PropType::Float, .label="Volume", .unit=" dB",
              .min=-96.0, .max=24.0, .decimals=1 },
        },
        .listPresentation=ListPresentation::Display,
        .get=[](const IComponent* c) -> QVariant {
            const ProjectItem* host = c ? c->hostItem() : nullptr;
            if (!host) return {};
            QVariantList out;
            for (const auto& in : ProjectManager::instance().collectIncomingSends(host->objectID))
                out.push_back(QVariantMap{
                    { QStringLiteral("source"), in.fromBusID },
                    { QStringLiteral("level"),  VMixNode::linearToDb(in.level) } });
            return QVariant(out);
        },
    };
}
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}

const PropertySchema& busInspectorSchema()
{
    static const PropertySchema s = [] {
        struct Entry { const PropertySchema& src; QLatin1String field; };
        const Entry entries[] = {
            { VMixNode::schema(), QLatin1String("volume") },
            { VMixNode::schema(), QLatin1String("pitch") },
            { VMixNode::schema(), QLatin1String("filter") },
            { VBus::schema(),     QLatin1String("pan") },
            { VMixNode::schema(), QLatin1String("outputBusId") },
            { VMixNode::schema(), QLatin1String("outputBusVolume") },
        };

        PropertySchema out;
        for (const auto& e : entries)
            if (const auto* d = pickSchemaField(e.src, e.field))
                out.push_back(*d);
        for (auto& d : out)
            if (d.field == QLatin1String("outputBusId"))
                d.flags.readOnly = true;
        out.push_back(inputsDescriptor());
        out.push_back(sendsInspectorDescriptor());
        out.push_back(receivesDescriptor());
        out.push_back(notesInspectorDescriptor());
        return out;
    }();
    return s;
}

}
