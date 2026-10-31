#pragma once
// ============================================================================
// CustomWz.h — Custom.wz 挂载开关（移植自 kaentake resman.cpp）
//
// 在客户端 CWvsApp::InitializeResMan 完成原始 WZ 挂载之后，向根命名空间
// 挂载 exe 同目录下的 Custom.wz，并对覆盖清单做两处回退钩子：
//   1) NameSpace.dll raw__OnGetLocalObject —— 原命名空间找不到的 UOL 回退到
//      Custom 命名空间；
//   2) PCOM.dll CWzProperty::raw_Serialize —— 序列化覆盖节点时把 Custom 的
//      非子属性叶子合并进原属性。
// ============================================================================

namespace CustomWz {
// bEnable=false 时不安装任何钩子（保持原版行为）
void Install(bool bEnable);
}
