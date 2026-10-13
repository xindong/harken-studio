#include <harken/ObjectId.h>

_Static_assert(sizeof(VnObjectId) == 16, "Object identity must preserve the full UUID");

VnObjectId ObjectIdRoundTripC(VnObjectId value)
{
    return value;
}
