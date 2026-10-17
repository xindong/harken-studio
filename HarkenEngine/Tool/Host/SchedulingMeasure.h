// Windows real-device callback scheduling measurement for harken_host.
#ifndef HARKEN_HOST_SCHEDULING_MEASURE_H
#define HARKEN_HOST_SCHEDULING_MEASURE_H

namespace harkenhost {

void PrintSchedulingMeasureUsage();
int RunSchedulingMeasure(int argc, char** argv);

} // namespace harkenhost

#endif // HARKEN_HOST_SCHEDULING_MEASURE_H
