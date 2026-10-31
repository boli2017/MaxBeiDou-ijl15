#pragma once
// ============================================================================
// WzInterfaces.h — v83 客户端内部 PCOM 接口的最小手工声明
//
// 来源：iw2d/WzLib (branch v83_DX8to9) 的 .idl 文件。原工程在构建期用 MIDL +
// #import 生成 .tlh/.tli，这里按 .idl 的方法顺序手工还原（vtable 顺序即声明
// 顺序，不可增删/调换，否则会调错槽位导致崩溃）。
//
// 当前仅包含 MobHpTag 等模块所需的接口；后续移植 tooltip/itemicon 等功能时
// 可按同样方式补充接口或方法。
// ============================================================================

#include <comdef.h>
#include "zcomdef.h"

// ---- enums（值与 .idl 一致） ----

typedef enum RESMAN_PARAM {
    RC_AUTO_SERIALIZE = 0x1,
    RC_AUTO_SERIALIZE_NO_CACHE = 0x2,
    RC_NO_AUTO_SERIALIZE = 0x4,
    RC_DEFAULT_AUTO_SERIALIZE = 0x0,
    RC_AUTO_SERIALIZE_MASK = 0x7,
    RC_AUTO_REPARSE = 0x10,
    RC_NO_AUTO_REPARSE = 0x20,
    RC_DEFAULT_AUTO_REPARSE = 0x0,
    RC_AUTO_REPARSE_MASK = 0x30
} RESMAN_PARAM;

typedef enum CANVAS_ALPHATYPE {
    CA_OVERWRITE     = 0xFF,
    CA_REMOVEALPHA   = 0xFE
} CANVAS_ALPHATYPE;

typedef enum CANVAS_LEVELMAP {
    CL_ALL16         = 0x1,
    CL_ALL32         = 0x2,
    CL_ALL56         = 0x201,
    CL_USE32OVER1    = 0x10002,
    CL_USE32OVER2    = 0x20002,
    CL_USE56OVER1    = 0x10201,
    CL_USE56OVER2    = 0x20201
} CANVAS_LEVELMAP;

typedef enum CANVAS_PIXFORMAT {
    CP_UNKNOWN       = 0x0,
    CP_A4R4G4B4      = 0x1,
    CP_A8R8G8B8      = 0x2,
    CP_R5G6B5        = 0x201,
    CP_DXT3          = 0x402
} CANVAS_PIXFORMAT;

typedef enum CANVAS_DITHERTYPE {
    CD_NONE          = 0x0,
    CD_FS0           = 0x1,
    CD_FS1           = 0x2,
    CD_FS            = 0x3,
    CD_FD0           = 0x11,
    CD_FD1           = 0x12,
    CD_FD            = 0x13
} CANVAS_DITHERTYPE;

typedef enum LAYER_BLENDTYPE {
    LB_NORMAL    = 0x0,
    LB_ADD       = 0x1,
    LB_INVERSE   = 0x2,
    LB_ISOLATED  = 0x4
} LAYER_BLENDTYPE;

typedef enum GR2D_ANITYPE {
    GA_NORMAL  = 0x0,
    GA_STOP    = 0x0,
    GA_FIRST   = 0x10,
    GA_REPEAT  = 0x20,
    GA_WAIT    = 0x100
} GR2D_ANITYPE;

// ---- 前向声明 ----

struct IWzArchive;
struct IWzRawCanvasAllocator;
struct IWzRawCanvas;
struct IWzFont;

// ---- 接口声明（vtable 顺序 = .idl 方法顺序） ----

// IWzSerialize.idl
struct __declspec(uuid("01110BC6-D843-4FF1-918C-9CF64632FB15")) IWzSerialize : public IUnknown {
    virtual HRESULT __stdcall get_persistentUOL(BSTR* pVal) = 0;
    virtual HRESULT __stdcall Serialize(IWzArchive* pArchive) = 0;
};

