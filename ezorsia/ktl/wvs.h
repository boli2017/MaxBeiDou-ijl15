#pragma once
// ============================================================================
// ktl/wvs.h — kaentake src/wvs 的子集合并移植（仅 InGameRes.cpp 使用）
//
// 覆盖运行时分辨率所需的客户端类：RTTI / 窗口体系 / 命名空间管理器 /
// 状态栏 / 临时属性视图 / 地图视野 / 配置项 / 资源管理器句柄。
// 所有成员偏移与 thunk 地址与 kaentake 一致（v83 GMS / BeiDou.exe）。
//
// 注意：只能被唯一一个翻译单元包含（含静态成员定义）。
// ============================================================================

#include "ztl.h"
#include "../WzLib/WzInterfaces.h"

// ---- hook.h: MEMBER_AT / MEMBER_ARRAY_AT ----

#define MEMBER_AT(T, OFFSET, NAME) \
	__declspec(property(get = get_##NAME, put = set_##NAME)) T NAME; \
	__forceinline const T& get_##NAME() const { \
		return *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(this) + OFFSET); \
	} \
	__forceinline T& get_##NAME() { \
		return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + OFFSET); \
	} \
	__forceinline void set_##NAME(const T& value) { \
		*reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + OFFSET) = const_cast<T&>(value); \
	} \
	__forceinline void set_##NAME(T& value) { \
		*reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + OFFSET) = value; \
	}

