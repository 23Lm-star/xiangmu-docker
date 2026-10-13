#pragma once

#include <stdlib.h>
#include <atomic>

namespace MemAlloc {

extern std::atomic<bool> s_poolEnabled;
extern std::atomic<bool> s_poolAvailable;

inline void enablePool(bool enable) {
    s_poolEnabled.store(enable);
}

inline bool isPoolEnabled() {
    return s_poolEnabled.load() && s_poolAvailable.load();
}

inline void setPoolAvailable(bool available) {
    s_poolAvailable.store(available);
}

void* allocate(size_t size);
void deallocate(void* ptr);

}

#define memalloc(size) MemAlloc::allocate(size)
#define memfree(ptr) MemAlloc::deallocate(ptr)