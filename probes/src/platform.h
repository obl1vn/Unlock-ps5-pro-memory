// SPDX-License-Identifier: GPL-3.0-or-later
// What the probes need from the machine they run on: a Vulkan entry point,
// a clock, somewhere to print, and a way to hand over the finished report.
#pragma once
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

PFN_vkGetInstanceProcAddr plat_get_instance_proc_addr(void);
double plat_now_seconds(void);
void plat_log(const char *line);
// 1 on a desktop test machine (smaller workloads, capped allocation test).
int plat_is_host(void);
// 1 to skip the allocation-ceiling test (the one that fills GPU memory).
int plat_skip_memory_test(void);
// Saves the report so far to disk (before the risky memory test).
void plat_checkpoint(const char *report);
// Called once with the whole report; may not return (the console serves it).
void plat_finish(const char *report);

// Lines describing memory as the operating system sees it (console only).
void plat_system_info(char *out, unsigned long size);

int run_probes(void);
const char *probe_report(void);

#ifdef __cplusplus
}
#endif
