#include "stdafx.h"
#include "CustomWz.h"
#include "Memory.h"
#include "WzLib/WzInterfaces.h"
#include "WzLib/ClientPcom.h"
#include <algorithm>
#include <vector>
#include <string>

// ============================================================================
// CustomWz.cpp — Custom.wz 挂载（kaentake resman.cpp 1:1 移植）
//
// 与 kaentake 的差异：
//   - Detours 通过 ezorsia 的 Memory::SetHook 安装；
//   - COM 接口来自 WzLib/WzInterfaces.h（按 .idl 手工还原，槽位与 MIDL 生成
//     代码一致），Ztl_bstr_t/Ztl_variant_t 来自 WzLib/zcomdef.h；
//   - kaentake 未移植的 inlink（_inlink）仍然不需要：血条百分比等覆盖资源
//     都是实体 canvas，不依赖 outlink。
// ============================================================================

namespace {

typedef void(__fastcall* CWvsApp__InitializeResMan_t)(void* pThis, void* edx);
CWvsApp__InitializeResMan_t CWvsApp__InitializeResMan_orig = reinterpret_cast<CWvsApp__InitializeResMan_t>(0x009F7159);

typedef void(__fastcall* CWvsApp__CleanUp_t)(void* pThis, void* edx);
CWvsApp__CleanUp_t CWvsApp__CleanUp_orig = reinterpret_cast<CWvsApp__CleanUp_t>(0x009F69B7);

IWzNameSpacePtr g_pCustomNameSpace;
std::vector<std::wstring> g_vecOverrides;

IWzResManPtr& GetResMan() {
	return *reinterpret_cast<IWzResManPtr*>(0x00BF14E8);
}

IWzNameSpacePtr& GetRoot() {
	return *reinterpret_cast<IWzNameSpacePtr*>(0x00BF14E0);
}

void Dir_BackSlashToSlash(char* sDir) {
	for (size_t i = 0; sDir[i]; ++i) {
		if (sDir[i] == '\\') {
			sDir[i] = '/';
		}
	}
}

void Dir_upDir(char* sDir) {
	size_t uLen = strlen(sDir);
	if (uLen > 0 && sDir[uLen - 1] == '/') {
		sDir[uLen - 1] = 0;
	}
	for (size_t i = strlen(sDir) - 1; i > 0; --i) {
		if (sDir[i] == '/') {
			sDir[i] = 0;
			return;
		}
	}
}

bool OverridesContain(const wchar_t* sPath) {
	if (!sPath) {
		return false;
	}
	return std::binary_search(g_vecOverrides.begin(), g_vecOverrides.end(), sPath);
}

// 从 ResMan 取对象并 QI 到指定接口
template <typename T>
bool GetObjectAs(const wchar_t* sUOL, T** ppObj) {
	*ppObj = nullptr;
	IWzResManPtr& pResMan = GetResMan();
	if (!pResMan) {
		return false;
	}
	Ztl_variant_t vObj;
	if (FAILED(pResMan->GetObject(sUOL, vtEmpty, vtEmpty, &vObj))) {
		return false;
	}
	IUnknown* pUnk = vObj.GetUnknown();
	if (!pUnk) {
		return false;
	}
	return SUCCEEDED(pUnk->QueryInterface(__uuidof(T), reinterpret_cast<void**>(ppObj)));
}

// ---- NameSpace.dll：原命名空间查找失败时回退到 Custom 命名空间 ----

typedef HRESULT(__stdcall* NS_OnGetLocalObject_t)(IWzNameSpace* pThis, INT nIndex, BSTR sPath, INT* pnPathUsed, VARIANT* pvRet);
NS_OnGetLocalObject_t NS_OnGetLocalObject_orig = nullptr;

HRESULT __stdcall NS_OnGetLocalObject_hook(IWzNameSpace* pThis, INT nIndex, BSTR sPath, INT* pnPathUsed, VARIANT* pvRet) {
	HRESULT hr = NS_OnGetLocalObject_orig(pThis, nIndex, sPath, pnPathUsed, pvRet);
	if (SUCCEEDED(hr)) {
		return hr;
	}
	if (!g_pCustomNameSpace || !OverridesContain(sPath)) {
		return hr;
	}
	return g_pCustomNameSpace->_OnGetLocalObject(nIndex, sPath, pnPathUsed, pvRet);
}

// ---- PCOM.dll：序列化覆盖节点时合并 Custom 的非子属性叶子 ----

typedef HRESULT(__stdcall* CWzProperty_Serialize_t)(void* pThis, IWzArchive* pArchive);
CWzProperty_Serialize_t CWzProperty__Serialize_orig = nullptr;

HRESULT __stdcall CWzProperty__Serialize_hook(void* pThis, IWzArchive* pArchive) {
	HRESULT hr = CWzProperty__Serialize_orig(pThis, pArchive);
	if (FAILED(hr)) {
		return hr;
	}
	BSTR bstrUOL = nullptr;
	if (FAILED(pArchive->get_absoluteUOL(&bstrUOL))) {
		return hr;
	}
	bool bContained = OverridesContain(bstrUOL);
	std::wstring sUOL = std::wstring(L"Custom/") + (bstrUOL ? bstrUOL : L"");
	// BSTR 由客户端分配器分配，用完必须用它自己的释放函数（见 zcomdef.h）
	ZComAPI::ZComSysFreeString(bstrUOL);
	if (!bContained) {
		return hr;
	}

	// 在 Custom 命名空间下定位同名属性（ResMan 路径 "Custom/<UOL>"）
	IWzPropertyPtr pCustom;
	if (!GetObjectAs<IWzProperty>(sUOL.c_str(), &pCustom) || !pCustom) {
		return hr;
	}
	IUnknownPtr pEnumUnk;
	if (FAILED(pCustom->get__NewEnum(&pEnumUnk)) || !pEnumUnk) {
		return hr;
	}
	IEnumVARIANTPtr pEnum = pEnumUnk;
	if (!pEnum) {
		return hr;
	}
	while (true) {
		Ztl_variant_t vNext;
		ULONG uCeltFetched = 0;
		if (FAILED(pEnum->Next(1, &vNext, &uCeltFetched)) || uCeltFetched == 0) {
			break;
		}
		if (V_VT(&vNext) != VT_BSTR || !V_BSTR(&vNext)) {
			continue;
		}
		Ztl_bstr_t sNext(V_BSTR(&vNext), true);
		Ztl_variant_t vChild;
		if (FAILED(pCustom->get_item(sNext, &vChild))) {
			continue;
		}
		IUnknown* pUnkChild = vChild.GetUnknown();
		IWzPropertyPtr pSub;
		bool bIsSub = pUnkChild && SUCCEEDED(pUnkChild->QueryInterface(__uuidof(IWzProperty), reinterpret_cast<void**>(&pSub)));
		if (!bIsSub) {
			// 非子属性叶子 → 覆盖进原属性（Add 引擎内部会复制 VARIANT）
			Ztl_variant_t vNoReplace(false);
			reinterpret_cast<IWzProperty*>(pThis)->Add(sNext.GetBSTR(), vChild, vNoReplace);
		}
	}
	return S_OK;
}

void OnInitializeResMan() {
	// 1) 根命名空间下注册 Custom 子命名空间
	IWzWritableNameSpace* pWritableRoot = nullptr;
	if (!GetRoot() || FAILED(GetRoot()->QueryInterface(__uuidof(IWzWritableNameSpace), reinterpret_cast<void**>(&pWritableRoot)))) {
		OutputDebugStringA("CustomWz: failed to cast root namespace\n");
		return;
	}
	IWzNameSpace* pNameSpaceRaw = nullptr;
	PcCreateObject<IWzNameSpace>(L"NameSpace", &pNameSpaceRaw, nullptr);
	if (pNameSpaceRaw) {
		Ztl_variant_t vObj(pNameSpaceRaw, true); // AddRef；AddObject 之后由 dtor 释放
		Ztl_variant_t vResult;
		pWritableRoot->AddObject(L"Custom", vObj, vResult.GetAddress());
		IUnknown* pUnkResult = vResult.GetUnknown();
		if (pUnkResult) {
			pUnkResult->QueryInterface(__uuidof(IWzNameSpace), reinterpret_cast<void**>(&g_pCustomNameSpace));
		}
		pNameSpaceRaw->Release(); // PcCreateObject 返回的初始引用已转移到 vObj
	}
	pWritableRoot->Release();
	if (!g_pCustomNameSpace) {
		OutputDebugStringA("CustomWz: failed to create Custom namespace\n");
		return;
	}

	// 2) 从文件系统加载 Custom.wz 并挂到根（与 exe 同目录）
	IWzFileSystem* pFsRaw = nullptr;
	PcCreateObject<IWzFileSystem>(L"NameSpace#FileSystem", &pFsRaw, nullptr);
	IWzPackage* pPkgRaw = nullptr;
	PcCreateObject<IWzPackage>(L"NameSpace#Package", &pPkgRaw, nullptr);
	do {
		if (!pFsRaw || !pPkgRaw) {
			break;
		}
		char sStartPath[MAX_PATH];
		GetModuleFileNameA(nullptr, sStartPath, MAX_PATH);
		Dir_BackSlashToSlash(sStartPath);
		Dir_upDir(sStartPath);
		if (FAILED(pFsRaw->Init(Ztl_bstr_t(sStartPath)))) {
			break;
		}
		IWzSeekableArchive* pArchiveRaw = nullptr;
		{
			Ztl_variant_t vArchive;
			if (FAILED(pFsRaw->get_item(L"Custom.wz", &vArchive))) {
				break;
			}
			IUnknown* pUnk = vArchive.GetUnknown();
			if (!pUnk || FAILED(pUnk->QueryInterface(__uuidof(IWzSeekableArchive), reinterpret_cast<void**>(&pArchiveRaw)))) {
				break;
			}
		}
		HRESULT hrInit = pPkgRaw->Init(L"83", L"Custom", pArchiveRaw);
		if (pArchiveRaw) {
			pArchiveRaw->Release();
		}
		if (FAILED(hrInit)) {
			break;
		}
		if (FAILED(g_pCustomNameSpace->Mount(L"/", pPkgRaw, 1))) {
			break;
		}
	} while (false);
	if (pFsRaw) {
		pFsRaw->Release();
	}
	if (pPkgRaw) {
		pPkgRaw->Release();
	}

	// 3) 递归收集 Custom 命名空间的覆盖清单（UOL 列表，二分查找用）
	OutputDebugStringA("CustomWz: Custom.wz begin\n");
	std::vector<std::pair<std::wstring, IEnumVARIANTPtr>> stack;
	{
		IUnknownPtr pEnumUnk;
		if (g_pCustomNameSpace->get__NewEnum(&pEnumUnk) == S_OK && pEnumUnk) {
			stack.emplace_back(std::wstring(), IEnumVARIANTPtr(pEnumUnk));
		}
	}
	while (!stack.empty()) {
		std::wstring sPath = stack.back().first;
		IEnumVARIANTPtr pEnum = stack.back().second;
		stack.pop_back();
		while (true) {
			Ztl_variant_t vNext;
			ULONG uCeltFetched = 0;
			if (FAILED(pEnum->Next(1, &vNext, &uCeltFetched)) || uCeltFetched == 0) {
				break;
			}
			if (V_VT(&vNext) != VT_BSTR || !V_BSTR(&vNext)) {
				continue;
			}
			std::wstring sUOL = (sPath.length() > 0 ? sPath + L"/" : L"") + V_BSTR(&vNext);
			std::wstring sQuery = L"Custom/" + sUOL;
			Ztl_variant_t vObj;
			IWzResManPtr& pResMan = GetResMan();
			if (pResMan && SUCCEEDED(pResMan->GetObject(sQuery.c_str(), vtEmpty, vtEmpty, &vObj))) {
				IUnknown* pUnk = vObj.GetUnknown();
				if (pUnk) {
					IWzNameSpacePtr pSub;
					if (SUCCEEDED(pUnk->QueryInterface(__uuidof(IWzNameSpace), reinterpret_cast<void**>(&pSub)))) {
						IUnknownPtr pEnumUnk;
						if (pSub->get__NewEnum(&pEnumUnk) == S_OK && pEnumUnk) {
							stack.emplace_back(sUOL, IEnumVARIANTPtr(pEnumUnk));
							continue;
						}
					}
					IWzPropertyPtr pProp;
					if (SUCCEEDED(pUnk->QueryInterface(__uuidof(IWzProperty), reinterpret_cast<void**>(&pProp)))) {
						IUnknownPtr pEnumUnk;
						if (pProp->get__NewEnum(&pEnumUnk) == S_OK && pEnumUnk) {
							stack.emplace_back(sUOL, IEnumVARIANTPtr(pEnumUnk));
							continue;
						}
					}
				}
			}
			g_vecOverrides.push_back(sUOL);
		}
	}
	std::sort(g_vecOverrides.begin(), g_vecOverrides.end());

	// 4) 安装两个回退钩子（NameSpace.dll / PCOM.dll，kaentake 同款 pattern）
	NS_OnGetLocalObject_orig = reinterpret_cast<NS_OnGetLocalObject_t>(
		ktscan::GetAddressByPattern("NAMESPACE.DLL", "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 81 EC 80"));
	if (NS_OnGetLocalObject_orig) {
		Memory::SetHook(true, reinterpret_cast<void**>(&NS_OnGetLocalObject_orig), NS_OnGetLocalObject_hook);
	} else {
		OutputDebugStringA("CustomWz: NAMESPACE.DLL pattern not found\n");
	}

	CWzProperty__Serialize_orig = reinterpret_cast<CWzProperty_Serialize_t>(
		ktscan::GetAddressByPattern("PCOM.DLL", "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 EC 68"));
	if (CWzProperty__Serialize_orig) {
		Memory::SetHook(true, reinterpret_cast<void**>(&CWzProperty__Serialize_orig), CWzProperty__Serialize_hook);
	} else {
		OutputDebugStringA("CustomWz: PCOM.DLL pattern not found\n");
	}

	OutputDebugStringA("CustomWz: Custom.wz ok\n");
}

void __fastcall CWvsApp__InitializeResMan_hook(void* pThis, void* edx) {
	CWvsApp__InitializeResMan_orig(pThis, edx);
	OnInitializeResMan();
}

void __fastcall CWvsApp__CleanUp_hook(void* pThis, void* edx) {
	CWvsApp__CleanUp_orig(pThis, edx);
	g_pCustomNameSpace = nullptr;
	g_vecOverrides.clear();
}

} // namespace

namespace CustomWz {

void Install(bool bEnable) {
	if (!bEnable) {
		return;
	}
	Memory::SetHook(true, reinterpret_cast<void**>(&CWvsApp__InitializeResMan_orig), CWvsApp__InitializeResMan_hook);
	Memory::SetHook(true, reinterpret_cast<void**>(&CWvsApp__CleanUp_orig), CWvsApp__CleanUp_hook);
}

} // namespace CustomWz