// IWzProperty.idl
struct __declspec(uuid("986515D9-0A0B-4929-8B4F-718682177B92")) IWzProperty : public IWzSerialize {
    virtual HRESULT __stdcall get_item(BSTR sPath, VARIANT* pvValue) = 0;
    virtual HRESULT __stdcall put_item(BSTR sPath, VARIANT vValue) = 0;
    virtual HRESULT __stdcall get__NewEnum(IUnknown** pVal) = 0;
    virtual HRESULT __stdcall get_count(UINT* pVal) = 0;
    virtual HRESULT __stdcall Add(BSTR sName, VARIANT vValue, VARIANT bNoReplace) = 0;
    virtual HRESULT __stdcall Remove(BSTR sName) = 0;
    virtual HRESULT __stdcall Import(BSTR sText) = 0;
};

// IWzResMan.idl
struct __declspec(uuid("57DFE40B-3E20-4DBC-97E8-805A50F381BF")) IWzResMan : public IUnknown {
    virtual HRESULT __stdcall get_rootNameSpace(IUnknown** ppVal) = 0;
    virtual HRESULT __stdcall put_rootNameSpace(IUnknown* pVal) = 0;
    virtual HRESULT __stdcall SetResManParam(RESMAN_PARAM nParam, INT nRetainTime, INT nNameSpaceCacheTime) = 0;
    virtual HRESULT __stdcall CreateObject(BSTR sUOL, IUnknown** ppObj) = 0;
    // 注：sUOL 在 .idl 中为 BSTR，此处声明为 const wchar_t* 以便直接传入
    // 字符串字面量；x86 stdcall 下两者二进制签名相同，不影响 vtable 槽位。
    virtual HRESULT __stdcall GetObject(const wchar_t* sUOL, VARIANT vParam, VARIANT vAux, VARIANT* pvObj) = 0;
    virtual HRESULT __stdcall SerializeObject(IWzArchive* pArchive, VARIANT vDispObject, IUnknown** ppObj) = 0;
    virtual HRESULT __stdcall FlushCachedObjects(INT nUsedBefore) = 0;
    virtual HRESULT __stdcall OverrideObject(BSTR sOrgUOL, BSTR sNewUOL) = 0;
};

