#pragma once

#include<iostream>
#include<vector>
#include<unordered_map>

#include<time.h>
#include<assert.h>

#include<thread>
#include<mutex>

#include<algorithm>
#include<atomic>

#include"ObjectPool.h"

using std::cout;
using std::endl;

static const size_t MaxFreeLists = 208;//���������������(������ֵ)
static const size_t MaxBytes = 256 * 1024;//(��������ֽ���)
static const size_t MaxPage = 129;//span�������ҳ��-1
static const size_t PageShift = 13;//�趨һҳ��СΪ2^13(8K)

//����ҳ����������Page_t
//_WIN64ֻ��64λ�����_WIN32���ж���
#ifdef _WIN64 //���л���64λ
	typedef unsigned long long Page_t;
#elif _WIN32 //���л���32λ
	typedef size_t Page_t;
#else 
	typedef size_t Page_t;
#endif

#ifdef _WIN32
#include<Windows.h>
#else 
	#include <sys/mman.h>
	#include <unistd.h>
#endif

//ʹ��ϵͳ��������ռ�(����ҳ����)
	inline static void* SystemAlloc(size_t page)
	{
#ifdef _WIN32
		void* ptr = VirtualAlloc(
			0,       // ��ϵͳ�Զ�ѡ���ַ
			page<<PageShift,          // �����С(�ֽ���)
			MEM_COMMIT | MEM_RESERVE, // �ύ�������ڴ�
			PAGE_READWRITE // �ɶ���д
		);
#else
		//ʹ��Linux��mmap...
		void* ptr = mmap(
			nullptr,					  // ��ϵͳ�Զ�ѡ���ַ
			page * (1 << 13),              // �����С
				PROT_READ | PROT_WRITE,       // �ɶ���д
				MAP_PRIVATE | MAP_ANONYMOUS,  // ˽�������ڴ棨�����ļ�������
				-1,                           // �ļ�������������ӳ����Ϊ -1��
				0                             // ƫ����
				);
#endif
#ifdef _WIN32
		if (ptr == nullptr)
#else
		if (ptr == nullptr || ptr == MAP_FAILED)
#endif
		{
			throw std::bad_alloc();
			exit(-1);
		}
		return ptr;
	}
	//ʹ��ϵͳ�����ͷſռ�
	inline static void SystemFree(void* ptr, size_t page){
#ifdef _WIN32
		VirtualFree(ptr, 0, MEM_RELEASE);
#else
		munmap(ptr, page * (1 << PageShift));
#endif
	}

//��ȡ����ǰ��ַ�� ǰ8���ֽ� ��ָ��
static void*& Next(void* obj){
	return *(void**)obj;
}


//������������
class FreeList{
public:
	//ͷ�������ڴ��
	void Push(void* obj){
		assert(obj);
		//ͷ��
		Next(obj) = _FreeList;
		_FreeList = obj;
		++_size;
	}
	//ͷ��ɾ���ڴ��&&����ɾ�����ڴ��
	void* Pop(){
		assert(_FreeList != nullptr);
		//ͷɾ
		void* obj = _FreeList;
		_FreeList = Next(obj);
		--_size;
		return obj;
	}
	//ͷ������һ���ڴ������
	void PushRange(void* start, void* end, size_t n){
		//ͷ��
		Next(end) = _FreeList;
		_FreeList = start;
		_size += n;
	}
	//ͷ��ɾ��һ���ڴ������
	void PopRange(void*& start, void*& end, size_t n){
		assert(n <= _size);
		//ͷɾ(ɾ������������ǰn���ڵ�)
		start = _FreeList;
		end = _FreeList;
		//end�ƶ��������ڶ����ڵ�(׼ȷ��˵�ǵ�n���ڵ�)
		for (size_t i = 0; i < n - 1; i++)
		{
			end = Next(end);
		}
		//�����һ���ڵ���뵽��Ӧλ�õ�Ͱ����Ϊ�µĿ�������
		_FreeList = Next(end);
		//ʣ��������ڵ�start-end��Ϊɾ���Ľڵ�
		Next(end) = nullptr;
		
		//�����ڵ��ܸ���-n
		_size -= n;
	}
	//�жϿ��������Ƿ�Ϊ��
	bool Empty(){
		return _FreeList == nullptr;
	}
	//��ȡ���������ڴ������������ֵ&+ÿһ�λ��յ��ڴ�����
	size_t& MaxSize(){
		return _MaxSize;
	}
	//��ȡ���������ڴ������
	size_t Size(){
		return _size;
	}

private:
	void* _FreeList=nullptr;
	size_t _MaxSize = 1;//������CentralCacle����ĸ���/���������������ɵ�����ڴ�����
	size_t _size = 0;//��ǰ_FreeList���ӵ��ڴ�����
};

//�ڴ湤���ࣺ
//���������ڴ����,����Ͱ��������
class SizeClass{
public:
	// ��������
	// [1,128]               8byte����			freelist[0,16)
	// [128+1,1024]			 16byte����			freelist[16,72)
	// [1024+1,8*1024]		 128byte����		freelist[72,128)
	// [8*1024+1,64*1024]	 1024byte����		freelist[128,184)
	// [64*1024+1,256*1024]  8*1024byte����		freelist[184,208)
	
	/// <summary>
	///���ݶ����ֽ���align����bytes�ֽڽ����ڴ���� 
	/// </summary>
	/// <param name="bytes">�ֽ���</param>
	/// <param name="align">�����ֽ���</param>
	/// <returns>������bytes�ֽ���</returns>
	static inline size_t _RoundUp(size_t bytes, size_t align){
		// -1��Ϊ����8�ܶԵ�8    ���λ��1���� �����1���0
		return (bytes + align - 1) & (~(align - 1));
	}

