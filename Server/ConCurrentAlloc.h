#pragma once 

#include "Common.h"
#include "ThreadCache.h"
#include "PageCache.h"
#include "ObjectPool.h"

void* ConCurrentAlloc(size_t size);
void ConCurrentFree(void* ptr);