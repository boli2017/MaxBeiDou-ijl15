#include "stdafx.h"
#include "InGameRes.h"
#include "Client.h"
#include "Memory.h"
#include "detours.h"

#include <windows.h>
#include <comdef.h>   // VARIANT（RelMove 的后两个参数）
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// 实现说明（所有客户端地址均已对照仓库内 BeiDou.exe / Gr2D_DX8.dll 反汇编验证）
//
// ezorsia 的分辨率方案在 DLL 加载时执行一次 Client::UpdateResolution()，把
// 宽高写死进客户端代码。本模块的切换流程：
//
//   1) 预检查：目标分辨率在显卡模式表里找得到吗？（CWzGr2D::FindScreenMode，
//      找不到就整体放弃，保持原分辨率，避免补丁和设备大小不一致）
//   2) Client::m_nGameWidth/Height 换成新值，重跑 Client::UpdateResolution()、
//      Client::WorldMap()、Client::MoreHook()——所有 WriteInt 补丁重写一遍，
//      代码洞穴（codecaves.h 里的 __declspec(naked) 函数）参数全局变量重新
//      赋值，都刷新到新分辨率；
//   3) CWzGr2D 换显示模式：FindScreenMode 找到新模式后整体拷进
//      m_screenMode，并把 m_hrErrorCode 置为 D3DERR_DEVICENOTRESET，
//      客户端自己的渲染循环下一帧看到错误码会重置 D3D 设备（这条错误码
//      路径是客户端自带的，kaentake 同样用法）。重置时 ModeTableFix 挂的
//      IDirect3DDevice8::Reset 钩子会把后备缓冲区尺寸钉成新的
//      Client::m_nGameWidth/Height（见 ModeTableFix.h），保证设备尺寸和
//      第 2 步重写的补丁常量一致；
//   4) CWndMan::m_pOrgWindow RelMove 到 (-w/2, -h/2)：所有窗口图层
//      （CWnd::CreateWnd 里 put_origin(m_pOrgWindow)）都以它为 origin，
//      锚点位移等于整个 UI 平移，按设计坐标定位的元素自动贴合新屏幕；
//   5) 同步主窗口大小（D3D 后备缓冲区变了，窗口客户区也要跟着变）；
//   6) CMapLoadable::RestoreViewRange + ReloadBack 重算地图视野。
//
// 触发：CWndMan::s_Update 每帧被 CWvsApp::CallUpdate 调用，在里面轮询热键
// （默认 Ctrl+Enter），边沿触发循环切档。修饰键用位掩码配置，
// 主键与所有要求的修饰键同时按下才算触发。
// ---------------------------------------------------------------------------

