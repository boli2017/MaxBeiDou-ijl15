#include "stdafx.h"
#include "ModeTableFix.h"
#include "detours.h"

#include <iostream>

namespace {

// d3d8 的最小 ABI 定义，不依赖 DirectX SDK 的 d3d8.h
struct D3DDisplayMode
{
	UINT  Width;
	UINT  Height;
	UINT  RefreshRate;
	DWORD Format;
};

// D3DPRESENT_PARAMETERS（d3d8types.h 里的原样布局，只用到前几个字段）
struct D3DPresentParameters
{
	UINT   BackBufferWidth;              // +0x00
	UINT   BackBufferHeight;             // +0x04
	DWORD  BackBufferFormat;             // +0x08
	UINT   BackBufferCount;              // +0x0C
	DWORD  MultiSampleType;              // +0x10
	DWORD  SwapEffect;                   // +0x14
	HWND   hDeviceWindow;                // +0x18
	BOOL   Windowed;                     // +0x1C
	BOOL   EnableAutoDepthStencil;       // +0x20
	DWORD  AutoDepthStencilFormat;       // +0x24
	DWORD  Flags;                        // +0x28
	UINT   FullScreen_RefreshRateInHz;   // +0x2C
	UINT   FullScreen_PresentationInterval; // +0x30
};

// IDirect3D8 vtable 槽位
const int kSlotGetAdapterModeCount = 6;
const int kSlotEnumAdapterModes = 7;
const int kSlotGetAdapterDisplayMode = 8;
const int kSlotCreateDevice = 15;

// IDirect3DDevice8 vtable 槽位。
// 已在本仓库 Gr2D_DX8.dll 里反汇编核对：客户端自己的设备重置在 0x50408E34，
//   call [ecx+0x0C]  = TestCooperativeLevel（槽位 3）
//   call [ecx+0x38]  = Reset（槽位 14），返回值写进 CWzGr2D::m_hrErrorCode(+0x94)
// 传进去的参数结构就是 D3DPRESENT_PARAMETERS（+0x1C Windowed / +0x2C 刷新率）。
const int kSlotDeviceReset = 14;

const DWORD kFormatX8R8G8B8 = 22;
const DWORD kFormatR5G6B5 = 23;
const int kMaxInjectedModes = 4;

typedef void* (WINAPI* Direct3DCreate8_t)(UINT);
typedef UINT(__stdcall* GetAdapterModeCount_t)(void*, UINT);
typedef HRESULT(__stdcall* EnumAdapterModes_t)(void*, UINT, UINT, D3DDisplayMode*);
typedef HRESULT(__stdcall* GetAdapterDisplayMode_t)(void*, UINT, D3DDisplayMode*);
typedef HMODULE(WINAPI* LoadLibraryA_t)(LPCSTR);
typedef HMODULE(WINAPI* LoadLibraryW_t)(LPCWSTR);
typedef HMODULE(WINAPI* LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);
typedef HMODULE(WINAPI* LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);

// IDirect3D8::CreateDevice / IDirect3DDevice8::Reset（都是 __stdcall）
typedef HRESULT(__stdcall* CreateDevice_t)(void*, UINT, UINT, HWND, DWORD, D3DPresentParameters*, void**);
typedef HRESULT(__stdcall* Reset_t)(void*, D3DPresentParameters*);

Direct3DCreate8_t oDirect3DCreate8 = nullptr;
GetAdapterModeCount_t oGetAdapterModeCount = nullptr;
EnumAdapterModes_t oEnumAdapterModes = nullptr;
LoadLibraryA_t oLoadLibraryA = nullptr;
LoadLibraryW_t oLoadLibraryW = nullptr;
LoadLibraryExA_t oLoadLibraryExA = nullptr;
LoadLibraryExW_t oLoadLibraryExW = nullptr;

bool g_bound = false;
bool g_vtableHooked = false;
bool g_injectPrepared = false;
int g_injectedCount = 0;
D3DDisplayMode g_injectedModes[kMaxInjectedModes];
DWORD g_desktopFormat = kFormatX8R8G8B8;
UINT g_desktopRefresh = 60;

// ---- 设备 Reset 钩子（可选，见 ModeTableFix_InstallResetFix）----
bool g_resetFix = false;
CreateDevice_t oCreateDevice = nullptr;

// 客户端只建一个设备，但还是按 vtable 记原函数，多个设备/不同实现时不会串
const int kMaxDevices = 4;
struct DeviceEntry
{
	void** vtable;
	Reset_t reset;
};
DeviceEntry g_devices[kMaxDevices];
int g_deviceCount = 0;

// COM 方法直接改 vtable 槽位：比改函数体稳（不受反汇编器影响），
// 而且 d3d8 在 Wine 下是内建实现，代码补丁不一定吃得住
bool PatchSlot(void** vtable, int index, void* hook, void** original)
{
	if (vtable == nullptr) {
		return false;
	}

	DWORD oldProtect = 0;
	if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect) == 0) {
		return false;
	}

	*original = vtable[index];
	vtable[index] = hook;

	DWORD restored = 0;
	VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &restored);
	return true;
}