// IWzCanvas.idl
struct __declspec(uuid("7600DC6C-9328-4BFF-9624-5B0F5C01179E")) IWzCanvas : public IWzSerialize {
    virtual HRESULT __stdcall get_defaultDither(CANVAS_DITHERTYPE* pnDefaultDither) = 0;
    virtual HRESULT __stdcall put_defaultDither(CANVAS_DITHERTYPE nDefaultDither) = 0;
    virtual HRESULT __stdcall get_defaultLevelMap(CANVAS_LEVELMAP* pnDefaultLevelMap) = 0;
    virtual HRESULT __stdcall put_defaultLevelMap(CANVAS_LEVELMAP nDefaultLevelMap) = 0;
    virtual HRESULT __stdcall get_defaultAllocator(IWzRawCanvasAllocator** ppAllocator) = 0;
    virtual HRESULT __stdcall put_defaultAllocator(IWzRawCanvasAllocator* pAllocator) = 0;
    virtual HRESULT __stdcall Create(UINT uWidth, UINT uHeight, VARIANT vMagLevel, VARIANT vPixFormat) = 0;
    virtual HRESULT __stdcall AddRawCanvas(INT x, INT y, IWzRawCanvas* pRawCanvas) = 0;
    virtual HRESULT __stdcall get_rawCanvas(INT x, INT y, IWzRawCanvas** ppRawCanvas) = 0;
    virtual HRESULT __stdcall get_tileWidth(UINT* puWidth) = 0;
    virtual HRESULT __stdcall get_tileHeight(UINT* puHeight) = 0;
    virtual HRESULT __stdcall get_width(UINT* puWidth) = 0;
    virtual HRESULT __stdcall put_width(UINT uWidth) = 0;
    virtual HRESULT __stdcall get_height(UINT* puHeight) = 0;
    virtual HRESULT __stdcall put_height(UINT uHeight) = 0;
    virtual HRESULT __stdcall get_pixelFormat(CANVAS_PIXFORMAT* pnFormat) = 0;
    virtual HRESULT __stdcall put_pixelFormat(CANVAS_PIXFORMAT nFormat) = 0;
    virtual HRESULT __stdcall get_magLevel(INT* pnMagLevel) = 0;
    virtual HRESULT __stdcall put_magLevel(INT nMagLevel) = 0;
    virtual HRESULT __stdcall GetSnapshotU(UINT* puWidth, UINT* puHeight, UINT* puTileWidth, UINT* puTileHeight, CANVAS_PIXFORMAT* pFormat, INT* pnMagLevel) = 0;
    virtual HRESULT __stdcall GetSnapshot(INT* pnWidth, INT* pnHeight, INT* pnTileWidth, INT* pnTileHeight, CANVAS_PIXFORMAT* pFormat, INT* pnMagLevel) = 0;
    virtual HRESULT __stdcall get_property(IWzProperty** ppProperty) = 0;
    virtual HRESULT __stdcall get_cx(INT* pnX) = 0;
    virtual HRESULT __stdcall put_cx(INT nX) = 0;
    virtual HRESULT __stdcall get_cy(INT* pnY) = 0;
    virtual HRESULT __stdcall put_cy(INT nY) = 0;
    virtual HRESULT __stdcall SetClipRect(INT nLeft, INT nTop, INT nWidth, INT nHeight, VARIANT bTemp, VARIANT* pvToken) = 0;
    virtual HRESULT __stdcall Copy(INT nDstLeft, INT nDstTop, IWzCanvas* pSource, VARIANT nAlpha) = 0;
    virtual HRESULT __stdcall CopyEx(INT nDstLeft, INT nDstTop, IWzCanvas* pSource, CANVAS_ALPHATYPE nAlpha, INT nWidth, INT nHeight, INT nSrcLeft, INT nSrcTop, INT nSrcWidth, INT nSrcHeight, VARIANT pAdjust) = 0;
    virtual HRESULT __stdcall get_pixel(INT nX, INT nY, UINT* puColor) = 0;
    virtual HRESULT __stdcall DrawRectangle(INT nLeft, INT nTop, UINT uWidth, UINT uHeight, UINT uColor) = 0;
    virtual HRESULT __stdcall DrawLine(INT x1, INT y1, INT x2, INT y2, UINT uColor, VARIANT nWidth) = 0;
    virtual HRESULT __stdcall DrawPolygon() = 0;
    virtual HRESULT __stdcall DrawText(INT nLeft, INT nTop, BSTR sText, IWzFont* pFont, VARIANT vAlpha, VARIANT vTabOrg, UINT* puHeight) = 0;
};

// IWzShape2D.idl
struct __declspec(uuid("4CFB57C7-E3EA-B340-AC98-4B2750E3642A")) IWzShape2D : public IWzSerialize {
    virtual HRESULT __stdcall get_item(VARIANT vIndex, VARIANT* pvValue) = 0;
    virtual HRESULT __stdcall get__NewEnum(IUnknown** pVal) = 0;
    virtual HRESULT __stdcall get_count(UINT* pVal) = 0;
    virtual HRESULT __stdcall get_x(INT* pVal) = 0;
    virtual HRESULT __stdcall put_x(INT newVal) = 0;
    virtual HRESULT __stdcall get_y(INT* pVal) = 0;
    virtual HRESULT __stdcall put_y(INT newVal) = 0;
    virtual HRESULT __stdcall get_x2(INT* pVal) = 0;
    virtual HRESULT __stdcall put_x2(INT newVal) = 0;
    virtual HRESULT __stdcall get_y2(INT* pVal) = 0;
    virtual HRESULT __stdcall put_y2(INT newVal) = 0;
    virtual HRESULT __stdcall Move(INT nX, INT nY) = 0;
    virtual HRESULT __stdcall Offset(INT nDX, INT nDY) = 0;
    virtual HRESULT __stdcall Scale(INT nXMul, INT nXDiv, INT nYMul, INT nYDiv, INT nXOrg, INT nYOrg) = 0;
    virtual HRESULT __stdcall Insert(VARIANT vIndexOrShape, VARIANT vShape) = 0;
    virtual HRESULT __stdcall Remove(VARIANT vIndex, VARIANT* pvRemoved) = 0;
    virtual HRESULT __stdcall Init(INT nX, INT nY) = 0;
};

