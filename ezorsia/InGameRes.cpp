#include "stdafx.h"
#include "InGameRes.h"
#include "ktl/wvs.h"
#include "WzLib/ClientPcom.h"
#include "Memory.h"
#include "Client.h"
#include <string>

// ============================================================================
// InGameRes.cpp — 运行时动态分辨率（kaentake resolution.cpp 1:1 移植）
//
// 与 kaentake 的差异：
//   1) 分辨率表扩到 6 档（新增 1280x720，与 ezorsia 推荐档一致），
//      soScreenResolution 取值范围 0..5；
//   2) 初始/登录/商城分辨率 = config.ini width/height（g_nResolutionHome），
//      而不是 kaentake 的固定 800x600（兼容 ezorsia 的登录框/商城静态补丁）；
//   3) ezorsia 的几百处静态补丁与本模块冲突的常量，在本模块安装时和每次
//      切换分辨率后重打（见 ReapplyInGameResPatches），并调用
//      Client::UpdateResolution() 重打其余常量；
//   4) 挂钩用 ezorsia 的 Memory::SetHook。
// ============================================================================

namespace {

const int SCREEN_WIDTH_MAX = 1920;
const int SCREEN_HEIGHT_MAX = 1080;
const int SCREEN_MESSAGE_WIDTH = 400;

// 分辨率档位表（0..5，soScreenResolution 持久化）
struct SCREEN_INFO {
	int nWidth;
	int nHeight;
};
const SCREEN_INFO SCREEN_INFO_SET[] = {
	{ 800, 600 },
	{ 1024, 768 },
	{ 1280, 720 },
	{ 1366, 768 },
	{ 1600, 900 },
	{ 1920, 1080 },
};
const int SCREEN_INFO_MAX = 6;

int g_nResolution = 0;
int g_nResolutionHome = 0;
int g_nScreenWidth = 800;
int g_nScreenHeight = 600;
int g_nAdjustCenterY = 0;
ZRef<CCtrlComboBox> g_cbResolution;

// ezorsia Client::UpdateResolution 也在写的数据区（CUtilDlgEx::CreateUtilDlgEx）
RECT& sRectQuestDlg = *reinterpret_cast<RECT*>(0x00BE2DF0);

// ---- CWzGr2D（Gr2D_DX8.DLL 对象布局） ----

struct SCREENMODE {
	unsigned char pad0[0x5C];
	MEMBER_AT(int, 0x0, nWidth)
	MEMBER_AT(int, 0x4, nHeight)
	MEMBER_AT(int, 0x58, bFullScreen)
};

class CWzGr2D : public IWzGr2D {
public:
	MEMBER_AT(SCREENMODE, 0x20, m_screenMode)
	MEMBER_AT(int, 0x90, m_bInitialized)
	MEMBER_AT(int, 0x94, m_hrErrorCode)

	// FindScreenMode 是 GR2D_DX8.DLL 内部函数（thiscall）：
	// (CWzGr2D*, SCREENMODE* pOut, int bFullScreen, int nWidth, int nHeight, int)
	typedef int(__thiscall* pfnFindScreenMode_t)(CWzGr2D*, SCREENMODE*, int, int, int, int);
	static pfnFindScreenMode_t s_FindScreenMode;

	HRESULT ScreenResolution(int nWidth, int nHeight) {
		if (!nWidth || !nHeight) {
			return E_INVALIDARG;
		}
		if (m_screenMode.nWidth == nWidth && m_screenMode.nHeight == nHeight) {
			return S_OK;
		}
		if (!s_FindScreenMode) {
			s_FindScreenMode = reinterpret_cast<pfnFindScreenMode_t>(
				ktscan::GetAddressByPattern("GR2D_DX8.DLL", "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 EC 68"));
			if (!s_FindScreenMode) {
				return E_FAIL;
			}
		}
		SCREENMODE mode;
		if (!m_bInitialized || !s_FindScreenMode(this, &mode, m_screenMode.bFullScreen, nWidth, nHeight, 0)) {
			return E_FAIL;
		}
		m_screenMode.nWidth = mode.nWidth;
		m_screenMode.nHeight = mode.nHeight;
		m_screenMode.bFullScreen = mode.bFullScreen;
		m_hrErrorCode = 0x88760869; // DEVICENOTRESET：让 Gr2D 每帧检查走设备重置
		return S_OK;
	}
};

CWzGr2D::pfnFindScreenMode_t CWzGr2D::s_FindScreenMode = nullptr;

} // namespace

// ---- util.h: 运行时分辨率/裁剪补偿 ----

int get_screen_width() {
	return g_nScreenWidth;
}

int get_screen_height() {
	return g_nScreenHeight;
}

int get_adjust_cy() {
	return g_nAdjustCenterY;
}

namespace {

// ---- 辅助类（CWvsContext / CInputSystem / CWvsPhysicalSpace2D / 对话框） ----

class CWvsContext : public TSingleton<CWvsContext, 0x00BE7918> {
public:
	MEMBER_AT(CTemporaryStatView, 0x2EA8, m_temporaryStatView)
};

class CInputSystem : public TSingleton<CInputSystem, 0x00BEC33C> {
public:
	MEMBER_AT(HWND, 0x0, m_hWnd)
	MEMBER_AT(IWzVector2DPtr, 0x9B0, m_pVectorCursor)
};

class CWvsPhysicalSpace2D : public TSingleton<CWvsPhysicalSpace2D, 0x00BEBFA0> {
public:
	MEMBER_AT(RECT, 0x24, m_rcMBR)
};

class CUtilDlgEx : public CWnd {
public:
	MEMBER_AT(int, 0x98, m_wndWidth)
	MEMBER_AT(int, 0x9C, m_wndHeight)
};

class CUIContextMenu : public CWnd {
public:
	MEMBER_AT(int, 0xCC, m_nBtNumber)
};

// 创建一个客户端 Shape2D#Vector2D（九锚点用）
void CreateVector2D(IWzVector2DPtr& dst) {
	dst = nullptr;
	IWzVector2D* pVec = nullptr;
	PcCreateObject<IWzVector2D>(L"Shape2D#Vector2D", &pVec, nullptr);
	if (pVec) {
		pVec->QueryInterface(__uuidof(IWzVector2D), reinterpret_cast<void**>(&dst));
		pVec->Release();
	}
}

// CWvsApp 单例（0x00BE7B38，m_hWnd 在 +0x4）
HWND GetGameHwnd() {
	void* pWvsApp = *reinterpret_cast<void**>(0x00BE7B38);
	if (!pWvsApp) {
		return nullptr;
	}
	return *reinterpret_cast<HWND*>(reinterpret_cast<uintptr_t>(pWvsApp) + 4);
}

// Gr2D_DX8 设备重置不会自动调整 OS 窗口大小，必须自己 SetWindowPos
void ResizeGameWindow(int nWidth, int nHeight) {
	HWND hWnd = GetGameHwnd();
	if (!hWnd) {
		return;
	}
	DWORD dwStyle = static_cast<DWORD>(GetWindowLongA(hWnd, GWL_STYLE));
	RECT rc = { 0, 0, nWidth, nHeight };
	AdjustWindowRect(&rc, dwStyle, FALSE);
	int nW = rc.right - rc.left;
	int nH = rc.bottom - rc.top;
	HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = { sizeof(mi) };
	if (!GetMonitorInfoA(hMon, &mi)) {
		SetWindowPos(hWnd, nullptr, 0, 0, nW, nH, SWP_NOZORDER | SWP_NOACTIVATE);
		return;
	}
	int nX = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - nW) / 2;
	int nY = mi.rcWork.top + zmax<int>(0, ((mi.rcWork.bottom - mi.rcWork.top) - nH) / 2);
	SetWindowPos(hWnd, nullptr, nX, nY, nW, nH, SWP_NOZORDER | SWP_NOACTIVATE);
}

