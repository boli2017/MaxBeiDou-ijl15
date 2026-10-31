#pragma once
// ============================================================================
// ktl/ztl.h — kaentake src/ztl 的合并移植（仅 InGameRes.cpp 使用）
//
// 必须与客户端（BeiDou.exe）内的分配器实例保持一致布局；ZXString/ZArray/
// ZRefCountedDummy 的分配全部走客户端进程内的 ZAllocEx 实例（固定地址），
// 绝不能使用本 DLL 自建内存池，否则客户端代码释放时会堆损坏。
//
// 注意：本头文件包含 ZAllocEx<T>::_s_pAlloc 的静态成员定义（ZALLOCEX），
// 只能被唯一一个翻译单元包含。
// ============================================================================

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <new>

// ---- zlock.h ----

class ZFatalSectionData {
public:
	void* _m_pTIB = nullptr;
	volatile long _m_nRef = 0;
};
static_assert(sizeof(ZFatalSectionData) == 0x8);

class ZFatalSection : public ZFatalSectionData {
private:
	typedef struct _TEB* (__fastcall* pfnTry_t)(volatile long*);
	static pfnTry_t _s_pfnTry;
	// 单 TU 内包含本头文件，静态成员定义放这里

	static struct _TEB* __fastcall _TryM(volatile long* __formal) {
		_TEB* pTeb = NtCurrentTeb();
		long result = InterlockedCompareExchange(__formal, reinterpret_cast<long>(pTeb), 0);
		if (result) {
			if (result == reinterpret_cast<long>(pTeb)) {
				InterlockedIncrement(__formal + 1);
				return nullptr;
			}
		} else {
			InterlockedExchange(__formal + 1, 1);
		}
		return reinterpret_cast<_TEB*>(result);
	}
	static struct _TEB* __fastcall _TryS(volatile long* __formal) {
		_TEB* pTeb = NtCurrentTeb();
		long result = InterlockedCompareExchange(__formal, reinterpret_cast<long>(pTeb), 0);
		InterlockedExchange(__formal + 1, 1);
		return reinterpret_cast<_TEB*>(result);
	}
	static struct _TEB* __fastcall _TryI(volatile long* p) {
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		pfnTry_t pfnTry = _TryM;
		if (si.dwNumberOfProcessors <= 1) {
			pfnTry = _TryS;
		}
		_s_pfnTry = pfnTry;
		return pfnTry(p);
	}

public:
	void Lock() {
		volatile long* p = reinterpret_cast<volatile long*>(this);
		if (ZFatalSection::_s_pfnTry(p) && ZFatalSection::_s_pfnTry(p)) {
			do {
				Sleep(0);
			} while (ZFatalSection::_s_pfnTry(p));
		}
	}
};
static_assert(sizeof(ZFatalSection) == 0x8);

ZFatalSection::pfnTry_t ZFatalSection::_s_pfnTry = &ZFatalSection::_TryI;

template <typename T>
class ZSynchronizedHelper;

template <>
class ZSynchronizedHelper<ZFatalSection> {
public:
	ZFatalSection* m_pLock;

	explicit ZSynchronizedHelper(ZFatalSection& lock) {
		m_pLock = &lock;
		lock.Lock();
	}
	explicit ZSynchronizedHelper(ZFatalSection* lock) {
		m_pLock = lock;
		lock->Lock();
	}
	~ZSynchronizedHelper() {
		if (m_pLock->_m_nRef-- == 1) {
			m_pLock->_m_pTIB = nullptr;
		}
	}
};
static_assert(sizeof(ZSynchronizedHelper<ZFatalSection>) == 0x4);

// ---- zalloc.h ----

template <typename T>
T* zaddressof(T& t) {
	return reinterpret_cast<T*>(&const_cast<char&>(reinterpret_cast<const volatile char&>(t)));
}

template <size_t S0, size_t S1, size_t S2, size_t S3>
class ZAllocAbstractSelector {
protected:
	static int SelectBufferIndex(size_t uSize) {
		if (uSize > S1) {
			if (uSize <= S2) {
				return 2;
			} else {
				return uSize > S3 ? -1 : 3;
			}
		} else {
			return uSize > S0 ? 1 : 0;
		}
	}
	static size_t GetBlockSize(int nIndex, int& nAllocBlocks) {
		switch (nIndex) {
		case 0: nAllocBlocks = 64; return S0;
		case 1: nAllocBlocks = 32; return S1;
		case 2: nAllocBlocks = 16; return S2;
		case 3: nAllocBlocks = 8; return S3;
		default: nAllocBlocks = 0; return 0;
		}
	}
};

