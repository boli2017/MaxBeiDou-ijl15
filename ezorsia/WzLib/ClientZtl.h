#pragma once
// ============================================================================
// ClientZtl.h — v83 客户端 ZTL 内部类镜像（最小集，仅 char 字符串）
//
// 移植自 kaentake 的 src/ztl/{zlock,zalloc,zstr}.h。必须使用客户端进程内的
// 分配器实例分配字符串：当 ZXString 按值传给客户端函数时，被调方（客户端
// 代码）释放参数会走客户端自己的分配器；若使用本 DLL 自建内存池
// （MapleClientCollectionTypes/ZAllocEx）会造成堆损坏。
// ============================================================================

#include <windows.h>

namespace client_ztl {

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
inline ZFatalSection::pfnTry_t ZFatalSection::_s_pfnTry = ZFatalSection::_TryI;
static_assert(sizeof(ZFatalSection) == 0x8);

class ZSynchronizedHelper {
public:
    ZFatalSection* m_pLock;

    explicit ZSynchronizedHelper(ZFatalSection& lock) {
        m_pLock = &lock;
        lock.Lock();
    }
    ~ZSynchronizedHelper() {
        if (m_pLock->_m_nRef-- == 1) {
            m_pLock->_m_pTIB = nullptr;
        }
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

// 客户端 ZAllocStrSelector<char>：块大小 0x1D/0x2D/0x4D/0x8D
class ZAllocStrSelectorChar {
protected:
    static int SelectBufferIndex(size_t uSize) {
        if (uSize > 0x2D) {
            if (uSize <= 0x4D) {
                return 2;
            } else {
                return uSize > 0x8D ? -1 : 3;
            }
        } else {
            return uSize > 0x1D ? 1 : 0;
        }
    }
    static size_t GetBlockSize(int nIndex, int& nAllocBlocks) {
        switch (nIndex) {
        case 0: nAllocBlocks = 64; return 0x1D;
        case 1: nAllocBlocks = 32; return 0x2D;
        case 2: nAllocBlocks = 16; return 0x4D;
        case 3: nAllocBlocks = 8;  return 0x8D;
        default: nAllocBlocks = 0; return 0;
        }
    }
};

// 直接操作客户端进程内的 ZAllocEx 实例（_s_pAlloc 指向客户端实例）
template <typename T>
class ZAllocEx : private ZAllocBase, private T {
private:
    ZFatalSection m_lock;
    void* m_apBuff[4] = {};
    void* m_apBlockHead[4] = {};

public:
    void* Alloc(size_t uSize) {
        int nIndex = T::SelectBufferIndex(uSize);
        if (nIndex < 0) {
            return ZAllocBase::_AllocRaw(uSize);
        }
        int nAllocBlocks;
        size_t uBlockSize = T::GetBlockSize(nIndex, nAllocBlocks);
        ZSynchronizedHelper _sync(m_lock);
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
        ZSynchronizedHelper _sync(m_lock);
        *reinterpret_cast<void**>(p) = m_apBuff[nIndex];
        m_apBuff[nIndex] = p;
    }
};

// 客户端 ZAllocEx<ZAllocStrSelector<char>> 实例地址（v83，与 kaentake 的
// ZALLOCEX(ZAllocStrSelector<char>, 0x00BF0A90) 一致）
static const unsigned int kZAllocStrSelectorCharInstance = 0x00BF0A90;

inline void* ClientStrAlloc(size_t uSize) {
    return reinterpret_cast<ZAllocEx<ZAllocStrSelectorChar>*>(kZAllocStrSelectorCharInstance)->Alloc(uSize);
}
inline void ClientStrFree(void* p) {
    reinterpret_cast<ZAllocEx<ZAllocStrSelectorChar>*>(kZAllocStrSelectorCharInstance)->Free(p);
}
static_assert(sizeof(ZAllocEx<ZAllocStrSelectorChar>) == 0x2C);

// kaentake 的 ZXString<char> 最小移植（布局 0x4，数据头 0xC，与客户端一致）
class ZXStringChar {
    struct _ZXStringData {
        volatile long nRef;
        int nCap;
        int nByteLen;

        char* pStr() {
            return reinterpret_cast<char*>(&this[1]);
        }
    };
    static_assert(sizeof(_ZXStringData) == 0xC);

    char* _m_pStr;

public:
    ~ZXStringChar() {
        if (_m_pStr) {
            _Release(_GetData());
        }
    }
    ZXStringChar(const char* s, int n = -1) : _m_pStr(nullptr) {
        Assign(s, n);
    }
    ZXStringChar() : _m_pStr(nullptr) {
    }

    void Assign(const char* s, int n = -1) {
        if (s) {
            int nLength = n == -1 ? static_cast<int>(strlen(s)) : n;
            char* pBuffer = GetBuffer(nLength, 0);
            memcpy(pBuffer, s, nLength * sizeof(char));
            ReleaseBuffer(nLength);
        } else if (_m_pStr) {
            _Release(_GetData());
            _m_pStr = nullptr;
        }
    }

private:
    _ZXStringData* _GetData() const {
        return reinterpret_cast<_ZXStringData*>(_m_pStr) - 1;
    }
    char* GetBuffer(int nMinLength, int bRetain) {
        int nCap = 0;
        auto pData = _m_pStr ? _GetData() : nullptr;
        if (pData) {
            if (pData->nRef <= 1 && pData->nCap >= nMinLength) {
                pData->nRef = -1;
                return _m_pStr;
            }
            nCap = (pData->nByteLen) / sizeof(char);
        }
        if (nCap < nMinLength) {
            nCap = nMinLength;
        }
        auto pAlloc = _Alloc(nCap);
        pAlloc->nRef = -1;
        _m_pStr = pAlloc->pStr();
        if (bRetain && pData) {
            memcpy(_m_pStr, pData->pStr(), pData->nByteLen + sizeof(char));
            pAlloc->nByteLen = pData->nByteLen;
        } else {
            pAlloc->nByteLen = 0;
            _m_pStr[0] = 0;
        }
        if (pData) {
            _Release(pData);
        }
        return _m_pStr;
    }
    void ReleaseBuffer(int nLength) {
        auto pData = _GetData();
        pData->nRef = 1;
        if (nLength == -1) {
            pData->nByteLen = static_cast<int>(strlen(pData->pStr())) * sizeof(char);
        } else {
            _m_pStr[nLength] = 0;
            pData->nByteLen = nLength * sizeof(char);
        }
    }
    static _ZXStringData* _Alloc(int nCap) {
        auto pData = static_cast<_ZXStringData*>(ClientStrAlloc(sizeof(char) * (nCap + 1) + sizeof(_ZXStringData)));
        pData->nCap = nCap;
        return pData;
    }
    static void _Release(_ZXStringData* pData) {
        if (InterlockedDecrement(&pData->nRef) <= 0) {
            _Free(pData);
        }
    }
    static void _Free(_ZXStringData* pFree) {
        ClientStrFree(pFree);
    }
};
static_assert(sizeof(ZXStringChar) == 0x4);

} // namespace client_ztl
