#pragma once

#include"Common.h"

//提供函数声明不然就看不到函数
inline static void* SystemAlloc(size_t page);
static void*& Next(void* obj);


//对象池,用于管理span等对象的创建和回收(类比于内存池的功能)
template<class T>
class ObjectPool{
public:
	//申请对象
	T* New(){
		T* object = nullptr;
		//free的对象可以重复利用！
		//如果对象池子里面又可以重复利用的对象
		if (_FreeList){
			//链表的头删操作
			object = (T*)_FreeList;
			_FreeList = Next(_FreeList);
		}
		//如果对象池子为nullptr，没有可以重复利用的对象
		else{
			//可能申请的空间很小不够4/8字节,就不能释放时挂到_FreeLis中
			//但这里如果用了4/8字节的空间存地址，后面我在用这块空间时不就少了4/8字节，会对下面的操作有影响吗？
			//释放时才存地址，所以使用时没影响！
			//记录它分配内存的大小(因为释放的时候需要记录地址，所以至少要分配一个void*大小的内存空间)
			size_t ObjectByte = sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
			//如果下次空间不够用了怎么办？ -> 引入_MemoryByte
			//if (_Memory == nullptr)
			//if (_MemoryByte < sizeof(T))
			//如果剩余内存小于你需要分配的内存大小
			if(_MemoryByte< ObjectByte){
				_MemoryByte = 1024 * 1024;//一次性分配的内存大小
				_Memory = (char*)SystemAlloc(_MemoryByte >> 13);//传入的是分配的页数，获取到的则是虚拟首地址
				//_MemoryByte = 128 * 1024;//申请128KB
				/*_Memory = (char*)malloc(_MemoryByte);
				if (_Memory == nullptr)
				{
					throw std::bad_alloc();
					exit(-1);
				}*/
			}
			//大块内存切出对象的大小
			object = (T*)_Memory;
			_Memory += ObjectByte;
			_MemoryByte -= ObjectByte;
		}
		//开辟的空间需要进行初始化:定位new,非常厉害
		new(object)T;
		return object;
	}

	//回收对象内存
	void Delete(T* object){
		//释放时可能T对象里开辟了空间，释放掉里面管理的动态空间
		object->~T();

		//相当于链表插入：把释放的对象进行头插保留起来
		//关键是怎么让对象保存保存下一个的地址？把前4/8个字节给空出来保存
		//*(int*)object = _FreeList;//这样做如果是64位指针是8字节呢？
		
		//方法1：通过sizeof运算符判断当前指针所占字节数
		//if (sizeof(t*) == 4) *(int*)object = (int)_freelist;
		//else *(long long*)object = (long long)_freelist;
		
		//方法2:void**解引用成void*就解决问题
		Next(object) = _FreeList;
		//释放的对象用_FreeLIst下次New直接用
		_FreeList = object;
	}
private:
	char* _Memory = nullptr;	 //大内存的起始地址
	size_t _MemoryByte = 0;     //当前剩余内存字节数
	void* _FreeList = nullptr;  //释放对象时挂起来的链表头指针

};