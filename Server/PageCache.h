#pragma once

#include "Common.h"
#include "ObjectPool.h"
#include "RadixTreeHash.h"
//PageCache（页缓存）
//单例模式(饿汉模式)
class PageCache{
public:
	static PageCache* GetInstance(){
		return &_PCIns;
	}
	//返回互斥量
	std::mutex& GetMutex(){
		return _mtx;
	}

	// 获取一个K页的span
	span* NewSpan(size_t k);

	// 获取从内存地址到span的映射
	span* MapObjectToSpan(void* obj);

	// 释放空闲span回到Pagecache，并合并相邻的span
	void ReleaseSpanToPageCache(span* span);

private:
	SpanList _PageList[MaxPage];
	std::mutex _mtx;//申请内存时加锁

	//记录页号到span的映射
	//通过任意内存地址，能快速找到它属于哪个 Span
	//std::unordered_map<Page_t, span*> _PageToSpan;//建立页号与span*的映射
#ifdef _WIN64
	TCMalloc_PageMap3<64 - PageShift> _PageToSpan;
#elif _WIN32
	TCMalloc_PageMap1<32 - PageShift> _PageToSpan;
#else
	TCMalloc_PageMap3<64 - PageShift> _PageToSpan;
#endif//..
	ObjectPool<span> _pool;
private:
	PageCache() {}
	PageCache(const PageCache&) = delete;
	static PageCache _PCIns;//全局只有一个
};