void ReapplyInGameResPatches() {
	// 与 ezorsia Client::UpdateResolution 冲突的常量，以 kaentake 动态方案为准：
	// 1) CWvsApp::CreateWndManager：窗口管理器层尺寸固定为最大档（层只创建
	//    一次，必须容纳任何档位）
	Memory::WriteInt(0x009F7078 + 1, SCREEN_HEIGHT_MAX);
	Memory::WriteInt(0x009F707D + 1, SCREEN_WIDTH_MAX);
	// 2) CUIToolTip::SetToolTip_Equip2：边界改为读运行时全局
	Memory::WriteByte(0x008EBC3C, 0xA1);
	Memory::WriteInt(0x008EBC3C + 1, reinterpret_cast<unsigned int>(&g_nScreenWidth));
	Memory::WriteByte(0x008EBC58, 0xA1);
	Memory::WriteInt(0x008EBC58 + 1, reinterpret_cast<unsigned int>(&g_nScreenHeight));
	// 3) CUIStatusBar::CUIStatusBar：画布宽度固定最大档
	Memory::WriteInt(0x008CFD50 + 1, SCREEN_WIDTH_MAX);
	// 4) CSlideNotice（滑动公告）层尺寸固定最大档
	Memory::WriteInt(0x007E15BE + 1, SCREEN_WIDTH_MAX);
	Memory::WriteInt(0x007E16BE + 1, SCREEN_WIDTH_MAX);
	Memory::WriteInt(0x007E1E07 + 2, SCREEN_WIDTH_MAX);
	// 5) 天气层固定最大档
	Memory::WriteInt(0x0064043E + 1, 960);
	Memory::WriteInt(0x00640443 + 1, 540);
	Memory::WriteInt(0x00640599 + 2, 950);
	Memory::WriteInt(0x006405BA + 2, 1070);
	Memory::WriteInt(0x00640606 + 1, SCREEN_WIDTH_MAX);
	Memory::WriteInt(0x00640618 + 1, SCREEN_HEIGHT_MAX);
	Memory::WriteInt(0x00640626 + 1, 960);
	Memory::WriteInt(0x00640639 + 1, SCREEN_WIDTH_MAX);
	Memory::WriteInt(0x0064064B + 1, SCREEN_HEIGHT_MAX);
	Memory::WriteInt(0x00640656 + 2, -960);
	Memory::WriteInt(0x006406C3 + 1, SCREEN_WIDTH_MAX);
	Memory::WriteInt(0x006406D5 + 1, SCREEN_HEIGHT_MAX);
	Memory::WriteInt(0x006406FA + 2, 540);
	// 6) CField_LimitedView（暗边）画布固定最大档
	Memory::WriteInt(0x0055B808 + 1, SCREEN_HEIGHT_MAX);
	Memory::WriteInt(0x0055B80D + 1, SCREEN_WIDTH_MAX);
	Memory::WriteInt(0x0055B884 + 1, SCREEN_HEIGHT_MAX);
	Memory::WriteInt(0x0055BB2F + 1, -(SCREEN_HEIGHT_MAX / 2) - (SCREEN_HEIGHT_MAX - 600) / 2);
	Memory::WriteInt(0x0055BB35 + 1, -(SCREEN_WIDTH_MAX / 2));
	// 7) CUIScreenMsg 消息条宽度固定
	Memory::WriteInt(0x0089AF33 + 1, SCREEN_MESSAGE_WIDTH);
	Memory::WriteInt(0x0089B2C6 + 1, SCREEN_MESSAGE_WIDTH);
	// 8) CUISysOpt ok/cancel 按钮随新增下拉框右移
	Memory::WriteInt(0x009945BC + 1, 372);
	// 9) CUIEquip::IsMyAddon
	Memory::WriteInt(0x007FDF30 + 2, 0x5B4);
}

// ---- 分辨率切换主入口（kaentake set_screen_resolution） ----

// 前向声明（定义见后文）
void CTemporaryStatView__AdjustPosition_hook(CTemporaryStatView* pThis);
void CMapLoadable__RestoreViewRange_hook(void* pThis, void* edx);
typedef void(__thiscall* CMapLoadable_ReloadBack_t)(void* pThis);
extern CMapLoadable_ReloadBack_t CMapLoadable__ReloadBack_orig;

void set_screen_resolution(int nResolution, bool bSave) {
	if (nResolution < 0 || nResolution >= SCREEN_INFO_MAX) {
		return;
	}
	int nWidth = SCREEN_INFO_SET[nResolution].nWidth;
	int nHeight = SCREEN_INFO_SET[nResolution].nHeight;
	if (nWidth != g_nScreenWidth || nHeight != g_nScreenHeight) {
		CWzGr2D* pGr = reinterpret_cast<CWzGr2D*>(get_gr().GetInterfacePtr());
		if (pGr) {
			pGr->ScreenResolution(nWidth, nHeight);
		}
		g_nScreenWidth = nWidth;
		g_nScreenHeight = nHeight;
		g_nAdjustCenterY = (nHeight - 600) / 2;
		if (CWndMan::IsInstantiated()) {
			CWndMan::GetInstance()->ResetOrgWindow();
			sRectQuestDlg.top = nHeight / 2;
			sRectQuestDlg.left = nWidth / 2;
		}
		if (CWvsContext::IsInstantiated()) {
			CTemporaryStatView__AdjustPosition_hook(&CWvsContext::GetInstance()->m_temporaryStatView);
		}
		CField* pField = get_field();
		if (pField) {
			CMapLoadable__RestoreViewRange_hook(pField, nullptr);
			CMapLoadable__ReloadBack_orig(pField);
		}
		// ezorsia 静态补丁与新档位对齐
		Client::m_nGameWidth = nWidth;
		Client::m_nGameHeight = nHeight;
		Client::UpdateResolution();
		ReapplyInGameResPatches();
		ResizeGameWindow(nWidth, nHeight);
	}
	if (bSave) {
		g_nResolution = nResolution;
		if (CConfig::IsInstantiated()) {
			CConfig::GetInstance()->SetOpt_Int(CConfig::GLOBAL_OPT, "soScreenResolution", g_nResolution);
		}
	}
}

// ---- 阶段切换（kaentake set_stage_hook） ----

typedef void(__cdecl* set_stage_t)(CStage*, void*);
set_stage_t set_stage_orig = reinterpret_cast<set_stage_t>(0x00777347);

void __cdecl set_stage_hook(CStage* pStage, void* pParam) {
	// CField（RTTI 0x00BED758）：进地图前切换到保存的档位
	if (pStage && pStage->IsKindOf(reinterpret_cast<const CRTTI*>(0x00BED758))) {
		set_screen_resolution(g_nResolution, false);
		set_stage_orig(pStage, pParam);
		return;
	}
	set_stage_orig(pStage, pParam);
	// 非 CInterStage（RTTI 0x00BED874）：离开地图后回到初始档位
	//（kaentake 原版固定回 800x600，这里改为 config.ini 的初始分辨率，
	// 以兼容 ezorsia 的登录框/商城静态补丁）
	if (pStage && !pStage->IsKindOf(reinterpret_cast<const CRTTI*>(0x00BED874))) {
		set_screen_resolution(g_nResolutionHome, false);
	}
}

// ---- CConfig（UI 位置持久化的边界跟随分辨率） ----

typedef void(__fastcall* CConfig_GetUIWndPos_t)(void* pThis, void* edx, int nUIType, int* x, int* y, int* op);
CConfig_GetUIWndPos_t CConfig__GetUIWndPos_orig = reinterpret_cast<CConfig_GetUIWndPos_t>(0x0049F0B5);