// 目标分辨率已经在真实模式表里就什么都不补，能正常启动的机器完全不受影响
void PrepareInjectedModes(void* self, UINT adapter)
{
	if (g_injectPrepared) {
		return;
	}
	g_injectPrepared = true;
	if (oGetAdapterModeCount == nullptr || oEnumAdapterModes == nullptr) {
		return;
	}

	const UINT targetWidth = (UINT)Client::m_nGameWidth;
	const UINT targetHeight = (UINT)Client::m_nGameHeight;

	const UINT realCount = oGetAdapterModeCount(self, adapter);
	for (UINT i = 0; i < realCount; ++i) {
		D3DDisplayMode mode;
		ZeroMemory(&mode, sizeof(mode));
		if (FAILED(oEnumAdapterModes(self, adapter, i, &mode))) {
			continue;
		}
		if (mode.Width == targetWidth && mode.Height == targetHeight) {
			return;   // 表里有，不用补
		}
	}

	// 客户端到底拿什么条件去匹配不好说，所以常见组合都摆上（去重）
	const DWORD formats[2] = { g_desktopFormat, kFormatR5G6B5 };
	const UINT rates[2] = { g_desktopRefresh, 60 };
	for (int f = 0; f < 2; ++f) {
		for (int r = 0; r < 2; ++r) {
			bool duplicate = false;
			for (int i = 0; i < g_injectedCount; ++i) {
				if (g_injectedModes[i].Format == formats[f] && g_injectedModes[i].RefreshRate == rates[r]) {
					duplicate = true;
				}
			}
			if (duplicate || g_injectedCount >= kMaxInjectedModes) {
				continue;
			}

			D3DDisplayMode& mode = g_injectedModes[g_injectedCount++];
			mode.Width = targetWidth;
			mode.Height = targetHeight;
			mode.RefreshRate = rates[r];
			mode.Format = formats[f];
		}
	}
}

UINT __stdcall HkGetAdapterModeCount(void* self, UINT adapter)
{
	const UINT count = oGetAdapterModeCount(self, adapter);
	PrepareInjectedModes(self, adapter);
	return count + (UINT)g_injectedCount;
}

HRESULT __stdcall HkEnumAdapterModes(void* self, UINT adapter, UINT mode, D3DDisplayMode* displayMode)
{
	PrepareInjectedModes(self, adapter);

	if (g_injectedCount > 0 && displayMode != nullptr) {
		const UINT realCount = oGetAdapterModeCount(self, adapter);
		if (mode >= realCount && (mode - realCount) < (UINT)g_injectedCount) {
			*displayMode = g_injectedModes[mode - realCount];
			return S_OK;
		}
	}
	return oEnumAdapterModes(self, adapter, mode, displayMode);
}

// ---------------------------------------------------------------------------
// IDirect3DDevice8::Reset 钩子
//
// 客户端每次重置设备（启动时建设备、m_hrErrorCode=D3DERR_DEVICENOTRESET 之后
// 的重建、设备丢失恢复）都走这里。两件事：
//   1. 后备缓冲区尺寸钉死在 Client::m_nGameWidth/Height。游戏内切分辨率时
//      InGameRes 改的是 Gr2D 的 m_screenMode，客户端拿它拼出来的参数不一定
//      等于我们要的分辨率（尤其模式表里那一档是 ModeTableFix 补出来的），
//      这里兜底保证"设备尺寸 == 补丁后的分辨率"，窗口画面不会和补丁错位；
//   2. 窗口模式下刷新率必须是 0（d3d8 的规定），Reset 失败时按"宽度 8 对齐"
//      和"像素格式交给 D3D 自选"两级回退重试，避免切档后黑屏/卡在设备丢失。
//
// 只接管窗口模式的 Reset：全屏那套（刷新率、显示模式切换）客户端自己更清楚。
// ---------------------------------------------------------------------------
HRESULT __stdcall HkReset(void* self, D3DPresentParameters* params);

Reset_t FindDeviceReset(void* device)
{
	if (device == nullptr) {
		return nullptr;
	}
	void** vtable = *(void***)device;
	for (int i = 0; i < g_deviceCount; ++i) {
		if (g_devices[i].vtable == vtable) {
			return g_devices[i].reset;
		}
	}
	return nullptr;
}

