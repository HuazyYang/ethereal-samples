#ifndef SRC_WWS_CUSTOMMEMORY_H
#define SRC_WWS_CUSTOMMEMORY_H
#include "Types.h"

extern void (*NVSDK_aligned_free)(void *);
extern void *(*NVSDK_aligned_malloc)(size_t, size_t);
extern void (*NVSDK_free)(void *);
extern void *(*NVSDK_malloc)(size_t);

void internalFree(void*);
void* internalMalloc(size_t);
void resetMemoryManagementCallbacksToDefaults();

#endif /* SRC_WWS_CUSTOMMEMORY_H */