void get_default_position(int nUIType, int* pnDefaultX, int* pnDefaultY) {
	int nDefaultX;
	int nDefaultY;
	switch (nUIType) {
	case 4:
		nDefaultX = 8;
		nDefaultY = 8;
		break;
	case 8:
		nDefaultX = 500;
		nDefaultY = 50;
		break;
	case 9:
	case 22:
		nDefaultX = (nUIType == 9) ? 250 : 500;
		nDefaultY = 100;
		break;
	case 14:
		nDefaultX = 600;
		nDefaultY = 35;
		break;
	case 15:
		nDefaultX = 730;
		nDefaultY = 400;
		break;
	case 18:
		nDefaultX = 11;
		nDefaultY = 24;
		break;
	case 20:
		nDefaultX = 720;
		nDefaultY = 80;
		break;
	case 23:
	case 31:
	case 33:
		nDefaultX = 100;
		nDefaultY = 100;
		break;
	case 24:
	case 25:
	case 26:
	case 27:
	case 29:
	case 32:
		nDefaultX = 244;
		nDefaultY = 105;
		break;
	case 30:
		nDefaultX = 769;
		nDefaultY = 343;
		break;
	default:
		nDefaultX = 8 * (3 * nUIType + 3);
		nDefaultY = nDefaultX;
		break;
	}
	if (pnDefaultX) {
		*pnDefaultX = nDefaultX;
	}
	if (pnDefaultY) {
		*pnDefaultY = nDefaultY;
	}
}

void __fastcall CConfig__GetUIWndPos_hook(void* pThis, void* edx, int nUIType, int* x, int* y, int* op) {
	CConfig__GetUIWndPos_orig(pThis, edx, nUIType, x, y, op);
	if (*x < -5 || *x > get_screen_width() - 6 || *y < -5 || *y > get_screen_height() - 6) {
		get_default_position(nUIType, x, y);
	}
}

typedef void(__fastcall* CConfig_LoadCharacter_t)(void* pThis, void* edx, int nWorldID, unsigned int dwCharacterId);
CConfig_LoadCharacter_t CConfig__LoadCharacter_orig = reinterpret_cast<CConfig_LoadCharacter_t>(0x0049D0B6);

void __fastcall CConfig__LoadCharacter_hook(void* pThis, void* edx, int nWorldID, unsigned int dwCharacterId) {
	CConfig__LoadCharacter_orig(pThis, edx, nWorldID, dwCharacterId);
	auto pConfig = reinterpret_cast<CConfig*>(pThis);
	for (size_t i = 0; i < 34; ++i) {
		int nDefaultX;
		int nDefaultY;
		get_default_position(static_cast<int>(i), &nDefaultX, &nDefaultY);

		char sBuffer[64];
		sprintf_s(sBuffer, sizeof(sBuffer), "uiWndX%d", static_cast<int>(i));
		pConfig->m_nUIWnd_X[i] = pConfig->GetOpt_Int(CConfig::GLOBAL_OPT, sBuffer, nDefaultX, -5, get_screen_width() - 6);
		sprintf_s(sBuffer, sizeof(sBuffer), "uiWndY%d", static_cast<int>(i));
		pConfig->m_nUIWnd_Y[i] = pConfig->GetOpt_Int(CConfig::GLOBAL_OPT, sBuffer, nDefaultY, -5, get_screen_height() - 6);
	}
}

typedef void(__fastcall* CConfig_LoadGlobal_t)(void* pThis, void* edx);
CConfig_LoadGlobal_t CConfig__LoadGlobal_orig = reinterpret_cast<CConfig_LoadGlobal_t>(0x0049C441);

void __fastcall CConfig__LoadGlobal_hook(void* pThis, void* edx) {
	CConfig__LoadGlobal_orig(pThis, edx);
	auto pConfig = reinterpret_cast<CConfig*>(pThis);
	// kaentake 原版固定回退 0（800x600），这里回退到 config.ini 的初始档位；
	// 无保存值（GetOpt_Int 越界钳制后仍可能返回边界值）时同样用初始档位
	int nSaved = pConfig->GetOpt_Int(CConfig::GLOBAL_OPT, "soScreenResolution", -1, 0, SCREEN_INFO_MAX - 1);
	g_nResolution = (nSaved >= 0 && nSaved <= SCREEN_INFO_MAX - 1) ? nSaved : g_nResolutionHome;
}

// 注意：CConfig::SaveGlobal(0x0049C8E7) 已被 HpMpAlert.cpp Detour，
// 这里不再重复挂钩，持久化改在 ApplySysOpt 应用时写入（见下）。

typedef void(__fastcall* CConfig_ApplySysOpt_t)(void* pThis, void* edx, void* pSysOpt, int bApplyVideo);
CConfig_ApplySysOpt_t CConfig__ApplySysOpt_orig = reinterpret_cast<CConfig_ApplySysOpt_t>(0x0049EA33);

void __fastcall CConfig__ApplySysOpt_hook(void* pThis, void* edx, void* pSysOpt, int bApplyVideo) {
	CConfig__ApplySysOpt_orig(pThis, edx, pSysOpt, bApplyVideo);
	if (pSysOpt && bApplyVideo && g_cbResolution) {
		set_screen_resolution(g_cbResolution->m_nSelect, true);
	}
}

// ---- CUISysOpt：注入分辨率下拉框 ----

typedef void(__fastcall* CUISysOpt_OnCreate_t)(void* pThis, void* edx, void* pData);
CUISysOpt_OnCreate_t CUISysOpt__OnCreate_orig = reinterpret_cast<CUISysOpt_OnCreate_t>(0x00994163);

void __fastcall CUISysOpt__OnCreate_hook(void* pThis, void* edx, void* pData) {
	CUISysOpt__OnCreate_orig(pThis, edx, pData);

	CCtrlComboBox::CREATEPARAM paramComboBox;
	paramComboBox.nBackColor = 0xFFEEEEEE;
	paramComboBox.nBackFocusedColor = 0xFFA5A198;
	paramComboBox.nBorderColor = 0xFF999999;

	g_cbResolution = new CCtrlComboBox();
	g_cbResolution->CreateCtrl(pThis, 2000, 0, 76, 338, 166, 18, &paramComboBox);
	const char* asResolution[] = {
		"800 x 600",
		"1024 x 768",
		"1280 x 720",
		"1366 x 768",
		"1600 x 900",
		"1920 x 1080",
	};
	unsigned int dwResolutionParam = 0;
	for (auto sResolution : asResolution) {
		g_cbResolution->AddItem(sResolution, dwResolutionParam++);
	}
	g_cbResolution->SetSelect(g_nResolution);
}

typedef void(__fastcall* CUISysOpt_Destructor_t)(void* pThis, void* edx);
CUISysOpt_Destructor_t CUISysOpt__Destructor_orig = reinterpret_cast<CUISysOpt_Destructor_t>(0x007FF4AA);

void __fastcall CUISysOpt__Destructor_hook(void* pThis, void* edx) {
	CUISysOpt__Destructor_orig(pThis, edx);
	g_cbResolution = nullptr;
}

// ---- CInputSystem：光标定位 ----

void CInputSystem__SetCursorVectorPos_impl(int x, int y) {
	IWzVector2DPtr& pVec = CInputSystem::GetInstance()->m_pVectorCursor;
	if (pVec) {
		pVec->RelMove(x - get_screen_width() / 2, y - get_screen_height() / 2 - get_adjust_cy(), vtEmpty, vtEmpty);
	}
}

