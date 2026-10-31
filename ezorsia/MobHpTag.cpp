#include "stdafx.h"
#include "MobHpTag.h"
#include "Memory.h"
#include "WzLib/WzInterfaces.h"
#include "WzLib/ClientZtl.h"

// 地址与常量（与 kaentake 一致，v83 客户端）
static const DWORD dwShowMobHPTag_GetHeightCall = 0x00533AFD; // ShowMobHPTag 内对 HPTag 高度的调用点
static const DWORD dwClientGetUnknown = 0x00414ADA;           // Ztl_variant_t -> IUnknownPtr 辅助函数
static const DWORD dwDrawTextByImage = 0x0098812C;            // 按图片数字绘制文本
static const DWORD dwResManInstance = 0x00BF14E8;             // 客户端 IWzResManPtr 全局

// CField::m_pLayerHPTag 成员偏移
static const unsigned int FIELD_HPTAG_LAYER_OFFSET = 0x1E4;

bool MobHpTag::enabled = false;

static IWzResManPtr& GetResMan() {
    return *reinterpret_cast<IWzResManPtr*>(dwResManInstance);
}

// 客户端辅助函数：Ztl_variant_t -> IUnknownPtr
static IUnknownPtr ClientGetUnknown(Ztl_variant_t& v) {
    typedef IUnknown* (__cdecl* ClientGetUnknown_t)(IUnknown**, Ztl_variant_t&);
    static auto fn = reinterpret_cast<ClientGetUnknown_t>(dwClientGetUnknown);
    IUnknownPtr pUnk;
    fn(&pUnk, v);
    return pUnk;
}

static bool GetObjectVariant(const wchar_t* sUOL, Ztl_variant_t& vObj) {
    IWzResManPtr& pResMan = GetResMan();
    if (!pResMan) {
        return false;
    }
    return SUCCEEDED(pResMan->GetObject(sUOL, vtEmpty, vtEmpty, &vObj));
}

static IWzCanvasPtr GetObjectCanvas(const wchar_t* sUOL) {
    IWzCanvasPtr pCanvas;
    Ztl_variant_t vObj;
    if (!GetObjectVariant(sUOL, vObj)) {
        return nullptr;
    }
    IUnknownPtr pUnk = ClientGetUnknown(vObj);
    if (!pUnk || FAILED(pUnk->QueryInterface(__uuidof(IWzCanvas), reinterpret_cast<void**>(&pCanvas)))) {
        return nullptr;
    }
    return pCanvas;
}

static IWzPropertyPtr GetObjectProperty(const wchar_t* sUOL) {
    IWzPropertyPtr pProp;
    Ztl_variant_t vObj;
    if (!GetObjectVariant(sUOL, vObj)) {
        return nullptr;
    }
    IUnknownPtr pUnk = ClientGetUnknown(vObj);
    if (!pUnk || FAILED(pUnk->QueryInterface(__uuidof(IWzProperty), reinterpret_cast<void**>(&pProp)))) {
        return nullptr;
    }
    return pProp;
}

// 按图片数字绘制文本（客户端函数；ZXString 参数必须来自客户端分配器，
// 见 WzLib/ClientZtl.h）
static auto draw_text_by_image = reinterpret_cast<int(__cdecl*)(
    IWzCanvasPtr pCanvas, int nLeft, int nTop,
    client_ztl::ZXStringChar sText, IWzPropertyPtr pBase, int nHorzSpace)>(dwDrawTextByImage);

static IWzCanvasPtr g_pCanvasHpBack;
static IWzPropertyPtr g_pPropHpNum;

// ShowMobHPTag 内部查询 HPTag 高度的调用点补丁：固定返回 54，
// 在血条下方留出百分比数字的绘制空间（与 kaentake 一致）
static unsigned int __fastcall ShowMobHPTag_GetHeight_hook(IWzCanvas* /*pThis*/, void* /*_EDX*/) {
    return 54;
}

void MobHpTag::Hook() {
    if (!enabled) {
        return;
    }
    Memory::WriteByte(dwShowMobHPTag_GetHeightCall, 0xE8);
    Memory::WriteInt(dwShowMobHPTag_GetHeightCall + 1,
                     static_cast<unsigned int>(reinterpret_cast<DWORD>(&ShowMobHPTag_GetHeight_hook) - dwShowMobHPTag_GetHeightCall - 5));
}

void MobHpTag::OnShowMobHPTag(void* pField, unsigned int /*dwMobID*/, int nHP, int nMaxHP) {
    if (!enabled || !pField || nMaxHP <= 0) {
        return;
    }

    // CField::m_pLayerHPTag（原生怪物血条层）
    IWzGr2DLayerPtr& pLayerHPTag = *reinterpret_cast<IWzGr2DLayerPtr*>(
        reinterpret_cast<uintptr_t>(pField) + FIELD_HPTAG_LAYER_OFFSET);
    if (!pLayerHPTag) {
        return;
    }

    IWzCanvas* pRawCanvas = nullptr;
    if (FAILED(pLayerHPTag->get_canvas(Ztl_variant_t(static_cast<long>(0)), &pRawCanvas)) || !pRawCanvas) {
        return;
    }
    IWzCanvasPtr pCanvas;
    pCanvas.Attach(pRawCanvas);

    // 惰性加载绘制资源（UI/UIWindowEx.img/MobHPRatioInfo）；缺失时静默跳过
    if (!g_pCanvasHpBack) {
        g_pCanvasHpBack = GetObjectCanvas(L"UI/UIWindowEx.img/MobHPRatioInfo/mob/0/backgrnd");
        if (!g_pCanvasHpBack) {
            return;
        }
    }
    if (!g_pPropHpNum) {
        g_pPropHpNum = GetObjectProperty(L"UI/UIWindowEx.img/MobHPRatioInfo/num");
        if (!g_pPropHpNum) {
            return;
        }
    }

    // 绘制血条底图
    pCanvas->Copy(0, 37, g_pCanvasHpBack, vtEmpty);

    // 绘制 HP 百分比
    char sRatio[20];
    float fRatio = static_cast<float>(nHP) / static_cast<float>(nMaxHP);
    if (fRatio > 0.99f) {
        sprintf_s(sRatio, 20, "100%%");
    } else {
        sprintf_s(sRatio, 20, "%.1f%%", fRatio * 100);
    }
    int nRatioLeft = fRatio > 0.99f ? 7 : (fRatio >= 0.1f ? 5 : 11);
    draw_text_by_image(pCanvas, nRatioLeft, 42, client_ztl::ZXStringChar(sRatio), g_pPropHpNum, 1);
}
