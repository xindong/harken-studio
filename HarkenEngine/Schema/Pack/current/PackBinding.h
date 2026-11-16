#ifndef HARKEN_PACK_BINDING_H
#define HARKEN_PACK_BINDING_H

#include "PackCurve.h"
#include "PackFile.h"

typedef struct HkPackBindingAttachment {
    HkPackObjectId bindingId;
    uint32_t scopeKind;
    uint32_t reserved20;
    HkPackObjectId objectId;
} HkPackBindingAttachment;
HK_PACK_SIZE(HkPackBindingAttachment, 40);
HK_PACK_OFFSET(HkPackBindingAttachment, bindingId, 0);
HK_PACK_OFFSET(HkPackBindingAttachment, scopeKind, 16);
HK_PACK_OFFSET(HkPackBindingAttachment, reserved20, 20);
HK_PACK_OFFSET(HkPackBindingAttachment, objectId, 24);

typedef struct HkPackBindingDefinition {
    HkPackObjectId bindingId;
    HkPackObjectId variableId;
    uint32_t targetScopeKind;
    uint32_t targetId;
    uint32_t transitionMs;
    uint32_t reserved44;
    HkPackBindingCurve curve;
} HkPackBindingDefinition;
HK_PACK_SIZE(HkPackBindingDefinition, 228);
HK_PACK_OFFSET(HkPackBindingDefinition, bindingId, 0);
HK_PACK_OFFSET(HkPackBindingDefinition, variableId, 16);
HK_PACK_OFFSET(HkPackBindingDefinition, targetScopeKind, 32);
HK_PACK_OFFSET(HkPackBindingDefinition, targetId, 36);
HK_PACK_OFFSET(HkPackBindingDefinition, transitionMs, 40);
HK_PACK_OFFSET(HkPackBindingDefinition, reserved44, 44);
HK_PACK_OFFSET(HkPackBindingDefinition, curve, 48);

#endif