// IWzVector2D.idl
struct __declspec(uuid("F28BD1ED-3DEB-4F92-9EEC-10EF5A1C3FB4")) IWzVector2D : public IWzShape2D {
    virtual HRESULT __stdcall get_currentTime(INT* pnTime) = 0;
    virtual HRESULT __stdcall put_currentTime(INT nTime) = 0;
    virtual HRESULT __stdcall get_origin(VARIANT* pvOrigin) = 0;
    virtual HRESULT __stdcall put_origin(VARIANT vOrigin) = 0;
    virtual HRESULT __stdcall get_rx(INT* pVal) = 0;
    virtual HRESULT __stdcall put_rx(INT newVal) = 0;
    virtual HRESULT __stdcall get_ry(INT* pVal) = 0;
    virtual HRESULT __stdcall put_ry(INT newVal) = 0;
    virtual HRESULT __stdcall get_a(DOUBLE* pVal) = 0;
    virtual HRESULT __stdcall get_ra(DOUBLE* pVal) = 0;
    virtual HRESULT __stdcall put_ra(DOUBLE newVal) = 0;
    virtual HRESULT __stdcall get_flipX(INT* pVal) = 0;
    virtual HRESULT __stdcall put_flipX(INT newVal) = 0;
    virtual HRESULT __stdcall _GetSnapshot(INT* px, INT* py, INT* prx, INT* pry, INT* pxOrg, INT* pyOrg, DOUBLE* pa, DOUBLE* paOrg, VARIANT vTime) = 0;
    virtual HRESULT __stdcall RelMove(INT nX, INT nY, VARIANT nTime, VARIANT nType) = 0;
    virtual HRESULT __stdcall RelOffset(INT nDX, INT nDY, VARIANT nTime, VARIANT nType) = 0;
    virtual HRESULT __stdcall Ratio(IWzVector2D* pOrigin, INT nOriginWidth, INT nOriginHeight, INT nScaleWidth, INT nScaleHeight) = 0;
    virtual HRESULT __stdcall WrapClip(VARIANT pOrigin, INT nWrapLeft, INT nWrapTop, UINT uWrapWidth, UINT uWrapHeight, VARIANT bClip) = 0;
    virtual HRESULT __stdcall Rotate(DOUBLE dAngleAmount, VARIANT nTime) = 0;
    virtual HRESULT __stdcall get_looseLevel(UINT* puLevel) = 0;
    virtual HRESULT __stdcall put_looseLevel(UINT uLevel) = 0;
    virtual HRESULT __stdcall Fly(VARIANT* aVar, INT nArgs) = 0;
};

