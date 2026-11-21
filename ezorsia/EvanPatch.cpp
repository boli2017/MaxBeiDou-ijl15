#include "stdafx.h"
#include "EvanPatch.h"
#include "Memory.h"
#include <iostream>
#include <cstring>

namespace EvanPatch {
    namespace {
        // 已在 BeiDou.exe（image base 0x400000）上反汇编验证：
        // 两处均是「职业 ID 属于 22xx 或 2001 时提前 return false」的 Evan 技能获取门禁。
        constexpr DWORD GATE1_ADDR = 0x0075C783;
        constexpr DWORD GATE2_ADDR = 0x00761714;
        constexpr DWORD SKILL_TAB_ADDR = 0x00BE2C68; // int，原值 5（技能窗口页数）

        constexpr unsigned char GATE1_OLD[4] = {0x33, 0xC0, 0xEB, 0x2E}; // xor eax,eax; jmp +0x2e
        constexpr unsigned char GATE2_OLD[21] = {
            0x83, 0xF8, 0x16,                         // cmp eax, 0x16
            0x0F, 0x84, 0xD7, 0x00, 0x00, 0x00,       // je  +0xd7
            0x81, 0xFE, 0xD1, 0x07, 0x00, 0x00,       // cmp esi, 0x7d1
            0x0F, 0x84, 0xCB, 0x00, 0x00, 0x00        // je  +0xcb
        };
        constexpr unsigned char TAB_OLD = 0x05;

        bool BytesMatch(DWORD addr, const unsigned char* expected, size_t count) {
            return memcmp(reinterpret_cast<const void*>(addr), expected, count) == 0;
        }

        bool AllNop(DWORD addr, size_t count) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(addr);
            for (size_t i = 0; i < count; ++i) {
                if (p[i] != 0x90) return false;
            }
            return true;
        }

        void Report(const char* name, const char* state) {
            std::cout << "[EvanPatch] " << name << ": " << state << std::endl;
        }
    }

    void Install(bool enable) {
        if (!enable) {
            Report("disabled", "by config.ini, no memory patched");
            return;
        }

        if (BytesMatch(GATE1_ADDR, GATE1_OLD, sizeof(GATE1_OLD))) {
            Memory::PatchNop(GATE1_ADDR, static_cast<int>(sizeof(GATE1_OLD)));
            Report("skill gate #1", "NOP 4 bytes at 0x75C783");
        } else if (AllNop(GATE1_ADDR, sizeof(GATE1_OLD))) {
            Report("skill gate #1", "already patched");
        } else {
            Report("skill gate #1", "SKIPPED: unexpected bytes (wrong EXE?)");
        }

        if (BytesMatch(GATE2_ADDR, GATE2_OLD, sizeof(GATE2_OLD))) {
            Memory::PatchNop(GATE2_ADDR, static_cast<int>(sizeof(GATE2_OLD)));
            Report("skill gate #2", "NOP 21 bytes at 0x761714");
        } else if (AllNop(GATE2_ADDR, sizeof(GATE2_OLD))) {
            Report("skill gate #2", "already patched");
        } else {
            Report("skill gate #2", "SKIPPED: unexpected bytes (wrong EXE?)");
        }

        const unsigned char tab = *reinterpret_cast<const unsigned char*>(SKILL_TAB_ADDR);
        if (tab == TAB_OLD) {
            Memory::WriteByte(SKILL_TAB_ADDR, 0x06);
            Report("skill tab count", "5 -> 6 at 0xBE2C68");
        } else if (tab == 0x06) {
            Report("skill tab count", "already 6");
        } else {
            Report("skill tab count", "SKIPPED: unexpected value (wrong EXE?)");
        }
    }
}
