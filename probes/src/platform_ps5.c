// SPDX-License-Identifier: GPL-3.0-or-later
// Console build: RADV linked in, output to klog, and the report served live
// over HTTP so a browser on the network can watch it fill in. The screen stays
// dark while the probes run; that is expected.
//
// Lessons taken from ProsperoAI and PS5_Vulkan:
// - GPU work runs on a thread with an 8 MiB stack: ProsperoAI's warmup crashed
//   in ggml_vk_get_device on a default-sized stack.
// - The web page starts before any GPU work, so a crash later still leaves the
//   earlier results visible (and every line is also in klog).
// - access() is refused in a title's file system (PS5_Vulkan 2026-09-28), so the
//   skip switch is checked with fopen.
#include "platform.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern PFN_vkVoidFunction radv_GetInstanceProcAddr(VkInstance, const char *);
extern int sceKernelDebugOutText(int, const char *);
extern int sceKernelUsleep(unsigned int);
extern int64_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t alignment, int64_t *start_out,
                                              size_t *size_out);

extern int scePthreadCreate(void **thread, const void *attributes, void *(*entry)(void *), void *argument,
                            const char *name);
extern int scePthreadJoin(void *thread, void **result);
extern int sceNetInit(void);
extern int sceNetPoolCreate(const char *name, int size, int flags);
extern int sceNetSocket(const char *name, int domain, int type, int protocol);
extern int sceNetSocketClose(int sock);
extern int sceNetBind(int sock, const void *address, unsigned int length);
extern int sceNetListen(int sock, int backlog);
extern int sceNetAccept(int sock, void *address, unsigned int *length);
extern int sceNetSend(int sock, const void *buffer, unsigned long length, int flags);
extern int sceNetRecv(int sock, void *buffer, unsigned long length, int flags);
extern int sceNetSetsockopt(int sock, int level, int option, const void *value, unsigned int length);

#define PROBE_HTTP_PORT 9050

// FreeBSD sockaddr_in layout, as ProsperoAI's HTTP server uses it.
struct probe_sockaddr_in {
    uint8_t sin_len;
    uint8_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
};

static volatile int g_finished;

PFN_vkGetInstanceProcAddr plat_get_instance_proc_addr(void)
{
    return (PFN_vkGetInstanceProcAddr)radv_GetInstanceProcAddr;
}

double plat_now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void plat_log(const char *line)
{
    char tagged[1100];
    snprintf(tagged, sizeof(tagged), "[ps5-probes] %s", line);
    sceKernelDebugOutText(0, tagged);
}

int plat_is_host(void) { return 0; }

// The kernel's own numbers: the title's direct-memory pool (what RADV splits
// into its heaps) and how much of it is free. Only the two calls RADV itself
// makes are used: v4 also asked for flexible memory and the Pro/Neo mode flags,
// and that build crashed at launch (CE-108255-1), most likely because one of
// those functions is not available to a title.
void plat_system_info(char *out, unsigned long size)
{
    const int64_t pool = sceKernelGetDirectMemorySize();
    int64_t start = -1;
    size_t free_direct = 0;
    const int free_rc = pool > 0 ? sceKernelAvailableDirectMemorySize(0, pool, 16384, &start, &free_direct) : -1;
    int n = snprintf(out, size, "  direct memory pool %.2f GiB, largest free block %.2f GiB (rc %d)\n",
                     pool / (1024.0 * 1024.0 * 1024.0), free_direct / (1024.0 * 1024.0 * 1024.0), free_rc);
    // Which param.json variant is this? Quote the settings the variants change.
    static char param[16384];
    FILE *f = fopen("/app0/sce_sys/param.json", "r");
    if (!f || n < 0 || (unsigned long)n >= size) { if (f) fclose(f); return; }
    size_t len = fread(param, 1, sizeof(param) - 1, f);
    fclose(f);
    param[len] = 0;
    const char *keys[] = { "\"titleId\"", "\"attribute3\"", "\"sdkVersion\"", "\"mfsrVersion\"" };
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]) && (unsigned long)n < size; ++k) {
        const char *at = strstr(param, keys[k]);
        if (!at) continue;
        const char *end = at;
        while (*end && *end != ',' && *end != '\n' && *end != '}') ++end;
        n += snprintf(out + n, size - n, "  param %.*s\n", (int)(end - at), at);
    }
}