typedef void(__fastcall* CInputSystem_SetCursorVectorPos_t)(void* pThis, void* edx, int x, int y);
CInputSystem_SetCursorVectorPos_t CInputSystem__SetCursorVectorPos_orig = reinterpret_cast<CInputSystem_SetCursorVectorPos_t>(0x0059A0CB);

void __fastcall CInputSystem__SetCursorVectorPos_hook(void* pThis, void* edx, int x, int y) {
	CInputSystem__SetCursorVectorPos_impl(x, y);
}

typedef int(__fastcall* CInputSystem_SetCursorPos_t)(void* pThis, void* edx, int x, int y);
CInputSystem_SetCursorPos_t CInputSystem__SetCursorPos_orig = reinterpret_cast<CInputSystem_SetCursorPos_t>(0x0059A887);

int __fastcall CInputSystem__SetCursorPos_hook(void* pThis, void* edx, int x, int y) {
	POINT pt;
	pt.x = zclamp(x, 0, get_screen_width());
	pt.y = zclamp(y, 0, get_screen_height());
	CInputSystem__SetCursorVectorPos_impl(pt.x, pt.y);
	HWND hWnd = CInputSystem::GetInstance()->m_hWnd;
	return ::ClientToScreen(hWnd, &pt) && ::SetCursorPos(pt.x, pt.y);
}

int CInputSystem__GetCursorPos(POINT* lpPoint) {
	auto pInput = CInputSystem::GetInstance();
	return ::GetCursorPos(lpPoint) && ::ScreenToClient(pInput->m_hWnd, lpPoint);
}

// ---- CWndMan：九锚点创建/销毁/按调用方分发 ----

typedef void(__fastcall* CWndMan_Constructor_t)(void* pThis, void* edx, HWND hWnd);
CWndMan_Constructor_t CWndMan__Constructor_orig = reinterpret_cast<CWndMan_Constructor_t>(0x009E2C42);

void __fastcall CWndMan__Constructor_hook(void* pThis, void* edx, HWND hWnd) {
	CWndMan__Constructor_orig(pThis, edx, hWnd);
	for (int i = 0; i < CWnd::UIOrigin::Origin_NUM; ++i) {
		CreateVector2D(CWndMan::ms_pOrgWindowEx[i]);
	}
	CreateVector2D(CWndMan::ms_pOrgStatusBar);
	CreateVector2D(CWndMan::ms_pOrgScreenMsg);
	CreateVector2D(CWndMan::ms_pOrgQuickSlot);
	reinterpret_cast<CWndMan*>(pThis)->ResetOrgWindow();
}

typedef void(__fastcall* CWndMan_Destructor_t)(void* pThis, void* edx);
CWndMan_Destructor_t CWndMan__Destructor_orig = reinterpret_cast<CWndMan_Destructor_t>(0x009E3026);

void __fastcall CWndMan__Destructor_hook(void* pThis, void* edx) {
	CWndMan__Destructor_orig(pThis, edx);
	for (int i = 0; i < CWnd::UIOrigin::Origin_NUM; ++i) {
		CWndMan::ms_pOrgWindowEx[i] = nullptr;
	}
	CWndMan::ms_pOrgStatusBar = nullptr;
	CWndMan::ms_pOrgScreenMsg = nullptr;
	CWndMan::ms_pOrgQuickSlot = nullptr;
}

typedef IWzVector2DPtr* (__fastcall* CWndMan_GetOrgWindow_t)(void* pThis, void* edx, IWzVector2DPtr* result);
CWndMan_GetOrgWindow_t CWndMan__GetOrgWindow_orig = reinterpret_cast<CWndMan_GetOrgWindow_t>(0x0048BBA5);

IWzVector2DPtr* __fastcall CWndMan__GetOrgWindow_hook(void* pThis, void* edx, IWzVector2DPtr* result) {
	auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
	auto pWndMan = reinterpret_cast<CWndMan*>(pThis);
	switch (ret) {
	case 0x00533B77: // CField::ShowMobHPTag
	case 0x005555B7: // CField_Dojang::OnClock
	case 0x0058C91C: // CFloatNotice::CreateFloatNotice
	case 0x006CD787: // CNoticeQuestProgress::CNoticeQuestProgress
		result->GetInterfacePtr() = pWndMan->GetOrgWindowEx(CWnd::UIOrigin::Origin_CT).GetInterfacePtr();
		break;
	case 0x009603F2: // CUserLocal::DrawCombo
		result->GetInterfacePtr() = pWndMan->GetOrgWindowEx(CWnd::UIOrigin::Origin_RT).GetInterfacePtr();
		break;
	case 0x0053502B: // CField::ShowScreenEffect
		result->GetInterfacePtr() = pWndMan->GetOrgWindowEx(CWnd::UIOrigin::Origin_CC).GetInterfacePtr();
		break;
	case 0x008DEB75: // CUIStatusBar::FlashHPBar
	case 0x008DEE11: // CUIStatusBar::FlashMPBar
		result->GetInterfacePtr() = CWndMan::ms_pOrgStatusBar.GetInterfacePtr();
		break;
	case 0x0089AF82: // CUIScreenMsg::CUIScreenMsg
		result->GetInterfacePtr() = CWndMan::ms_pOrgScreenMsg.GetInterfacePtr();
		break;
	case 0x008D15EE: // CUIStatusBar::OnCreate - CQuickSlot
		result->GetInterfacePtr() = CWndMan::ms_pOrgQuickSlot.GetInterfacePtr();
		break;
	default:
		if (ret >= 0x00554005 && ret <= 0x0055478F) { // CField_Dojang::Init
			result->GetInterfacePtr() = pWndMan->GetOrgWindowEx(CWnd::UIOrigin::Origin_CT).GetInterfacePtr();
		} else if (ret >= 0x008D01B2 && ret <= 0x008D3ADF) { // CUIStatusBar::OnCreate
			result->GetInterfacePtr() = CWndMan::ms_pOrgStatusBar.GetInterfacePtr();
		} else {
			result->GetInterfacePtr() = pWndMan->m_pOrgWindow.GetInterfacePtr();
		}
		break;
	}
	result->AddRef();
	return result;
}

// ---- CWnd::CreateWnd：按调用方把新建窗口挂到对应锚点 ----

typedef void(__fastcall* CWnd_CreateWnd_t)(void* pThis, void* edx, int l, int t, int w, int h, int z, int bScreenCoord, void* pData, int bSetFocus);
CWnd_CreateWnd_t CWnd__CreateWnd_orig = reinterpret_cast<CWnd_CreateWnd_t>(0x009DE4D2);

void CWnd__SetLayerOrigin(CWnd* pWnd, IWzVector2DPtr& pOrigin) {
	if (pWnd && pWnd->m_pLayer && pOrigin) {
		Ztl_variant_t vOrigin(static_cast<IUnknown*>(pOrigin.GetInterfacePtr()), true);
		pWnd->m_pLayer->put_origin(vOrigin);
		vOrigin.Clear();
	}
}

