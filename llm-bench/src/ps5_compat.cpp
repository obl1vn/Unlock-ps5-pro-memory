// SPDX-License-Identifier: GPL-3.0-or-later
// C library functions that the PS5 runtime lacks or gets wrong, taken unchanged
// from ProsperoAI (src/runtime_support.cpp, Copyright (C) 2026 BlackBearReloaded)
// and linked the same way as its release (--defsym name=prospero_name).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <pthread.h>
extern "C" int sceKernelUsleep(std::uint32_t microseconds);
extern "C" int sceKernelDebugOutText(int channel, const char *text);

extern "C" double prospero_strtod(const char *value, char **end);

extern "C" int prospero_pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
    constexpr int running = 2;
    int state = __atomic_load_n(&once_control->state, __ATOMIC_ACQUIRE);
    if (state == PTHREAD_DONE_INIT)
        return 0;

    int expected = PTHREAD_NEEDS_INIT;
    if (__atomic_compare_exchange_n(&once_control->state, &expected, running, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        init_routine();
        __atomic_store_n(&once_control->state, PTHREAD_DONE_INIT, __ATOMIC_RELEASE);
        return 0;
    }

    while (__atomic_load_n(&once_control->state, __ATOMIC_ACQUIRE) != PTHREAD_DONE_INIT)
    {
        sceKernelUsleep(100);
    }
    return 0;
}

extern "C" float prospero_strtof(const char *value, char **end)
{
    return static_cast<float>(prospero_strtod(value, end));
}

extern "C" double prospero_strtod(const char *value, char **end)
{
    const char *cursor = value;
    const char *input_end = value + std::strlen(value);
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r' ||
           *cursor == '\f' || *cursor == '\v')
        ++cursor;
    const char *number = cursor;
    bool negative = false;
    if (*cursor == '+' || *cursor == '-')
        negative = *cursor++ == '-';
    double result = 0;
    int fractional_digits = 0;
    bool has_digits = false;
    while (cursor < input_end && *cursor >= '0' && *cursor <= '9')
    {
        has_digits = true;
        result = result * 10 + (*cursor++ - '0');
    }
    if (cursor < input_end && *cursor == '.')
    {
        ++cursor;
        while (cursor < input_end && *cursor >= '0' && *cursor <= '9')
        {
            has_digits = true;
            result = result * 10 + (*cursor++ - '0');
            ++fractional_digits;
        }
    }
    if (!has_digits)
    {
        if (end)
            *end = const_cast<char *>(value);
        return 0;
    }
    int exponent = -fractional_digits;
    if (cursor < input_end && (*cursor == 'e' || *cursor == 'E'))
    {
        const char *marker = cursor++;
        bool exponent_negative = false;
        if (cursor < input_end && (*cursor == '+' || *cursor == '-'))
            exponent_negative = *cursor++ == '-';
        if (cursor >= input_end || *cursor < '0' || *cursor > '9')
            cursor = marker;
        else
        {
            int parsed_exponent = 0;
            while (cursor < input_end && *cursor >= '0' && *cursor <= '9')
            {
                parsed_exponent =
                    parsed_exponent < 10000 ? parsed_exponent * 10 + (*cursor - '0') : 10000;
                ++cursor;
            }
            exponent += exponent_negative ? -parsed_exponent : parsed_exponent;
        }
    }
    if (exponent > 308)
        result = std::numeric_limits<double>::infinity();
    else if (exponent < -324)
        result = 0;
    else if (exponent)
        result *= std::pow(10.0, exponent);
    if (end)
        *end = const_cast<char *>(cursor == number ? value : cursor);
#if 1
    if (cursor != input_end)
    {
        char line[128];
        std::snprintf(line, sizeof(line), "[ProsperoAI] strtod scanned=%td size=%td first=%02x\n",
                      cursor - value, input_end - value, static_cast<unsigned char>(*input_end));
        sceKernelDebugOutText(0, line);
    }
#endif
    return negative ? -result : result;
}

extern "C" int prospero_fseek(std::FILE *file, long offset, int origin)
{
    return fseeko(file, offset, origin);
}

extern "C" long prospero_ftell(std::FILE *file)
{
    return static_cast<long>(ftello(file));
}

extern "C" char *prospero_strcasestr(const char *haystack, const char *needle)
{
    if (!*needle)
        return const_cast<char *>(haystack);
    for (; *haystack; ++haystack)
    {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n)
        {
            const char hc = *h >= 'A' && *h <= 'Z' ? static_cast<char>(*h + ('a' - 'A')) : *h;
            const char nc = *n >= 'A' && *n <= 'Z' ? static_cast<char>(*n + ('a' - 'A')) : *n;
            if (hc != nc)
                break;
            ++h;
            ++n;
        }
        if (!*n)
            return const_cast<char *>(haystack);
    }
    return nullptr;
}

