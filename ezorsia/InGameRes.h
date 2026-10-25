#pragma once
#include <string>
#include <vector>

// 游戏内实时切换分辨率（热键循环，无需重启客户端）。
//
// 移植来源：kaentake/src/resolution.cpp。kaentake 用的是"全动态钩子"路线
// （钩客户端函数、坐标全部现算、系统设置里放下拉框），而 ezorsia 的分辨率
// 方案是"启动时把宽高写进客户端几百处常量/代码洞穴"（Client::UpdateResolution），
// 两条路线不兼容，不能直接搬代码。本模块把 kaentake 的"运行时切换"能力
// 移植到 ezorsia 的方案上，核心是四步（详见 InGameRes.cpp 顶部注释）：
//   1. 用新宽高重跑 Client::UpdateResolution()——补丁幂等，代码洞穴读的
//      都是全局参数，重跑一次全部刷新；
//   2. 直接操作 Gr2D_DX8 的 CWzGr2D 对象换显示模式，并置
//      m_hrErrorCode = D3DERR_DEVICENOTRESET，客户端下一帧自己重置设备；
//   3. 把 CWndMan::m_pOrgWindow 锚点 RelMove 到新屏幕中心——所有窗口图层
//      都以它为 origin，锚点一动全部跟着动（已反汇编 BeiDou.exe 验证：
//      CWnd::CreateWnd 里 layer->put_origin(CWndMan::m_pOrgWindow)）；
//   4. 重算地图视野范围并同步主窗口大小。
//
// 已知限制（与 kaentake 全动态方案的差距，详见 InGameRes.cpp 末尾注释）：
//   - 切换只对"锚点定位的元素 + 之后新建的窗口"完全正确。状态栏的血条/
//     蓝条/快捷栏、聊天框等常驻 UI 的坐标是启动时写死在客户端代码里的，
//     切换后已创建的元素会偏移（分辨率跨度越大越明显），需要重新登录
//     （或关闭重开）才能恢复；地图/视野/窗口大小/D3D 立即正确；
//   - 目标分辨率必须真实存在于显卡模式表里（ModeTableFix 只补启动那一档），
//     找不到就保持原分辨率不动；
//   - 切换幅度小的时候（如 1280x720 <-> 1366x768）偏移很小，几乎看不出来。
namespace InGameRes {

	struct Resolution {
		int width;
		int height;
	};

	extern bool g_bEnabled;                 // config.ini [general] InGameResSwitch
	extern bool g_bSaveToConfig;            // config.ini [general] InGameResSave
	extern int g_nHotkey;                   // config.ini [general] InGameResHotkey（虚拟键码）
	extern std::vector<Resolution> g_aResolutions;  // config.ini [general] InGameResList
	extern int g_nCurrentIndex;             // 当前档位（-1 = 当前分辨率不在列表里）

	// 解析 "800x600,1024x768,..." 形式的分辨率列表，非法项自动跳过。
	void ParseResolutionList(const std::string& sList);

	// 装每帧轮询钩子（CWndMan::s_Update）。在 dllmain 里 Client::UpdateResolution() 之后调用。
	void Install(bool enable);

	// 切换到第 nIndex 档，返回是否成功（模式表里没有该分辨率时返回 false 且保持原状）。
	bool ApplyResolution(int nIndex);
}