void __fastcall CWnd__CreateWnd_hook(void* pThis, void* edx, int l, int t, int w, int h, int z, int bScreenCoord, void* pData, int bSetFocus) {
	CWnd__CreateWnd_orig(pThis, edx, l, t, w, h, z, bScreenCoord, pData, bSetFocus);
	if (!bScreenCoord) {
		return;
	}
	auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
	auto pWnd = reinterpret_cast<CWnd*>(pThis);
	switch (ret) {
	case 0x005362BC: // CField::OnClock(CField*, int)
	case 0x0053638B: // CField::OnClock(CField*, int)
	case 0x00545D24: // CField_Battlefield::OnClock(CField*, int)
	case 0x0056042E: // CField_Massacre::OnClock(CField*, int)
	case 0x00578B30: // CField_SpaceGAGA::OnClock(CField*, int)
	case 0x00A24D15: // CWvsContext::SetEventTimer(CWvsContext*, int)
		CWnd__SetLayerOrigin(pWnd, CWndMan::ms_pOrgWindowEx[CWnd::UIOrigin::Origin_CT]);
		return;
	case 0x0045A5EF: // CAvatarMegaphone::CAvatarMegaphone
		CWnd__SetLayerOrigin(pWnd, CWndMan::ms_pOrgWindowEx[CWnd::UIOrigin::Origin_RT]);
		return;
	case 0x004EDAEB: // CDialog::CreateDlg(CDialog*, int, int, int, void*)
	case 0x004EDB9A: // CDialog::CreateDlg(CDialog*, const wchar_t*, int, void*)
	case 0x004EDAB3: // CDialog::CreateDlg(CDialog*, int, int, int, int, int, int, void*)
	case 0x007F202C: // CUISkillEffectChange::CUISkillEffectChange
	case 0x00897BD8: // CUIRevive::CUIRevive
	case 0x00994CB8: // CUIChannelShift::CUIChannelShift
		CWnd__SetLayerOrigin(pWnd, CWndMan::ms_pOrgWindowEx[CWnd::UIOrigin::Origin_CC]);
		return;
	case 0x0051FA03: // CFadeWnd::CreateFadeWnd
	case 0x008CFD65: // CUIStatusBar::CUIStatusBar
		CWnd__SetLayerOrigin(pWnd, CWndMan::ms_pOrgStatusBar);
		return;
	}
}

// ---- 窗口拖拽：吸附到屏幕边缘/相邻窗口（kaentake OnMoveWnd_hook） ----

typedef void(__thiscall* CWnd_OnMoveWnd_t)(CWnd*, int, int);
CWnd_OnMoveWnd_t CWnd__OnMoveWnd_orig = reinterpret_cast<CWnd_OnMoveWnd_t>(0x009DEB57);

void MoveWndToAbsPos(CWnd* pWnd, int l, int t) {
	IWzVector2DPtr& pAbsOrigin = CWndMan::GetInstance()->m_pOrgWindow;
	VARIANT vWndOrigin;
	VariantInit(&vWndOrigin);
	if (FAILED(pWnd->m_pLayer->get_origin(&vWndOrigin)) || V_VT(&vWndOrigin) != VT_UNKNOWN || !V_UNKNOWN(&vWndOrigin)) {
		VariantClear(&vWndOrigin);
		return;
	}
	IWzVector2DPtr pWndOrigin = V_UNKNOWN(&vWndOrigin);
	VariantClear(&vWndOrigin);
	int nOffsetX = 0;
	int nOffsetY = 0;
	if (pAbsOrigin) {
		int nAbsX = 0;
		int nAbsY = 0;
		int nWndX = 0;
		int nWndY = 0;
		pAbsOrigin->get_x(&nAbsX);
		pAbsOrigin->get_y(&nAbsY);
		pWndOrigin->get_x(&nWndX);
		pWndOrigin->get_y(&nWndY);
		nOffsetX = nAbsX - nWndX;
		nOffsetY = nAbsY - nWndY;
	}
	pWnd->m_pLayer->RelMove(l + nOffsetX, t + nOffsetY, vtEmpty, vtEmpty);
}

void __fastcall CWnd__OnMoveWnd_hook(CWnd* pThis, void* _EDX, int l, int t) {
	int nLeft = pThis->GetAbsLeft();
	int nTop = pThis->GetAbsTop();
	int nWidth = 0;
	int nHeight = 0;
	if (pThis->m_pLayer) {
		INT nW = 0;
		INT nH = 0;
		pThis->m_pLayer->get_width(&nW);
		pThis->m_pLayer->get_height(&nH);
		nWidth = nW;
		nHeight = nH;
	}
	// Save m_ptCursorRel
	POINT pt;
	CInputSystem__GetCursorPos(&pt);
	POINT ptRel;
	ptRel.x = pt.x - nLeft;
	ptRel.y = pt.y - nTop;
	if (pThis->m_ptCursorRel.x == -1 && pThis->m_ptCursorRel.y == -1) {
		pThis->m_ptCursorRel.x = ptRel.x;
		pThis->m_ptCursorRel.y = ptRel.y;
	}
	// Iterate ZList<CWnd*> for window snapping
	RECT rcThis;
	SetRect(&rcThis, nLeft, nTop, nLeft + nWidth, nTop + nHeight);
	auto pos = CWndMan::ms_lpWindow.GetHeadPosition();
	while (pos) {
		// ZList<CWnd*>：GetNext 返回 CWnd*&，pNext 本身就是 CWnd*
		auto pNext = CWndMan::ms_lpWindow.GetNext(pos);
		if (pNext == pThis || pThis->IsMyAddOn(pNext) ||
			pNext == reinterpret_cast<CWnd*>(CUIStatusBar::GetInstance())) {
			continue;
		}
		int nNextLeft = pNext->GetAbsLeft();
		int nNextTop = pNext->GetAbsTop();
		int nNextWidth = 0;
		int nNextHeight = 0;
		if (pNext->m_pLayer) {
			INT nW = 0;
			INT nH = 0;
			pNext->m_pLayer->get_width(&nW);
			pNext->m_pLayer->get_height(&nH);
			nNextWidth = nW;
			nNextHeight = nH;
		}
		RECT rcNext, rcIntersect;
		SetRect(&rcNext, nNextLeft - 10, nNextTop - 10, nNextLeft + nNextWidth + 10, nNextTop + nNextHeight + 10);
		if (!IntersectRect(&rcIntersect, &rcThis, &rcNext)) {
			continue;
		}
		if (abs(nLeft - nNextLeft - nNextWidth) <= 10) {
			MoveWndToAbsPos(pThis, nNextLeft + nNextWidth, nTop);
		}
		if (abs(nLeft - nNextLeft + nWidth) <= 10) {
			MoveWndToAbsPos(pThis, nNextLeft - nWidth, nTop);
		}
		if (abs(nTop - nNextTop - nNextHeight) <= 10) {
			MoveWndToAbsPos(pThis, nLeft, nNextTop + nNextHeight);
		}
		if (abs(nTop - nNextTop + nHeight) <= 10) {
			MoveWndToAbsPos(pThis, nLeft, nNextTop - nHeight);
		}
	}
	// Window snapping to screen border
	if (abs(nLeft) <= 10) {
		MoveWndToAbsPos(pThis, 0, nTop);
	}
	if (abs(nTop) <= 10) {
		MoveWndToAbsPos(pThis, nLeft, 0);
	}
	if (abs(nLeft + nWidth - get_screen_width()) <= 10) {
		MoveWndToAbsPos(pThis, get_screen_width() - nWidth, nTop);
	}
	if (abs(nTop + nHeight - get_screen_height()) <= 10) {
		MoveWndToAbsPos(pThis, nLeft, get_screen_height() - nHeight);
	}
	// Handle m_ptCursorRel
	if (abs(pThis->m_ptCursorRel.x - ptRel.x) > 15) {
		MoveWndToAbsPos(pThis, pt.x - pThis->m_ptCursorRel.x, nTop);
	}
	if (abs(pThis->m_ptCursorRel.y - ptRel.y) > 15) {
		MoveWndToAbsPos(pThis, nLeft, pt.y - pThis->m_ptCursorRel.y);
	}
}

// ---- CUtilDlgEx::CreateUtilDlgEx：对话框按屏幕边界重定位 ----