// IWzGr2DLayer.idl
struct __declspec(uuid("6DC8C7CE-8E81-4420-B4F6-4B60B7D5FCDF")) IWzGr2DLayer : public IWzVector2D {
    virtual HRESULT __stdcall get_z(INT* pnZ) = 0;
    virtual HRESULT __stdcall put_z(INT nZ) = 0;
    virtual HRESULT __stdcall get_width(INT* pnWidth) = 0;
    virtual HRESULT __stdcall put_width(INT nWidth) = 0;
    virtual HRESULT __stdcall get_height(INT* pnHeight) = 0;
    virtual HRESULT __stdcall put_height(INT nHeight) = 0;
    virtual HRESULT __stdcall get_lt(IWzVector2D** ppVector) = 0;
    virtual HRESULT __stdcall get_rb(IWzVector2D** ppVector) = 0;
    virtual HRESULT __stdcall InterLockedOffset(INT xlt, INT ylt, INT xrb, INT yrb) = 0;
    virtual HRESULT __stdcall get_flip(INT* pnFlip) = 0;
    virtual HRESULT __stdcall put_flip(INT nFlip) = 0;
    virtual HRESULT __stdcall get_color(UINT* puColor) = 0;
    virtual HRESULT __stdcall put_color(UINT uColor) = 0;
    virtual HRESULT __stdcall get_alpha(IWzVector2D** ppVector) = 0;
    virtual HRESULT __stdcall get_redTone(IWzVector2D** ppVector) = 0;
    virtual HRESULT __stdcall get_greenBlueTone(IWzVector2D** ppVector) = 0;
    virtual HRESULT __stdcall get_blend(LAYER_BLENDTYPE* pnBlend) = 0;
    virtual HRESULT __stdcall put_blend(LAYER_BLENDTYPE nblend) = 0;
    virtual HRESULT __stdcall get_overlay(VARIANT* pvLayer) = 0;
    virtual HRESULT __stdcall put_overlay(VARIANT vLayer) = 0;
    virtual HRESULT __stdcall get_canvas(VARIANT vIndex, IWzCanvas** ppCanvas) = 0;
    virtual HRESULT __stdcall InsertCanvas(IWzCanvas* pCanvas, VARIANT vDelay, VARIANT vAlpha0, VARIANT vAlpha1, VARIANT vZoom0, VARIANT vZoom1, VARIANT* pvIndex) = 0;
    virtual HRESULT __stdcall RemoveCanvas(VARIANT vIndex, IWzCanvas** ppCanvas) = 0;
    virtual HRESULT __stdcall ShiftCanvas(VARIANT vValue) = 0;
    virtual HRESULT __stdcall Animate(GR2D_ANITYPE nType, VARIANT vDelayRate, VARIANT vRepeat) = 0;
    virtual HRESULT __stdcall get_animationState(INT* pnState) = 0;
    virtual HRESULT __stdcall get_visible(INT* pnVisible) = 0;
    virtual HRESULT __stdcall put_visible(INT nVisible) = 0;
    virtual HRESULT __stdcall RenderFlash(INT nRenderFlash) = 0;
};

// IWzNameSpace.idl
struct __declspec(uuid("A0B5164F-F89B-41B8-8264-15371FD02B4C")) IWzNameSpaceProperty : public IUnknown {
    virtual HRESULT __stdcall get_item(VARIANT vAux, VARIANT* pvObj) = 0;
    virtual HRESULT __stdcall get_name(BSTR* psVal) = 0;
    virtual HRESULT __stdcall get_path(BSTR* psVal) = 0;
    virtual HRESULT __stdcall get_size(VARIANT* pvVal) = 0;
    virtual HRESULT __stdcall get_checksum(INT* pnVal) = 0;
    virtual HRESULT __stdcall get_property(BSTR sPropName, VARIANT* pvVal) = 0;
};

struct __declspec(uuid("2AEEEB36-A4E1-4E2B-8F6F-2E7BDEC5C53D")) IWzNameSpace : public IUnknown {
    // 注：[in] BSTR 一律声明为 const wchar_t*（x86 stdcall 下二进制签名相同）
    virtual HRESULT __stdcall get_item(const wchar_t* sPath, VARIANT* pvObj) = 0;
    virtual HRESULT __stdcall get_property(const wchar_t* sPath, VARIANT vpPathUsed, IWzNameSpaceProperty** ppVal) = 0;
    virtual HRESULT __stdcall get__NewEnum(IUnknown** pVal) = 0;
    virtual HRESULT __stdcall Mount(const wchar_t* sPath, IWzNameSpace* pDown, INT nPriority) = 0;
    virtual HRESULT __stdcall Unmount(const wchar_t* sPath, VARIANT vDown) = 0;
    virtual HRESULT __stdcall _OnMountEvent(IWzNameSpace* pUp, IWzNameSpace* pRoot, const wchar_t* sLocation, INT nState) = 0;
    virtual HRESULT __stdcall _OnGetLocalObject(INT nIndex, const wchar_t* sPath, INT* pnPathUsed, VARIANT* pvRet) = 0;
};

