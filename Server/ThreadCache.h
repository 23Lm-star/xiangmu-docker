#pragma once

#include"Common.h"

class ThreadCache
{
public:
	void* Allocate(size_t size);
	void Deallocate(void* ptr, size_t size);
	void* FetchFromCentralCache(size_t index, size_t size);
	void ListTooLong(FreeList& list, size_t size);

private:
	FreeList _FreeLists[MaxFreeLists];
};

inline ThreadCache*& GetTLSThreadCache()
{
#ifdef _WIN32
	__declspec(thread) static ThreadCache* pTLSThreadCache = nullptr;
#else
	thread_local static ThreadCache* pTLSThreadCache = nullptr;
#endif
	return pTLSThreadCache;
}