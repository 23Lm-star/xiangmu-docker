#include"CentralCache.h"
#include"PageCache.h"

//静态成员定义在.cpp中
CentralCache CentralCache::_CenIns;
ObjectPool<span> SpanList::_SpanPool;
//将中心缓存中寻找batchNum个内存块对齐大小为size的内存块链表
size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size)
{
	assert(batchNum > 0);
	//获取到对应桶索引
	size_t index = SizeClass::Index(size);
	//从CentralCache指定桶中申请span要加桶锁
	_SpanList[index].Mutex().lock();
	//在指定的桶里找空闲的span
	span* GetSpan = GetOneSpan(_SpanList[index], size);
	assert(GetSpan);
	assert(GetSpan->_FreeList);

	//span下的小块内存就根据实际需要给ThreadCache(可能够，可能不够)
	//通过start与end给出去
	start = GetSpan->_FreeList;
	end = GetSpan->_FreeList;
	size_t ActualNum = 1;
	size_t i = 0;
	//要考虑内存不够申请的情况
	//记录明确可以申请的内存块的个数
	while (i < batchNum - 1 && Next(end) != nullptr)
	{
		end = Next(end);
		i++;
		ActualNum++;
	}
	//,end尾巴置空，剩余的空闲链表作为新的空闲链表归还给GetSpan
	GetSpan->_FreeList = Next(end);
	Next(end) = nullptr;

	//GetSpan记录从里面拿了多少块内存给ThreadCache
	GetSpan->_UseCount += ActualNum;

	_SpanList[index].Mutex().unlock();//记得解锁

	return ActualNum;//返回实际分配了多少块内存块
}

span* CentralCache::GetOneSpan(SpanList& list, size_t size)
{
	span* it = list.begin();
	while (it != list.end())
	{
		if (it->_FreeList != nullptr)
		{
			return it;
		}
		else
		{
			it = it->_next;
		}
	}

	//申请之前把桶锁解了：方便后面线程将释放回来的span不会阻塞（我要到PageCache去申请内存啦）
	list.Mutex().unlock();

	//找不到span就要到PageCache里申请(记得要进行加锁)
	PageCache::GetInstance()->GetMutex().lock();
	span* Span = PageCache::GetInstance()->NewSpan(SizeClass::NumMovePage(size));
	Span->_FreeKnowSize = size;//释放时知道对齐数
	Span->_IsUse = true;//BUG(从NewSpan出来的Span有多种情况)(得在这里标记)

	PageCache::GetInstance()->GetMutex().unlock();

	char* start = (char*)(Span->Page_Adder << PageShift);//换算成char*方便加减
	char* end = start + (Span->_n << PageShift);//一页4K

	//申请到的span里有大块内存的信息,将它进行切分成小块内存后挂在span上
	//span* SpanList = list.begin();
	//尾插之前先给头节点
	Span->_FreeList = start;//现将这块内存的开始地址存入空闲链表
	void* tail = start;//用于记录上一个小内存块的开始位置
	start += size;
	while (start < end)
	{
		Next(tail) = start;
		tail = start;
		start += size;
	}
	Next(tail) = nullptr;

	//将NewSpan头插到CentralCache桶中(这步就是线程重新竞争桶锁了)
	list.Mutex().lock();
	list.InsertFront(Span);

	return Span;
}

/// <summary>
///从ThreadCache中回收来的内存挂到CentralCache对应的桶的span上
/// </summary>
/// <param name="start">回收的内存块链表<we/param>
/// <param name="size">每一块内存块的对齐大小</param>
void CentralCache::ReleaseListToSpans(void* start, size_t size)
{
	assert(start != nullptr);//保证回收的内存块链表不是空的
	//桶索引
	size_t index = SizeClass::Index(size);
	//回收到CentralCache指定的桶中之前先加桶锁
	_SpanList[index].Mutex().lock();

	while (start != nullptr)
	{
		void* next = Next(start);
		//每块内存不一定是在同一个Span(不能放在外面)
		span* Span = PageCache::GetInstance()->MapObjectToSpan(start);
		//头插到Span中(头插插晕了！)(回收的 span->_FreeList 一定为空)(它实现的是无头节点的链表)
		//start = Next(Span->_FreeList);//这里Span->_FreeList为nullptr?
		//Span->_FreeList = start;
		
		//利用头插法，插入到Span的空闲链表中
		Next(start) = Span->_FreeList;
		Span->_FreeList = start;

		Span->_UseCount--;//Span里面正在被线程使用的内存块个数-1
		//Span记录的UseCount为0说明是ThreadCache把内存全部回来了
		if (Span->_UseCount == 0)
		{
			//Span回收前从桶中删除,归还Span给页缓存
			//清除空闲链表和前后指针
			_SpanList[index].Erase(Span);
			Span->_FreeList = nullptr;
			Span->_next = nullptr;
			Span->_prev = nullptr;

			//还给PageCache之前解桶锁，防止死锁
			_SpanList[index].Mutex().unlock();
			//加PageCache的大锁
			PageCache::GetInstance()->GetMutex().lock();
			//将Span还给页缓存
			PageCache::GetInstance()->ReleaseSpanToPageCache(Span);
			//释放PageCache的大锁
			PageCache::GetInstance()->GetMutex().unlock();
			//span加桶锁
			_SpanList[index].Mutex().lock();
		}

		start = next;
	}

	_SpanList[index].Mutex().unlock();//解桶锁
}