typedef void(__fastcall* CUtilDlgEx_CreateUtilDlgEx_t)(void* pThis, void* edx);
CUtilDlgEx_CreateUtilDlgEx_t CUtilDlgEx__CreateUtilDlgEx_orig = reinterpret_cast<CUtilDlgEx_CreateUtilDlgEx_t>(0x009A3E38);

void __fastcall CUtilDlgEx__CreateUtilDlgEx_hook(void* pThis, void* edx) {
	auto pDlg = reinterpret_cast<CUtilDlgEx*>(pThis);
	int nLeft = zclamp<int>(sRectQuestDlg.left - pDlg->m_wndWidth / 2, 0, get_screen_width());
	int nTop = zclamp<int>(sRectQuestDlg.top - pDlg->m_wndHeight / 2, 0, get_screen_height());
	// CDialog::CreateDlg(this, nLeft, nTop, m_wndWidth, m_wndHeight, 10, 1, 0, 1);
	CWnd__CreateWnd_orig(pThis, edx, nLeft, nTop, pDlg->m_wndWidth, pDlg->m_wndHeight, 10, 1, nullptr, 1);
}

// ---- CUIToolTip::MakeLayer：tooltip 最大边界处理 ----

typedef IWzCanvasPtr* (__fastcall* CUIToolTip_MakeLayer_t)(void* pThis, void* edx, IWzCanvasPtr* result, int nLeft, int nTop, int bDoubleOutline, int bLogin, int bCharToolTip, unsigned int uColor);
CUIToolTip_MakeLayer_t CUIToolTip__MakeLayer_orig = reinterpret_cast<CUIToolTip_MakeLayer_t>(0x008F3141);

IWzCanvasPtr* __fastcall CUIToolTip__MakeLayer_hook(void* pThis, void* edx, IWzCanvasPtr* result, int nLeft, int nTop, int bDoubleOutline, int bLogin, int bCharToolTip, unsigned int uColor) {
	CUIToolTip__MakeLayer_orig(pThis, edx, result, nLeft, nTop, bDoubleOutline, bLogin, bCharToolTip, uColor);
	auto pTip = reinterpret_cast<CUIToolTip*>(pThis);
	if (!bCharToolTip) {
		if (nLeft < 0) {
			nLeft = 0;
		}
		if (nTop < 0) {
			nTop = 0;
		}
		int nBoundX = get_screen_width() - 1;
		if (nLeft + pTip->m_nWidth > nBoundX) {
			nLeft = nBoundX - pTip->m_nWidth;
		}
		int nBoundY = get_screen_height() - 1;
		if (nTop + pTip->m_nHeight > nBoundY) {
			nTop = nBoundY - pTip->m_nHeight;
		}
		if (pTip->m_pLayer) {
			pTip->m_pLayer->RelMove(nLeft, nTop, vtEmpty, vtEmpty);
		}
	}
	return result;
}

// ---- CUIContextMenu：右键菜单重定位（PatchCall 到 CreateDlg） ----

void __fastcall CUIContextMenu__CreateDlg_hook(void* pThis, void* edx, int l, int t, int w, int h, int z, int bScreenCoord, void* pData) {
	POINT ptCursor;
	CInputSystem__GetCursorPos(&ptCursor);
	auto pMenu = reinterpret_cast<CUIContextMenu*>(pThis);
	int nWidth = 100;
	int nHeight = 15 * (pMenu->m_nBtNumber + 2);
	if (ptCursor.x + nWidth > get_screen_width()) {
		ptCursor.x = get_screen_width() - nWidth;
	}
	if (ptCursor.y + nHeight > get_screen_height()) {
		ptCursor.y = get_screen_height() - nHeight;
	}
	CWnd__CreateWnd_orig(pThis, edx, ptCursor.x, ptCursor.y, nWidth, nHeight, 10, 1, pData, 1);
}

// ---- CTemporaryStatView：buff 图标/tooltip 定位 ----

typedef void(__fastcall* CTemporaryStatView_AdjustPosition_t)(void* pThis, void* edx);
CTemporaryStatView_AdjustPosition_t CTemporaryStatView__AdjustPosition_orig = reinterpret_cast<CTemporaryStatView_AdjustPosition_t>(0x007B2BB0);

void CTemporaryStatView__AdjustPosition_hook(CTemporaryStatView* pThis) {
	int nOffsetX = (get_screen_width() / 2) - 3 + (-32 * static_cast<int>(pThis->m_lTemporaryStat.GetCount()));
	int nOffsetY = (get_screen_height() / 2) + get_adjust_cy() - 23;
	auto pos = pThis->m_lTemporaryStat.GetHeadPosition();
	while (pos) {
		auto& pNext = pThis->m_lTemporaryStat.GetNext(pos);
		int nW = 0;
		int nH = 0;
		int nWs = 0;
		int nHs = 0;
		if (pNext->pLayer) {
			INT uW = 0;
			INT uH = 0;
			pNext->pLayer->get_width(&uW);
			pNext->pLayer->get_height(&uH);
			nW = uW;
			nH = uH;
			pNext->pLayer->RelMove((32 - nW) / 2 + nOffsetX, (32 - nH) / 2 - nOffsetY, vtEmpty, vtEmpty);
		}
		if (pNext->pLayerShadow) {
			INT uW = 0;
			INT uH = 0;
			pNext->pLayerShadow->get_width(&uW);
			pNext->pLayerShadow->get_height(&uH);
			nWs = uW;
			nHs = uH;
			pNext->pLayerShadow->RelMove((32 - nWs) / 2 + nOffsetX, (32 - nHs) / 2 - nOffsetY, vtEmpty, vtEmpty);
		}
		nOffsetX += 32;
	}
}

void __fastcall CTemporaryStatView__AdjustPosition_hook_fwd(void* pThis, void* edx) {
	CTemporaryStatView__AdjustPosition_hook(reinterpret_cast<CTemporaryStatView*>(pThis));
}

typedef int(__fastcall* CTemporaryStatView_ShowToolTip_t)(void* pThis, void* edx, CUIToolTip* uiToolTip, const POINT& ptCursor, int rx, int ry);
CTemporaryStatView_ShowToolTip_t CTemporaryStatView__ShowToolTip_orig = reinterpret_cast<CTemporaryStatView_ShowToolTip_t>(0x007B2E58);

int __fastcall CTemporaryStatView__ShowToolTip_hook(void* pThis, void* edx, CUIToolTip* uiToolTip, const POINT& ptCursor, int rx, int ry) {
	POINT ptAdjust = { ptCursor.x + 800 - get_screen_width(), ptCursor.y };
	return CTemporaryStatView__ShowToolTip_orig(pThis, edx, uiToolTip, ptAdjust, rx, ry);
}

typedef int(__fastcall* CTemporaryStatView_FindIcon_t)(void* pThis, void* edx, const POINT& ptCursor, int& nType, int& nID);
CTemporaryStatView_FindIcon_t CTemporaryStatView__FindIcon_orig = reinterpret_cast<CTemporaryStatView_FindIcon_t>(0x007B3055);

int __fastcall CTemporaryStatView__FindIcon_hook(void* pThis, void* edx, const POINT& ptCursor, int& nType, int& nID) {
	POINT ptAdjust = { ptCursor.x + 800 - get_screen_width(), ptCursor.y };
	return CTemporaryStatView__FindIcon_orig(pThis, edx, ptAdjust, nType, nID);
}

// ---- CUIStatusBar：快捷栏索引换算 ----

typedef int(__fastcall* CUIStatusBar_GetShortCutIndexByPos_t)(void* pThis, void* edx, int x, int y);
CUIStatusBar_GetShortCutIndexByPos_t CUIStatusBar__GetShortCutIndexByPos_orig = reinterpret_cast<CUIStatusBar_GetShortCutIndexByPos_t>(0x008DE8D5);

