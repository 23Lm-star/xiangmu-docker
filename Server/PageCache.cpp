#include"PageCache.h"

PageCache PageCache::_PCIns;



/// <summary>
/// 向PageCache申请一个K页大小的span 
/// </summary>
/// <param name="k">所需要的页数</param>
/// <returns>包装好的span</returns>
span* PageCache::NewSpan(size_t k){
	//如果申请的页数超过了PageCache的管理最大页数
	//assert(k > 0 && k < MaxPage);
	if (k > MaxPage - 1){
		//直接去系统申请内存
		void* ptr= SystemAlloc(k);
		//span* Span = new span;
		//从span对象池里面New出来一个span对象
		span* Span = _pool.New();
		//把地址切换为页号(页号 为 地址/2^13)
		Span->Page_Adder = (Page_t)ptr >> PageShift;
		Span->_n = k;//记录Span占了多少页

		//要进行记录(后面回收时能找到对应的span)
		//_PageToSpan[Span->Page_Adder] = Span;
		_PageToSpan.set(Span->Page_Adder, Span);
		return Span;
	}

	//如果占有K页大小的span存在
	if (!_PageList[k].Empty()){
		//分配一个span出来
		span* NewSpan = _PageList[k].EraseFront();

		//统计分出去的页号与span*的映射：为了PageCache回收能快速找到对应的span
		//这里不进行统计大对象申请释放时没存就会报错(BUG)
		for (Page_t i = NewSpan->Page_Adder; i < NewSpan->Page_Adder + k; i++){
			//_PageToSpan[i] = NewSpan;
			_PageToSpan.set(i, NewSpan);
		}
		return NewSpan;
	}
	//往下找大span进行切分
	for (size_t i = k + 1; i < MaxPage; i++)
	{
		if (!_PageList[i].Empty())
		{
			//BigSpan要切k页NewSpan带出来给CentralCache
			//1、先将首个span从spanlist中切下来
			span* BigSpan = _PageList[i].EraseFront();
			//2、从span对象池里面分配一个span对象用来存储BigSpan的K页内存
			//span* NewSpan = new span;
			span* NewSpan = _pool.New();
			//3、将BigSpan的K页内存移交到NewSpan里面
			NewSpan->Page_Adder = BigSpan->Page_Adder;
			NewSpan->_n = k;
			BigSpan->Page_Adder += k;
			BigSpan->_n -= k;
			//3、BigSpan剩下的部分根据它剩下的页数来插在对应的桶上
			_PageList[BigSpan->_n].InsertFront(BigSpan);

			//4、统计分出去的页号与span*的映射
			for (Page_t i = NewSpan->Page_Adder; i < NewSpan->Page_Adder + k; i++){
				//_PageToSpan[i] = NewSpan;
				_PageToSpan.set(i, NewSpan);
			}
			//5、将BigSpan内部剩下的页的 左右 进行和span的映射:为了回收回来的span进行合并
			//_PageToSpan[BigSpan->Page_Adder] = BigSpan;
			//注意存右位置要减1
			//_PageToSpan[BigSpan->Page_Adder + BigSpan->_n - 1] = BigSpan;
			_PageToSpan.set(BigSpan->Page_Adder, BigSpan);
			_PageToSpan.set(BigSpan->Page_Adder + BigSpan->_n - 1, BigSpan);
			return NewSpan;//返回分配的K页占用的Span
		}
	}

	//找不到要向堆申请(分配可以分配的最大页数)
	void* ptr = SystemAlloc(MaxPage - 1);
	//span* BigSpan = new span;
	span* BigSpan = _pool.New();
	BigSpan->Page_Adder = (Page_t)ptr >> PageShift;
	BigSpan->_n = MaxPage - 1;//不是k页！
	_PageList[MaxPage - 1].InsertFront(BigSpan);
	//递归复用前面的逻辑就不用再写一遍
	return NewSpan(k);//在新分配了MaxPage页span的情况下，再次进行k页span的分配
}


/// <summary>
///调用该函数就说明你: ThreadCache回收内存找Span / 从系统申请的内存进行释放 
/// </summary>
/// <param name="obj">需要进行回收的内存地址</param>
/// <returns> 回收的内存 对应的 span* </returns>
span* PageCache::MapObjectToSpan(void* obj){
	//.get()方法取出的是值的地址
	span* FindSpan = (span*)_PageToSpan.get((Page_t)obj >> PageShift);
	assert(FindSpan != nullptr);
	return FindSpan;
}

//回收内存到PageCache中进行合并操作
void PageCache::ReleaseSpanToPageCache(span* Span){
	assert(Span!=nullptr);
	//如果要回收的span的所占页数大小超过了PageCache的MaxPage范围-->直接回收给系统
	if (Span->_n > MaxPage - 1){
		//系统申请的用系统调用释放(将Span的页号切换成首地址)
		SystemFree((void*)(Span->Page_Adder << PageShift), Span->_n);
		//delete Span;回收Span管理对象            
  		_pool.Delete(Span); 
		return;
	}

	//回收回来的span要与没有被分配给CentralCache/ThreadCache的span进行合并，解决内存碎片问题
	//左合并
	while (1){
		//计算span的存储首页的前一页的span*(左span)
		//auto tmp = _PageToSpan.find(Span->Page_Adder - 1);
		span* tmp = (span*)_PageToSpan.get(Span->Page_Adder - 1);
		//没有span不合并
		if (tmp == nullptr){
			break;
		}
		//span在被CentralCache使用不合并
		if (tmp->_IsUse == true){
			break;
		}
		//合成的span太大不合并
		if (Span->_n + tmp->_n > MaxPage - 1){
			break;
		}
		//span变成大span
		//span ---> tmp+span
		Span->Page_Adder = tmp->Page_Adder;
		Span->_n += tmp->_n;
		//把prev从PageCache桶中删除后再delete
		_PageList[tmp->_n].Erase(tmp);
		_pool.Delete(tmp);	
	}
	//右合并
	while (1){
		//寻找Span的右页的下一个页对应的span
		span* tmp = (span*)_PageToSpan.get(Span->Page_Adder + Span->_n);
		//没有span不合并
		if (tmp == nullptr){
			break;
		}
		//span在被CentralCache使用不合并
		if (tmp->_IsUse == true){
			break;
		}
		//合成的span太大不合并
		if (Span->_n + tmp->_n > MaxPage - 1){
			break;
		}
		//Span变成大Span
		Span->_n += tmp->_n;
		_PageList[tmp->_n].Erase(tmp);
		_pool.Delete(tmp);
	}
	//执行大Span的相关操作
	Span->_IsUse = false;
	//将合并之后的span插入到它对应的页数桶当中去准备分配
	_PageList[Span->_n].InsertFront(Span);
	//设计好span的左右页号到Span的映射
	_PageToSpan.set(Span->Page_Adder, Span);
	_PageToSpan.set(Span->Page_Adder + Span->_n - 1, Span);
}

