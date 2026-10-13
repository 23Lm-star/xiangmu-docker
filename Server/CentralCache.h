#pragma once
#include"Common.h"
//CentralCache（中心缓存）
//设计成单例模式(饿汉模式)
class CentralCache{
public:
	static CentralCache* GetInstance(){
		return &_CenIns;
	}

	// 在CentralCage指定的桶里找span给ThreadCache
	span* GetOneSpan(SpanList& list, size_t size);

	// 从CentralCache获取一定数量的内存给ThreadCache
	size_t FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size);

	// 将一定数量的对象释放到span
	void ReleaseListToSpans(void* start, size_t size);

private:
	SpanList _SpanList[MaxFreeLists];
private:
	CentralCache() 
	{}
	CentralCache(const CentralCache&) = delete;

	static CentralCache _CenIns;
};