// IWzWritableNameSpace.idl
struct __declspec(uuid("EE7659A2-23CD-4E44-86E5-D82A680E83F7")) IWzWritableNameSpace : public IWzNameSpace {
    virtual HRESULT __stdcall CreateChildNameSpace(const wchar_t* sName, IWzNameSpace** ppNameSpace) = 0;
    virtual HRESULT __stdcall AddObject(const wchar_t* sName, VARIANT vObj, VARIANT* pvObj) = 0;
    virtual HRESULT __stdcall Unlink(const wchar_t* sName) = 0;
};

// IWzFileSystem.idl
struct __declspec(uuid("352D8655-51E4-4668-8CE4-0866E2B6A5B5")) IWzFileSystem : public IWzWritableNameSpace {
    virtual HRESULT __stdcall Init(const wchar_t* sPath) = 0;
};

// IWzArchive.idl
struct __declspec(uuid("3C39B9AA-18CC-408C-8716-C4FB2117266E")) IWzArchive : public IUnknown {
    virtual HRESULT __stdcall get_loading(INT* pbLoading) = 0;
    virtual HRESULT __stdcall Read(BYTE* pv, UINT cb, UINT* pcbRead) = 0;
    virtual HRESULT __stdcall Write(BYTE* pBuffer, UINT cb, UINT* pcbWritten) = 0;
    virtual HRESULT __stdcall get_absoluteUOL(BSTR* pVal) = 0;
    virtual HRESULT __stdcall put_absoluteUOL(const wchar_t* newVal) = 0;
    virtual HRESULT __stdcall get_position(UINT* puPos) = 0;
    virtual HRESULT __stdcall get_context(VARIANT* pvContext) = 0;
    virtual HRESULT __stdcall put_context(VARIANT vContext) = 0;
};

// IWzSeekableArchive.idl
struct __declspec(uuid("35C1F133-7F61-496E-878F-9A1758AFA9EA")) IWzSeekableArchive : public IWzArchive {
    virtual HRESULT __stdcall put_position(UINT uPos) = 0;
    virtual HRESULT __stdcall get_length(UINT* puLength) = 0;
    virtual HRESULT __stdcall put_length(UINT uLength) = 0;
    virtual HRESULT __stdcall get_address(VARIANT* pv) = 0;
    virtual HRESULT __stdcall Rewind() = 0;
    virtual HRESULT __stdcall ReadLock(BSTR sRelUOL, UINT uSize, IWzSeekableArchive** ppArchive) = 0;
};

// IWzPackage.idl
struct __declspec(uuid("E610818B-038D-4522-9232-30FCD5F4737C")) IWzPackage : public IWzNameSpace {
    virtual HRESULT __stdcall Init(const wchar_t* sKey, const wchar_t* sBaseUOL, IWzSeekableArchive* pArchive) = 0;
};