void HookDeviceReset(void* device)
{
	if (device == nullptr || g_deviceCount >= kMaxDevices) {
		return;
	}
	void** vtable = *(void***)device;
	for (int i = 0; i < g_deviceCount; ++i) {
		if (g_devices[i].vtable == vtable) {
			return;   // 同一张 vtable 已经钩过
		}
	}
	Reset_t original = nullptr;
	if (!PatchSlot(vtable, kSlotDeviceReset, (void*)HkReset, (void**)&original)) {
		return;
	}
	g_devices[g_deviceCount].vtable = vtable;
	g_devices[g_deviceCount].reset = original;
	++g_deviceCount;
	std::cout << "ModeTableFix: IDirect3DDevice8::Reset hooked" << std::endl;
}

HRESULT __stdcall HkReset(void* self, D3DPresentParameters* params)
{
	Reset_t original = FindDeviceReset(self);
	if (original == nullptr) {
		return E_FAIL;
	}
	if (params == nullptr || !g_resetFix) {
		return original(self, params);
	}
	if (!params->Windowed) {
		return original(self, params);   // 全屏重置不插手
	}

	const int wantedWidth = Client::m_nGameWidth;
	const int wantedHeight = Client::m_nGameHeight;
	bool adjusted = false;
	if (wantedWidth > 0 && wantedHeight > 0 &&
		((int)params->BackBufferWidth != wantedWidth || (int)params->BackBufferHeight != wantedHeight)) {
		params->BackBufferWidth = (UINT)wantedWidth;
		params->BackBufferHeight = (UINT)wantedHeight;
		adjusted = true;
	}
	if (params->FullScreen_RefreshRateInHz != 0) {
		params->FullScreen_RefreshRateInHz = 0;
		adjusted = true;
	}

	if (adjusted) {
		std::cout << "ModeTableFix: Reset -> backbuffer " << params->BackBufferWidth
			<< "x" << params->BackBufferHeight << std::endl;
	}

	HRESULT hr = original(self, params);
	if (SUCCEEDED(hr)) {
		return hr;
	}

	// 回退 1：部分驱动 / wine 的 d3d8 包装层要求宽度按 8 字节对齐
	if (params->BackBufferWidth != 0 && (params->BackBufferWidth % 8) != 0) {
		const UINT alignedWidth = (params->BackBufferWidth + 7) & ~7u;
		std::cout << "ModeTableFix: Reset failed 0x" << std::hex << (unsigned long)hr << std::dec
			<< ", retry aligned width " << params->BackBufferWidth << " -> " << alignedWidth << std::endl;
		params->BackBufferWidth = alignedWidth;
		hr = original(self, params);
		if (SUCCEEDED(hr)) {
			return hr;
		}
	}

	// 回退 2：像素格式（含补出来的假模式带的格式）可能是驱动不认的，让 D3D 自己挑
	if (params->BackBufferFormat != 0) {
		std::cout << "ModeTableFix: Reset failed 0x" << std::hex << (unsigned long)hr << std::dec
			<< ", retry with default backbuffer format" << std::endl;
		params->BackBufferFormat = 0;
		hr = original(self, params);
	}
	return hr;
}

HRESULT __stdcall HkCreateDevice(void* self, UINT adapter, UINT deviceType, HWND focusWindow,
	DWORD behaviorFlags, D3DPresentParameters* params, void** ppDevice)
{
	if (oCreateDevice == nullptr) {
		return E_FAIL;
	}
	const HRESULT hr = oCreateDevice(self, adapter, deviceType, focusWindow, behaviorFlags, params, ppDevice);
	if (SUCCEEDED(hr) && ppDevice != nullptr && *ppDevice != nullptr) {
		HookDeviceReset(*ppDevice);
	}
	return hr;
}

void* WINAPI HkDirect3DCreate8(UINT sdkVersion)
{
	void* d3d8 = oDirect3DCreate8(sdkVersion);
	if (d3d8 == nullptr || g_vtableHooked) {
		return d3d8;
	}
	g_vtableHooked = true;

	void** vtable = *(void***)d3d8;
	PatchSlot(vtable, kSlotGetAdapterModeCount, (void*)HkGetAdapterModeCount, (void**)&oGetAdapterModeCount);
	PatchSlot(vtable, kSlotEnumAdapterModes, (void*)HkEnumAdapterModes, (void**)&oEnumAdapterModes);
	if (g_resetFix) {
		// 拿到设备后钩 Reset，设备是 CreateDevice 里才建出来的
		PatchSlot(vtable, kSlotCreateDevice, (void*)HkCreateDevice, (void**)&oCreateDevice);
	}

	// 自己问一次桌面模式，保证补进去的像素格式/刷新率是这台机器真实的值
	GetAdapterDisplayMode_t getDisplayMode = (GetAdapterDisplayMode_t)vtable[kSlotGetAdapterDisplayMode];
	D3DDisplayMode desktop;
	ZeroMemory(&desktop, sizeof(desktop));
	if (SUCCEEDED(getDisplayMode(d3d8, 0, &desktop))) {
		if (desktop.Format != 0) {
			g_desktopFormat = desktop.Format;
		}
		if (desktop.RefreshRate != 0) {
			g_desktopRefresh = desktop.RefreshRate;
		}
	}
	return d3d8;
}