class ZAllocAnonSelector : protected ZAllocAbstractSelector<0x10, 0x20, 0x40, 0x80> {
};

template <typename T>
class ZAllocStrSelector;

template <>
class ZAllocStrSelector<char> : protected ZAllocAbstractSelector<0x1D, 0x2D, 0x4D, 0x8D> {
};

template <>
class ZAllocStrSelector<wchar_t> : protected ZAllocAbstractSelector<0x2E, 0x4E, 0x8E, 0x10E> {
};

class ZAllocHelper {
public:
	explicit ZAllocHelper(int nDummy) {
	}
};

class ZAllocBase {
public:
	static void* _AllocRaw(size_t uSize) {
		HANDLE hHeap = GetProcessHeap();
		auto pAlloc = static_cast<size_t*>(HeapAlloc(hHeap, 0, uSize + sizeof(size_t)));
		*pAlloc = uSize;
		return pAlloc + 1;
	}
	static void _FreeRaw(void* p) {
		HANDLE hHeap = GetProcessHeap();
		HeapFree(hHeap, 0, static_cast<size_t*>(p) - 1);
	}
	static void* _AllocRawBlocks(size_t uBlockSize, size_t u) {
		size_t uAllocSize = uBlockSize + sizeof(size_t);
		auto pAlloc = static_cast<size_t*>(_AllocRaw(u * uAllocSize + sizeof(size_t)));
		*pAlloc++ = 0;
		*pAlloc = uBlockSize;
		size_t* pBlock = pAlloc + 1;
		for (size_t nRemain = u - 1; nRemain > 0; --nRemain) {
			*pBlock = reinterpret_cast<uintptr_t>(pBlock) + uAllocSize;
			pBlock = reinterpret_cast<size_t*>(*pBlock);
			*(pBlock - 1) = uBlockSize;
		}
		*pBlock = 0;
		return pAlloc + 1;
	}
	static void _FreeRawBlocks(void* p) {
		if (!p) {
			return;
		}
		_FreeRaw(static_cast<size_t*>(p) - 2);
	}
	static size_t _MemSize(void* p) {
		size_t uSize = *(static_cast<size_t*>(p) - 1);
		if (uSize & 0x80000000) {
			uSize = ~uSize;
		}
		return uSize;
	}
	static void*& _NextHeadBlock(void* p) {
		return *reinterpret_cast<void**>(static_cast<size_t*>(p) - 2);
	}
};

template <typename T>
class ZAllocEx : private ZAllocBase, private T {
private:
	ZFatalSection m_lock;
	void* m_apBuff[4] = {};
	void* m_apBlockHead[4] = {};

public:
	~ZAllocEx() {
		for (size_t i = 0; i < 4; ++i) {
			void* pNext = m_apBlockHead[i];
			while (pNext) {
				void* pFree = pNext;
				pNext = ZAllocBase::_NextHeadBlock(pNext);
				ZAllocBase::_FreeRawBlocks(pFree);
			}
		}
	}
	void* Alloc(size_t uSize) {
		int nIndex = T::SelectBufferIndex(uSize);
		if (nIndex < 0) {
			return ZAllocBase::_AllocRaw(uSize);
		}
		int nAllocBlocks;
		size_t uBlockSize = T::GetBlockSize(nIndex, nAllocBlocks);
		ZSynchronizedHelper<ZFatalSection> _sync(m_lock);
		if (!m_apBuff[nIndex]) {
			void* pBlock = ZAllocBase::_AllocRawBlocks(uBlockSize, nAllocBlocks);
			*reinterpret_cast<void**>(static_cast<size_t*>(pBlock) - 2) = m_apBlockHead[nIndex];
			m_apBlockHead[nIndex] = pBlock;
			m_apBuff[nIndex] = pBlock;
		}
		void* pBuffer = m_apBuff[nIndex];
		m_apBuff[nIndex] = *static_cast<void**>(pBuffer);
		return pBuffer;
	}
	void Free(void* p) {
		if (!p) {
			return;
		}
		int nIndex = T::SelectBufferIndex(_MemSize(p));
		if (nIndex < 0) {
			ZAllocBase::_FreeRaw(p);
			return;
		}
		ZSynchronizedHelper<ZFatalSection> _sync(m_lock);
		*reinterpret_cast<void**>(p) = m_apBuff[nIndex];
		m_apBuff[nIndex] = p;
	}
	// 客户端进程内的 ZAllocEx<T> 实例（固定地址，见下方特化定义）
	static void* s_Alloc(size_t uSize) {
		return _s_pAlloc->Alloc(uSize);
	}
	static void s_Free(void* p) {
		_s_pAlloc->Free(p);
	}
};
static_assert(sizeof(ZAllocEx<ZAllocAnonSelector>) == 0x2C);