// IWzGr2D.idl
struct __declspec(uuid("E576EA33-D465-4F08-AAB1-E78DF73EE6D9")) IWzGr2D : public IUnknown {
    virtual HRESULT __stdcall Initialize(UINT uWidth, UINT uHeight, VARIANT vHwnd, VARIANT vBPP, VARIANT vRefreshRate) = 0;
    virtual HRESULT __stdcall Uninitialize() = 0;
    virtual HRESULT __stdcall get_nextRenderTime(INT* pnTime) = 0;
    virtual HRESULT __stdcall UpdateCurrentTime(INT tTime) = 0;
    virtual HRESULT __stdcall RenderFrame() = 0;
    virtual HRESULT __stdcall SetFrameSkip() = 0;
    virtual HRESULT __stdcall ToggleFpsPanel() = 0;
    virtual HRESULT __stdcall DisableFpsPanel() = 0;
    virtual HRESULT __stdcall get_width(UINT* puWidth) = 0;
    virtual HRESULT __stdcall get_height(UINT* puHeight) = 0;
    virtual HRESULT __stdcall get_bpp(UINT* puBPP) = 0;
    virtual HRESULT __stdcall get_refreshRate(UINT* puRefreshRate) = 0;
    virtual HRESULT __stdcall get_fps100(UINT* puFps100) = 0;
    virtual HRESULT __stdcall get_currentTime(INT* pnCurrentTime) = 0;
    virtual HRESULT __stdcall get_fullScreen(INT* pnFullScreen) = 0;
    virtual HRESULT __stdcall put_fullScreen(INT nFullScreen) = 0;
    virtual HRESULT __stdcall get_backColor(UINT* puColor) = 0;
    virtual HRESULT __stdcall put_backColor(UINT uColor) = 0;
    virtual HRESULT __stdcall get_redTone(IWzVector2D** ppVector) = 0;
    virtual HRESULT __stdcall get_greenBlueTone(IWzVector2D** ppVector) = 0;
    virtual HRESULT __stdcall get_center(IWzVector2D** ppVector) = 0;
    virtual HRESULT __stdcall GetSnapshot(VARIANT pvBuffer, UINT uBufferSize, INT x, INT y) = 0;
    virtual HRESULT __stdcall CreateLayer(INT nLeft, INT nTop, UINT uWidth, UINT uHeight, INT nZ, VARIANT vCanvas, VARIANT dwFilter, IWzGr2DLayer** ppLayer) = 0;
    virtual HRESULT __stdcall InitializeFlashByDevice() = 0;
    virtual HRESULT __stdcall LoadMediaFile(const char* sSource) = 0;
    virtual HRESULT __stdcall SetFlashAbsLocation(INT nX, INT nY) = 0;
    virtual HRESULT __stdcall RenderFlash() = 0;
    virtual HRESULT __stdcall ReleaseFlash() = 0;
    virtual HRESULT __stdcall IsRenderFlash(INT* pnRenderFlash) = 0;
};

// ---- 智能指针 ----

_COM_SMARTPTR_TYPEDEF(IUnknown, __uuidof(IUnknown));
_COM_SMARTPTR_TYPEDEF(IEnumVARIANT, __uuidof(IEnumVARIANT));
_COM_SMARTPTR_TYPEDEF(IWzSerialize, __uuidof(IWzSerialize));
_COM_SMARTPTR_TYPEDEF(IWzProperty, __uuidof(IWzProperty));
_COM_SMARTPTR_TYPEDEF(IWzResMan, __uuidof(IWzResMan));
_COM_SMARTPTR_TYPEDEF(IWzCanvas, __uuidof(IWzCanvas));
_COM_SMARTPTR_TYPEDEF(IWzShape2D, __uuidof(IWzShape2D));
_COM_SMARTPTR_TYPEDEF(IWzVector2D, __uuidof(IWzVector2D));
_COM_SMARTPTR_TYPEDEF(IWzGr2DLayer, __uuidof(IWzGr2DLayer));
_COM_SMARTPTR_TYPEDEF(IWzNameSpaceProperty, __uuidof(IWzNameSpaceProperty));
_COM_SMARTPTR_TYPEDEF(IWzNameSpace, __uuidof(IWzNameSpace));
_COM_SMARTPTR_TYPEDEF(IWzWritableNameSpace, __uuidof(IWzWritableNameSpace));
_COM_SMARTPTR_TYPEDEF(IWzFileSystem, __uuidof(IWzFileSystem));
_COM_SMARTPTR_TYPEDEF(IWzArchive, __uuidof(IWzArchive));
_COM_SMARTPTR_TYPEDEF(IWzSeekableArchive, __uuidof(IWzSeekableArchive));
_COM_SMARTPTR_TYPEDEF(IWzPackage, __uuidof(IWzPackage));
_COM_SMARTPTR_TYPEDEF(IWzGr2D, __uuidof(IWzGr2D));