#define MEMBER_ARRAY_AT(T, OFFSET, NAME, N) \
	__declspec(property(get = get_##NAME)) T(&NAME)[N]; \
	__forceinline T(&get_##NAME())[N] { \
		return *reinterpret_cast<T(*)[N]>(reinterpret_cast<uintptr_t>(this) + OFFSET); \
	}

// ---- rtti.h ----

class CRTTI {
public:
	const CRTTI* m_pPrev;

	CRTTI(const CRTTI* pPrev) : m_pPrev(pPrev) {
	}
	int IsKindOf(const CRTTI* pRTTI) const {
		CRTTI result = { this };
		while (result.m_pPrev != pRTTI) {
			result.m_pPrev = result.m_pPrev->m_pPrev;
			if (!result.m_pPrev) {
				return 0;
			}
		}
		return 1;
	}
};

// ---- gobj.h / msghandler.h ----

class IGObj {
public:
	virtual void Update() {}
};

class CInPacket;
class IDraggable;
class CWnd;

class IUIMsgHandler {
public:
	inline static CRTTI& ms_RTTI_IUIMsgHandler = *reinterpret_cast<CRTTI*>(0x00BEDBB8);

	virtual void OnKey(unsigned int wParam, unsigned int lParam) {}
	virtual int OnSetFocus(int bFocus) { return 0; }
	virtual void OnMouseButton(unsigned int msg, unsigned int wParam, int rx, int ry) {}
	virtual int OnMouseMove(int rx, int ry) { return 0; }
	virtual int OnMouseWheel(int rx, int ry, int nWheel) { return 0; }
	virtual void OnMouseEnter(int bEnter) {}
	virtual void OnDraggableMove(int nState, IDraggable* pObj, int rx, int ry) {}
	virtual void SetEnable(int bEnable) {}
	virtual int IsEnabled() const { return 1; }
	virtual void SetShow(int bShow) {}
	virtual int IsShown() const { return 1; }
	virtual int GetAbsLeft() { return 0; }
	virtual int GetAbsTop() { return 0; }
	virtual void ClearToolTip() {}
	virtual void OnIMEModeChange(char cMode) {}
	virtual void OnIMEResult(const char* sComp) {}
	virtual void OnIMEComp(const char* sComp, void* adwCls, unsigned int nClsIdx, int nCursor, void* lCand, int nBegin, int nPage, int nCur) {}
	virtual const CRTTI* GetRTTI() const {
		return &ms_RTTI_IUIMsgHandler;
	}
	virtual int IsKindOf(const CRTTI* pRTTI) const {
		return ms_RTTI_IUIMsgHandler.IsKindOf(pRTTI);
	}
};

class INetMsgHandler {
public:
	virtual void OnPacket(int nType, CInPacket& iPacket) {}
};

// ---- stage.h / field.h / util.h ----

class CStage : public IGObj, public IUIMsgHandler, public INetMsgHandler, public ZRefCounted {
public:
	virtual void Init(void* pParam) {}
	virtual void Close() {}
};

inline ZRef<CStage>& get_stage() {
	return *reinterpret_cast<ZRef<CStage>*>(0x00BEDED0);
}

class CMapLoadable : public CStage {
public:
	MEMBER_AT(IWzPropertyPtr, 0x2C, m_pPropFieldInfo)
	MEMBER_AT(RECT, 0xF0, m_rcViewRange)
};

class CField : public CMapLoadable {
public:
	MEMBER_AT(ZRef<CWnd>, 0x1C8, m_pClock) // ZRef<CClock>
};

inline CField* get_field() {
	return reinterpret_cast<CField* (__cdecl*)()>(0x00437A0C)();
}

inline IWzGr2DPtr& get_gr() {
	return *reinterpret_cast<IWzGr2DPtr*>(0x00BF14EC);
}

inline IWzResManPtr& get_rm() {
	return *reinterpret_cast<IWzResManPtr*>(0x00BF14E8);
}

inline IWzNameSpacePtr& get_root() {
	return *reinterpret_cast<IWzNameSpacePtr*>(0x00BF14E0);
}

inline int get_int32(Ztl_variant_t& v, int nDefault) {
	Ztl_variant_t vInt;
	if (V_VT(&v) == VT_EMPTY || V_VT(&v) == VT_ERROR || FAILED(ZComAPI::ZComVariantChangeType(&vInt, &v, 0, VT_I4))) {
		return nDefault;
	} else {
		return V_I4(&vInt);
	}
}

// 实现在 InGameRes.cpp
int get_screen_width();
int get_screen_height();
int get_adjust_cy();

// ---- ctrlwnd.h ----

class CCtrlWnd : public IGObj, public IUIMsgHandler, public ZRefCounted {
public:
	unsigned char pad0[0x34 - sizeof(IGObj) - sizeof(IUIMsgHandler) - sizeof(ZRefCounted)];

	virtual void Update() override {}
	virtual int OnDragDrop(int nState, void* ctx, int rx, int ry) { return 0; }
	virtual void CreateCtrl(CWnd* pParent, unsigned int nId, int l, int t, int w, int h, void* pData) {}
	virtual void Destroy() {}
	virtual void OnCreate(void* pData) {}
	virtual void OnDestroy(void* pData) {}
	virtual int HitTest(unsigned int rx, unsigned int ry) { return 0; }
	virtual void Draw(int rx, int ry, const RECT* pRect) {}
};
static_assert(sizeof(CCtrlWnd) == 0x34);

class CCtrlComboBox : public CCtrlWnd {
public:
	unsigned char pad0[0x10C - sizeof(CCtrlWnd)];
	MEMBER_AT(int, 0x68, m_nSelect)

	struct CREATEPARAM {
		unsigned char pad0[0x50];
		MEMBER_AT(int, 0x0, nBackColor)
		MEMBER_AT(int, 0x4, nBackFocusedColor)
		MEMBER_AT(int, 0x8, nBorderColor)

		CREATEPARAM() {
			reinterpret_cast<void(__thiscall*)(CREATEPARAM*)>(0x004B620C)(this);
		}
		~CREATEPARAM() {
			reinterpret_cast<void(__thiscall*)(CREATEPARAM*)>(0x004B63AE)(this);
		}
	};

	virtual void CreateCtrl(void* pParent, unsigned int nId, int nType, int l, int t, int w, int h, void* pData) {}
	virtual ~CCtrlComboBox() override {
		reinterpret_cast<void(__thiscall*)(CCtrlComboBox*)>(0x004C4372)(reinterpret_cast<CCtrlComboBox*>(reinterpret_cast<uintptr_t>(this) + 0x8));
	}
	CCtrlComboBox() {
		reinterpret_cast<void(__thiscall*)(CCtrlComboBox*)>(0x004C4259)(this);
	}
	// 控件本体必须走客户端匿名分配器（客户端销毁时用它自己的分配器释放）
	void* operator new(size_t uSize) {
		return ZAllocEx<ZAllocAnonSelector>::s_Alloc(uSize);
	}
	void operator delete(void* p) {
		ZAllocEx<ZAllocAnonSelector>::s_Free(p);
	}

	void AddItem(const char* sItemName, unsigned int dwParam) {
		reinterpret_cast<void(__thiscall*)(CCtrlComboBox*, const char*, unsigned int)>(0x004C6DD6)(this, sItemName, dwParam);
	}
	void SetSelect(int nSelect) {
		reinterpret_cast<void(__thiscall*)(CCtrlComboBox*, int)>(0x004C738B)(this, nSelect);
	}
};
static_assert(sizeof(CCtrlComboBox) == 0x10C);

// ---- tooltip.h（子集） ----

class CUIToolTip {
public:
	MEMBER_AT(int, 0x8, m_nHeight)
	MEMBER_AT(int, 0xC, m_nWidth)
	MEMBER_AT(IWzGr2DLayerPtr, 0x10, m_pLayer)
};

// ---- wnd.h ----

class CWnd : public IGObj, public IUIMsgHandler, public ZRefCounted {
public:
	inline static CRTTI& ms_RTTI_CWnd = *reinterpret_cast<CRTTI*>(0x00C6194C);

	enum UIOrigin {
		Origin_LT = 0x0,
		Origin_CT = 0x1,
		Origin_RT = 0x2,
		Origin_LC = 0x3,
		Origin_CC = 0x4,
		Origin_RC = 0x5,
		Origin_LB = 0x6,
		Origin_CB = 0x7,
		Origin_RB = 0x8,
		Origin_NUM = 0x9,
	};

	unsigned char pad[0x6C - sizeof(IGObj) - sizeof(IUIMsgHandler) - sizeof(ZRefCounted)];
	MEMBER_AT(IWzGr2DLayerPtr, 0x18, m_pLayer)
	MEMBER_AT(IWzGr2DLayerPtr, 0x20, m_pOverlabLayer)
	MEMBER_AT(int, 0x24, m_width)
	MEMBER_AT(int, 0x28, m_height)
	MEMBER_AT(int, 0x3C, m_bScreenCoord)
	MEMBER_AT(POINT, 0x48, m_ptCursorRel)

	virtual int OnDragDrop(int nState, void* ctx, int rx, int ry) {
		return 0;
	}
	virtual void PreCreateWnd(int l, int t, int w, int h, int z, int bScreenCoord, void* pData) {
		reinterpret_cast<void(__thiscall*)(CWnd*, int, int, int, int, int, int, void*)>(0x009DE7FB)(this, l, t, w, h, z, bScreenCoord, pData);
	}
	virtual void OnCreate(void* pData) {
		;
	}
	virtual void OnDestroy() {
		;
	}
	virtual void OnMoveWnd(int l, int t) {
		reinterpret_cast<void(__thiscall*)(CWnd*, int, int)>(0x009DEB57)(this, l, t);
	}
	virtual void OnEndMoveWnd() {
		reinterpret_cast<void(__thiscall*)(CWnd*)>(0x009E00A6)(this);
	}
	virtual void OnChildNotify(unsigned int nId, unsigned int param1, unsigned int param2) {
		if (param1 == 100) {
			OnButtonClicked(nId);
		}
	}
	virtual void OnButtonClicked(unsigned int nId) {
		;
	}
	virtual int HitTest(int rx, int ry, CCtrlWnd** ppCtrl) {
		return reinterpret_cast<int(__thiscall*)(CWnd*, int, int, CCtrlWnd**)>(0x009E01E7)(this, rx, ry, ppCtrl);
	}
	virtual int OnActivate(int bActive) {
		return reinterpret_cast<int(__thiscall*)(CWnd*, int)>(0x009E03A6)(this, bActive);
	}
	virtual void Draw(const RECT* pRect) {
		reinterpret_cast<void(__thiscall*)(CWnd*, const RECT*)>(0x009E0502)(this, pRect);
	}
	virtual int IsMyAddOn(CWnd* pWnd) {
		return 0;
	}

	virtual int OnSetFocus(int bFocus) override {
		return reinterpret_cast<int(__thiscall*)(IUIMsgHandler*, int)>(0x009E0369)(this, bFocus);
	}
	virtual int OnMouseWheel(int rx, int ry, int nWheel) override {
		return reinterpret_cast<int(__thiscall*)(IUIMsgHandler*, int, int, int)>(0x009E02AE)(this, rx, ry, nWheel);
	}
	virtual void OnMouseEnter(int bEnter) override {
		reinterpret_cast<void(__thiscall*)(IUIMsgHandler*, int)>(0x009E01C8)(this, bEnter);
	}
	virtual int GetAbsLeft() override {
		return reinterpret_cast<int(__thiscall*)(IUIMsgHandler*)>(0x009E03C5)(this);
	}
	virtual int GetAbsTop() override {
		return reinterpret_cast<int(__thiscall*)(IUIMsgHandler*)>(0x009E0447)(this);
	}
	virtual const CRTTI* GetRTTI() const override {
		return &ms_RTTI_CWnd;
	}
	virtual int IsKindOf(const CRTTI* pRTTI) const override {
		return ms_RTTI_CWnd.IsKindOf(pRTTI);
	}

	virtual ~CWnd() override {
		// MSVC ABI 差异：客户端 CWnd 析构函数要求 this 对齐到 ZRefCounted vftable
		reinterpret_cast<void(__thiscall*)(CWnd*)>(0x009DE438)(reinterpret_cast<CWnd*>(reinterpret_cast<uintptr_t>(this) + 0x8));
	}
	CWnd() {
		reinterpret_cast<void(__thiscall*)(CWnd*)>(0x009DE383)(this);
	}
	explicit CWnd(int nDummy) {
		;
	}

	void MoveWnd(int l, int t) {
		m_pLayer->RelMove(l, t, vtEmpty, vtEmpty);
	}
	void InvalidateRect(const RECT* pRect) {
		reinterpret_cast<void(__thiscall*)(CWnd*, const RECT*)>(0x009E04C9)(this, pRect);
	}
	void Destroy() {
		reinterpret_cast<void(__thiscall*)(CWnd*)>(0x009E00AF)(this);
	}
};
static_assert(sizeof(CWnd) == 0x6C);

// ---- statusbar.h / tempstat.h / config.h ----

class CUIStatusBar : public CWnd, public TSingleton<CUIStatusBar, 0x00BEC208> {
public:
	MEMBER_AT(int, 0xD10, m_bQuickSlotUp)
};

class CTemporaryStatView {
public:
	struct TEMPORARY_STAT : public ZRefCounted {
		unsigned char pad0[0x40 - sizeof(ZRefCounted)];
		MEMBER_AT(int, 0x1C, nType)
		MEMBER_AT(int, 0x20, nID)
		MEMBER_AT(IWzGr2DLayerPtr, 0x28, pLayer)
		MEMBER_AT(IWzGr2DLayerPtr, 0x2C, pLayerShadow)
		MEMBER_AT(int, 0x30, nIndexShadow)
		MEMBER_AT(int, 0x34, bNoShadow)
		MEMBER_AT(int, 0x38, tLeft)
		MEMBER_AT(int, 0x3C, tLeftUnit)
	};
	MEMBER_AT(ZList<ZRef<TEMPORARY_STAT>>, 0x4, m_lTemporaryStat)
};

class CConfig : public TSingleton<CConfig, 0x00BEBF9C> {
public:
	enum {
		GLOBAL_OPT = 0x0,
		LAST_CONNECT_INFO = 0x1,
		CHARACTER_OPT = 0x2,
	};
	MEMBER_ARRAY_AT(int, 0xCC, m_nUIWnd_X, 34)
	MEMBER_ARRAY_AT(int, 0x154, m_nUIWnd_Y, 34)

	int GetOpt_Int(int nType, const char* sKey, int nDefaultX, int nLowBound, int nHighBound) {
		return reinterpret_cast<int(__thiscall*)(CConfig*, int, const char*, int, int, int)>(0x0049EF65)(this, nType, sKey, nDefaultX, nLowBound, nHighBound);
	}
	void SetOpt_Int(int nType, const char* sKey, int nValue) {
		reinterpret_cast<void(__thiscall*)(CConfig*, int, const char*, int)>(0x0049EFB5)(this, nType, sKey, nValue);
	}
};

// ---- wndman.h（CWndMan，含九锚点扩展） ----

class CWndMan : public CWnd, public TSingleton<CWndMan, 0x00BEC20C> {
public:
	inline static ZList<CWnd*>& ms_lpWindow = *reinterpret_cast<ZList<CWnd*>*>(0x00BF1648);
	inline static IWzVector2DPtr ms_pOrgWindowEx[UIOrigin::Origin_NUM];
	inline static IWzVector2DPtr ms_pOrgStatusBar;
	inline static IWzVector2DPtr ms_pOrgScreenMsg;
	inline static IWzVector2DPtr ms_pOrgQuickSlot;

	MEMBER_AT(IWzVector2DPtr, 0xDC, m_pOrgWindow)

	IWzVector2DPtr& GetOrgWindowEx(CWnd::UIOrigin org) {
		return ms_pOrgWindowEx[org];
	}

	// 九锚点重绑（kaentake ResetOrgWindow）：m_pOrgWindow 锚到 Gr2D center，
	// 另有 9 个 (L/C/R)x(T/C/B) 锚点 + 状态栏/屏幕消息/快捷栏专属锚点
	void ResetOrgWindow() {
		IWzVector2D* pCenter = nullptr;
		if (get_gr()) {
			get_gr()->get_center(&pCenter);
		}
		Ztl_variant_t vCenter(static_cast<IUnknown*>(pCenter), true);

		m_pOrgWindow->put_origin(vCenter);
		m_pOrgWindow->RelMove(-(get_screen_width() / 2), -(get_screen_height() / 2) - get_adjust_cy(), vtEmpty, vtEmpty);
		for (int i = 0; i < UIOrigin::Origin_NUM; ++i) {
			int nX = -(get_screen_width() / 2);
			if (i % 3 == 1) {
				nX += (get_screen_width() - 800) / 2;
			} else if (i % 3 == 2) {
				nX += (get_screen_width() - 800);
			}
			int nY = -(get_screen_height() / 2) - get_adjust_cy();
			if (i / 3 == 1) {
				nY += (get_screen_height() - 600) / 2;
			} else if (i / 3 == 2) {
				nY += (get_screen_height() - 600);
			}
			ms_pOrgWindowEx[i]->put_origin(vCenter);
			ms_pOrgWindowEx[i]->RelMove(nX, nY, vtEmpty, vtEmpty);
		}
		Ztl_variant_t vLB(static_cast<IUnknown*>(ms_pOrgWindowEx[Origin_LB].GetInterfacePtr()), true);
		Ztl_variant_t vRB(static_cast<IUnknown*>(ms_pOrgWindowEx[Origin_RB].GetInterfacePtr()), true);
		ms_pOrgStatusBar->put_origin(vLB);
		ms_pOrgScreenMsg->put_origin(vRB);
		ms_pOrgQuickSlot->put_origin(vLB);
		vRB.Clear();
		vLB.Clear();
		if (get_screen_width() > 800) {
			ms_pOrgScreenMsg->RelMove(0, -10, vtEmpty, vtEmpty);
			ms_pOrgQuickSlot->RelMove(152, 68, vtEmpty, vtEmpty);
		} else {
			ms_pOrgScreenMsg->RelMove(0, 0, vtEmpty, vtEmpty);
			ms_pOrgQuickSlot->RelMove(0, 0, vtEmpty, vtEmpty);
		}
		vCenter.Clear();
		if (pCenter) {
			pCenter->Release();
		}
	}
};
