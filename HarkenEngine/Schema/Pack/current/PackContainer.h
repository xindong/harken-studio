#ifndef HARKEN_PACK_CONTAINER_H
#define HARKEN_PACK_CONTAINER_H

#include "PackFile.h"

typedef struct HkPackContainerChild {
    HkPackObjectId playableId;
    uint32_t weight;
    uint32_t matchValue;
    uint32_t delayMs;
    uint32_t delayJitterMs;
} HkPackContainerChild;
HK_PACK_SIZE(HkPackContainerChild, 32);
HK_PACK_OFFSET(HkPackContainerChild, playableId, 0);
HK_PACK_OFFSET(HkPackContainerChild, weight, 16);
HK_PACK_OFFSET(HkPackContainerChild, matchValue, 20);
HK_PACK_OFFSET(HkPackContainerChild, delayMs, 24);
HK_PACK_OFFSET(HkPackContainerChild, delayJitterMs, 28);

typedef struct HkPackContainerHeader {
    HkPackObjectId containerId;
    uint32_t selectionOrder;
    uint32_t avoidRepeatWindow;
    HkPackObjectId selectionVariableId;
    uint32_t defaultIndex;
    uint32_t childCount;
} HkPackContainerHeader;
HK_PACK_SIZE(HkPackContainerHeader, 48);
HK_PACK_OFFSET(HkPackContainerHeader, containerId, 0);
HK_PACK_OFFSET(HkPackContainerHeader, selectionOrder, 16);
HK_PACK_OFFSET(HkPackContainerHeader, avoidRepeatWindow, 20);
HK_PACK_OFFSET(HkPackContainerHeader, selectionVariableId, 24);
HK_PACK_OFFSET(HkPackContainerHeader, defaultIndex, 40);
HK_PACK_OFFSET(HkPackContainerHeader, childCount, 44);

HK_PACK_TABLE HkPackStreamStep hkPackStreamContainerPayload[] = {
    HK_PACK_STREAM_RECORD(HkPackContainerHeader),
    HK_PACK_STREAM_RECORD_EACH(HkPackContainerChild, HkPackContainerHeader, childCount),
};

#endif