int __fastcall CUIStatusBar__GetShortCutIndexByPos_hook(void* pThis, void* edx, int x, int y) {
	int nSbX = 0;
	int nSbY = 0;
	int nQsX = 0;
	int nQsY = 0;
	if (CWndMan::ms_pOrgStatusBar) {
		CWndMan::ms_pOrgStatusBar->get_x(&nSbX);
		CWndMan::ms_pOrgStatusBar->get_y(&nSbY);
	}
	if (CWndMan::ms_pOrgQuickSlot) {
		CWndMan::ms_pOrgQuickSlot->get_x(&nQsX);
		CWndMan::ms_pOrgQuickSlot->get_y(&nQsY);
	}
	x = x + nSbX - nQsX;
	y = y + nSbY - nQsY;
	return CUIStatusBar__GetShortCutIndexByPos_orig(pThis, edx, x, y);
}

// ---- CMapLoadable::RestoreViewRange：视野范围跟随分辨率 ----

typedef void(__fastcall* CMapLoadable_RestoreViewRange_t)(void* pThis, void* edx);
CMapLoadable_RestoreViewRange_t CMapLoadable__RestoreViewRange_orig = reinterpret_cast<CMapLoadable_RestoreViewRange_t>(0x00641EF1);

// 从地图属性里取 int（VRLeft 等），不存在时返回默认值
int get_fieldprop_int(IWzPropertyPtr& pProp, const wchar_t* sName, int nDefault) {
	Ztl_variant_t v;
	if (!pProp || FAILED(pProp->get_item(Ztl_bstr_t(sName), &v))) {
		return nDefault;
	}
	return get_int32(v, nDefault);
}

void CMapLoadable__RestoreViewRange_hook(void* pThis, void* edx) {
	auto pMap = reinterpret_cast<CMapLoadable*>(pThis);
	auto pSpace2D = CWvsPhysicalSpace2D::GetInstance();
	int nW2 = get_screen_width() / 2;
	int nH2 = get_screen_height() / 2;
	int nVRLeft = get_fieldprop_int(pMap->m_pPropFieldInfo, L"VRLeft", pSpace2D->m_rcMBR.left - 20);
	int nVRTop = get_fieldprop_int(pMap->m_pPropFieldInfo, L"VRTop", pSpace2D->m_rcMBR.top - 60);
	int nVRRight = get_fieldprop_int(pMap->m_pPropFieldInfo, L"VRRight", pSpace2D->m_rcMBR.right + 20);
	int nVRBottom = get_fieldprop_int(pMap->m_pPropFieldInfo, L"VRBottom", pSpace2D->m_rcMBR.bottom + 190);
	pMap->m_rcViewRange.left = nVRLeft + nW2;
	pMap->m_rcViewRange.top = nVRTop + nH2;
	pMap->m_rcViewRange.right = nVRRight - nW2;
	pMap->m_rcViewRange.bottom = nVRBottom - nH2;
	if (pMap->m_rcViewRange.right - pMap->m_rcViewRange.left <= 0) {
		int mid = (pMap->m_rcViewRange.left + pMap->m_rcViewRange.right) / 2;
		pMap->m_rcViewRange.left = mid;
		pMap->m_rcViewRange.right = mid;
	}
	if (pMap->m_rcViewRange.bottom - pMap->m_rcViewRange.top <= 0) {
		int mid = (pMap->m_rcViewRange.top + pMap->m_rcViewRange.bottom) / 2;
		pMap->m_rcViewRange.top = mid;
		pMap->m_rcViewRange.bottom = mid;
	}
	pMap->m_rcViewRange.top += get_adjust_cy();
	pMap->m_rcViewRange.bottom += get_adjust_cy();
}

// CMapLoadable::ReloadBack
typedef void(__thiscall* CMapLoadable_ReloadBack_t)(void* pThis);
CMapLoadable_ReloadBack_t CMapLoadable__ReloadBack_orig = reinterpret_cast<CMapLoadable_ReloadBack_t>(0x00644491);

// ---- CMapLoadable::MakeGrid：格线 Y 裁剪补偿（MSVC 内联 asm） ----

static uintptr_t CMapLoadable__MakeGrid_ret = 0x0063EADC;

void __declspec(naked) CMapLoadable__MakeGrid_hook() {
	__asm {
		sar     ecx, 1                          // 被覆盖的指令
		neg     eax
		sub     eax, ecx
		sub     eax, g_nAdjustCenterY           // eax -= g_nAdjustCenterY
		jmp     [CMapLoadable__MakeGrid_ret]
	}
}

// ---- PatchCall 系列：天气 / 暗边 / 屏幕消息（直接改参数后调虚表方法） ----

HRESULT __fastcall CMapLoadable__raw_WrapClip_hook(IWzVector2D* pThis, void* edx, VARIANT pOrigin, int nWrapLeft, int nWrapTop, unsigned int uWrapWidth, unsigned int uWrapHeight, VARIANT bClip) {
	nWrapLeft = nWrapLeft + 400 - (SCREEN_WIDTH_MAX / 2);
	nWrapTop = nWrapTop + 300 - (SCREEN_HEIGHT_MAX / 2) - ((SCREEN_HEIGHT_MAX - 600) / 2);
	uWrapWidth = uWrapWidth - 800 + SCREEN_WIDTH_MAX;
	uWrapHeight = uWrapHeight - 600 + SCREEN_HEIGHT_MAX;
	return pThis->WrapClip(pOrigin, nWrapLeft, nWrapTop, uWrapWidth, uWrapHeight, bClip);
}

HRESULT __fastcall CField_LimitedView__raw_Copy_hook(IWzCanvas* pThis, void* edx, int nDstLeft, int nDstTop, IWzCanvas* pSource, VARIANT nAlpha) {
	nDstLeft = nDstLeft + (SCREEN_WIDTH_MAX / 2) - 400;
	nDstTop = nDstTop + (SCREEN_HEIGHT_MAX / 2) - 300 + ((SCREEN_HEIGHT_MAX - 600) / 2);
	return pThis->Copy(nDstLeft, nDstTop, pSource, nAlpha);
}

HRESULT __fastcall CField_LimitedView__CopyEx_hook(IWzCanvas* pThis, void* edx, int nDstLeft, int nDstTop, IWzCanvas* pSource, CANVAS_ALPHATYPE nAlpha, int nWidth, int nHeight, int nSrcLeft, int nSrcTop, int nSrcWidth, int nSrcHeight, VARIANT pAdjust) {
	nDstLeft = nDstLeft + (SCREEN_WIDTH_MAX / 2) - 400;
	nDstTop = nDstTop + (SCREEN_HEIGHT_MAX / 2) - 300 + ((SCREEN_HEIGHT_MAX - 600) / 2);
	return pThis->CopyEx(nDstLeft, nDstTop, pSource, nAlpha, nWidth, nHeight, nSrcLeft, nSrcTop, nSrcWidth, nSrcHeight, pAdjust);
}

HRESULT __fastcall CUIScreenMsg__raw_RelMove_hook(IWzVector2D* pThis, void* edx, int nX, int nY, VARIANT nTime, VARIANT nType) {
	nX = nX + 290 - SCREEN_MESSAGE_WIDTH;
	if (get_screen_width() > 800 && CUIStatusBar::IsInstantiated() && CUIStatusBar::GetInstance()->m_bQuickSlotUp) {
		nY = nY + 443 - 365;
	}
	return pThis->RelMove(nX, nY, nTime, nType);
}

