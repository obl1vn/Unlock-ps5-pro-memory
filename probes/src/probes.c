// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 Pro probes for local LLM work, run through the Vulkan driver (RADV).
//
//   Probe 1  device and memory: name, heaps, budget, limits, extensions, and
//            the real allocation ceiling (allocate until the driver says no).
//   Probe 2  compute: FP32 throughput, read bandwidth, and a workgroup
//            "staircase" that shows how many compute units take work.
//   Probe 3  INT8 dot product: correctness of the instruction against a CPU
//            reference, and its speed against plain integer math.
//
// Every line goes to the platform log; the full report is handed to
// plat_finish(), which on the console serves it over HTTP.
#define VK_NO_PROTOTYPES
#include "platform.h"
#include "shaders.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024.0 * 1024.0)
#define GIB (1024.0 * 1024.0 * 1024.0)

/* ------------------------------------------------------------------ report */

static char g_report[96 * 1024];
static size_t g_report_len;

static void emit(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    if (g_report_len + n + 1 < sizeof(g_report)) {
        memcpy(g_report + g_report_len, line, n + 1);
        g_report_len += n;
    }
    plat_log(line);
}

const char *probe_report(void) { return g_report; }

/* ------------------------------------------------------- vulkan functions */

#define VK_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceMemoryProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr)

#define VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) \
    X(vkUnmapMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) \
    X(vkCmdPipelineBarrier) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) \
    X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) X(vkQueueSubmit) \
    X(vkDeviceWaitIdle)

#define DECLARE(name) static PFN_##name name;
VK_INSTANCE_FUNCS(DECLARE)
VK_DEVICE_FUNCS(DECLARE)
static PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR;

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static float timestamp_period_ns;
static uint32_t timestamp_bits;
static int host;

/* ------------------------------------------------------------- state */

static struct {
    double heap_gib, budget_gib, ceiling_gib, max_alloc_gib, max_storage_range, total_heap_gib;
    double ceiling_device_gib, ceiling_other_gib;
    int device_heap;
    double fp32_tflops, bandwidth_gbs;
    int knee_workgroups, lds_bytes, subgroup;
    int dot_supported, dot_accel_u, dot_accel_s, dot_mismatch_manual, dot_mismatch_instr;
    double dot_manual_gops, dot_instr_gops;
    int coopmat_ext, fp16_supported;
    double fp16_tflops;
    char name[256];
} R = { .knee_workgroups = -1, .dot_mismatch_manual = -1, .dot_mismatch_instr = -1 };