// 客户端实例地址（v83 / BeiDou.exe，与 kaentake injector.cpp 的 ZALLOCEX 一致）
template <typename T>
ZAllocEx<T>* ZAllocEx<T>::_s_pAlloc = nullptr;

template <>
ZAllocEx<ZAllocAnonSelector>* ZAllocEx<ZAllocAnonSelector>::_s_pAlloc = reinterpret_cast<ZAllocEx<ZAllocAnonSelector>*>(0x00BF0B00);
template <>
ZAllocEx<ZAllocStrSelector<char>>* ZAllocEx<ZAllocStrSelector<char>>::_s_pAlloc = reinterpret_cast<ZAllocEx<ZAllocStrSelector<char>>*>(0x00BF0A90);
template <>
ZAllocEx<ZAllocStrSelector<wchar_t>>* ZAllocEx<ZAllocStrSelector<wchar_t>>::_s_pAlloc = reinterpret_cast<ZAllocEx<ZAllocStrSelector<wchar_t>>*>(0x00BF0BA8);

// ---- zrecyclable（仅保留布局必需部分：ZRefCountedDummy 的第二个 vtable） ----

class ZRecyclableStatic {
public:
	class CallBack {
	public:
		CallBack* m_pNext;

		CallBack() {
			m_pNext = ZRecyclableStatic::_s_pStatic->m_pHead;
		}
		virtual ~CallBack() = 0;
	};
	static_assert(sizeof(CallBack) == 0x8);

private:
	CallBack* m_pHead;
	static ZRecyclableStatic* _s_pStatic;
};
static_assert(sizeof(ZRecyclableStatic) == 0x4);

ZRecyclableStatic* ZRecyclableStatic::_s_pStatic = nullptr;

template <typename T, size_t SIZE, typename U>
class ZRecyclableAvBuffer : public ZRecyclableStatic::CallBack {
public:
	void* m_pStat;
	ZFatalSection m_l;
	void* m_pAv;
	static ZRecyclableAvBuffer<T, SIZE, U>*& s_pInstance;
};
static_assert(sizeof(ZRecyclableAvBuffer<void, 16, void>) == 0x18);

template <typename T, size_t SIZE, typename U>
class ZRecyclable : public ZAllocBase {
protected:
	virtual ~ZRecyclable() = default;
};

template <typename T>
class ZRefCountedDummy : public ZRefCounted, public ZRecyclable<ZRefCountedDummy<T>, 0x10, T> {
public:
	T t;

	virtual ~ZRefCountedDummy() override = default;
	static ZRefCountedDummy<T>* From(const T* pT) {
		return reinterpret_cast<ZRefCountedDummy<T>*>(reinterpret_cast<uintptr_t>(pT) - offsetof(ZRefCountedDummy<T>, t));
	}
};

class ZRefCountedAccessorBase {
protected:
	static ZRefCounted* _Set1(ZRefCounted* p) {
		InterlockedExchange(&p->_m_nRef, 1);
		return p;
	}
	static void _Delete(ZRefCounted* p) {
		delete p;
	}
	static long _AddRef(ZRefCounted* p) {
		return InterlockedIncrement(&p->_m_nRef);
	}
	static long _Release(ZRefCounted* p) {
		return InterlockedDecrement(&p->_m_nRef);
	}
	static ZRefCounted*& _GetPrev(ZRefCounted* p) {
		return p->_m_pPrev;
	}
	static ZRefCounted*& _GetNext(ZRefCounted* p) {
		return p->_m_pNext;
	}
};

template <typename T>
class ZRefCountedAccessor : public ZRefCountedAccessorBase {
};

template <typename T>
class ZRef : private ZRefCountedAccessor<T>, private ZRefCountedAccessor<ZRefCountedDummy<T>> {
private:
	T* p;

public:
	~ZRef() {
		_Release();
	}
	explicit ZRef(const ZAllocHelper& _ALLOC) : p(nullptr) {
		_Alloc();
	}
	ZRef(T* pT, bool bAddRef = true) : p(pT) {
		if (bAddRef) {
			_AddRef();
		}
	}
	ZRef(const ZRef<T>& r, bool bAddRef = true) : p(r.p) {
		if (bAddRef) {
			_AddRef();
		}
	}
	ZRef() : p(nullptr) {
	}
	operator bool() const {
		return p != nullptr;
	}
	int operator!() const {
		return p == nullptr;
	}
	operator T*() const {
		return p;
	}
	T* operator->() const {
		return p;
	}