int plat_skip_memory_test(void)
{
    FILE *f = fopen("/app0/skip-memory-test.txt", "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void save_report(const char *report)
{
    const char *paths[] = { "/data/ps5-probes-report.txt", "/download0/ps5-probes-report.txt" };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *f = fopen(paths[i], "w");
        char line[160];
        if (!f) {
            snprintf(line, sizeof(line), "could not write %s\n", paths[i]);
            plat_log(line);
            continue;
        }
        fputs(report, f);
        fclose(f);
        snprintf(line, sizeof(line), "report saved to %s\n", paths[i]);
        plat_log(line);
    }
}

void plat_checkpoint(const char *report) { save_report(report); }

static void send_all(int sock, const char *data, size_t length)
{
    while (length > 0) {
        int sent = sceNetSend(sock, data, length, 0);
        if (sent <= 0) return;
        data += sent;
        length -= (size_t)sent;
    }
}

// Serves the report as it stands on every request, forever.
static void *http_thread(void *argument)
{
    (void)argument;
    char line[160];
    int r = sceNetInit();
    if (r < 0) { snprintf(line, sizeof(line), "sceNetInit=%x; no web page, use klog\n", r); plat_log(line); return NULL; }
    if (sceNetPoolCreate("ps5-probes", 1 << 20, 0) < 0) { plat_log("sceNetPoolCreate failed; no web page\n"); return NULL; }
    int listen_sock = sceNetSocket("ps5-probes", 2, 1, 0);
    if (listen_sock < 0) { plat_log("sceNetSocket failed; no web page\n"); return NULL; }
    int reuse = 1;
    sceNetSetsockopt(listen_sock, 0xffff, 0x0004, &reuse, sizeof(reuse));
    struct probe_sockaddr_in address = { .sin_len = sizeof(address), .sin_family = 2,
        .sin_port = (uint16_t)((PROBE_HTTP_PORT >> 8) | ((PROBE_HTTP_PORT & 0xff) << 8)), .sin_addr = 0 };
    if (sceNetBind(listen_sock, &address, sizeof(address)) < 0 || sceNetListen(listen_sock, 4) < 0) {
        plat_log("bind/listen failed; no web page\n");
        return NULL;
    }
    snprintf(line, sizeof(line), "web page at http://<PS5-IP>:%d/\n", PROBE_HTTP_PORT);
    plat_log(line);
    for (;;) {
        struct probe_sockaddr_in client;
        unsigned int client_length = sizeof(client);
        int sock = sceNetAccept(listen_sock, &client, &client_length);
        if (sock < 0) { sceKernelUsleep(100000); continue; }
        char request[2048];
        sceNetRecv(sock, request, sizeof(request), 0);
        const char *status = g_finished ? "" : "STILL RUNNING - refresh in a few seconds.\n\n";
        const char *report = probe_report();
        size_t length = strlen(report);
        char header[256];
        snprintf(header, sizeof(header),
                 "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\n"
                 "Cache-Control: no-store\r\nConnection: close\r\n\r\n", strlen(status) + length);
        send_all(sock, header, strlen(header));
        send_all(sock, status, strlen(status));
        send_all(sock, report, length);
        sceNetSocketClose(sock);
    }
    return NULL;
}

static void *probe_thread(void *argument)
{
    (void)argument;
    run_probes();
    return NULL;
}

static int start_thread(void **thread, void *(*entry)(void *), const char *name, size_t stack)
{
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error) return error;
    error = pthread_attr_setstacksize(&attributes, stack);
    if (!error) error = scePthreadCreate(thread, &attributes, entry, NULL, name);
    pthread_attr_destroy(&attributes);
    return error;
}

void plat_finish(const char *report)
{
    save_report(report);
    g_finished = 1;
    plat_log("finished; the web page keeps serving the report. Close the app with the PS button.\n");
    for (;;) sceKernelUsleep(1000000);
}

int main(void)
{
    plat_log("main entered\n");
    // Experiment switch: a file next to eboot.bin turns on the patched driver's
    // accelerated INT8 dot product (driver/ps5-dot4-experiment.patch).
    FILE *dot4 = fopen("/app0/enable-dot4.txt", "r");
    if (dot4) {
        fclose(dot4);
        setenv("RADV_PS5_DOT4", "1", 1);
    }
    void *http = NULL, *worker = NULL;
    if (start_thread(&http, http_thread, "ps5-probes-http", 256 * 1024) != 0)
        plat_log("could not start the web page thread; results go to klog only\n");
    sceKernelUsleep(500000);
    if (start_thread(&worker, probe_thread, "ps5-probes-gpu", 8 * 1024 * 1024) == 0) {
        scePthreadJoin(worker, NULL);
    } else {
        plat_log("could not start the 8 MiB probe thread; running on the main thread\n");
        run_probes();
    }
    plat_finish(probe_report());
    return 0;
}