namespace InGameRes {

// ---- 客户端地址 ----
// CWvsApp 单例（CWvsApp::CWvsApp 里 mov [0x00BE7B38], ecx），m_hWnd 在 +0x4
static const DWORD dwCWvsApp = 0x00BE7B38;
static const DWORD dwCWvsApp_m_hWnd = 0x4;

// CWndMan 单例（CWnd::CreateWnd 里 mov eax,[0x00BEC20C]），m_pOrgWindow 在 +0xDC
static const DWORD dwCWndMan = 0x00BEC20C;
static const DWORD dwCWndMan_m_pOrgWindow = 0xDC;

// IWzGr2D 全局指针（ezorsia 的 RefreshRate 钩子用的就是它）
static const DWORD dwGr2DPtr = 0x00BF14EC;

// CWzGr2D 对象内偏移（Gr2D_DX8.dll 里访问 [ebx+0x20]/[ebx+0x90]/[ebx+0x94] 的代码验证）：
//   m_screenMode +0x20，长 0x5C：nWidth +0x20 / nHeight +0x24 / bFullScreen +0x78
//   m_bInitialized +0x90，m_hrErrorCode +0x94
static const DWORD dwGr2D_m_screenMode = 0x20;
static const DWORD dwGr2D_m_bInitialized = 0x90;
static const DWORD dwGr2D_m_hrErrorCode = 0x94;

// IWzVector2D::RelMove：__stdcall(void* pThis, int x, int y, VARIANT vtTime, VARIANT vtType)，
// 位于 IWzVector2D 子虚表槽位 0x90/4（客户端自己就是这么调的：
// mov ecx,[obj]; call [ecx+0x90]。time/type 为 VT_EMPTY/VT_ERROR 时是绝对定位，
// Shape2D.dll 里对应函数以 ret 0x2c 结尾、直接 mov [this+0x1c],x / mov [this+0x20],y）。
static const int nRelMoveSlot = 0x90 / 4;

// CMapLoadable::RestoreViewRange / CMapLoadable::ReloadBack / get_field()
static const DWORD dwRestoreViewRange = 0x00641EF1;
static const DWORD dwReloadBack = 0x00644491;
static const DWORD dwGetField = 0x00437A0C;

// CWndMan::s_Update：每帧调用（CWvsApp::CallUpdate 循环里），用来轮询热键
static const DWORD dwCWndMan_s_Update = 0x009E47C3;

// CWzGr2D::FindScreenMode 入口特征（kaentake 同款；已在本仓库 Gr2D_DX8.dll
// 里验证是唯一命中）：int __thiscall FindScreenMode(this, SCREENMODE* pOut,
// int nFormatSel, int nWidth, int nHeight, int nRefreshRate)
static const char* const sFindScreenModePattern = "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 EC 68";

static const int SCREENMODE_SIZE = 0x5C;
static const int D3DERR_DEVICENOTRESET = 0x88760869; // 客户端设备重置的错误码

typedef int(__thiscall* FindScreenMode_t)(void* pThis, void* pOutMode, int nFormatSel, int nWidth, int nHeight, int nRefreshRate);
static FindScreenMode_t g_pFindScreenMode = nullptr;

// ---- 全局状态（默认值与 config.ini 里的默认值保持一致，INI 缺失时走这里）----
bool g_bEnabled = true;
bool g_bSaveToConfig = true;
int g_nHotkey = VK_RETURN;                                       // Enter
int g_nHotkeyMod = HOTKEY_MOD_CTRL;                              // Ctrl+Enter

// 默认预设列表（config.ini 的 InGameResList 可覆盖）
const Resolution kDefaultResolutions[] = {
	{ 800, 600 }, { 1024, 768 }, { 1280, 720 },
	{ 1366, 768 }, { 1600, 900 }, { 1920, 1080 },
};
std::vector<Resolution> g_aResolutions(kDefaultResolutions, kDefaultResolutions + sizeof(kDefaultResolutions) / sizeof(kDefaultResolutions[0]));
int g_nCurrentIndex = -1;

static bool g_bHotkeyDown = false;
static int g_nPendingIndex = -1;
static void(__cdecl* g_pCWndMan_s_Update_orig)() = (void(__cdecl*)())dwCWndMan_s_Update;


// ---------------------------------------------------------------------------
// 模式扫描（"??" 通配），用来在 GR2D_DX8.DLL 里定位 FindScreenMode
// ---------------------------------------------------------------------------
static void* FindPattern(HMODULE hModule, const char* sPattern)
{
	if (!hModule || !sPattern) {
		return nullptr;
	}

	// 自己解析 PE 头拿镜像范围，免得为 GetModuleInformation 多链一个 psapi
	auto* pDosHeader = (IMAGE_DOS_HEADER*)hModule;
	if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
		return nullptr;
	}
	auto* pNtHeader = (IMAGE_NT_HEADERS*)((char*)hModule + pDosHeader->e_lfanew);
	if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) {
		return nullptr;
	}

	unsigned char* pBase = (unsigned char*)hModule;
	size_t uImageSize = pNtHeader->OptionalHeader.SizeOfImage;

	// 解析 "B8 ?? ?? ?? ?? E8 ..." 为字节+掩码
	unsigned char abPattern[128];
	unsigned char abMask[128];
	size_t uPatternSize = 0;
	const char* p = sPattern;
	while (*p && uPatternSize < sizeof(abPattern)) {
		if (*p == ' ') {
			++p;
			continue;
		}
		auto hexValue = [](char c, unsigned char* out) -> bool {
			if (c >= '0' && c <= '9') { *out = c - '0'; return true; }
			if (c >= 'A' && c <= 'F') { *out = 10 + (c - 'A'); return true; }
			if (c >= 'a' && c <= 'f') { *out = 10 + (c - 'a'); return true; }
			return false;
		};
		if (p[0] == '?' && p[1] == '?') {
			abMask[uPatternSize] = 0x00;
			p += 2;
		} else {
			unsigned char high, low;
			if (!hexValue(p[0], &high) || !hexValue(p[1], &low)) {
				return nullptr;
			}
			abPattern[uPatternSize] = (high << 4) | low;
			abMask[uPatternSize] = 0xFF;
			p += 2;
		}
		++uPatternSize;
	}
	if (uPatternSize == 0 || uImageSize < uPatternSize) {
		return nullptr;
	}

	for (size_t i = 0; i <= uImageSize - uPatternSize; ++i) {
		size_t j = 0;
		for (; j < uPatternSize; ++j) {
			if ((pBase[i + j] & abMask[j]) != (abPattern[j] & abMask[j])) {
				break;
			}
		}
		if (j == uPatternSize) {
			return pBase + i;
		}
	}
	return nullptr;
}

