#pragma once
// ============================================================================
// InGameRes.h — 运行时动态分辨率（移植自 kaentake resolution.cpp）
//
// 组成：CUISysOpt 注入分辨率下拉框 + CWndMan 九锚点重绑 + 窗口/光标/对话框/
// tooltip/状态栏/buff 栏/地图视野/天气/滑动公告 全套重排。
//
// 初始分辨率沿用 config.ini 的 width/height（登录、商城等非 CField 阶段保持
// 初始分辨率，与 ezorsia 既有静态补丁兼容）；进入游戏地图（CField）后切换到
// 系统选项里选择并持久化（soScreenResolution）的档位。
// ============================================================================

namespace InGameRes {
// bEnable=false 时不安装任何钩子（保持原版行为）。必须在 Client::UpdateResolution()
// 之后调用（部分常量补丁需要覆盖 ezorsia 的静态值）。
void Install(bool bEnable);

// 把 config.ini 的初始分辨率告知本模块（在 Install 之前调用）
void SetInitialResolution(int nWidth, int nHeight);

int GetScreenWidth();
int GetScreenHeight();
}
