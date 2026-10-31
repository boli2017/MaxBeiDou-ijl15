#pragma once
// ============================================================================
// MobHpTag.h — 全怪物血条百分比标签
//
// 移植自 kaentake 项目（src/mobhptag.cpp）：在客户端原生的怪物血条
// （CField::ShowMobHPTag 绘制的 HPTag 层）下方追加一行 HP 百分比数字。
//
// 依赖说明：
//  - 血条 hook 本体复用 BossHP::HookShowMobHPTag（同一目标地址 0x005336CA），
//    本模块只追加绘制逻辑与 GetHeight 调用点补丁，避免对同一地址二次 detour。
//  - 需要客户端 WZ 中存在 UI/UIWindowEx.img/MobHPRatioInfo 资源；缺失时静默
//    跳过绘制，不会崩溃。
// ============================================================================

class MobHpTag
{
public:
	// config.ini [optional] ShowMobHPTag，需在 DllMain 中赋值后再调用 Hook()
	static bool enabled;

	static void Hook();

	// 由 BossHP::HookShowMobHPTag 在调用原始 ShowMobHPTag 之后回调
	static void OnShowMobHPTag(void* pField, unsigned int dwMobID, int nHP, int nMaxHP);
};
