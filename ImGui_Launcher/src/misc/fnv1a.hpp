#pragma once
#include "pch.hpp"

extern "C" uint32_t fnv1a_32_asm(const char* str);

namespace misc
{
    //算法练习测试
    inline uint32_t fnv1a_32(const char* str)
    {
        constexpr uint32_t offset_basis = 0x811C9DC5;
        constexpr uint32_t prime = 0x01000193;
        uint32_t hash = offset_basis;
        size_t len = strlen(str);
        for (int i = 0; i < len; i++)
        {
            hash ^= static_cast<uint8_t>(str[i]);
            hash *= prime;
        }
        return hash;
    }
}