static bool EnsureFindScreenMode()
{
	if (g_pFindScreenMode) {
		return true;
	}
	HMODULE hGr2D = GetModuleHandleA("GR2D_DX8.DLL");
	if (!hGr2D) {
		return false;
	}
	g_pFindScreenMode = (FindScreenMode_t)FindPattern(hGr2D, sFindScreenModePattern);
	return g_pFindScreenMode != nullptr;
}

// ---------------------------------------------------------------------------
// D3D 显示模式切换
// ---------------------------------------------------------------------------

// 只查模式表不改状态，成功时 pabMode 填入 0x5C 字节的 SCREENMODE
static bool TryFindScreenMode(int nWidth, int nHeight, unsigned char* pabMode)
{
	void** ppGr2D = (void**)dwGr2DPtr;
	if (!ppGr2D) {
		return false;
	}
	unsigned char* pGr2D = (unsigned char*)*ppGr2D;
	if (!pGr2D) {
		return false;
	}
	// 设备还没初始化（InitializeGr2D 之前）不切
	if (!*(int*)(pGr2D + dwGr2D_m_bInitialized)) {
		return false;
	}
	if (!EnsureFindScreenMode()) {
		return false;
	}
	int nFormatSel = *(int*)(pGr2D + dwGr2D_m_screenMode + 0x58); // m_screenMode.bFullScreen
	memset(pabMode, 0, SCREENMODE_SIZE);
	return g_pFindScreenMode(pGr2D, pabMode, nFormatSel, nWidth, nHeight, 0) != 0;
}

// 写入新模式并请求客户端重置设备（错误码由客户端自己的渲染循环处理）
static void ApplyScreenMode(const unsigned char* pabMode)
{
	unsigned char* pGr2D = *(unsigned char**)(void*)dwGr2DPtr;
	memcpy(pGr2D + dwGr2D_m_screenMode, pabMode, SCREENMODE_SIZE);
	*(int*)(pGr2D + dwGr2D_m_hrErrorCode) = D3DERR_DEVICENOTRESET;
}

// ---------------------------------------------------------------------------
// UI 锚点 / 窗口 / 视野
// ---------------------------------------------------------------------------