	ZRef<T>& operator=(const ZRef<T>& r) {
		if (p == r.p) {
			return *this;
		}
		_Release();
		if (r.p) {
			p = r.p;
			_AddRef();
		}
		return *this;
	}
	ZRef<T>& operator=(T* pT) {
		_Release();
		p = pT;
		_AddRef();
		return *this;
	}

private:
	void _AddRef() {
		if (p) {
			_AddRefRaw(p);
		}
	}
	void _AddRefRaw(void* __formal) {
		auto pDummy = ZRefCountedDummy<T>::From(p);
		ZRefCountedAccessorBase::_AddRef(pDummy);
	}
	void _AddRefRaw(ZRefCounted* __formal) {
		ZRefCountedAccessorBase::_AddRef(p);
	}
	void _Release() {
		if (p) {
			_ReleaseRaw(p);
			p = nullptr;
		}
	}
	void _ReleaseRaw(void* __formal) {
		auto pDummy = ZRefCountedDummy<T>::From(p);
		if (ZRefCountedAccessorBase::_Release(pDummy) == 0) {
			ZRefCountedAccessorBase::_AddRef(pDummy);
			ZRefCountedAccessorBase::_Delete(pDummy);
		}
	}
	void _ReleaseRaw(ZRefCounted* __formal) {
		if (ZRefCountedAccessorBase::_Release(p) == 0) {
			ZRefCountedAccessorBase::_AddRef(p);
			ZRefCountedAccessorBase::_Delete(p);
		}
	}
};

// ---- zcoll.h: ZList ----

template <typename T>
class ZList : private ZRefCountedAccessor<T>, private ZRefCountedAccessor<ZRefCountedDummy<T>> {
private:
	size_t _m_uCount;
	T* _m_pHead;
	T* _m_pTail;

public:
	virtual ~ZList() {
	}
	ZList() : _m_uCount(0), _m_pHead(nullptr), _m_pTail(nullptr) {
	}
	size_t GetCount() const {
		return _m_uCount;
	}
	int IsEmpty() const {
		return _m_uCount == 0;
	}
	T* GetHeadPosition() const {
		return _m_pHead;
	}
	T* GetTailPosition() const {
		return _m_pTail;
	}
	T* FindIndex(size_t uIndex) {
		auto p = _m_pHead;
		for (size_t i = 0; i < uIndex; ++i) {
			p = _GetNext(p);
		}
		return p;
	}
	static T& GetNext(T*& pos) {
		T& result = *pos;
		pos = pos ? _GetNext(pos) : nullptr;
		return result;
	}
	static T& GetPrev(T*& pos) {
		T& result = *pos;
		pos = pos ? _GetPrev(pos) : nullptr;
		return result;
	}

private:
	static T* _GetNext(void* p) {
		auto pDummy = ZRefCountedDummy<T>::From(reinterpret_cast<T*>(p));
		auto pNext = ZRefCountedAccessor<T>::_GetNext(pDummy);
		return pNext ? zaddressof(static_cast<ZRefCountedDummy<T>*>(pNext)->t) : nullptr;
	}
	static T* _GetNext(ZRefCounted* p) {
		return ZRefCountedAccessor<T>::_GetNext(p);
	}
	static T* _GetPrev(void* p) {
		auto pDummy = ZRefCountedDummy<T>::From(reinterpret_cast<T*>(p));
		auto pPrev = ZRefCountedAccessor<T>::_GetPrev(pDummy);
		return pPrev ? zaddressof(static_cast<ZRefCountedDummy<T>*>(pPrev)->t) : nullptr;
	}
	static T* _GetPrev(ZRefCounted* p) {
		return ZRefCountedAccessor<T>::_GetPrev(p);
	}
};

// ---- tsingleton.h ----

template <typename T, uintptr_t ADDRESS>
class TSingleton {
public:
	inline static T*& ms_pInstance = *reinterpret_cast<T**>(ADDRESS);

	static T* GetInstance() {
		return ms_pInstance;
	}
	static bool IsInstantiated() {
		return ms_pInstance != nullptr;
	}
};

// ---- 公共小工具 ----

template <typename T>
__forceinline T zmax(T a, T b) {
	return b < a ? a : b;
}

template <typename T>
__forceinline T zmin(T a, T b) {
	return a < b ? a : b;
}

template <typename T>
__forceinline T zclamp(T value, T min, T max) {
	return zmax(min, zmin(max, value));
}
