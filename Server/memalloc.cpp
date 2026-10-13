#include "memalloc.h"
#include "ConCurrentAlloc.h"

namespace MemAlloc {

std::atomic<bool> s_poolEnabled(true);
std::atomic<bool> s_poolAvailable(true);

struct MemHeader {
    bool fromPool;
    char padding[7];
};

void* allocate(size_t size) {
    if (size == 0) {
        return nullptr;
    }

    size_t totalSize = size + sizeof(MemHeader);

    if (!isPoolEnabled()) {
        MemHeader* header = (MemHeader*)malloc(totalSize);
        if (header != nullptr) {
            header->fromPool = false;
            return (void*)(header + 1);
        }
        return nullptr;
    }

    MemHeader* header = nullptr;
    try {
        header = (MemHeader*)ConCurrentAlloc(totalSize);
    } catch (...) {
        setPoolAvailable(false);
    }

    if (header == nullptr) {
        setPoolAvailable(false);
        header = (MemHeader*)malloc(totalSize);
        if (header != nullptr) {
            header->fromPool = false;
            return (void*)(header + 1);
        }
        return nullptr;
    }

    header->fromPool = true;
    return (void*)(header + 1);
}

void deallocate(void* ptr) {
    if (ptr == nullptr) {
        return;
    }

    MemHeader* header = (MemHeader*)ptr - 1;

    if (header->fromPool) {
        try {
            ConCurrentFree(header);
            return;
        } catch (...) {
        }
    }

    free(header);
}

}