	//���������
	static inline size_t RoundUp(size_t bytes){
		if (bytes <= 128){
			return _RoundUp(bytes, 8);
		}
		else if (bytes <= 1024){
			return _RoundUp(bytes, 16);
		}
		else if (bytes <= 8 * 1024){
			return _RoundUp(bytes, 128);
		}
		else if (bytes <= 64 * 1024){
			return _RoundUp(bytes, 1024);
		}
		else if (bytes <= 256 * 1024){
			return _RoundUp(bytes, 8 * 1024);
		}
		else{
			//����256k��ҳ���ж���
			return _RoundUp(bytes, 1 << PageShift);
		}
	}

	//������ + Ͱ�Ķ�����(+8��+16��+128...)->��ϣͰ����
	static inline size_t _Index(size_t bytes, size_t AlignIndex)//��2��ָ������
{
		//        -1Ҳ��Ϊ����8�����λ���ֲ���    ֻʣ���λ     
		return ((bytes + ((size_t)1 << AlignIndex) - 1) >> AlignIndex) - 1;
	}

	//ӳ���ϣͰ���±�
	static inline size_t Index(size_t bytes){
		//�Ż���
		if (bytes <= 128){
			return _Index(bytes, 3);
		}
		else if (bytes <= 1024){
			return _Index(bytes, 4);
		}
		else if (bytes <= 8 * 1024){
			return _Index(bytes, 7);
		}
		else if (bytes <= 64 * 1024){
			return _Index(bytes, 10);
		}
		else if (bytes <= 256 * 1024){
			return _Index(bytes, 13);
		}
		else{
			assert(false);
			return -1;
		}
	}

	// ������CentralCache��ȡ���ٸ�
	static size_t NumMoveSize(size_t size){
		assert(size <= MaxBytes);
		size_t num = MaxBytes / size;
		//�����
		if (num < 2){
			num = 2;
		}
		//С����
		if (num > 512){
			num = 512;
		}
		return num;
	}

	// ����һ����ϵͳ��ȡ����ҳ(1ҳ8KB)
	static size_t NumMovePage(size_t size){
		//����size����Ҫ������ٸ�size��С���ڴ��
		size_t num = NumMoveSize(size);
		//���ݸ��������������ֽ�����Ȼ���ټ���������ҳ�� 
		size_t npage = num * size;
		npage >>= PageShift;

		if (npage == 0)
			npage = 1;
		return npage;
	}
};

//�ڴ��Ļ�����λ������һ��ҳ�������ڴ�
struct span{
	Page_t Page_Adder = 0;//�������ڴ����ʼҳ��(��ַ)
	size_t _n = 0;//ҳ������
	//ǰ��span��ָ��
	span* _prev=nullptr;
	span* _next=nullptr;

	size_t _UseCount = 0;//ThreadCache���˶��ٸ�С���ڴ�
	void* _FreeList = nullptr;//span���ŵ�һ��һ���ڴ����׵�ַ(ҳ�Ŀ�������)

	bool _IsUse = false;//��PageCache�����Ƿ��CentralCache��(span�Ƿ����ڱ�ʹ��)
	//�����ڴ�����
	size_t _FreeKnowSize = 0;
};

//��span��֯����(һ��ѭ������,����_head����ڵ�)
class SpanList{
public:
	SpanList(){//����span��������������span�ڵ�
		//_head = new span;
		_head = _SpanPool.New();
		_head->_next = _head;
		_head->_prev = _head;
	}

	//ͷ����span
	void InsertFront(span* NewPos){
		Insert(begin(), NewPos);//��ͷ��һ��spanǰ����newspan
	}
	//posǰ������span
	void Insert(span* pos, span* NewPos){
		assert(NewPos != nullptr);
		// prev NewPos pos
		span* prev = pos->_prev;

		prev->_next = NewPos;
		NewPos->_prev = prev;
		NewPos->_next = pos;
		pos->_prev = NewPos;
	}

	//ͷɾ��span
	span* EraseFront(){
		//��¼ͷspan
		span* EraseSpan = _head->_next;
		//ɾ��ͷspan��span������
		Erase(EraseSpan);
		//����ͷspan
		return EraseSpan;
	}
	//��span������ɾ��span
	void Erase(span* pos){
		assert(pos != nullptr);
		assert(pos != _head);
		//��¼prev,next,Ȼ���span������ɾ����span
		span* prev = pos->_prev;
		span* next = pos->_next;

		prev->_next = next;
		next->_prev = prev;
	}
	//���ػ�������Ͱ����
	std::mutex& Mutex(){
		return _mtx;
	}
	//���������е�ͷspan
	span* begin(){
		return _head->_next;
	}
	//���������е�βspan(��Ч�ڵ�,Խ��λ��)
	span* end(){
		return _head;
	}
	//�ж�span�����Ƿ�Ϊnullptr(��Чspan����Ϊ0)
	bool Empty(){
		return _head->_next == _head;
	}

private:
	span* _head;
	std::mutex _mtx;//Ͱ�� ÿ��thread����ͬһͰ(ͬһ��Ͱ��¼����һ��spanlist)��Ҫ����

	//span����(�ظ��ˣ�û���õ�)
	static ObjectPool<span> _SpanPool;//staticʹ��ʱ�ȶ���
};