// 所有窗口图层都以 CWndMan::m_pOrgWindow 为 origin，把它移到新屏幕中心即可整体平移 UI
static bool MoveOrgWindow(int nWidth, int nHeight)
{
	void* pWndMan = *(void**)(void*)dwCWndMan;
	if (!pWndMan) {
		return false;
	}
	void* pOrgWindow = *(void**)((char*)pWndMan + dwCWndMan_m_pOrgWindow);
	if (!pOrgWindow) {
		return false;
	}
	void** pVTable = *(void***)pOrgWindow;
	if (!pVTable || !pVTable[nRelMoveSlot]) {
		return false;
	}
	typedef HRESULT(__stdcall* RelMove_t)(void*, int, int, VARIANT, VARIANT);
	RelMove_t pRelMove = (RelMove_t)pVTable[nRelMoveSlot];

	VARIANT vtEmpty;
	memset(&vtEmpty, 0, sizeof(vtEmpty)); // VT_EMPTY：time/type 缺省，走绝对定位分支
	bool bOk = false;
	__try {
		pRelMove(pOrgWindow, -(nWidth / 2), -(nHeight / 2), vtEmpty, vtEmpty);
		bOk = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		bOk = false;
	}
	return bOk;
}

// D3D 后备缓冲区换了尺寸，窗口客户区也要跟着换（按当前窗口样式算非客户区）
static void ResizeMainWindow(int nWidth, int nHeight)
{
	void* pApp = *(void**)(void*)dwCWvsApp;
	if (!pApp) {
		return;
	}
	HWND hWnd = *(HWND*)((char*)pApp + dwCWvsApp_m_hWnd);
	if (!hWnd || !IsWindow(hWnd)) {
		return;
	}

	RECT rcClient = { 0, 0, nWidth, nHeight };
	DWORD dwStyle = (DWORD)GetWindowLongPtrA(hWnd, GWL_STYLE);
	DWORD dwExStyle = (DWORD)GetWindowLongPtrA(hWnd, GWL_EXSTYLE);
	if (!AdjustWindowRectEx(&rcClient, dwStyle, FALSE, dwExStyle)) {
		return;
	}
	int nWindowWidth = rcClient.right - rcClient.left;
	int nWindowHeight = rcClient.bottom - rcClient.top;

	RECT rcCurrent;
	if (!GetWindowRect(hWnd, &rcCurrent)) {
		return;
	}
	if (rcCurrent.right - rcCurrent.left == nWindowWidth && rcCurrent.bottom - rcCurrent.top == nWindowHeight) {
		return; // 大小已经一致（客户端自己的重置流程可能已经改过）
	}

	// 保持在当前显示器的工作区里居中
	int x = rcCurrent.left;
	int y = rcCurrent.top;
	MONITORINFO mi;
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoA(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
		x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - nWindowWidth) / 2;
		y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - nWindowHeight) / 2;
	}
	SetWindowPos(hWnd, nullptr, x, y, nWindowWidth, nWindowHeight, SWP_NOZORDER);
}

// 重算地图视野范围（CMapLoadable::RestoreViewRange 里读的就是刚刷新过的补丁常量）
static void RefreshViewRange()
{
	typedef void* (__cdecl* GetField_t)();
	typedef void(__thiscall* RestoreViewRange_t)(void*);
	typedef void(__thiscall* ReloadBack_t)(void*);

	__try {
		void* pField = ((GetField_t)dwGetField)();
		if (!pField) {
			return; // 不在地图里（登录/选人界面），没有视野可重算
		}
		((RestoreViewRange_t)dwRestoreViewRange)(pField);
		((ReloadBack_t)dwReloadBack)(pField);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		// 视野没重算成功不影响整体切换，顶多切图后恢复
	}
}

// ---------------------------------------------------------------------------
// 切换
// ---------------------------------------------------------------------------

static void SaveResolutionToConfig(int nWidth, int nHeight)
{
	if (!g_bSaveToConfig) {
		return;
	}
	WritePrivateProfileStringA("general", "width", std::to_string(nWidth).c_str(), ".\\config.ini");
	WritePrivateProfileStringA("general", "height", std::to_string(nHeight).c_str(), ".\\config.ini");
}