HRESULT __fastcall CUIScreenMsg__RelMove_hook(IWzVector2D* pThis, void* edx, int nX, int nY, const Ztl_variant_t& nTime, const Ztl_variant_t& nType) {
	nX = nX + 290 - SCREEN_MESSAGE_WIDTH;
	if (get_screen_width() > 800 && CUIStatusBar::IsInstantiated() && CUIStatusBar::GetInstance()->m_bQuickSlotUp) {
		nY = nY + 443 - 365;
	}
	return pThis->RelMove(nX, nY, nTime, nType);
}

// ---- CField::ShowMobHPTag：Boss 血条 nLeft（与 ezorsia MobHpTag 兼容） ----

int __stdcall CField__ShowMobHPTag_hook1() {
	// 迷你地图宽度的等价读取：CUIMiniMap 单例 0x00BED788，m_width 在 CWnd::0x24
	auto pMiniMap = *reinterpret_cast<void**>(0x00BED788);
	if (pMiniMap && g_nResolution == 0) {
		return *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pMiniMap) + 0x24);
	}
	return 0;
}

// ---- CWzGr2D__AdjustCenterY：地图取景 Y 补偿（MSVC 内联 asm） ----

static uintptr_t CWzGr2D__AdjustCenterY_ret = 0;

void __declspec(naked) CWzGr2D__AdjustCenterY_hook() {
	__asm {
		pushfd                                  // 保存标志位
		sub     ecx, g_nAdjustCenterY           // nCenterY -= nAdjustCenterY
		mov     [ebp - 0x14], ecx
		lea     edx, [esi + 0xC4]               // 被覆盖的指令
		popfd                                   // 恢复标志位
		jmp     [CWzGr2D__AdjustCenterY_ret]
	}
}

// ---- 常量补丁工具（kaentake Patch1/Patch4/PatchCall/PatchJmp） ----

void Patch1(unsigned int uAddress, unsigned char uValue) {
	Memory::WriteByte(uAddress, uValue);
}

void Patch4(unsigned int uAddress, unsigned int uValue) {
	Memory::WriteInt(uAddress, uValue);
}

void PatchCall(unsigned int uAddress, const void* pDetour, unsigned int uSize = 5) {
	auto pfnTarget = reinterpret_cast<unsigned char*>(uAddress);
	unsigned int uRelative = reinterpret_cast<unsigned int>(pDetour) - (uAddress + 5);
	Memory::WriteByte(uAddress, 0xE8);
	for (unsigned int i = 0; i < uSize - 5; ++i) {
		Memory::WriteByte(uAddress + 5 + i, 0x90); // NOP 剩余部分
	}
	Memory::WriteInt(uAddress + 1, uRelative);
}

void PatchJmp(unsigned int uAddress, const void* pDetour) {
	unsigned int uRelative = reinterpret_cast<unsigned int>(pDetour) - (uAddress + 5);
	Memory::WriteByte(uAddress, 0xE9);
	Memory::WriteInt(uAddress + 1, uRelative);
}

} // namespace

// ---- 模块入口 ----

namespace InGameRes {

void SetInitialResolution(int nWidth, int nHeight) {
	g_nScreenWidth = nWidth;
	g_nScreenHeight = nHeight;
	g_nAdjustCenterY = (nHeight - 600) / 2;
	// 初始档位 = 最接近 config.ini 分辨率的档位
	int nBest = 0;
	long nBestDelta = MAXLONG;
	for (int i = 0; i < SCREEN_INFO_MAX; ++i) {
		long nDelta = labs(static_cast<long>(SCREEN_INFO_SET[i].nWidth) * SCREEN_INFO_SET[i].nHeight -
			static_cast<long>(nWidth) * nHeight);
		if (nDelta < nBestDelta) {
			nBestDelta = nDelta;
			nBest = i;
		}
	}
	g_nResolutionHome = nBest;
}

int GetScreenWidth() {
	return g_nScreenWidth;
}

int GetScreenHeight() {
	return g_nScreenHeight;
}

void Install(bool bEnable) {
	if (!bEnable) {
		return;
	}

	// ---- 钩子 ----
	Memory::SetHook(true, reinterpret_cast<void**>(&set_stage_orig), set_stage_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CConfig__GetUIWndPos_orig), CConfig__GetUIWndPos_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CConfig__LoadCharacter_orig), CConfig__LoadCharacter_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CConfig__LoadGlobal_orig), CConfig__LoadGlobal_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CConfig__ApplySysOpt_orig), CConfig__ApplySysOpt_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CUISysOpt__OnCreate_orig), CUISysOpt__OnCreate_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CUISysOpt__Destructor_orig), CUISysOpt__Destructor_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CInputSystem__SetCursorVectorPos_orig), CInputSystem__SetCursorVectorPos_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CInputSystem__SetCursorPos_orig), CInputSystem__SetCursorPos_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CWndMan__Constructor_orig), CWndMan__Constructor_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CWndMan__Destructor_orig), CWndMan__Destructor_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CWndMan__GetOrgWindow_orig), CWndMan__GetOrgWindow_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CWnd__CreateWnd_orig), CWnd__CreateWnd_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CWnd__OnMoveWnd_orig), CWnd__OnMoveWnd_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CUtilDlgEx__CreateUtilDlgEx_orig), CUtilDlgEx__CreateUtilDlgEx_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CUIToolTip__MakeLayer_orig), CUIToolTip__MakeLayer_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CTemporaryStatView__AdjustPosition_orig), CTemporaryStatView__AdjustPosition_hook_fwd);
	Memory::SetHook(true, reinterpret_cast<void**>(&CTemporaryStatView__ShowToolTip_orig), CTemporaryStatView__ShowToolTip_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CTemporaryStatView__FindIcon_orig), CTemporaryStatView__FindIcon_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CUIStatusBar__GetShortCutIndexByPos_orig), CUIStatusBar__GetShortCutIndexByPos_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CMapLoadable__RestoreViewRange_orig), CMapLoadable__RestoreViewRange_hook);

	// ---- 常量补丁（须在 Client::UpdateResolution() 之后执行） ----
	ReapplyInGameResPatches();

	// CUIContextMenu::CUIContextMenu - reposition right click menu
	PatchCall(0x009966E3, CUIContextMenu__CreateDlg_hook);

	// CUIToolTip::SetToolTip_Equip2 - another bounds check（已在 Reapply 中处理）

	// CMapLoadable - handle view range
	PatchJmp(0x0063EAD6, CMapLoadable__MakeGrid_hook);

	// CMapLoadable::TransientLayer_Weather - weather effects
	PatchCall(0x0064106B, CMapLoadable__raw_WrapClip_hook, 6);
	// CField_LimitedView::DrawViewRange
	PatchCall(0x0055BEFE, CField_LimitedView__raw_Copy_hook, 6);
	PatchCall(0x0055C08E, CField_LimitedView__CopyEx_hook);
	PatchCall(0x0055C1DD, CField_LimitedView__CopyEx_hook);
	// CUIScreenMsg
	PatchCall(0x0089B6FE, CUIScreenMsg__raw_RelMove_hook, 6);
	PatchCall(0x0089BA13, CUIScreenMsg__RelMove_hook);
	// CField::ShowMobHPTag - boss hp bar position
	PatchCall(0x00533705, CField__ShowMobHPTag_hook1, 15);

	// Gr2D_DX8.dll - AdjustCenterY
	uintptr_t uAdjustCenterY = reinterpret_cast<uintptr_t>(
		ktscan::GetAddressByPattern("GR2D_DX8.DLL", "8D 96 C4 00 00 00"));
	if (uAdjustCenterY) {
		CWzGr2D__AdjustCenterY_ret = uAdjustCenterY + 6;
		PatchJmp(uAdjustCenterY, CWzGr2D__AdjustCenterY_hook);
	}

	OutputDebugStringA("InGameRes: installed\n");
}

} // namespace InGameRes