static int has_ext(const VkExtensionProperties *ext, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; ++i)
        if (strcmp(ext[i].extensionName, name) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------- probe 1: device */

static int create_instance(void)
{
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!create) { emit("FAIL no vkCreateInstance\n"); return 0; }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "ps5-probes", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkResult r = create(&ci, NULL, &instance);
    emit("vkCreateInstance=%d\n", r);
    if (r != VK_SUCCESS) return 0;
#define LOAD_I(name) name = (PFN_##name)gipa(instance, #name); if (!name) { emit("FAIL missing %s\n", #name); return 0; }
    VK_INSTANCE_FUNCS(LOAD_I)
    vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
        gipa(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    return 1;
}

static void probe_device(void)
{
    {
        const char *dot4 = getenv("RADV_PS5_DOT4");
        emit("dot4 experiment (RADV_PS5_DOT4): %s\n", dot4 && dot4[0] == '1' ? "ON" : "off");
    }
    emit("\n== PROBE 1: device and memory ==\n");
    {
        char info[1024];
        plat_system_info(info, sizeof(info));
        emit("system (kernel) view:\n%s", info);
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    VkPhysicalDevice devs[8];
    if (count > 8) count = 8;
    vkEnumeratePhysicalDevices(instance, &count, devs);
    emit("physical devices: %u\n", count);
    phys = devs[0];

    VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceShaderIntegerDotProductProperties dotp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES, .pNext = &drv };
    VkPhysicalDeviceVulkan11Properties v11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, .pNext = &dotp };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &v11 };
    vkGetPhysicalDeviceProperties2(phys, &p2);
    const VkPhysicalDeviceProperties *p = &p2.properties;
    snprintf(R.name, sizeof(R.name), "%s", p->deviceName);
    emit("GPU: %s (vendor %04x device %04x type %d)\n", p->deviceName, p->vendorID, p->deviceID, p->deviceType);
    emit("Vulkan %u.%u.%u, driver %s / %s\n", VK_VERSION_MAJOR(p->apiVersion), VK_VERSION_MINOR(p->apiVersion),
         VK_VERSION_PATCH(p->apiVersion), drv.driverName, drv.driverInfo);
    emit("subgroup size %u, timestampPeriod %.3f ns\n", v11.subgroupSize, p->limits.timestampPeriod);
    emit("maxComputeSharedMemorySize %u, maxComputeWorkGroupInvocations %u\n",
         p->limits.maxComputeSharedMemorySize, p->limits.maxComputeWorkGroupInvocations);
    emit("maxStorageBufferRange %.2f GiB, maxMemoryAllocationSize %.2f GiB\n",
         p->limits.maxStorageBufferRange / GIB, v11.maxMemoryAllocationSize / GIB);
    emit("int8 dot accelerated (driver report): unsigned=%u signed=%u\n",
         dotp.integerDotProduct4x8BitPackedUnsignedAccelerated, dotp.integerDotProduct4x8BitPackedSignedAccelerated);
    timestamp_period_ns = p->limits.timestampPeriod;
    R.subgroup = (int)v11.subgroupSize;
    R.lds_bytes = (int)p->limits.maxComputeSharedMemorySize;
    R.max_alloc_gib = v11.maxMemoryAllocationSize / GIB;
    R.max_storage_range = (double)p->limits.maxStorageBufferRange;
    R.dot_accel_u = (int)dotp.integerDotProduct4x8BitPackedUnsignedAccelerated;
    R.dot_accel_s = (int)dotp.integerDotProduct4x8BitPackedSignedAccelerated;

    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13 };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    emit("features: shaderFloat16=%u shaderInt8=%u storageBuffer8Bit=%u shaderInt16=%u shaderInt64=%u integerDotProduct=%u\n",
         f12.shaderFloat16, f12.shaderInt8, f12.storageBuffer8BitAccess, f2.features.shaderInt16,
         f2.features.shaderInt64, f13.shaderIntegerDotProduct);
    R.dot_supported = (int)f13.shaderIntegerDotProduct;
    R.fp16_supported = (int)f12.shaderFloat16;

    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &next, NULL);
    VkExtensionProperties *ext = calloc(next ? next : 1, sizeof(*ext));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &next, ext);
    const char *watch[] = { "VK_KHR_cooperative_matrix", "VK_NV_cooperative_matrix2", "VK_EXT_memory_budget",
        "VK_KHR_shader_integer_dot_product", "VK_KHR_shader_float16_int8", "VK_KHR_16bit_storage",
        "VK_KHR_8bit_storage", "VK_EXT_subgroup_size_control", "VK_KHR_shader_bfloat16", "VK_AMD_shader_core_properties2" };
    emit("device extensions: %u\n", next);
    for (size_t i = 0; i < sizeof(watch) / sizeof(watch[0]); ++i)
        emit("  %-36s %s\n", watch[i], has_ext(ext, next, watch[i]) ? "yes" : "no");
    R.coopmat_ext = has_ext(ext, next, "VK_KHR_cooperative_matrix");
    int budget_ext = has_ext(ext, next, "VK_EXT_memory_budget");

    if (R.coopmat_ext && vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR) {
        uint32_t n = 0;
        vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(phys, &n, NULL);
        VkCooperativeMatrixPropertiesKHR *cm = calloc(n ? n : 1, sizeof(*cm));
        for (uint32_t i = 0; i < n; ++i) cm[i].sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
        vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(phys, &n, cm);
        emit("cooperative matrix shapes: %u\n", n);
        for (uint32_t i = 0; i < n; ++i)
            emit("  M%u N%u K%u A=%d B=%d C=%d R=%d\n", cm[i].MSize, cm[i].NSize, cm[i].KSize,
                 cm[i].AType, cm[i].BType, cm[i].CType, cm[i].ResultType);
        free(cm);
    }
    free(ext);

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
    VkPhysicalDeviceMemoryProperties2 m2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
        .pNext = budget_ext ? &budget : NULL };
    vkGetPhysicalDeviceMemoryProperties2(phys, &m2);
    const VkPhysicalDeviceMemoryProperties *m = &m2.memoryProperties;
    for (uint32_t h = 0; h < m->memoryHeapCount; ++h) {
        emit("heap %u: %.2f GiB flags=%x", h, m->memoryHeaps[h].size / GIB, m->memoryHeaps[h].flags);
        if (budget_ext)
            emit(" budget=%.2f GiB used=%.2f GiB", budget.heapBudget[h] / GIB, budget.heapUsage[h] / GIB);
        emit("\n");
        R.total_heap_gib += m->memoryHeaps[h].size / GIB;
        if (m->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            double gib = m->memoryHeaps[h].size / GIB;
            if (gib > R.heap_gib) {
                R.heap_gib = gib;
                R.device_heap = (int)h;
                R.budget_gib = budget_ext ? budget.heapBudget[h] / GIB : 0;
            }
        }
    }
    for (uint32_t t = 0; t < m->memoryTypeCount; ++t)
        emit("type %u: heap %u flags=%x\n", t, m->memoryTypes[t].heapIndex, m->memoryTypes[t].propertyFlags);
}