bool ApplyResolution(int nIndex)
{
	if (!g_bEnabled) {
		return false;
	}
	if (nIndex < 0 || nIndex >= (int)g_aResolutions.size()) {
		return false;
	}

	const int nWidth = g_aResolutions[nIndex].width;
	const int nHeight = g_aResolutions[nIndex].height;
	if (nWidth == Client::m_nGameWidth && nHeight == Client::m_nGameHeight) {
		g_nCurrentIndex = nIndex; // 已经是这一档
		return true;
	}

	// 1) 预检查：模式表里得有这个分辨率，否则整体放弃（避免补丁和设备大小不一致）
	unsigned char abScreenMode[SCREENMODE_SIZE];
	if (!TryFindScreenMode(nWidth, nHeight, abScreenMode)) {
		std::cout << "InGameRes: " << nWidth << "x" << nHeight
			<< " not available in the display mode list, keeping "
			<< Client::m_nGameWidth << "x" << Client::m_nGameHeight << std::endl;
		return false;
	}

	std::cout << "InGameRes: switching resolution to " << nWidth << "x" << nHeight << std::endl;

	// 2) 重跑所有分辨率补丁（WriteInt 幂等；代码洞穴参数是全局变量，重新赋值即刷新）
	Client::m_nGameWidth = nWidth;
	Client::m_nGameHeight = nHeight;
	Client::UpdateResolution();
	Client::WorldMap();  // 世界大地图居中参数
	Client::MoreHook();  // 窗口保存位置边界等

	// 3) 换显示模式，置 DEVICENOTRESET 让客户端下一帧自己重置设备
	ApplyScreenMode(abScreenMode);

	// 4) 锚点位移 = 整个 UI 平移到新屏幕中心
	MoveOrgWindow(nWidth, nHeight);

	// 5) 窗口客户区跟随
	ResizeMainWindow(nWidth, nHeight);

	// 6) 地图视野
	RefreshViewRange();

	g_nCurrentIndex = nIndex;
	SaveResolutionToConfig(nWidth, nHeight);
	return true;
}

// ---------------------------------------------------------------------------
// 热键轮询
// ---------------------------------------------------------------------------

static HWND GetGameWindow()
{
	void* pApp = *(void**)(void*)dwCWvsApp;
	if (!pApp) {
		return nullptr;
	}
	return *(HWND*)((char*)pApp + dwCWvsApp_m_hWnd);
}

// 主键按下 + 配置里要求的修饰键（Alt/Shift/Ctrl）都按下才算热键触发。
// 注意：修饰键是用 GetAsyncKeyState 轮询的全局状态，不是消息，所以游戏
// 窗口内的输入框（聊天框等）拿到焦点时同样会触发——默认组合 Ctrl+Enter
// 在聊天里也不会误触（v83 的聊天框只认单独 Enter）。
static bool IsHotkeyDown()
{
	if ((GetAsyncKeyState(g_nHotkey) & 0x8000) == 0) {
		return false;
	}
	if ((g_nHotkeyMod & HOTKEY_MOD_ALT) && (GetAsyncKeyState(VK_MENU) & 0x8000) == 0) {
		return false;
	}
	if ((g_nHotkeyMod & HOTKEY_MOD_SHIFT) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0) {
		return false;
	}
	if ((g_nHotkeyMod & HOTKEY_MOD_CTRL) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0) {
		return false;
	}
	return true;
}

void __cdecl CWndMan_s_Update_hook()
{
	g_pCWndMan_s_Update_orig();

	if (!g_bEnabled || g_aResolutions.empty()) {
		return;
	}

	// 只在游戏窗口是前台时响应，避免 alt-tab 到别的程序误触
	HWND hWndGame = GetGameWindow();
	if (!hWndGame || GetForegroundWindow() != hWndGame) {
		g_bHotkeyDown = false;
		return;
	}

	const bool bDown = IsHotkeyDown();
	if (bDown && !g_bHotkeyDown) {
		g_bHotkeyDown = true;
		g_nPendingIndex = g_nCurrentIndex + 1;
		if (g_nPendingIndex >= (int)g_aResolutions.size()) {
			g_nPendingIndex = 0;
		}
	} else if (!bDown) {
		g_bHotkeyDown = false;
	}

	if (g_nPendingIndex >= 0) {
		const int nIndex = g_nPendingIndex;
		g_nPendingIndex = -1;
		ApplyResolution(nIndex);
	}
}

