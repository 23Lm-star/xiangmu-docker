#include"ThreadCache.h"
#include"CentralCache.h"
#include<algorithm>

//��������ڴ�
//����:�����ڴ���ֽڴ�С
// �������ڴ��ֽ������벢��ת���ɶ�Ӧ��Ͱ�������ҿ����ڴ棺
// 1�����ҵ��ڴ棬����Ͱ�ڵ�һ�������ڴ��
// 2��δ���ҵ���ȥ CentralCache��������
// return : �����ڴ��׵�ַ
void* ThreadCache::Allocate(size_t size)
{
	//assert(size <= MaxBytes);
	//������ ��Ͱ����
	size_t align = SizeClass::RoundUp(size);
	size_t index = SizeClass::Index(size);

	if (_FreeLists[index].Empty()){
		//����Ϊ�����벻���ڴ棬ȥCentralCache�����ڴ沢����
		return FetchFromCentralCache(index, align);//Ҫ��������
	}
	else{
		//������Ϊ�գ����������׽ڵ��ַ�����Ƴ��׽ڵ�
		return _FreeLists[index].Pop();
	}
}

//��CentralCache��ȡ�ڴ�
// ������Ͱ����,������ڴ���С
//����:ThreadCache û�ڴ���->ȥ CentralCache ��������һ���ڴ�
// �Լ��� 1 ��  ʣ�µĹҵ�������������
//return : ��ȡ���ڴ��ַ
void* ThreadCache::FetchFromCentralCache(size_t index, size_t size)
{
	//������CentralCache����(��������޵ͣ�С�������޸� [2,512])
	//ͬʱ���ܳ��������������ڴ���������
	size_t batchNum = std::min(SizeClass::NumMoveSize(size), _FreeLists[index].MaxSize());
	//�����������ڴ�����==�������������ֵ����������Ҫ������������
	//����ʼ�㷨(��������ӵ��)
	//ֻҪ��������������ޣ�����+1.��ֹ�̻߳�������ռ�ÿ����ڴ����
	if (_FreeLists[index].MaxSize() == batchNum){
		_FreeLists[index].MaxSize()++;
	}

	//��ʽ��CentralCache����(ʵ������)
	void* start = nullptr;//��������ͷ
	void* end = nullptr;//����������β
	//��CentralCache���Ļ�������batchNum��size��С�ڴ�飬����ʵ���������
	size_t ActualNum = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, size);
	//�������뵽һ���ڴ�
	assert(ActualNum >= 1);
	//������ڴ������û�г����쳣
	assert(start != nullptr && end != nullptr);

	if (ActualNum == 1){
		assert(start == end);
		return start;
	}
	else{
		//���뵽��һ�����ڴ�ȡ��һ��������,
		//ʣ�µĹҵ�ThreadCache��Ӧ��_freeList��
		_FreeLists[index].PushRange(Next(start), end, ActualNum - 1);//�ǵü������Լ�Ҫ�õ�һ��
		return start;
	}
}

//�ͷ��ڴ�����̻߳����
//����:�ͷ��ڴ��׵�ַ���ͷ��ڴ��С
//return: 
void ThreadCache::Deallocate(void* ptr, size_t size)
{
	//���յ�_FreeList��Ӧindex��(ͷ��)
	//size_t align = SizeClass::RoundUp(size);
	size_t index = SizeClass::Index(size);
	//��Ϊ֮ǰ�����ڴ��ʱ����Ƿ������֮����ڴ棬��˻��յ�ʱ��Ͳ���Ҫ����
	_FreeLists[index].Push(ptr);

	//_FreeLIsts[index]̫��Ҫ��CentralCache����
	if (_FreeLists[index].Size() >= _FreeLists[index].MaxSize())
	{
		//�����������׽ڵ���ո�CentralCache
		ListTooLong(_FreeLists[index], size);
	}
}
/// <summary>
/// ȡ������ �ڴ��size��С���ڴ��������Ȼ���ڴ���������յ�CentralCache���Ļ���
/// </summary>
/// <param name="list">��������</param>
/// <param name="size">�����ڴ���С</param>
/// return :
void ThreadCache::ListTooLong(FreeList& list, size_t size)
{
	//Ϊʲô��Ҫһ�����黹MaxSize()���ڴ��;
	//�������գ���������������ҪƵ�����ع黹��
	//	���ÿ��ֻ�� 1 �飺	�ͷ� �� �� 1 ��,�ͷ� �� �� 1 ��
	//	ÿ�ζ�Ҫ�� CentralCache,�������ܻ��ر��
	//	�������˼·�ǣ�
	//	��һ���ٻ�,һ�λ�һ���,���ٹ黹����,��������ͻ
	//	���������������ȫ�ԳƵ���ƣ�
	//	���룺һ����һ�����ٷ��� CentralCache
	//	�ͷţ���һ���ٻ����ٷ��� CentralCache

	void* start = nullptr;//����������ʼλ��
	void* end = nullptr;//������������λ��
	//list.PopRange(start, end, list.Size());
	//ɾ��maxsize��С�Ŀ�
	list.PopRange(start, end, list.MaxSize());
	//��Maxsize()��С���ڴ���������뵽���Ļ�������                 
	CentralCache::GetInstance()->ReleaseListToSpans(start, size);
}
