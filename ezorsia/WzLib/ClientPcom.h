#pragma once
// ============================================================================
// ClientPcom.h — 客户端 PCOM API 表访问 + 模块模式扫描
//
// 移植自 kaentake 的 ztl/zcom.h 与 hook.cpp（GetAddressByPattern）。
// PCOM API 表是 v83 客户端（BeiDou.exe，image base 0x400000）内部的
// COM 创建函数指针表，[0] = PcCreateObject：
//   HRESULT __cdecl PcCreateObject(const wchar_t* sUOL, const GUID* piid,
//                                  void** ppObj, void* pUnkOuter)
// ============================================================================

#include <windows.h>
#include <comdef.h>
#include <cstdint>
#include <psapi.h>

// 客户端 PCOM API 表（kaentake: _g_ppfnPCOMAPIs = 0x00BF0CC0）
inline FARPROC* const g_ppfnPCOMAPIs = reinterpret_cast<FARPROC*>(0x00BF0CC0);

// 按接口类型创建客户端 COM 对象（sUOL 形如 L"NameSpace"、L"NameSpace#FileSystem"、
// L"NameSpace#Package"、L"Shape2D#Vector2D" 等）
template <typename T>
inline void PcCreateObject(const wchar_t* sUOL, T** ppObj, IUnknown* pUnkOuter) {
	*ppObj = nullptr;
	if (!g_ppfnPCOMAPIs[0]) {
		_com_issue_error(CO_E_NOTINITIALIZED);
	}
	auto fn = reinterpret_cast<HRESULT(__cdecl*)(const wchar_t*, const GUID*, void**, void*)>(g_ppfnPCOMAPIs[0]);
	_com_util::CheckError(fn(sUOL, &__uuidof(T), reinterpret_cast<void**>(ppObj), pUnkOuter));
}

// ---- 模式扫描（kaentake hook.cpp 的 ParsePattern/FindPattern 1:1 移植） ----

namespace ktscan {

inline bool HexCharToByte(char c, unsigned char* b) {
	if ('0' <= c && c <= '9') { *b = static_cast<unsigned char>(c - '0'); return true; }
	if ('A' <= c && c <= 'F') { *b = static_cast<unsigned char>(c - 'A' + 10); return true; }
	if ('a' <= c && c <= 'f') { *b = static_cast<unsigned char>(c - 'a' + 10); return true; }
	return false;
}

inline size_t ParsePattern(const char* sPattern, unsigned char* abPattern, unsigned char* abMask) {
	size_t i = 0;
	while (*sPattern) {
		if (*sPattern == ' ') {
			sPattern++;
			continue;
		}
		if (sPattern[0] == '?' && sPattern[1] == '?') {
			abMask[i] = 0x00;
		} else {
			unsigned char high, low;
			if (!HexCharToByte(sPattern[0], &high) || !HexCharToByte(sPattern[1], &low)) {
				return 0;
			}
			abPattern[i] = static_cast<unsigned char>((high << 4) | low);
			abMask[i] = 0xFF;
		}
		sPattern += 2;
		i += 1;
	}
	return i;
}

inline void* FindPattern(unsigned char* pModuleBase, size_t uModuleSize, unsigned char* abPattern, unsigned char* abMask, size_t uPatternSize) {
	if (uModuleSize < uPatternSize) {
		return nullptr;
	}
	for (size_t i = 0; i <= uModuleSize - uPatternSize; ++i) {
		size_t j;
		for (j = 0; j < uPatternSize; ++j) {
			if ((pModuleBase[i + j] & abMask[j]) != (abPattern[j] & abMask[j])) {
				break;
			}
		}
		if (j == uPatternSize) {
			return &pModuleBase[i];
		}
	}
	return nullptr;
}

// 在指定模块内按 "B8 ?? ?? ?? ?? E8 ..." 风格模式查找地址；找不到返回 nullptr。
inline void* GetAddressByPattern(const char* sModuleName, const char* sPattern) {
	HMODULE hModule = GetModuleHandleA(sModuleName);
	if (!hModule) {
		hModule = LoadLibraryA(sModuleName);
		if (!hModule) {
			return nullptr;
		}
	}
	MODULEINFO mi;
	if (!GetModuleInformation(GetCurrentProcess(), hModule, &mi, sizeof(mi))) {
		return nullptr;
	}
	unsigned char abPattern[256];
	unsigned char abMask[256];
	size_t uSize = ParsePattern(sPattern, abPattern, abMask);
	if (uSize == 0) {
		return nullptr;
	}
	return FindPattern(static_cast<unsigned char*>(mi.lpBaseOfDll), mi.SizeOfImage, abPattern, abMask, uSize);
}

} // namespace ktscan