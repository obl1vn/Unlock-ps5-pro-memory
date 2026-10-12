// SPDX-License-Identifier: GPL-3.0-or-later
// Desktop test build: any Vulkan driver (lavapipe works), output to stdout.
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

PFN_vkGetInstanceProcAddr plat_get_instance_proc_addr(void) { return vkGetInstanceProcAddr; }

double plat_now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void plat_log(const char *line)
{
    fputs(line, stdout);
    fflush(stdout);
}

int plat_is_host(void) { return 1; }

int plat_skip_memory_test(void) { return getenv("PROBE_SKIP_MEMORY_TEST") != NULL; }

void plat_system_info(char *out, unsigned long size) { snprintf(out, size, "  (not available on the host)\n"); }

void plat_checkpoint(const char *report) { (void)report; }

void plat_finish(const char *report) { (void)report; }

int main(void)
{
    run_probes();
    plat_finish(probe_report());
    return 0;
}
