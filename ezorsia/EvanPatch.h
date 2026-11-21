#pragma once

// 龙神（Evan）v83 客户端补丁。
// 仅对已验证的 GMS083 镜像生效：地址或原始字节不符时跳过对应补丁并记录日志。
namespace EvanPatch {
    void Install(bool enable);
}