void BindD3D8()
{
	if (g_bound) {
		return;
	}
	const HMODULE module = GetModuleHandleA("d3d8.dll");
	if (module == nullptr) {
		return;
	}
	g_bound = true;

	oDirect3DCreate8 = (Direct3DCreate8_t)GetProcAddress(module, "Direct3DCreate8");
	if (oDirect3DCreate8 != nullptr) {
		if (!Memory::SetHook(true, (void**)&oDirect3DCreate8, (void*)HkDirect3DCreate8)) {
			oDirect3DCreate8 = nullptr;
		}
	}
}

// d3d8.dll 在我们 DllMain 时通常还没加载，而在 DllMain 里 LoadLibrary 会踩 loader lock，
// 所以钩住 LoadLibrary 系列，等客户端自己加载 d3d8 时再绑
HMODULE WINAPI HkLoadLibraryA(LPCSTR fileName)
{
	const HMODULE result = oLoadLibraryA(fileName);
	if (result != nullptr && !g_bound) {
		BindD3D8();
	}
	return result;
}

HMODULE WINAPI HkLoadLibraryW(LPCWSTR fileName)
{
	const HMODULE result = oLoadLibraryW(fileName);
	if (result != nullptr && !g_bound) {
		BindD3D8();
	}
	return result;
}

HMODULE WINAPI HkLoadLibraryExA(LPCSTR fileName, HANDLE file, DWORD flags)
{
	const HMODULE result = oLoadLibraryExA(fileName, file, flags);
	if (result != nullptr && !g_bound) {
		BindD3D8();
	}
	return result;
}

HMODULE WINAPI HkLoadLibraryExW(LPCWSTR fileName, HANDLE file, DWORD flags)
{
	const HMODULE result = oLoadLibraryExW(fileName, file, flags);
	if (result != nullptr && !g_bound) {
		BindD3D8();
	}
	return result;
}

void InstallLoaderHooks()
{
	const HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
	if (kernel32 == nullptr) {
		return;
	}

	oLoadLibraryA = (LoadLibraryA_t)GetProcAddress(kernel32, "LoadLibraryA");
	oLoadLibraryW = (LoadLibraryW_t)GetProcAddress(kernel32, "LoadLibraryW");
	oLoadLibraryExA = (LoadLibraryExA_t)GetProcAddress(kernel32, "LoadLibraryExA");
	oLoadLibraryExW = (LoadLibraryExW_t)GetProcAddress(kernel32, "LoadLibraryExW");

	if (oLoadLibraryA != nullptr) {
		Memory::SetHook(true, (void**)&oLoadLibraryA, (void*)HkLoadLibraryA);
	}
	if (oLoadLibraryW != nullptr) {
		Memory::SetHook(true, (void**)&oLoadLibraryW, (void*)HkLoadLibraryW);
	}
	if (oLoadLibraryExA != nullptr) {
		Memory::SetHook(true, (void**)&oLoadLibraryExA, (void*)HkLoadLibraryExA);
	}
	if (oLoadLibraryExW != nullptr) {
		Memory::SetHook(true, (void**)&oLoadLibraryExW, (void*)HkLoadLibraryExW);
	}
}

}   // namespace

namespace {

void EnsureD3D8Hooks()
{
	BindD3D8();
	if (!g_bound) {
		InstallLoaderHooks();
	}
}

}   // namespace

void ModeTableFix_Install(bool enable)
{
	if (!enable) {
		return;
	}
	EnsureD3D8Hooks();
}

void ModeTableFix_InstallResetFix(bool enable)
{
	if (!enable) {
		return;
	}
	// 必须在客户端调 Direct3DCreate8 之前置位：CreateDevice 的钩子是在建出
	// d3d8 工厂对象那一刻装的，晚了就钩不到设备了（DllMain 阶段调用即可）
	g_resetFix = true;
	EnsureD3D8Hooks();
}