static int find_memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer)
{
    VkPhysicalDeviceMemoryProperties m;
    vkGetPhysicalDeviceMemoryProperties(phys, &m);
    int fallback = -1;
    for (uint32_t t = 0; t < m.memoryTypeCount; ++t) {
        if (!(bits & (1u << t))) continue;
        VkMemoryPropertyFlags f = m.memoryTypes[t].propertyFlags;
        if ((f & want) != want) continue;
        if ((f & prefer) == prefer) return (int)t;
        if (fallback < 0) fallback = (int)t;
    }
    return fallback;
}

static int create_device(void)
{
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, NULL);
    VkQueueFamilyProperties qf[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, qf);
    queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        emit("queue family %u: flags=%x count=%u timestampBits=%u\n", i, qf[i].queueFlags, qf[i].queueCount,
             qf[i].timestampValidBits);
        if (queue_family == UINT32_MAX && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            queue_family = i;
            timestamp_bits = qf[i].timestampValidBits;
        }
    }
    if (queue_family == UINT32_MAX) { emit("FAIL no compute queue\n"); return 0; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .shaderFloat16 = R.fp16_supported ? VK_TRUE : VK_FALSE };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &f12, .shaderIntegerDotProduct = R.dot_supported ? VK_TRUE : VK_FALSE };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkResult r = vkCreateDevice(phys, &dci, NULL, &device);
    emit("vkCreateDevice=%d (compute queue family %u)\n", r, queue_family);
    if (r != VK_SUCCESS) return 0;
#define LOAD_D(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name); if (!name) { emit("FAIL missing %s\n", #name); return 0; }
    VK_DEVICE_FUNCS(LOAD_D)
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    return 1;
}

// Allocate memory until the driver refuses, then free everything. First from
// the device-local heap, then from the other heap (on the PS5 both heaps are
// the same physical pool, split for reporting), each in 256 MiB chunks and
// then 64 MiB chunks to refine the edge. The sum is what a model plus its KV
// cache can really use. v1-v3 stopped at a cap based on one heap; v4 caps at
// all heaps together plus 10%, so the driver's own refusal ends the test.
enum { MAX_CHUNKS = 1024 };
static VkDeviceMemory g_chunks[MAX_CHUNKS];
static int g_chunk_count;

