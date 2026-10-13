#pragma once
#include <cstring>

#include"Common.h"


// 单层基数树
template <int BITS>
class TCMalloc_PageMap1 
{
private:
	//指针数组长度:页号最大值
	static const int LENGTH = 1 << BITS; //开 2^32 / 2^13 个位置
	void** array_; //指针数组(记录的是span*) 下标(页号)->span*

public:
	typedef uintptr_t Number;
	TCMalloc_PageMap1()
	{
		//直接把空间开好
		size_t size = sizeof(void*) << BITS;
		size_t bytes = SizeClass::RoundUp(size);
		array_ = (void**)SystemAlloc(bytes>>PageShift);
		memset(array_, 0, sizeof(void*) << BITS);
	}

	//按照页号获取
	void* get(Number k) const
	{
		if ((k >> BITS) > 0)//k / 2^BITS 如果页号超过了页号总数，证明当前页号没有对应的内存
		{
			return NULL;
		}
		//返回页号对应的span*
		return array_[k];
	}

	//设置页号:span*的映射
	void set(Number k, void* v) 
	{
		array_[k] = v;
	}
};

// 双层基数树
template <int BITS>
class TCMalloc_PageMap2 {
private:
	//确定好页号的分级表示 root-leaf
	//完整的页号为 root页号+leaf页号
	static const int ROOT_BITS = 5;				   //通过页号的前5个bit表示位置
	static const int ROOT_LENGTH = 1 << ROOT_BITS; //第一层开多少个元素
	static const int LEAF_BITS = BITS - ROOT_BITS; //通过页号的前6~14个bit表示位置
	static const int LEAF_LENGTH = 1 << LEAF_BITS; //第二层开多少个元素 
	
	//叶子页表的类型定义
	struct Leaf {
		void* values[LEAF_LENGTH];
	};
	//root页表——每一个位置存储的是root页号对应的leaf页表
	Leaf* root_[ROOT_LENGTH]; 

public:
	typedef uintptr_t Number;

	explicit TCMalloc_PageMap2() 
	{
		memset(root_, 0, sizeof(root_));
		//直接开空间
		Ensure(0, 1 << BITS);
	}

	//为start页到后面n页 为其开辟存储其span*的页表
	bool Ensure(Number start, size_t n)
	{
		for (Number key = start; key <= start + n - 1;) 
		{
			const Number i1 = key >> LEAF_BITS;//根页表下标
			if (root_[i1] == NULL) {
				//若是对应的叶子页表不存在那么就从对象池里面new一个叶子页表出来
				static ObjectPool<Leaf> LeafPool;
				Leaf* leaf = LeafPool.New();
				memset(leaf, 0, sizeof(*leaf));
				root_[i1] = leaf;
			}
			//跳到下一个叶子页表的第一个叶子页的页号
			key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
		}
		return true;
	}

	//根据页号->span*
	void* get(Number k) const {
		const Number i1 = k >> LEAF_BITS;//取出根页表索引
		const Number i2 = k & (LEAF_LENGTH - 1);//取出叶子页表索引
		//如果当前页号根本不存在||当前根页表的叶子页表没构建
		//证明当前页号->span*的映射根本存储
		if ((k >> BITS) > 0 || root_[i1] == NULL) {
			return NULL;
		}
		//返回对应 根页表索引i1->子页表索引i2->span*
		return root_[i1]->values[i2];
	}

	//设置页号->span*
	void set(Number k, void* v) {
		//获取根页表索引和叶子页表索引
		const Number i1 = k >> LEAF_BITS;
		const Number i2 = k & (LEAF_LENGTH - 1);
		assert(i1 < ROOT_LENGTH);//检查是否超过了根页表下标最大值
		//记录下span*到对应的i1\i2位置
		root_[i1]->values[i2] = v;
	}
};

// 处理64位情况
template <int BITS>
class TCMalloc_PageMap3 {
private:
	//计算前两层和第三层的页表的所占用的bit位数和页号长度
	static const int INTERIOR_BITS = (BITS + 2) / 3;         // 第一层和第二层的页号的比特位个数
	static const int INTERIOR_LENGTH = 1 << INTERIOR_BITS;   // 第一层和第二层开多少元素
	static const int LEAF_BITS = BITS - 2 * INTERIOR_BITS;	 // 第三层的页号的比特位个数
	static const int LEAF_LENGTH = 1 << LEAF_BITS;			 // 第三层开多少元素
	
	//第一,二层结构（一个统一的页表结构：每一个位置指向下一个页表）
	struct Node {
		Node* ptrs[INTERIOR_LENGTH];
	};
	//第三层结构(叶子页表结构:每一个位置存储span*)
	struct Leaf {
		void* values[LEAF_LENGTH];
	};
	//指针实现
	//三级页表（就是比双层基数树多了一层,唯一的问题就是到叶子页表时候需要转换成leaf类型）
	Node* root_;

	Node* NewNode() {
		static ObjectPool<Node> ObjectNode;
		Node* result = ObjectNode.New();

		if (result != NULL) {
			memset(result, 0, sizeof(*result));
		}
		return result;
	}
public:
	typedef uintptr_t Number;
	//初始化先开辟第一层空间
	explicit TCMalloc_PageMap3() {
		root_ = NewNode();
	}

	void* get(Number k) const {
		const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS); //第一层下标
		const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1); //第二层小标
		const Number i3 = k & (LEAF_LENGTH - 1); //第三层下标
		//页号超出范围，或映射该页号的空间未开辟
		if ((k >> BITS) > 0 || root_->ptrs[i1] == NULL || root_->ptrs[i1]->ptrs[i2] == NULL){
			return NULL;
		}
		return reinterpret_cast<Leaf*>(root_->ptrs[i1]->ptrs[i2])->values[i3];
	}

	void set(Number k, void* v) {
		assert(k >> BITS == 0);
		const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
		const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
		const Number i3 = k & (LEAF_LENGTH - 1);
		//没空间去开辟一组映射出来
		Ensure(k, 1);
		reinterpret_cast<Leaf*>(root_->ptrs[i1]->ptrs[i2])->values[i3] = v;
	}

	bool Ensure(Number start, size_t n) {
		for (Number key = start; key <= start + n - 1;) {
			const Number i1 = key >> (LEAF_BITS + INTERIOR_BITS);
			const Number i2 = (key >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
			if (i1 >= INTERIOR_LENGTH || i2 >= INTERIOR_LENGTH){
				return false;
			}

			if (root_->ptrs[i1] == NULL) {
				Node* n = NewNode();
				if (n == NULL) return false;
				root_->ptrs[i1] = n;
			}

			if (root_->ptrs[i1]->ptrs[i2] == NULL) {
				static ObjectPool<Leaf> ObjectLeaf;
				Leaf* leaf = ObjectLeaf.New();

				memset(leaf, 0, sizeof(*leaf));
				root_->ptrs[i1]->ptrs[i2] = reinterpret_cast<Node*>(leaf);
			}
			key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
		}
		return true;
	}
};