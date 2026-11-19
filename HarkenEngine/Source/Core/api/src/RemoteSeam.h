#ifndef HARKEN_API_REMOTE_SEAM_H
#define HARKEN_API_REMOTE_SEAM_H

namespace harken::remote {

#if HARKEN_ENGINE_DEV
void Pump();
void Shutdown(bool engineTerminating = false);
#else
inline void Pump() {}
inline void Shutdown(bool = false) {}
#endif

}

#endif