static double fill_type(int type, double cap_gib, double already_gib, VkResult *last)
{
    double got_gib = 0;
    VkDeviceSize sizes[2] = { 256ull << 20, 64ull << 20 };
    for (int pass = 0; pass < 2; ++pass) {
        VkDeviceSize chunk = sizes[pass];
        if (R.max_alloc_gib > 0 && (double)chunk / GIB > R.max_alloc_gib) chunk = (VkDeviceSize)(R.max_alloc_gib * GIB);
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = chunk,
            .memoryTypeIndex = (uint32_t)type };
        while (g_chunk_count < MAX_CHUNKS && already_gib + got_gib + (double)chunk / GIB <= cap_gib) {
            *last = vkAllocateMemory(device, &ai, NULL, &g_chunks[g_chunk_count]);
            if (*last != VK_SUCCESS) break;
            ++g_chunk_count;
            got_gib += (double)chunk / GIB;
        }
    }
    return got_gib;
}

static void probe_allocation_ceiling(void)
{
    double cap_gib = R.total_heap_gib * 1.10 + 0.5;
    if (host) {
        const char *env = getenv("PROBE_ALLOC_CAP_GIB");
        cap_gib = env ? atof(env) : 2.0;
    }
    VkPhysicalDeviceMemoryProperties m;
    vkGetPhysicalDeviceMemoryProperties(phys, &m);
    int dev_type = -1, other_type = -1;
    for (uint32_t t = 0; t < m.memoryTypeCount; ++t) {
        VkMemoryPropertyFlags f = m.memoryTypes[t].propertyFlags;
        if (dev_type < 0 && (int)m.memoryTypes[t].heapIndex == R.device_heap && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            dev_type = (int)t;
        if (other_type < 0 && (int)m.memoryTypes[t].heapIndex != R.device_heap)
            other_type = (int)t;
    }
    if (dev_type < 0) { emit("allocation ceiling: no device-local type\n"); return; }
    emit("cap for this test: %.2f GiB (all heaps %.2f GiB + 10%% + 0.5)\n", cap_gib, R.total_heap_gib);
    double t0 = plat_now_seconds();
    VkResult last_dev = VK_SUCCESS, last_other = VK_SUCCESS;
    g_chunk_count = 0;
    R.ceiling_device_gib = fill_type(dev_type, cap_gib, 0, &last_dev);
    emit("device-local heap %d (type %d): %.2f GiB, stopped: %s (result %d)\n", R.device_heap, dev_type,
         R.ceiling_device_gib, last_dev == VK_SUCCESS ? "test cap" : "driver refused", last_dev);
    if (other_type >= 0) {
        R.ceiling_other_gib = fill_type(other_type, cap_gib, R.ceiling_device_gib, &last_other);
        emit("other heap %u (type %d): +%.2f GiB more, stopped: %s (result %d)\n",
             m.memoryTypes[other_type].heapIndex, other_type, R.ceiling_other_gib,
             last_other == VK_SUCCESS ? "test cap" : "driver refused", last_other);
    }
    R.ceiling_gib = R.ceiling_device_gib + R.ceiling_other_gib;
    char info[1024];
    plat_system_info(info, sizeof(info));
    emit("while holding %.2f GiB:\n%s", R.ceiling_gib, info);
    for (int i = 0; i < g_chunk_count; ++i) vkFreeMemory(device, g_chunks[i], NULL);
    emit("allocation ceiling: %.2f GiB total in %d chunks, %.2f s; all freed\n", R.ceiling_gib, g_chunk_count,
         plat_now_seconds() - t0);
    g_chunk_count = 0;
}

/* --------------------------------------------------- compute scaffolding */

typedef struct { VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size; } Buffer;

static int make_buffer(Buffer *b, VkDeviceSize size)
{
    memset(b, 0, sizeof(*b));
    b->size = size;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(device, &bi, NULL, &b->buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b->buf, &req);
    int type = find_memory_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type };
    if (vkAllocateMemory(device, &ai, NULL, &b->mem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(device, b->buf, b->mem, 0) != VK_SUCCESS) return 0;
    if (vkMapMemory(device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) return 0;
    return 1;
}

static void free_buffer(Buffer *b)
{
    if (b->map) vkUnmapMemory(device, b->mem);
    if (b->buf) vkDestroyBuffer(device, b->buf, NULL);
    if (b->mem) vkFreeMemory(device, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

static VkDescriptorSetLayout dsl;
static VkPipelineLayout layout;
static VkDescriptorPool dpool;
static VkDescriptorSet dset;
static VkCommandPool cpool;
static VkCommandBuffer cmd;
static VkQueryPool qpool;
static VkFence fence;

static int setup_compute(void)
{
    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = b };
    if (vkCreateDescriptorSetLayout(device, &dli, NULL, &dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (vkCreatePipelineLayout(device, &pli, NULL, &layout) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (vkCreateDescriptorPool(device, &dpi, NULL, &dpool) != VK_SUCCESS) return 0;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    if (vkAllocateDescriptorSets(device, &dai, &dset) != VK_SUCCESS) return 0;
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = queue_family };
    if (vkCreateCommandPool(device, &cpi, NULL, &cpool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if (vkAllocateCommandBuffers(device, &cai, &cmd) != VK_SUCCESS) return 0;
    VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
    if (vkCreateQueryPool(device, &qpi, NULL, &qpool) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    return vkCreateFence(device, &fci, NULL, &fence) == VK_SUCCESS;
}

static void bind_buffers(Buffer *in, Buffer *out)
{
    VkDescriptorBufferInfo bi[2] = { { in->buf, 0, VK_WHOLE_SIZE }, { out->buf, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; ++i)
        w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset,
            .dstBinding = (uint32_t)i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &bi[i] };
    vkUpdateDescriptorSets(device, 2, w, 0, NULL);
}

static VkPipeline make_pipeline(const uint32_t *code, uint32_t size, int32_t spec)
{
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule mod;
    if (vkCreateShaderModule(device, &smi, NULL, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkSpecializationMapEntry me = { 0, 0, sizeof(int32_t) };
    VkSpecializationInfo si = { 1, &me, sizeof(spec), &spec };
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = mod, .pName = "main", .pSpecializationInfo = &si },
        .layout = layout };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    vkDestroyShaderModule(device, mod, NULL);
    if (r != VK_SUCCESS) emit("pipeline creation failed: %d\n", r);
    return r == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

// One timed dispatch. Returns GPU seconds (timestamps) or wall seconds, < 0 on failure.
static double run(VkPipeline pipe, uint32_t groups, const uint32_t push[4])
{
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cmd, &bi);
    int ts = timestamp_bits > 0;
    if (ts) {
        vkCmdResetQueryPool(cmd, qpool, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push);
    vkCmdDispatch(cmd, groups, 1, 1);
    if (ts) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cmd);
    vkResetFences(device, 1, &fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    double t0 = plat_now_seconds();
    if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) return -1;
    if (vkWaitForFences(device, 1, &fence, VK_TRUE, 60ull * 1000000000ull) != VK_SUCCESS) return -2;
    double wall = plat_now_seconds() - t0;
    if (!ts) return wall;
    uint64_t q[2] = { 0, 0 };
    if (vkGetQueryPoolResults(device, qpool, 0, 2, sizeof(q), q, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
        return wall;
    uint64_t mask = timestamp_bits >= 64 ? ~0ull : ((1ull << timestamp_bits) - 1);
    uint64_t d = (q[1] - q[0]) & mask;
    return d * (double)timestamp_period_ns * 1e-9;
}

// Best of `reps` runs after one warm-up.
static double best_of(VkPipeline pipe, uint32_t groups, const uint32_t push[4], int reps)
{
    double best = 1e30;
    if (run(pipe, groups, push) < 0) return -1;
    for (int i = 0; i < reps; ++i) {
        double t = run(pipe, groups, push);
        if (t < 0) return t;
        if (t < best) best = t;
    }
    return best;
}

/* ------------------------------------------------------ probe 2: compute */

static void probe_compute(Buffer *in, Buffer *out)
{
    emit("\n== PROBE 2: compute units and bandwidth ==\n");
    bind_buffers(in, out);

    // 2a FP32 throughput.
    VkPipeline alu = make_pipeline(alu_spv, alu_spv_size, 0);
    if (alu) {
        uint32_t groups = host ? 256 : 16384, iters = host ? 256 : 2048;
        uint32_t push[4] = { iters, 0, 0, 0 };
        double t = best_of(alu, groups, push, 3);
        double flops = 2.0 * 8.0 * iters * (double)groups * 256.0;
        R.fp32_tflops = t > 0 ? flops / t / 1e12 : 0;
        emit("FP32: %.2f TFLOPS (%.3f ms for %.1f GFLOP)\n", R.fp32_tflops, t * 1e3, flops / 1e9);
        // RDNA: 64 FMA lanes per CU per clock = 128 FLOP/clock/CU.
        emit("  implies %.1f active CUs at 2.23 GHz, %.1f at 2.35 GHz (base PS5: 36)\n",
             R.fp32_tflops * 1e12 / (128.0 * 2.23e9), R.fp32_tflops * 1e12 / (128.0 * 2.35e9));
        vkDestroyPipeline(device, alu, NULL);
    }

    // 2d packed FP16 throughput.
    VkPipeline alu16 = R.fp16_supported ? make_pipeline(alu16_spv, alu16_spv_size, 0) : VK_NULL_HANDLE;
    if (alu16) {
        uint32_t groups = host ? 256 : 16384, iters = host ? 256 : 2048;
        uint32_t push[4] = { iters, 0, 0, 0 };
        double t = best_of(alu16, groups, push, 3);
        double flops = 2.0 * 2.0 * 8.0 * iters * (double)groups * 256.0;
        R.fp16_tflops = t > 0 ? flops / t / 1e12 : 0;
        emit("FP16 (packed): %.2f TFLOPS (%.3f ms), %.2fx FP32\n", R.fp16_tflops, t * 1e3,
             R.fp32_tflops > 0 ? R.fp16_tflops / R.fp32_tflops : 0.0);
        vkDestroyPipeline(device, alu16, NULL);
    }

    // 2b read bandwidth over the whole input buffer.
    VkPipeline bw = make_pipeline(bandwidth_spv, bandwidth_spv_size, 0);
    if (bw) {
        uint32_t elems = (uint32_t)(in->size / 16);
        uint32_t push[4] = { elems, 0, 0, 0 };
        double t = best_of(bw, host ? 1024 : 8192, push, 5);
        R.bandwidth_gbs = t > 0 ? in->size / t / 1e9 : 0;
        emit("read bandwidth: %.1f GB/s (%.0f MiB in %.3f ms; base PS5 peak 448, Pro ~576)\n",
             R.bandwidth_gbs, in->size / MIB, t * 1e3);
        vkDestroyPipeline(device, bw, NULL);
    }

    // 2c workgroup staircase.
    int lds_bytes = R.lds_bytes < 65536 ? R.lds_bytes : 65536;
    VkPipeline sc = make_pipeline(staircase_spv, staircase_spv_size, lds_bytes / 4);
    if (sc) {
        uint32_t iters = host ? 2048 : (1u << 17);
        emit("staircase: %d KiB LDS per workgroup, %u iterations; workgroups -> ms\n", lds_bytes / 1024, iters);
        double base = 0;
        char row[256];
        size_t len = 0;
        row[0] = 0;
        int max_w = host ? 32 : 192, step = 4;
        for (int w = step; w <= max_w; w += step) {
            uint32_t push[4] = { iters, (uint32_t)w * 2654435761u, 0, 0 };
            double t = best_of(sc, (uint32_t)w, push, 2);
            if (t <= 0) break;
            if (w <= 3 * step) base = base > 0 ? (base < t ? base : t) : t;
            if (R.knee_workgroups < 0 && w > 3 * step && t > base * 1.4) R.knee_workgroups = w - step;
            len += (size_t)snprintf(row + len, sizeof(row) - len, " %d:%.2f", w, t * 1e3);
            if (len > 180) { emit(" %s\n", row); len = 0; row[0] = 0; }
        }
        if (len) emit(" %s\n", row);
        if (R.knee_workgroups > 0)
            emit("first step after %d workgroups run at once (base PS5 expectation: ~36; full Pro: ~60)\n",
                 R.knee_workgroups);
        else
            emit("no clear step found in the sweep\n");
        vkDestroyPipeline(device, sc, NULL);
    }
}

/* -------------------------------------------------------- probe 3: int8 */

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }

static void probe_dot(Buffer *in, Buffer *out)
{
    emit("\n== PROBE 3: INT8 dot product ==\n");
    if (!R.dot_supported) { emit("shaderIntegerDotProduct not supported; skipped\n"); return; }
    bind_buffers(in, out);

    VkPipeline check = make_pipeline(dot_check_spv, dot_check_spv_size, 0);
    if (check) {
        uint32_t n = 1u << 18, seed = 12345;
        uint32_t *v = in->map;
        // Edge cases first: all-0x80 (most negative signed), all-0xFF, mixed.
        uint32_t edges[] = { 0x80808080u, 0x80808080u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x7F7F7F7Fu, 0x80808080u,
                             0x01FF7F80u, 0xFF0180FFu };
        for (uint32_t i = 0; i < 2 * n; ++i) v[i] = i < 8 ? edges[i] : lcg(&seed);
        memset(out->map, 0, (size_t)n * 16);
        uint32_t push[4] = { n, 0, 0, 0 };
        double t = run(check, (n + 255) / 256, push);
        uint32_t *o = out->map;
        int bad_m = 0, bad_i = 0;
        for (uint32_t i = 0; i < n && t >= 0; ++i) {
            uint32_t a = v[2 * i], b = v[2 * i + 1], ru = 0;
            int32_t rs = 0;
            for (int k = 0; k < 4; ++k) {
                ru += ((a >> (8 * k)) & 0xFF) * ((b >> (8 * k)) & 0xFF);
                rs += (int32_t)(int8_t)(a >> (8 * k)) * (int32_t)(int8_t)(b >> (8 * k));
            }
            if (o[4 * i] != ru || o[4 * i + 2] != (uint32_t)rs) ++bad_m;
            if (o[4 * i + 1] != ru || o[4 * i + 3] != (uint32_t)rs) {
                if (bad_i < 3) emit("  mismatch a=%08x b=%08x: udot %u (want %u) sdot %d (want %d)\n", a, b,
                                    o[4 * i + 1], ru, (int32_t)o[4 * i + 3], rs);
                ++bad_i;
            }
        }
        R.dot_mismatch_manual = bad_m;
        R.dot_mismatch_instr = bad_i;
        emit("correctness over %u pairs: manual math %d wrong, dot instruction %d wrong\n", n, bad_m, bad_i);
        vkDestroyPipeline(device, check, NULL);
    }

    double gops[2] = { 0, 0 };
    for (int mode = 0; mode < 2; ++mode) {
        VkPipeline sp = make_pipeline(dot_speed_spv, dot_speed_spv_size, mode);
        if (!sp) continue;
        uint32_t groups = host ? 128 : 8192, iters = host ? 128 : 1024;
        uint32_t push[4] = { iters, 0x03050709u, 0, 0 };
        double t = best_of(sp, groups, push, 3);
        double dots = 8.0 * iters * (double)groups * 256.0;
        gops[mode] = t > 0 ? dots * 8.0 / t / 1e9 : 0;
        emit("%s: %.0f INT8 Gops/s (%.3f ms)\n", mode ? "dot instruction" : "manual math", gops[mode], t * 1e3);
        vkDestroyPipeline(device, sp, NULL);
    }
    R.dot_manual_gops = gops[0];
    R.dot_instr_gops = gops[1];
    if (gops[0] > 0)
        emit("instruction vs manual: %.2fx (near 1x = emulated; 3x or more = hardware path live)\n", gops[1] / gops[0]);
}

/* ----------------------------------------------------------------- main */

int run_probes(void)
{
    host = plat_is_host();
    gipa = plat_get_instance_proc_addr();
    emit("ps5-probes v6 (%s run)\n", host ? "host test" : "console");
    if (!gipa || !create_instance()) goto done;
    probe_device();
    if (!create_device()) goto done;
    if (!setup_compute()) { emit("FAIL compute setup\n"); goto done; }

    Buffer in, out;
    VkDeviceSize in_size = host ? (256ull << 20) : (1024ull << 20);
    while (in_size > (VkDeviceSize)R.max_storage_range && in_size > (16ull << 20)) in_size /= 2;
    if (!make_buffer(&in, in_size) || !make_buffer(&out, 8ull << 20)) {
        emit("FAIL could not create %.0f MiB test buffers\n", in_size / MIB);
        goto done;
    }
    // Fill with a pattern so reads are real, not zero pages.
    uint32_t seed = 7, *words = in.map;
    for (VkDeviceSize i = 0; i < in_size / 4; ++i) words[i] = lcg(&seed);

    probe_compute(&in, &out);
    probe_dot(&in, &out);

    vkDeviceWaitIdle(device);
    free_buffer(&in);
    free_buffer(&out);

    // Last, because it pushes memory to the limit: if the driver misbehaves
    // here, every other result is already in the report and in klog.
    emit("\n== PROBE 1b: allocation ceiling ==\n");
    emit("(results above saved to disk before this test)\n");
    plat_checkpoint(g_report);
    if (plat_skip_memory_test())
        emit("skipped (skip-memory-test.txt present)\n");
    else
        probe_allocation_ceiling();

done:
    emit("\n== SUMMARY ==\n");
    emit("gpu=\"%s\" heaps_total_gib=%.2f device_heap_gib=%.2f budget_gib=%.2f\n", R.name, R.total_heap_gib,
         R.heap_gib, R.budget_gib);
    emit("alloc_ceiling_gib=%.2f (device %.2f + other %.2f)\n", R.ceiling_gib, R.ceiling_device_gib,
         R.ceiling_other_gib);
    emit("fp16_tflops=%.2f\n", R.fp16_tflops);
    emit("fp32_tflops=%.2f read_gbs=%.1f concurrent_workgroups=%d lds_kib=%d subgroup=%d\n", R.fp32_tflops,
         R.bandwidth_gbs, R.knee_workgroups, R.lds_bytes / 1024, R.subgroup);
    emit("dot_reported_accel=%d/%d dot_wrong_instr=%d dot_wrong_manual=%d dot_speedup=%.2f coopmat_ext=%d\n",
         R.dot_accel_u, R.dot_accel_s, R.dot_mismatch_instr, R.dot_mismatch_manual,
         R.dot_manual_gops > 0 ? R.dot_instr_gops / R.dot_manual_gops : 0.0, R.coopmat_ext);
    emit("PROBES COMPLETE. Send this whole report back for analysis.\n");
    if (device) {
        if (fence) vkDestroyFence(device, fence, NULL);
        if (qpool) vkDestroyQueryPool(device, qpool, NULL);
        if (cpool) vkDestroyCommandPool(device, cpool, NULL);
        if (dpool) vkDestroyDescriptorPool(device, dpool, NULL);
        if (layout) vkDestroyPipelineLayout(device, layout, NULL);
        if (dsl) vkDestroyDescriptorSetLayout(device, dsl, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (instance && vkDestroyInstance) vkDestroyInstance(instance, NULL);
    return 0;
}