// ---------------------------------------------------------------------------
// 安装
// ---------------------------------------------------------------------------

void ParseResolutionList(const std::string& sList)
{
	g_aResolutions.clear();

	size_t uPos = 0;
	while (uPos < sList.size()) {
		size_t uEnd = sList.find_first_of(",;| \t", uPos);
		if (uEnd == std::string::npos) {
			uEnd = sList.size();
		}
		const std::string sEntry = sList.substr(uPos, uEnd - uPos);
		uPos = uEnd + 1;
		if (sEntry.empty()) {
			continue;
		}

		int nWidth = 0;
		int nHeight = 0;
		if (sscanf_s(sEntry.c_str(), "%d%*[xX*: ]%d", &nWidth, &nHeight) != 2) {
			continue;
		}
		if (nWidth < 640 || nWidth > 7680 || nHeight < 480 || nHeight > 4320) {
			continue;
		}

		bool bDuplicate = false;
		for (size_t i = 0; i < g_aResolutions.size(); ++i) {
			if (g_aResolutions[i].width == nWidth && g_aResolutions[i].height == nHeight) {
				bDuplicate = true;
				break;
			}
		}
		if (!bDuplicate) {
			Resolution r;
			r.width = nWidth;
			r.height = nHeight;
			g_aResolutions.push_back(r);
		}
	}
}

void Install(bool enable)
{
	if (!enable) {
		return;
	}
	if (g_aResolutions.empty()) {
		std::cout << "InGameRes: no valid resolution in InGameResList, feature disabled" << std::endl;
		g_bEnabled = false;
		return;
	}

	// 当前档位按启动时的 width/height 匹配
	g_nCurrentIndex = -1;
	for (size_t i = 0; i < g_aResolutions.size(); ++i) {
		if (g_aResolutions[i].width == Client::m_nGameWidth && g_aResolutions[i].height == Client::m_nGameHeight) {
			g_nCurrentIndex = (int)i;
			break;
		}
	}

	if (!Memory::SetHook(true, (void**)&g_pCWndMan_s_Update_orig, (void*)CWndMan_s_Update_hook)) {
		std::cout << "InGameRes: failed to hook CWndMan::s_Update, hotkey disabled" << std::endl;
		return;
	}

	std::cout << "InGameRes: hotkey resolution switch installed (" << g_aResolutions.size()
		<< " presets, current index " << g_nCurrentIndex << ")" << std::endl;
}

} // namespace InGameRes

// ---------------------------------------------------------------------------
// 已知限制（与 kaentake 全动态方案的差距）：
//
// 1. ezorsia 的分辨率补丁是"启动时一次性把宽高写进客户端代码"，已创建的
//    UI 元素不会重算坐标。切换后：地图/视野/窗口大小/D3D 立即正确，锚点
//    定位的元素（状态栏主层、居中对话框等）也正确；但状态栏的血条/蓝条/
//    经验条/快捷栏、聊天框这些"位置常量打过补丁"的常驻元素会偏移
//    （量 = 新旧分辨率差），需要重新登录或关闭重开才恢复。kaentake 用
//    九个屏幕边缘锚点 + 钩子动态算坐标，没这个问题——那是另一套架构，
//    等于重写 ezorsia 的分辨率 Mod，本次没有搬。
//    补充：切换幅度小的时候（1280x720 <-> 1366x768，差 48 像素）基本看不出来。
// 2. 目标分辨率必须在显卡模式表里真实存在。ModeTableFix 只补启动那一档，
//    其他档位找不到就保持原分辨率（有日志）。正常 PC 上 800x600~
//    1920x1080 都是标准档，RDP/特殊适配器等极端环境可能缺档。
// 3. 没有做 kaentake 那样的系统设置下拉框——那需要 Custom.wz 扩展对话框
//    （ezorsia 没有 Custom.wz 挂载机制），留待后续。
// 4. 切换只在主线程进行（钩在每帧的 CWndMan::s_Update 里），不会出现
//    跨线程调 COM/D3D 的问题。
// ---------------------------------------------------------------------------
