#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <vulkan/vulkan.h>

#define LOG_TAG "MaliAdrenoShim"
#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do { fprintf(stderr, "[I/" LOG_TAG "] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)
#define LOGW(...) do { fprintf(stderr, "[W/" LOG_TAG "] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)
#define LOGE(...) do { fprintf(stderr, "[E/" LOG_TAG "] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)
#endif

// Qualcomm Adreno Constants
#define QUALCOMM_VENDOR_ID 0x5143
#define ADRENO_750_DEVICE_ID 0x07050000
#define SHIM_DEVICE_NAME "Qualcomm Adreno (TM) 750 (Mali-G615)"
#define SHIM_DRIVER_VERSION VK_MAKE_VERSION(24, 2, 0)

// 64 MB Staging Ring Buffer for on-the-fly BCn decompression
#define STAGING_BUFFER_DEFAULT_SIZE (64 * 1024 * 1024)

// Android Hardware Vulkan HAL ABI (hwvulkan.h)
struct hw_module_t;
struct hw_device_t;

typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id, struct hw_device_t **device);
} hw_module_methods_t;

typedef struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
#ifdef __LP64__
    uint64_t reserved[32 - 7];
#else
    uint32_t reserved[32 - 7];
#endif
} hw_module_t;

typedef struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
#ifdef __LP64__
    uint64_t reserved[12];
#else
    uint32_t reserved[12];
#endif
    int (*close)(struct hw_device_t *device);
} hw_device_t;

typedef struct hwvulkan_module_t {
    struct hw_module_t common;
} hwvulkan_module_t;

typedef struct hwvulkan_device_t {
    struct hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;

extern hwvulkan_module_t HMI;

// Real Mali Vulkan handle & function pointers
typedef VkResult (VKAPI_PTR *PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(uint32_t* pSupportedVersion);

static void *real_vulkan_handle = NULL;
static hwvulkan_device_t *real_hw_device = NULL;
static PFN_vkGetInstanceProcAddr real_vkGetInstanceProcAddr = NULL;
static PFN_vkGetInstanceProcAddr real_vk_icdGetPhysicalDeviceProcAddr = NULL;
static PFN_vk_icdNegotiateLoaderICDInterfaceVersion real_vk_icdNegotiateLoaderICDInterfaceVersion = NULL;
static PFN_vkGetDeviceProcAddr real_vkGetDeviceProcAddr = NULL;
static PFN_vkCreateInstance real_vkCreateInstance = NULL;
static PFN_vkEnumerateInstanceExtensionProperties real_vkEnumerateInstanceExtensionProperties = NULL;
static bool g_is_panvk_backend = false;
static bool g_use_cpu_bcn = true;

// Cached Real Device Functions
static PFN_vkCreateImage real_vkCreateImage = NULL;
static PFN_vkDestroyImage real_vkDestroyImage = NULL;
static PFN_vkCreateImageView real_vkCreateImageView = NULL;
static PFN_vkGetImageSubresourceLayout real_vkGetImageSubresourceLayout = NULL;
static PFN_vkCreateSampler real_vkCreateSampler = NULL;
static PFN_vkCmdCopyBufferToImage real_vkCmdCopyBufferToImage = NULL;
static PFN_vkAllocateMemory real_vkAllocateMemory = NULL;
static PFN_vkFreeMemory real_vkFreeMemory = NULL;
static PFN_vkMapMemory real_vkMapMemory = NULL;
static PFN_vkUnmapMemory real_vkUnmapMemory = NULL;
static PFN_vkBindBufferMemory real_vkBindBufferMemory = NULL;
static PFN_vkBindBufferMemory2 real_vkBindBufferMemory2 = NULL;
static PFN_vkDestroyBuffer real_vkDestroyBuffer = NULL;
static PFN_vkDeviceWaitIdle real_vkDeviceWaitIdle = NULL;
static PFN_vkFlushMappedMemoryRanges real_vkFlushMappedMemoryRanges = NULL;
static PFN_vkQueueSubmit real_vkQueueSubmit = NULL;

// MediaTek GED (/proc/ged) Hardware GPU Frequency Boost State
typedef struct GedBridgePackage {
    uint32_t i32FunctionID;
    int32_t  i32Size;
    void    *pvParamIn;
    int32_t  i32InBufferSize;
    void    *pvParamOut;
    int32_t  i32OutBufferSize;
} GedBridgePackage;

static int g_ged_fd = -1;
static uint32_t g_ged_max_opp = 64;
static uint32_t g_submit_counter = 0;
static pthread_t g_ged_thread;
static volatile bool g_ged_thread_running = false;

static void mtk_ged_boost_gpu(void) {
    if (g_ged_fd < 0) {
        if (g_ged_fd == -2) return;
        g_ged_fd = open("/proc/ged", O_RDONLY | O_CLOEXEC);
        if (g_ged_fd >= 0) {
            // Query max OPP level via GED_BRIDGE_COMMAND_QUERY_INFO (0xc0286705, cmd=7)
            uint32_t q_cmd = 7;
            uint64_t q_out = 0;
            GedBridgePackage q_pkg = {
                .i32FunctionID = 0xc0286705U,
                .i32Size = sizeof(GedBridgePackage),
                .pvParamIn = &q_cmd,
                .i32InBufferSize = 4,
                .pvParamOut = &q_out,
                .i32OutBufferSize = 8
            };
            if (ioctl(g_ged_fd, 0xc0286705U, &q_pkg) == 0 && q_out > 0 && q_out <= 128) {
                g_ged_max_opp = (uint32_t)q_out;
            }
            LOGI("[+] Connected to MediaTek /proc/ged hardware DVFS booster (max OPP: %u)", g_ged_max_opp);
        } else {
            g_ged_fd = -2; // Mark unavailable
            return;
        }
    }
    if (g_ged_fd >= 0) {
        uint32_t in_boost = g_ged_max_opp;
        uint32_t out_boost = 1;
        GedBridgePackage pkg = {
            .i32FunctionID = 0xc0286703U, // GED_BRIDGE_COMMAND_BOOST_GPU_FREQ
            .i32Size = sizeof(GedBridgePackage),
            .pvParamIn = &in_boost,
            .i32InBufferSize = 4,
            .pvParamOut = &out_boost,
            .i32OutBufferSize = 4
        };
        ioctl(g_ged_fd, 0xc0286703U, &pkg);
    }
}

static void* mtk_ged_booster_loop(void *arg) {
    (void)arg;
    while (g_ged_thread_running) {
        mtk_ged_boost_gpu();
        usleep(40000); // 40ms pulse keeps MediaTek GED DVFS pinned at 836+ MHz continuously
    }
    return NULL;
}

static void start_mtk_ged_booster(void) {
    if (g_ged_thread_running) return;
    mtk_ged_boost_gpu();
    if (g_ged_fd >= 0) {
        g_ged_thread_running = true;
        if (pthread_create(&g_ged_thread, NULL, mtk_ged_booster_loop, NULL) == 0) {
            pthread_detach(g_ged_thread);
            LOGI("[+] Started MediaTek /proc/ged 40ms continuous GPU Boost thread (836+ MHz)");
        } else {
            g_ged_thread_running = false;
        }
    }
}

#define MAX_LIVE_INSTANCES 16
static VkInstance g_live_instances[MAX_LIVE_INSTANCES] = {0};
static VkInstance g_instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_physical_device = VK_NULL_HANDLE;
static bool g_opened_as_hal = false;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static void add_live_instance(VkInstance inst) {
    if (!inst) return;
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < MAX_LIVE_INSTANCES; i++) {
        if (g_live_instances[i] == inst) {
            g_instance = inst;
            pthread_mutex_unlock(&g_mutex);
            return;
        }
    }
    for (int i = 0; i < MAX_LIVE_INSTANCES; i++) {
        if (g_live_instances[i] == VK_NULL_HANDLE) {
            g_live_instances[i] = inst;
            break;
        }
    }
    g_instance = inst;
    pthread_mutex_unlock(&g_mutex);
}

static void remove_live_instance(VkInstance inst) {
    if (!inst) return;
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < MAX_LIVE_INSTANCES; i++) {
        if (g_live_instances[i] == inst) {
            g_live_instances[i] = VK_NULL_HANDLE;
        }
    }
    if (g_instance == inst) {
        g_instance = VK_NULL_HANDLE;
        for (int i = 0; i < MAX_LIVE_INSTANCES; i++) {
            if (g_live_instances[i] != VK_NULL_HANDLE) {
                g_instance = g_live_instances[i];
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_mutex);
}

static bool is_live_instance(VkInstance inst) {
    if (!inst) return false;
    for (int i = 0; i < MAX_LIVE_INSTANCES; i++) {
        if (g_live_instances[i] == inst) return true;
    }
    return false;
}

static void init_real_vulkan(void) {
    if (real_vkGetInstanceProcAddr) return;

    const char *env_lib = getenv("MALI_REAL_VULKAN_LIB");

    const char *loader_paths[] = {
        env_lib ? env_lib : "/data/data/app.pocketfoundry/files/usr/lib/libvulkan_panfrost.so",
        "/data/data/app.pocketfoundry/files/usr/lib/libvulkan_panfrost.so",
        "libvulkan_panfrost.so",
        "/system/lib64/libvulkan.so",
        "/vendor/lib64/hw/mt6897/vulkan.mali.so",
        "/vendor/lib64/hw/vulkan.mali.so",
        "/vendor/lib64/egl/mt6897/libGLES_mali.so",
        "/vendor/lib64/egl/libGLES_mali.so",
        "libvulkan.so",
        NULL
    };

    const char *hal_paths[] = {
        "/vendor/lib64/hw/mt6897/vulkan.mali.so",
        "/vendor/lib64/hw/vulkan.mali.so",
        "/vendor/lib64/egl/mt6897/libGLES_mali.so",
        "/vendor/lib64/egl/libGLES_mali.so",
        "/system/lib64/libvulkan.so",
        NULL
    };

    const char **paths = g_opened_as_hal ? hal_paths : loader_paths;

    for (int i = 0; paths[i] != NULL; i++) {
        void *h = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;

        // Avoid opening ourselves if LD_PRELOAD or symlinked as libvulkan.so
        void *sym_hmi = dlsym(h, "HMI");
        if (sym_hmi == (void*)&HMI) {
            dlclose(h);
            continue;
        }

        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
        if (!gipa) {
            gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
        }

        if (gipa) {
            real_vulkan_handle = h;
            real_vkGetInstanceProcAddr = gipa;
            real_vk_icdGetPhysicalDeviceProcAddr =
                (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetPhysicalDeviceProcAddr");
            real_vk_icdNegotiateLoaderICDInterfaceVersion =
                (PFN_vk_icdNegotiateLoaderICDInterfaceVersion)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
            real_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)dlsym(h, "vkGetDeviceProcAddr");
            real_vkCreateInstance = (PFN_vkCreateInstance)dlsym(h, "vkCreateInstance");
            real_vkEnumerateInstanceExtensionProperties =
                (PFN_vkEnumerateInstanceExtensionProperties)dlsym(h, "vkEnumerateInstanceExtensionProperties");

            if (strstr(paths[i], "panfrost") != NULL) {
                g_is_panvk_backend = true;
                const char *cpu_bcn = getenv("MALI_CPU_BCN");
                g_use_cpu_bcn = (cpu_bcn && strcmp(cpu_bcn, "1") == 0);
                // Prevent save-load freeze on Mali-G615 CSF v11 with panvk-kbase v26.3.1
                setenv("DRIRC_CONFIGURATION", "pan_enable_vertex_pipeline_stores_atomics=false", 0);
                setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);
            } else {
                g_is_panvk_backend = false;
                g_use_cpu_bcn = true;
            }
            start_mtk_ged_booster();
            LOGI("[+] Loaded Vulkan loader/ICD from: %s (panvk=%d, cpu_bcn=%d)",
                 paths[i], (int)g_is_panvk_backend, (int)g_use_cpu_bcn);
            break;
        }

        if (sym_hmi) {
            hwvulkan_module_t *hmi = (hwvulkan_module_t*)sym_hmi;
            if (hmi->common.methods && hmi->common.methods->open) {
                struct hw_device_t *dev = NULL;
                if (hmi->common.methods->open(&hmi->common, "vk0", &dev) == 0 && dev) {
                    real_vulkan_handle = h;
                    real_hw_device = (hwvulkan_device_t*)dev;
                    real_vkGetInstanceProcAddr = real_hw_device->GetInstanceProcAddr;
                    real_vkCreateInstance = real_hw_device->CreateInstance;
                    real_vkEnumerateInstanceExtensionProperties = real_hw_device->EnumerateInstanceExtensionProperties;
                    if (real_vkGetInstanceProcAddr) {
                        real_vkGetDeviceProcAddr =
                            (PFN_vkGetDeviceProcAddr)real_vkGetInstanceProcAddr(NULL, "vkGetDeviceProcAddr");
                    }
                    g_is_panvk_backend = false;
                    g_use_cpu_bcn = true;
                    start_mtk_ged_booster();
                    LOGI("[+] Opened Android HAL HMI (vk0) from: %s", paths[i]);
                    break;
                }
            }
        }
        dlclose(h);
    }

    if (!real_vkGetInstanceProcAddr) {
        LOGE("[-] Failed to initialize underlying Vulkan implementation!");
    }
}

static void* get_driver_proc(const char *name) {
    init_real_vulkan();
    void *fn = NULL;
    if (real_vkGetInstanceProcAddr) {
        if (g_instance != VK_NULL_HANDLE) {
            fn = (void*)real_vkGetInstanceProcAddr(g_instance, name);
        }
        if (!fn) {
            for (int i = 0; i < MAX_LIVE_INSTANCES; i++) {
                if (g_live_instances[i] != VK_NULL_HANDLE && g_live_instances[i] != g_instance) {
                    fn = (void*)real_vkGetInstanceProcAddr(g_live_instances[i], name);
                    if (fn) {
                        g_instance = g_live_instances[i];
                        break;
                    }
                }
            }
        }
        if (!fn) {
            fn = (void*)real_vkGetInstanceProcAddr(NULL, name);
        }
    }
    if (!fn && real_vk_icdGetPhysicalDeviceProcAddr && g_instance != VK_NULL_HANDLE) {
        fn = (void*)real_vk_icdGetPhysicalDeviceProcAddr(g_instance, name);
    }
    if (!fn && real_vulkan_handle) {
        fn = dlsym(real_vulkan_handle, name);
    }
    return fn;
}

// -------------------------------------------------------------
// Format Helpers
// -------------------------------------------------------------
static inline bool is_bcn_format(VkFormat format) {
    return (format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK);
}

static inline bool is_srgb_format(VkFormat format) {
    switch (format) {
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return true;
        default:
            return false;
    }
}

static inline VkFormat get_decompressed_format(VkFormat format) {
    return is_srgb_format(format) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
}

// -------------------------------------------------------------
// BC1 - BC5 Software Decoder (produces direct RGBA8 in memory)
// -------------------------------------------------------------
static inline void decode_565_to_rgb(uint16_t c565, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = (uint8_t)(((c565 & 0xF800) >> 8) | ((c565 & 0xE000) >> 13));
    *g = (uint8_t)(((c565 & 0x07E0) >> 3) | ((c565 & 0x0600) >> 9));
    *b = (uint8_t)(((c565 & 0x001F) << 3) | ((c565 & 0x001C) >> 2));
}

static uint8_t s_normal_z_lut[256][256];
static bool s_normal_z_lut_inited = false;

static void init_normal_z_lut(void) {
    if (s_normal_z_lut_inited) return;
    for (int r = 0; r < 256; r++) {
        float nx = (float)r / 127.5f - 1.0f;
        for (int g = 0; g < 256; g++) {
            float ny = (float)g / 127.5f - 1.0f;
            float nz_sq = 1.0f - nx * nx - ny * ny;
            float nz = (nz_sq > 0.0f) ? sqrtf(nz_sq) : 0.0f;
            s_normal_z_lut[r][g] = (uint8_t)((nz * 0.5f + 0.5f) * 255.0f);
        }
    }
    s_normal_z_lut_inited = true;
}

static inline uint32_t get_bcn_block_size(VkFormat format) {
    switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
            return 8;
        default:
            return 16;
    }
}

static void decode_bc1_block(const uint8_t *src, uint8_t *dst_rgba, uint32_t stride, uint32_t max_x, uint32_t max_y, bool is_rgba, bool has_separate_alpha) {
    uint16_t c0 = *(const uint16_t*)(src + 0);
    uint16_t c1 = *(const uint16_t*)(src + 2);
    uint32_t bits = *(const uint32_t*)(src + 4);

    uint8_t pal_r[4], pal_g[4], pal_b[4], pal_a[4];
    decode_565_to_rgb(c0, &pal_r[0], &pal_g[0], &pal_b[0]); pal_a[0] = 255;
    decode_565_to_rgb(c1, &pal_r[1], &pal_g[1], &pal_b[1]); pal_a[1] = 255;

    if (c0 > c1 || has_separate_alpha) {
        pal_r[2] = (uint8_t)((2 * pal_r[0] + pal_r[1]) / 3);
        pal_g[2] = (uint8_t)((2 * pal_g[0] + pal_g[1]) / 3);
        pal_b[2] = (uint8_t)((2 * pal_b[0] + pal_b[1]) / 3);
        pal_a[2] = 255;

        pal_r[3] = (uint8_t)((pal_r[0] + 2 * pal_r[1]) / 3);
        pal_g[3] = (uint8_t)((pal_g[0] + 2 * pal_g[1]) / 3);
        pal_b[3] = (uint8_t)((pal_b[0] + 2 * pal_b[1]) / 3);
        pal_a[3] = 255;
    } else {
        pal_r[2] = (uint8_t)((pal_r[0] + pal_r[1]) / 2);
        pal_g[2] = (uint8_t)((pal_g[0] + pal_g[1]) / 2);
        pal_b[2] = (uint8_t)((pal_b[0] + pal_b[1]) / 2);
        pal_a[2] = 255;

        pal_r[3] = 0;
        pal_g[3] = 0;
        pal_b[3] = 0;
        pal_a[3] = is_rgba ? 0 : 255;
    }

    for (uint32_t py = 0; py < 4 && py < max_y; py++) {
        uint8_t *row = dst_rgba + py * stride;
        for (uint32_t px = 0; px < 4 && px < max_x; px++) {
            uint32_t idx = (bits >> (2 * (py * 4 + px))) & 0x3;
            row[px * 4 + 0] = pal_r[idx];
            row[px * 4 + 1] = pal_g[idx];
            row[px * 4 + 2] = pal_b[idx];
            row[px * 4 + 3] = pal_a[idx];
        }
    }
}

static void decode_bc2_block(const uint8_t *src, uint8_t *dst_rgba, uint32_t stride, uint32_t max_x, uint32_t max_y) {
    uint64_t alpha_data = *(const uint64_t*)src;
    decode_bc1_block(src + 8, dst_rgba, stride, max_x, max_y, false, true);

    for (uint32_t py = 0; py < 4 && py < max_y; py++) {
        uint8_t *row = dst_rgba + py * stride;
        for (uint32_t px = 0; px < 4 && px < max_x; px++) {
            int shift = (py * 4 + px) * 4;
            uint8_t a = (uint8_t)((alpha_data >> shift) & 0x0F);
            row[px * 4 + 3] = (uint8_t)((a << 4) | a);
        }
    }
}

static void decode_bc3_alpha(const uint8_t *src, uint8_t *dst_rgba, uint32_t stride, uint32_t max_x, uint32_t max_y) {
    uint8_t a0 = src[0];
    uint8_t a1 = src[1];
    uint64_t bits = 0;
    memcpy(&bits, src + 2, 6);

    uint8_t pal_a[8];
    pal_a[0] = a0;
    pal_a[1] = a1;

    if (a0 > a1) {
        for (int i = 1; i <= 6; i++) {
            pal_a[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
        }
    } else {
        for (int i = 1; i <= 4; i++) {
            pal_a[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
        }
        pal_a[6] = 0;
        pal_a[7] = 255;
    }

    for (uint32_t py = 0; py < 4 && py < max_y; py++) {
        uint8_t *row = dst_rgba + py * stride;
        for (uint32_t px = 0; px < 4 && px < max_x; px++) {
            int shift = (py * 4 + px) * 3;
            uint32_t idx = (uint32_t)((bits >> shift) & 0x7);
            row[px * 4 + 3] = pal_a[idx];
        }
    }
}

static void decode_bc3_block(const uint8_t *src, uint8_t *dst_rgba, uint32_t stride, uint32_t max_x, uint32_t max_y) {
    decode_bc1_block(src + 8, dst_rgba, stride, max_x, max_y, false, true);
    decode_bc3_alpha(src, dst_rgba, stride, max_x, max_y);
}

static void decode_bc4_block(const uint8_t *src, uint8_t *dst_rgba, uint32_t stride, uint32_t max_x, uint32_t max_y) {
    uint8_t a0 = src[0];
    uint8_t a1 = src[1];
    uint64_t bits = 0;
    memcpy(&bits, src + 2, 6);

    uint8_t pal[8];
    pal[0] = a0;
    pal[1] = a1;

    if (a0 > a1) {
        for (int i = 1; i <= 6; i++) {
            pal[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
        }
    } else {
        for (int i = 1; i <= 4; i++) {
            pal[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
        }
        pal[6] = 0;
        pal[7] = 255;
    }

    for (uint32_t py = 0; py < 4 && py < max_y; py++) {
        uint8_t *row = dst_rgba + py * stride;
        for (uint32_t px = 0; px < 4 && px < max_x; px++) {
            int shift = (py * 4 + px) * 3;
            uint32_t idx = (uint32_t)((bits >> shift) & 0x7);
            uint8_t val = pal[idx];
            row[px * 4 + 0] = val;
            row[px * 4 + 1] = val;
            row[px * 4 + 2] = val;
            row[px * 4 + 3] = 255;
        }
    }
}

static void decode_bc5_block(const uint8_t *src, uint8_t *dst_rgba, uint32_t stride, uint32_t max_x, uint32_t max_y) {
    init_normal_z_lut();
    // Red channel (bytes 0..7)
    uint8_t r0 = src[0], r1 = src[1];
    uint64_t r_bits = 0;
    memcpy(&r_bits, src + 2, 6);
    uint8_t pal_r[8];
    pal_r[0] = r0; pal_r[1] = r1;
    if (r0 > r1) {
        for (int i = 1; i <= 6; i++) pal_r[i + 1] = (uint8_t)(((7 - i) * r0 + i * r1) / 7);
    } else {
        for (int i = 1; i <= 4; i++) pal_r[i + 1] = (uint8_t)(((5 - i) * r0 + i * r1) / 5);
        pal_r[6] = 0; pal_r[7] = 255;
    }

    // Green channel (bytes 8..15)
    uint8_t g0 = src[8], g1 = src[9];
    uint64_t g_bits = 0;
    memcpy(&g_bits, src + 10, 6);
    uint8_t pal_g[8];
    pal_g[0] = g0; pal_g[1] = g1;
    if (g0 > g1) {
        for (int i = 1; i <= 6; i++) pal_g[i + 1] = (uint8_t)(((7 - i) * g0 + i * g1) / 7);
    } else {
        for (int i = 1; i <= 4; i++) pal_g[i + 1] = (uint8_t)(((5 - i) * g0 + i * g1) / 5);
        pal_g[6] = 0; pal_g[7] = 255;
    }

    for (uint32_t py = 0; py < 4 && py < max_y; py++) {
        uint8_t *row = dst_rgba + py * stride;
        for (uint32_t px = 0; px < 4 && px < max_x; px++) {
            int shift = (py * 4 + px) * 3;
            uint32_t r_idx = (uint32_t)((r_bits >> shift) & 0x7);
            uint32_t g_idx = (uint32_t)((g_bits >> shift) & 0x7);
            uint8_t r = pal_r[r_idx];
            uint8_t g = pal_g[g_idx];
            row[px * 4 + 0] = r;
            row[px * 4 + 1] = g;
            row[px * 4 + 2] = s_normal_z_lut[r][g]; // Exact Unit Z component
            row[px * 4 + 3] = 255;
        }
    }
}

static inline uint8_t bc7_scale_7to8(uint8_t val, uint8_t p) {
    return (uint8_t)((val << 1) | p);
}

static void decode_bc7_block(const uint8_t *src, uint8_t *dst_rgba, uint32_t stride, uint32_t max_x, uint32_t max_y) {
    uint8_t b0 = src[0];
    int mode = -1;
    for (int m = 0; m < 8; m++) {
        if (b0 & (1 << m)) {
            mode = m;
            break;
        }
    }

    if (mode == 6) {
        // Mode 6: 1 subset, 7-bit endpoints + P-bit, 4-bit indices
        uint64_t low = *(const uint64_t*)(src + 0);
        uint64_t high = *(const uint64_t*)(src + 8);

        uint8_t r0 = (uint8_t)((low >> 7) & 0x7F);
        uint8_t r1 = (uint8_t)((low >> 14) & 0x7F);
        uint8_t g0 = (uint8_t)((low >> 21) & 0x7F);
        uint8_t g1 = (uint8_t)((low >> 28) & 0x7F);
        uint8_t b0_ep = (uint8_t)((low >> 35) & 0x7F);
        uint8_t b1_ep = (uint8_t)((low >> 42) & 0x7F);
        uint8_t a0 = (uint8_t)((low >> 49) & 0x7F);
        uint8_t a1 = (uint8_t)((low >> 56) & 0x7F);
        uint8_t p0 = (uint8_t)((low >> 63) & 0x01);
        uint8_t p1 = (uint8_t)(high & 0x01);

        uint8_t ep0[4] = { bc7_scale_7to8(r0, p0), bc7_scale_7to8(g0, p0), bc7_scale_7to8(b0_ep, p0), bc7_scale_7to8(a0, p0) };
        uint8_t ep1[4] = { bc7_scale_7to8(r1, p1), bc7_scale_7to8(g1, p1), bc7_scale_7to8(b1_ep, p1), bc7_scale_7to8(a1, p1) };

        uint64_t idx_stream = high >> 1;

        for (uint32_t py = 0; py < 4 && py < max_y; py++) {
            uint8_t *row = dst_rgba + py * stride;
            for (uint32_t px = 0; px < 4 && px < max_x; px++) {
                uint32_t pixel_idx = py * 4 + px;
                uint32_t idx = 0;
                if (pixel_idx == 0) {
                    idx = (uint32_t)(idx_stream & 0x07);
                } else {
                    int shift = 3 + (pixel_idx - 1) * 4;
                    idx = (uint32_t)((idx_stream >> shift) & 0x0F);
                }

                for (int c = 0; c < 4; c++) {
                    row[px * 4 + c] = (uint8_t)(((15 - idx) * ep0[c] + idx * ep1[c] + 7) / 15);
                }
            }
        }
    } else {
        // Generalized BC7 color reconstruction
        uint16_t c0 = *(const uint16_t*)(src + 1);
        uint16_t c1 = *(const uint16_t*)(src + 3);
        uint8_t r0, g0, b0_col, r1, g1, b1_col;
        decode_565_to_rgb(c0, &r0, &g0, &b0_col);
        decode_565_to_rgb(c1, &r1, &g1, &b1_col);
        uint64_t indices = *(const uint64_t*)(src + 8);

        for (uint32_t py = 0; py < 4 && py < max_y; py++) {
            uint8_t *row = dst_rgba + py * stride;
            for (uint32_t px = 0; px < 4 && px < max_x; px++) {
                int shift = (py * 4 + px) * 3;
                uint32_t idx = (uint32_t)((indices >> shift) & 0x07);
                row[px * 4 + 0] = (uint8_t)(((7 - idx) * r0 + idx * r1) / 7);
                row[px * 4 + 1] = (uint8_t)(((7 - idx) * g0 + idx * g1) / 7);
                row[px * 4 + 2] = (uint8_t)(((7 - idx) * b0_col + idx * b1_col) / 7);
                row[px * 4 + 3] = 255;
            }
        }
    }
}

static void decompress_bcn_image(
    VkFormat format,
    const uint8_t *src,
    uint8_t *dst,
    uint32_t width,
    uint32_t height,
    uint32_t bufferRowLength)
{
    uint32_t block_size = 16;
    int bc_type = 1;
    bool is_rgba = false;

    switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            block_size = 8;
            bc_type = 1;
            is_rgba = false;
            break;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            block_size = 8;
            bc_type = 1;
            is_rgba = true;
            break;
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
            block_size = 16;
            bc_type = 2;
            break;
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
            block_size = 16;
            bc_type = 3;
            break;
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
            block_size = 8;
            bc_type = 4;
            break;
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
            block_size = 16;
            bc_type = 5;
            break;
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            block_size = 16;
            bc_type = 7;
            break;
        default:
            block_size = 16;
            bc_type = 3;
            break;
    }

    uint32_t blocks_x = (width + 3) / 4;
    uint32_t blocks_y = (height + 3) / 4;
    uint32_t pitch_blocks = bufferRowLength ? (bufferRowLength + 3) / 4 : blocks_x;
    uint32_t dst_stride = width * 4;

    for (uint32_t by = 0; by < blocks_y; by++) {
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            const uint8_t *block_src = src + (by * pitch_blocks + bx) * block_size;
            uint8_t *block_dst = dst + (by * 4 * dst_stride) + (bx * 4 * 4);
            uint32_t max_x = (bx == blocks_x - 1) ? (width - bx * 4) : 4;
            uint32_t max_y = (by == blocks_y - 1) ? (height - by * 4) : 4;

            switch (bc_type) {
                case 1:
                    decode_bc1_block(block_src, block_dst, dst_stride, max_x, max_y, is_rgba, false);
                    break;
                case 2:
                    decode_bc2_block(block_src, block_dst, dst_stride, max_x, max_y);
                    break;
                case 3:
                    decode_bc3_block(block_src, block_dst, dst_stride, max_x, max_y);
                    break;
                case 4:
                    decode_bc4_block(block_src, block_dst, dst_stride, max_x, max_y);
                    break;
                case 5:
                    decode_bc5_block(block_src, block_dst, dst_stride, max_x, max_y);
                    break;
                case 7:
                    decode_bc7_block(block_src, block_dst, dst_stride, max_x, max_y);
                    break;
            }
        }
    }
}

// -------------------------------------------------------------
// State Tracking (Images, Buffers, Memory, Staging Ring)
// -------------------------------------------------------------
typedef struct TrackedImage {
    VkImage image;
    VkFormat originalFormat;
    VkFormat targetFormat;
    uint32_t width;
    uint32_t height;
    struct TrackedImage *next;
} TrackedImage;

typedef struct TrackedBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize offset;
    struct TrackedBuffer *next;
} TrackedBuffer;

typedef struct TrackedMemory {
    VkDeviceMemory memory;
    VkDeviceSize size;
    void *mappedPtr;
    struct TrackedMemory *next;
} TrackedMemory;

#define HASH_SIZE 1024
static TrackedImage *g_images[HASH_SIZE] = {0};
static TrackedBuffer *g_buffers[HASH_SIZE] = {0};
static TrackedMemory *g_memories[HASH_SIZE] = {0};

static inline uint32_t hash_ptr(uint64_t ptr) {
    return (uint32_t)(((ptr >> 4) ^ (ptr >> 14)) & (HASH_SIZE - 1));
}

// Tracked Images
static void add_tracked_image(VkImage image, VkFormat orig, VkFormat target, uint32_t w, uint32_t h) {
    uint32_t bucket = hash_ptr((uint64_t)image);
    TrackedImage *item = calloc(1, sizeof(TrackedImage));
    item->image = image;
    item->originalFormat = orig;
    item->targetFormat = target;
    item->width = w;
    item->height = h;

    pthread_mutex_lock(&g_mutex);
    item->next = g_images[bucket];
    g_images[bucket] = item;
    pthread_mutex_unlock(&g_mutex);
}

static TrackedImage* find_tracked_image(VkImage image) {
    uint32_t bucket = hash_ptr((uint64_t)image);
    pthread_mutex_lock(&g_mutex);
    TrackedImage *curr = g_images[bucket];
    while (curr) {
        if (curr->image == image) {
            pthread_mutex_unlock(&g_mutex);
            return curr;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

static void remove_tracked_image(VkImage image) {
    uint32_t bucket = hash_ptr((uint64_t)image);
    pthread_mutex_lock(&g_mutex);
    TrackedImage **curr = &g_images[bucket];
    while (*curr) {
        if ((*curr)->image == image) {
            TrackedImage *to_free = *curr;
            *curr = (*curr)->next;
            free(to_free);
            break;
        }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&g_mutex);
}

// Tracked Buffers
static void add_tracked_buffer(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    uint32_t bucket = hash_ptr((uint64_t)buffer);
    TrackedBuffer *item = calloc(1, sizeof(TrackedBuffer));
    item->buffer = buffer;
    item->memory = memory;
    item->offset = offset;

    pthread_mutex_lock(&g_mutex);
    item->next = g_buffers[bucket];
    g_buffers[bucket] = item;
    pthread_mutex_unlock(&g_mutex);
}

static TrackedBuffer* find_tracked_buffer(VkBuffer buffer) {
    uint32_t bucket = hash_ptr((uint64_t)buffer);
    pthread_mutex_lock(&g_mutex);
    TrackedBuffer *curr = g_buffers[bucket];
    while (curr) {
        if (curr->buffer == buffer) {
            pthread_mutex_unlock(&g_mutex);
            return curr;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

static void remove_tracked_buffer(VkBuffer buffer) {
    uint32_t bucket = hash_ptr((uint64_t)buffer);
    pthread_mutex_lock(&g_mutex);
    TrackedBuffer **curr = &g_buffers[bucket];
    while (*curr) {
        if ((*curr)->buffer == buffer) {
            TrackedBuffer *to_free = *curr;
            *curr = (*curr)->next;
            free(to_free);
            break;
        }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&g_mutex);
}

// Tracked Memories
static void add_tracked_memory(VkDeviceMemory memory, VkDeviceSize size) {
    uint32_t bucket = hash_ptr((uint64_t)memory);
    TrackedMemory *item = calloc(1, sizeof(TrackedMemory));
    item->memory = memory;
    item->size = size;
    item->mappedPtr = NULL;

    pthread_mutex_lock(&g_mutex);
    item->next = g_memories[bucket];
    g_memories[bucket] = item;
    pthread_mutex_unlock(&g_mutex);
}

static TrackedMemory* find_tracked_memory(VkDeviceMemory memory) {
    uint32_t bucket = hash_ptr((uint64_t)memory);
    pthread_mutex_lock(&g_mutex);
    TrackedMemory *curr = g_memories[bucket];
    while (curr) {
        if (curr->memory == memory) {
            pthread_mutex_unlock(&g_mutex);
            return curr;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

static void remove_tracked_memory(VkDeviceMemory memory) {
    uint32_t bucket = hash_ptr((uint64_t)memory);
    pthread_mutex_lock(&g_mutex);
    TrackedMemory **curr = &g_memories[bucket];
    while (*curr) {
        if ((*curr)->memory == memory) {
            TrackedMemory *to_free = *curr;
            *curr = (*curr)->next;
            free(to_free);
            break;
        }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&g_mutex);
}

// -------------------------------------------------------------
// Staging Ring Buffer Manager
// -------------------------------------------------------------
typedef struct StagingRing {
    VkDevice device;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mappedPtr;
    VkDeviceSize size;
    VkDeviceSize head;
    bool isHostCoherent;
} StagingRing;

static StagingRing g_staging = {0};

static uint32_t find_host_visible_memory_type(uint32_t typeFilter, bool *isCoherent) {
    if (isCoherent) *isCoherent = false;
    if (g_physical_device) {
        PFN_vkGetPhysicalDeviceMemoryProperties p_get =
            (PFN_vkGetPhysicalDeviceMemoryProperties)get_driver_proc("vkGetPhysicalDeviceMemoryProperties");
        if (p_get) {
            VkPhysicalDeviceMemoryProperties memProperties;
            p_get(g_physical_device, &memProperties);
            // 1. First priority: HOST_VISIBLE | HOST_COHERENT
            for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
                VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                if ((typeFilter & (1 << i)) && ((memProperties.memoryTypes[i].propertyFlags & wanted) == wanted)) {
                    if (isCoherent) *isCoherent = true;
                    return i;
                }
            }
            // 2. Fallback: HOST_VISIBLE
            for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
                if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                    if (isCoherent) *isCoherent = false;
                    return i;
                }
            }
        }
    }
    return 0;
}

static bool ensure_staging_ring(VkDevice device, VkDeviceSize needed_size) {
    if (g_staging.buffer && g_staging.size >= needed_size) {
        return true;
    }

    init_real_vulkan();
    PFN_vkCreateBuffer real_create_buf = (PFN_vkCreateBuffer)real_vkGetDeviceProcAddr(device, "vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements real_get_reqs = (PFN_vkGetBufferMemoryRequirements)real_vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory real_alloc_mem = (PFN_vkAllocateMemory)real_vkGetDeviceProcAddr(device, "vkAllocateMemory");
    PFN_vkBindBufferMemory real_bind_mem = (PFN_vkBindBufferMemory)real_vkGetDeviceProcAddr(device, "vkBindBufferMemory");
    PFN_vkMapMemory real_map_mem = (PFN_vkMapMemory)real_vkGetDeviceProcAddr(device, "vkMapMemory");
    PFN_vkDestroyBuffer real_destroy_buf = (PFN_vkDestroyBuffer)real_vkGetDeviceProcAddr(device, "vkDestroyBuffer");
    PFN_vkFreeMemory real_free_mem = (PFN_vkFreeMemory)real_vkGetDeviceProcAddr(device, "vkFreeMemory");

    if (g_staging.buffer) {
        real_destroy_buf(device, g_staging.buffer, NULL);
        real_free_mem(device, g_staging.memory, NULL);
        g_staging.buffer = VK_NULL_HANDLE;
        g_staging.memory = VK_NULL_HANDLE;
        g_staging.mappedPtr = NULL;
    }

    VkDeviceSize alloc_size = STAGING_BUFFER_DEFAULT_SIZE;
    if (needed_size > alloc_size) {
        alloc_size = (needed_size + 0x00FFFFFFULL) & ~0x00FFFFFFULL;
    }

    VkBufferCreateInfo bufInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = alloc_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    if (real_create_buf(device, &bufInfo, NULL, &g_staging.buffer) != VK_SUCCESS) {
        LOGE("[-] Failed to create staging ring buffer!");
        return false;
    }

    VkMemoryRequirements memReqs;
    real_get_reqs(device, g_staging.buffer, &memReqs);

    bool is_coherent = false;
    uint32_t memTypeIdx = find_host_visible_memory_type(memReqs.memoryTypeBits, &is_coherent);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memTypeIdx
    };

    if (real_alloc_mem(device, &allocInfo, NULL, &g_staging.memory) != VK_SUCCESS) {
        LOGE("[-] Failed to allocate staging memory!");
        real_destroy_buf(device, g_staging.buffer, NULL);
        g_staging.buffer = VK_NULL_HANDLE;
        return false;
    }

    real_bind_mem(device, g_staging.buffer, g_staging.memory, 0);
    real_map_mem(device, g_staging.memory, 0, alloc_size, 0, &g_staging.mappedPtr);

    g_staging.device = device;
    g_staging.size = alloc_size;
    g_staging.head = 0;
    g_staging.isHostCoherent = is_coherent;
    LOGI("[+] Staging Ring Buffer initialized: %llu MB (coherent: %d)", (unsigned long long)(alloc_size / (1024 * 1024)), (int)is_coherent);
    return true;
}

// -------------------------------------------------------------
// Vulkan Interceptors
// -------------------------------------------------------------

// 1. Device Properties (Report as Adreno 750 or Mali-G615 + Adreno-Mali 24.2)
static void patch_device_properties(VkPhysicalDeviceProperties *props) {
    const char *spoof = getenv("MALI_SPOOF_ADRENO");
    if (g_is_panvk_backend && (!spoof || strcmp(spoof, "1") != 0)) {
        props->driverVersion = SHIM_DRIVER_VERSION;
        strncpy(props->deviceName, "Mali-G615 (Mali-Adreno 24.2 + GED Boost)", sizeof(props->deviceName) - 1);
        props->deviceName[sizeof(props->deviceName) - 1] = '\0';
        return;
    }
    props->vendorID = QUALCOMM_VENDOR_ID;
    props->deviceID = ADRENO_750_DEVICE_ID;
    props->driverVersion = SHIM_DRIVER_VERSION;
    strncpy(props->deviceName, SHIM_DEVICE_NAME, sizeof(props->deviceName) - 1);
    props->deviceName[sizeof(props->deviceName) - 1] = '\0';
}

VKAPI_ATTR void VKAPI_CALL shim_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties)
{
    init_real_vulkan();
    g_physical_device = physicalDevice;
    PFN_vkGetPhysicalDeviceProperties real_fn = 
        (PFN_vkGetPhysicalDeviceProperties)get_driver_proc("vkGetPhysicalDeviceProperties");
    if (real_fn) real_fn(physicalDevice, pProperties);
    patch_device_properties(pProperties);
}

VKAPI_ATTR void VKAPI_CALL shim_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties)
{
    init_real_vulkan();
    g_physical_device = physicalDevice;
    PFN_vkGetPhysicalDeviceProperties2 real_fn = 
        (PFN_vkGetPhysicalDeviceProperties2)get_driver_proc("vkGetPhysicalDeviceProperties2");
    if (real_fn) real_fn(physicalDevice, pProperties);
    patch_device_properties(&pProperties->properties);
}

// 2. Format Properties
static void patch_format_properties(VkFormat format, VkFormatProperties *props) {
    if (!g_use_cpu_bcn) return;
    if (is_bcn_format(format)) {
        props->optimalTilingFeatures |= 
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
            VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

        props->linearTilingFeatures |=
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

        props->bufferFeatures |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    }
}

VKAPI_ATTR void VKAPI_CALL shim_vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physicalDevice,
    VkFormat format,
    VkFormatProperties* pFormatProperties)
{
    init_real_vulkan();
    PFN_vkGetPhysicalDeviceFormatProperties real_fn =
        (PFN_vkGetPhysicalDeviceFormatProperties)get_driver_proc("vkGetPhysicalDeviceFormatProperties");
    if (real_fn) real_fn(physicalDevice, format, pFormatProperties);
    patch_format_properties(format, pFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL shim_vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physicalDevice,
    VkFormat format,
    VkFormatProperties2* pFormatProperties)
{
    init_real_vulkan();
    PFN_vkGetPhysicalDeviceFormatProperties2 real_fn =
        (PFN_vkGetPhysicalDeviceFormatProperties2)get_driver_proc("vkGetPhysicalDeviceFormatProperties2");
    if (real_fn) real_fn(physicalDevice, format, pFormatProperties);
    patch_format_properties(format, &pFormatProperties->formatProperties);
}

// 3. Image Format Properties
VKAPI_ATTR VkResult VKAPI_CALL shim_vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physicalDevice,
    VkFormat format,
    VkImageType type,
    VkImageTiling tiling,
    VkImageUsageFlags usage,
    VkImageCreateFlags flags,
    VkImageFormatProperties* pImageFormatProperties)
{
    init_real_vulkan();
    PFN_vkGetPhysicalDeviceImageFormatProperties real_fn =
        (PFN_vkGetPhysicalDeviceImageFormatProperties)get_driver_proc("vkGetPhysicalDeviceImageFormatProperties");

    if (g_use_cpu_bcn && is_bcn_format(format)) {
        VkFormat mapped = get_decompressed_format(format);
        if (real_fn) {
            VkResult res = real_fn(physicalDevice, mapped, type, tiling, usage, flags, pImageFormatProperties);
            if (res == VK_SUCCESS) return VK_SUCCESS;
        }

        pImageFormatProperties->maxExtent.width = 16384;
        pImageFormatProperties->maxExtent.height = 16384;
        pImageFormatProperties->maxExtent.depth = 1;
        pImageFormatProperties->maxMipLevels = 15;
        pImageFormatProperties->maxArrayLayers = 2048;
        pImageFormatProperties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        pImageFormatProperties->maxResourceSize = 1ULL << 31;
        return VK_SUCCESS;
    }

    if (real_fn) return real_fn(physicalDevice, format, type, tiling, usage, flags, pImageFormatProperties);
    return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL shim_vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
    VkImageFormatProperties2* pImageFormatProperties)
{
    init_real_vulkan();
    PFN_vkGetPhysicalDeviceImageFormatProperties2 real_fn =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2)get_driver_proc("vkGetPhysicalDeviceImageFormatProperties2");

    if (g_use_cpu_bcn && is_bcn_format(pImageFormatInfo->format)) {
        VkPhysicalDeviceImageFormatInfo2 mappedInfo = *pImageFormatInfo;
        mappedInfo.format = get_decompressed_format(pImageFormatInfo->format);
        if (real_fn) {
            VkResult res = real_fn(physicalDevice, &mappedInfo, pImageFormatProperties);
            if (res == VK_SUCCESS) return VK_SUCCESS;
        }

        pImageFormatProperties->imageFormatProperties.maxExtent.width = 16384;
        pImageFormatProperties->imageFormatProperties.maxExtent.height = 16384;
        pImageFormatProperties->imageFormatProperties.maxExtent.depth = 1;
        pImageFormatProperties->imageFormatProperties.maxMipLevels = 15;
        pImageFormatProperties->imageFormatProperties.maxArrayLayers = 2048;
        pImageFormatProperties->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        pImageFormatProperties->imageFormatProperties.maxResourceSize = 1ULL << 31;
        return VK_SUCCESS;
    }

    if (real_fn) return real_fn(physicalDevice, pImageFormatInfo, pImageFormatProperties);
    return VK_ERROR_INITIALIZATION_FAILED;
}

// 4. Features
static void patch_device_features(VkPhysicalDeviceFeatures *features) {
    features->textureCompressionBC = VK_TRUE;
    features->shaderClipDistance = VK_TRUE;
    features->shaderCullDistance = VK_TRUE;
    features->samplerAnisotropy = VK_TRUE;
    features->multiDrawIndirect = VK_TRUE;
    features->depthClamp = VK_TRUE;
    features->fillModeNonSolid = VK_TRUE;
}

typedef struct ShimCustomBorderColorFeaturesEXT {
    VkStructureType sType;
    void* pNext;
    VkBool32 customBorderColors;
    VkBool32 customBorderColorWithoutFormat;
} ShimCustomBorderColorFeaturesEXT;

typedef struct ShimCustomBorderColorPropertiesEXT {
    VkStructureType sType;
    void* pNext;
    uint32_t maxCustomBorderColorSamplers;
} ShimCustomBorderColorPropertiesEXT;

typedef struct ShimSamplerCustomBorderColorCreateInfoEXT {
    VkStructureType sType;
    const void* pNext;
    VkClearColorValue customBorderColor;
    VkFormat format;
} ShimSamplerCustomBorderColorCreateInfoEXT;

static void patch_features2_pnext(void *pNext) {
    VkBaseOutStructure *curr = (VkBaseOutStructure*)pNext;
    while (curr) {
        if ((uint32_t)curr->sType == 1000287001U) { // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT
            ShimCustomBorderColorFeaturesEXT *cbc = (ShimCustomBorderColorFeaturesEXT*)curr;
            cbc->customBorderColors = VK_TRUE;
            cbc->customBorderColorWithoutFormat = VK_TRUE;
        }
        curr = curr->pNext;
    }
}

VKAPI_ATTR void VKAPI_CALL shim_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures)
{
    init_real_vulkan();
    PFN_vkGetPhysicalDeviceFeatures real_fn =
        (PFN_vkGetPhysicalDeviceFeatures)get_driver_proc("vkGetPhysicalDeviceFeatures");
    if (real_fn) real_fn(physicalDevice, pFeatures);
    patch_device_features(pFeatures);
}

VKAPI_ATTR void VKAPI_CALL shim_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures)
{
    init_real_vulkan();
    PFN_vkGetPhysicalDeviceFeatures2 real_fn =
        (PFN_vkGetPhysicalDeviceFeatures2)get_driver_proc("vkGetPhysicalDeviceFeatures2");
    if (real_fn) real_fn(physicalDevice, pFeatures);
    patch_device_features(&pFeatures->features);
    patch_features2_pnext(pFeatures->pNext);
}

// 4b. Physical Device Memory Properties (4GB+ guarantee & VK_EXT_memory_budget)
typedef struct ShimPhysicalDeviceMemoryBudgetPropertiesEXT {
    VkStructureType sType;
    void* pNext;
    VkDeviceSize heapBudget[VK_MAX_MEMORY_HEAPS];
    VkDeviceSize heapUsage[VK_MAX_MEMORY_HEAPS];
} ShimPhysicalDeviceMemoryBudgetPropertiesEXT;

static void patch_memory_properties(VkPhysicalDeviceMemoryProperties *memProps) {
    const VkDeviceSize min_vram = 4ULL * 1024ULL * 1024ULL * 1024ULL; // 4 GB
    for (uint32_t i = 0; i < memProps->memoryHeapCount; i++) {
        if ((memProps->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            memProps->memoryHeaps[i].size < min_vram) {
            memProps->memoryHeaps[i].size = min_vram;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL shim_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* pMemoryProperties)
{
    init_real_vulkan();
    g_physical_device = physicalDevice;
    PFN_vkGetPhysicalDeviceMemoryProperties real_fn =
        (PFN_vkGetPhysicalDeviceMemoryProperties)get_driver_proc("vkGetPhysicalDeviceMemoryProperties");
    if (real_fn) real_fn(physicalDevice, pMemoryProperties);
    patch_memory_properties(pMemoryProperties);
}

VKAPI_ATTR void VKAPI_CALL shim_vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties2* pMemoryProperties)
{
    init_real_vulkan();
    g_physical_device = physicalDevice;
    PFN_vkGetPhysicalDeviceMemoryProperties2 real_fn =
        (PFN_vkGetPhysicalDeviceMemoryProperties2)get_driver_proc("vkGetPhysicalDeviceMemoryProperties2");
    if (real_fn) real_fn(physicalDevice, pMemoryProperties);
    patch_memory_properties(&pMemoryProperties->memoryProperties);

    VkBaseOutStructure *curr = (VkBaseOutStructure*)pMemoryProperties->pNext;
    while (curr) {
        if ((uint32_t)curr->sType == 1000237000U) { // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT
            ShimPhysicalDeviceMemoryBudgetPropertiesEXT *budget = (ShimPhysicalDeviceMemoryBudgetPropertiesEXT*)curr;
            for (uint32_t i = 0; i < pMemoryProperties->memoryProperties.memoryHeapCount; i++) {
                if (budget->heapBudget[i] == 0) {
                    budget->heapBudget[i] = (pMemoryProperties->memoryProperties.memoryHeaps[i].size * 85) / 100;
                    budget->heapUsage[i] = 256ULL * 1024ULL * 1024ULL;
                }
            }
        }
        curr = curr->pNext;
    }
}

// 5. Extensions
VKAPI_ATTR VkResult VKAPI_CALL shim_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties)
{
    init_real_vulkan();
    PFN_vkEnumerateDeviceExtensionProperties real_fn =
        (PFN_vkEnumerateDeviceExtensionProperties)get_driver_proc("vkEnumerateDeviceExtensionProperties");

    if (!pProperties) {
        if (!real_fn) return VK_ERROR_INITIALIZATION_FAILED;
        VkResult res = real_fn(physicalDevice, pLayerName, pPropertyCount, NULL);
        if (pPropertyCount) *pPropertyCount += 3;
        return res;
    }

    uint32_t real_count = *pPropertyCount;
    VkResult res = real_fn ? real_fn(physicalDevice, pLayerName, &real_count, pProperties) : VK_SUCCESS;

    const struct { const char *name; uint32_t ver; } extra_exts[] = {
        { "VK_EXT_custom_border_color", 12 },
        { "VK_KHR_shader_float16_int8", 1 },
        { "VK_EXT_memory_budget", 1 }
    };

    for (int e = 0; e < 3; e++) {
        bool exists = false;
        for (uint32_t i = 0; i < real_count; i++) {
            if (strcmp(pProperties[i].extensionName, extra_exts[e].name) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists && real_count < *pPropertyCount) {
            strncpy(pProperties[real_count].extensionName, extra_exts[e].name, VK_MAX_EXTENSION_NAME_SIZE - 1);
            pProperties[real_count].extensionName[VK_MAX_EXTENSION_NAME_SIZE - 1] = '\0';
            pProperties[real_count].specVersion = extra_exts[e].ver;
            real_count++;
        }
    }
    *pPropertyCount = real_count;
    return res;
}

// 6. Memory Interceptions
VKAPI_ATTR VkResult VKAPI_CALL shim_vkAllocateMemory(
    VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory* pMemory)
{
    if (!real_vkAllocateMemory) {
        init_real_vulkan();
        real_vkAllocateMemory = (PFN_vkAllocateMemory)real_vkGetDeviceProcAddr(device, "vkAllocateMemory");
    }
    VkResult res = real_vkAllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
    if (g_use_cpu_bcn && res == VK_SUCCESS && pMemory && *pMemory) {
        add_tracked_memory(*pMemory, pAllocateInfo->allocationSize);
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL shim_vkFreeMemory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator)
{
    if (!real_vkFreeMemory) {
        init_real_vulkan();
        real_vkFreeMemory = (PFN_vkFreeMemory)real_vkGetDeviceProcAddr(device, "vkFreeMemory");
    }
    if (g_use_cpu_bcn) remove_tracked_memory(memory);
    real_vkFreeMemory(device, memory, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL shim_vkMapMemory(
    VkDevice device,
    VkDeviceMemory memory,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkMemoryMapFlags flags,
    void** ppData)
{
    if (!real_vkMapMemory) {
        init_real_vulkan();
        real_vkMapMemory = (PFN_vkMapMemory)real_vkGetDeviceProcAddr(device, "vkMapMemory");
    }
    VkResult res = real_vkMapMemory(device, memory, offset, size, flags, ppData);
    if (g_use_cpu_bcn && res == VK_SUCCESS && ppData && *ppData) {
        TrackedMemory *item = find_tracked_memory(memory);
        if (item) {
            item->mappedPtr = (uint8_t*)(*ppData) - offset;
        }
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL shim_vkUnmapMemory(
    VkDevice device,
    VkDeviceMemory memory)
{
    if (!real_vkUnmapMemory) {
        init_real_vulkan();
        real_vkUnmapMemory = (PFN_vkUnmapMemory)real_vkGetDeviceProcAddr(device, "vkUnmapMemory");
    }
    if (g_use_cpu_bcn) {
        TrackedMemory *item = find_tracked_memory(memory);
        if (item) {
            item->mappedPtr = NULL;
        }
    }
    real_vkUnmapMemory(device, memory);
}

// 7. Buffer Interceptions
VKAPI_ATTR VkResult VKAPI_CALL shim_vkBindBufferMemory(
    VkDevice device,
    VkBuffer buffer,
    VkDeviceMemory memory,
    VkDeviceSize memoryOffset)
{
    if (!real_vkBindBufferMemory) {
        init_real_vulkan();
        real_vkBindBufferMemory = (PFN_vkBindBufferMemory)real_vkGetDeviceProcAddr(device, "vkBindBufferMemory");
    }
    VkResult res = real_vkBindBufferMemory(device, buffer, memory, memoryOffset);
    if (g_use_cpu_bcn && res == VK_SUCCESS) {
        add_tracked_buffer(buffer, memory, memoryOffset);
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL shim_vkBindBufferMemory2(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindBufferMemoryInfo* pBindInfos)
{
    if (!real_vkBindBufferMemory2) {
        init_real_vulkan();
        real_vkBindBufferMemory2 = (PFN_vkBindBufferMemory2)real_vkGetDeviceProcAddr(device, "vkBindBufferMemory2");
    }
    VkResult res = real_vkBindBufferMemory2(device, bindInfoCount, pBindInfos);
    if (g_use_cpu_bcn && res == VK_SUCCESS && pBindInfos) {
        for (uint32_t i = 0; i < bindInfoCount; i++) {
            add_tracked_buffer(pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        }
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL shim_vkDestroyBuffer(
    VkDevice device,
    VkBuffer buffer,
    const VkAllocationCallbacks* pAllocator)
{
    if (!real_vkDestroyBuffer) {
        init_real_vulkan();
        real_vkDestroyBuffer = (PFN_vkDestroyBuffer)real_vkGetDeviceProcAddr(device, "vkDestroyBuffer");
    }
    if (g_use_cpu_bcn) remove_tracked_buffer(buffer);
    real_vkDestroyBuffer(device, buffer, pAllocator);
}

// 8. Image Interceptions
VKAPI_ATTR VkResult VKAPI_CALL shim_vkCreateImage(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage)
{
    if (!real_vkCreateImage) {
        init_real_vulkan();
        real_vkCreateImage = (PFN_vkCreateImage)real_vkGetDeviceProcAddr(device, "vkCreateImage");
    }

    if (g_use_cpu_bcn && is_bcn_format(pCreateInfo->format)) {
        VkFormat targetFormat = get_decompressed_format(pCreateInfo->format);
        VkImageCreateInfo newInfo = *pCreateInfo;
        newInfo.format = targetFormat;
        newInfo.flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
        newInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        newInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        static const VkFormat s_view_formats[] = {
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R8G8B8A8_SRGB
        };
        VkImageFormatListCreateInfo formatList = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
            .pNext = NULL,
            .viewFormatCount = 2,
            .pViewFormats = s_view_formats
        };
        newInfo.pNext = &formatList;

        VkResult res = real_vkCreateImage(device, &newInfo, pAllocator, pImage);
        if (res == VK_SUCCESS && pImage && *pImage) {
            add_tracked_image(*pImage, pCreateInfo->format, targetFormat, pCreateInfo->extent.width, pCreateInfo->extent.height);
            LOGI("[+] Remapped BCn Image: orig 0x%x -> RGBA8 0x%x (%ux%ux%u, %u layers)",
                 pCreateInfo->format, targetFormat, pCreateInfo->extent.width, pCreateInfo->extent.height, pCreateInfo->extent.depth, pCreateInfo->arrayLayers);
        }
        return res;
    }

    return real_vkCreateImage(device, pCreateInfo, pAllocator, pImage);
}

VKAPI_ATTR void VKAPI_CALL shim_vkDestroyImage(
    VkDevice device,
    VkImage image,
    const VkAllocationCallbacks* pAllocator)
{
    if (!real_vkDestroyImage) {
        init_real_vulkan();
        real_vkDestroyImage = (PFN_vkDestroyImage)real_vkGetDeviceProcAddr(device, "vkDestroyImage");
    }
    if (g_use_cpu_bcn) remove_tracked_image(image);
    real_vkDestroyImage(device, image, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL shim_vkGetImageSubresourceLayout(
    VkDevice device,
    VkImage image,
    const VkImageSubresource* pSubresource,
    VkSubresourceLayout* pLayout)
{
    if (!real_vkGetImageSubresourceLayout) {
        init_real_vulkan();
        real_vkGetImageSubresourceLayout =
            (PFN_vkGetImageSubresourceLayout)real_vkGetDeviceProcAddr(device, "vkGetImageSubresourceLayout");
    }
    if (real_vkGetImageSubresourceLayout) {
        real_vkGetImageSubresourceLayout(device, image, pSubresource, pLayout);
    }
    TrackedImage *tracked = find_tracked_image(image);
    if (tracked && pLayout && pSubresource) {
        uint32_t mip_w = tracked->width >> pSubresource->mipLevel;
        uint32_t mip_h = tracked->height >> pSubresource->mipLevel;
        if (mip_w == 0) mip_w = 1;
        if (mip_h == 0) mip_h = 1;
        uint32_t blocks_x = (mip_w + 3) / 4;
        uint32_t blocks_y = (mip_h + 3) / 4;
        uint32_t block_size = get_bcn_block_size(tracked->originalFormat);
        pLayout->rowPitch = (VkDeviceSize)blocks_x * block_size;
        pLayout->size = pLayout->rowPitch * blocks_y;
        pLayout->arrayPitch = pLayout->size;
        pLayout->depthPitch = pLayout->size;
    }
}

// 9. Image View & Sampler Interceptions
VKAPI_ATTR VkResult VKAPI_CALL shim_vkCreateImageView(
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView* pView)
{
    if (!real_vkCreateImageView) {
        init_real_vulkan();
        real_vkCreateImageView = (PFN_vkCreateImageView)real_vkGetDeviceProcAddr(device, "vkCreateImageView");
    }

    if (g_use_cpu_bcn) {
        TrackedImage *tracked = find_tracked_image(pCreateInfo->image);
        if (tracked || is_bcn_format(pCreateInfo->format)) {
            VkImageViewCreateInfo newInfo = *pCreateInfo;
            bool want_srgb = is_srgb_format(pCreateInfo->format) ||
                             pCreateInfo->format == VK_FORMAT_R8G8B8A8_SRGB ||
                             (tracked && is_srgb_format(tracked->originalFormat));
            newInfo.format = want_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            return real_vkCreateImageView(device, &newInfo, pAllocator, pView);
        }
    }

    return real_vkCreateImageView(device, pCreateInfo, pAllocator, pView);
}

VKAPI_ATTR VkResult VKAPI_CALL shim_vkCreateSampler(
    VkDevice device,
    const VkSamplerCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler* pSampler)
{
    if (!real_vkCreateSampler) {
        init_real_vulkan();
        real_vkCreateSampler = (PFN_vkCreateSampler)real_vkGetDeviceProcAddr(device, "vkCreateSampler");
    }

    // Map VK_EXT_custom_border_color (1000287003 / 1000287004) to standard Vulkan border colors for Mali
    if ((uint32_t)pCreateInfo->borderColor == 1000287003U || (uint32_t)pCreateInfo->borderColor == 1000287004U) {
        VkSamplerCreateInfo newInfo = *pCreateInfo;
        newInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

        const VkBaseInStructure *curr = (const VkBaseInStructure*)pCreateInfo->pNext;
        while (curr) {
            if ((uint32_t)curr->sType == 1000287000U) { // VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT
                const ShimSamplerCustomBorderColorCreateInfoEXT *cbc =
                    (const ShimSamplerCustomBorderColorCreateInfoEXT*)curr;
                float r = cbc->customBorderColor.float32[0];
                float g = cbc->customBorderColor.float32[1];
                float b = cbc->customBorderColor.float32[2];
                float a = cbc->customBorderColor.float32[3];
                if (r > 0.5f && g > 0.5f && b > 0.5f) {
                    newInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
                } else if (a > 0.5f) {
                    newInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
                } else {
                    newInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
                }
                newInfo.pNext = curr->pNext;
                break;
            }
            curr = curr->pNext;
        }
        return real_vkCreateSampler(device, &newInfo, pAllocator, pSampler);
    }

    return real_vkCreateSampler(device, pCreateInfo, pAllocator, pSampler);
}

// 10. Copy Buffer to Image Interception (Decompresses on the fly)
VKAPI_ATTR void VKAPI_CALL shim_vkCmdCopyBufferToImage(
    VkCommandBuffer commandBuffer,
    VkBuffer srcBuffer,
    VkImage dstImage,
    VkImageLayout dstImageLayout,
    uint32_t regionCount,
    const VkBufferImageCopy* pRegions)
{
    if (!real_vkCmdCopyBufferToImage) {
        init_real_vulkan();
        real_vkCmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)real_vkGetDeviceProcAddr(g_staging.device, "vkCmdCopyBufferToImage");
    }

    TrackedImage *tracked = find_tracked_image(dstImage);
    if (!tracked) {
        if (real_vkCmdCopyBufferToImage) real_vkCmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
        return;
    }

    TrackedBuffer *buf = find_tracked_buffer(srcBuffer);
    if (!buf) {
        LOGE("[-] Source buffer not tracked for BCn image copy!");
        if (real_vkCmdCopyBufferToImage) real_vkCmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
        return;
    }

    TrackedMemory *mem = find_tracked_memory(buf->memory);
    if (!mem) {
        LOGE("[-] Source memory not tracked for BCn image copy!");
        if (real_vkCmdCopyBufferToImage) real_vkCmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
        return;
    }

    void *src_base = mem->mappedPtr;
    bool needs_unmap = false;
    if (!src_base) {
        if (!real_vkMapMemory) real_vkMapMemory = (PFN_vkMapMemory)real_vkGetDeviceProcAddr(g_staging.device, "vkMapMemory");
        if (real_vkMapMemory && real_vkMapMemory(g_staging.device, buf->memory, 0, mem->size, 0, &src_base) == VK_SUCCESS) {
            needs_unmap = true;
        }
    }

    if (!src_base) {
        LOGE("[-] Cannot map source buffer for BCn decompression!");
        if (real_vkCmdCopyBufferToImage) real_vkCmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
        return;
    }

    for (uint32_t i = 0; i < regionCount; i++) {
        const VkBufferImageCopy *region = &pRegions[i];
        uint32_t width = region->imageExtent.width;
        uint32_t height = region->imageExtent.height;
        uint32_t layers = region->imageSubresource.layerCount ? region->imageSubresource.layerCount : 1;
        uint32_t depth = region->imageExtent.depth ? region->imageExtent.depth : 1;
        uint32_t total_slices = layers * depth;

        VkDeviceSize decomp_slice_size = (VkDeviceSize)width * height * 4;
        VkDeviceSize decomp_total_size = decomp_slice_size * total_slices;

        if (!ensure_staging_ring(g_staging.device, decomp_total_size)) {
            LOGE("[-] Staging buffer allocation failed for %llu bytes!", (unsigned long long)decomp_total_size);
            break;
        }

        pthread_mutex_lock(&g_mutex);
        if (g_staging.head + decomp_total_size > g_staging.size) {
            if (!real_vkDeviceWaitIdle) {
                real_vkDeviceWaitIdle = (PFN_vkDeviceWaitIdle)real_vkGetDeviceProcAddr(g_staging.device, "vkDeviceWaitIdle");
            }
            if (real_vkDeviceWaitIdle && g_staging.device) {
                real_vkDeviceWaitIdle(g_staging.device);
            }
            g_staging.head = 0; // GPU finished copying from offset 0, safe to wrap
        }
        VkDeviceSize offset = g_staging.head;
        g_staging.head = (g_staging.head + decomp_total_size + 255) & ~255ULL;
        pthread_mutex_unlock(&g_mutex);

        uint32_t blocks_x = (width + 3) / 4;
        uint32_t blocks_y = (height + 3) / 4;
        uint32_t row_blocks = region->bufferRowLength ? ((region->bufferRowLength + 3) / 4) : blocks_x;
        uint32_t col_blocks = region->bufferImageHeight ? ((region->bufferImageHeight + 3) / 4) : blocks_y;
        uint32_t block_size = get_bcn_block_size(tracked->originalFormat);

        VkDeviceSize src_slice_stride = (VkDeviceSize)row_blocks * col_blocks * block_size;
        const uint8_t *src_base_ptr = (const uint8_t*)src_base + (buf->offset + region->bufferOffset);
        uint8_t *dst_base_ptr = (uint8_t*)g_staging.mappedPtr + offset;

        // Decompress all layers / depth slices
        for (uint32_t s = 0; s < total_slices; s++) {
            const uint8_t *src_slice = src_base_ptr + s * src_slice_stride;
            uint8_t *dst_slice = dst_base_ptr + s * decomp_slice_size;
            decompress_bcn_image(tracked->originalFormat, src_slice, dst_slice, width, height, region->bufferRowLength);
        }

        if (!g_staging.isHostCoherent) {
            if (!real_vkFlushMappedMemoryRanges) {
                real_vkFlushMappedMemoryRanges = (PFN_vkFlushMappedMemoryRanges)real_vkGetDeviceProcAddr(g_staging.device, "vkFlushMappedMemoryRanges");
            }
            if (real_vkFlushMappedMemoryRanges) {
                VkMappedMemoryRange range = {
                    .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                    .pNext = NULL,
                    .memory = g_staging.memory,
                    .offset = offset,
                    .size = (decomp_total_size + 63) & ~63ULL
                };
                real_vkFlushMappedMemoryRanges(g_staging.device, 1, &range);
            }
        }

        // Record copy from staging ring into dstImage
        VkBufferImageCopy newRegion = *region;
        newRegion.bufferOffset = offset;
        newRegion.bufferRowLength = width;
        newRegion.bufferImageHeight = height;

        if (real_vkCmdCopyBufferToImage) {
            real_vkCmdCopyBufferToImage(commandBuffer, g_staging.buffer, dstImage, dstImageLayout, 1, &newRegion);
        }
    }

    if (needs_unmap) {
        if (!real_vkUnmapMemory) real_vkUnmapMemory = (PFN_vkUnmapMemory)real_vkGetDeviceProcAddr(g_staging.device, "vkUnmapMemory");
        if (real_vkUnmapMemory) real_vkUnmapMemory(g_staging.device, buf->memory);
    }
}

VKAPI_ATTR void VKAPI_CALL shim_vkCmdCopyBufferToImage2(
    VkCommandBuffer commandBuffer,
    const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo)
{
    shim_vkCmdCopyBufferToImage(
        commandBuffer,
        pCopyBufferToImageInfo->srcBuffer,
        pCopyBufferToImageInfo->dstImage,
        pCopyBufferToImageInfo->dstImageLayout,
        pCopyBufferToImageInfo->regionCount,
        (const VkBufferImageCopy*)pCopyBufferToImageInfo->pRegions);
}

// 11. Create Instance & Device
VKAPI_ATTR VkResult VKAPI_CALL shim_vkEnumerateInstanceExtensionProperties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties)
{
    init_real_vulkan();
    if (!real_vkEnumerateInstanceExtensionProperties) {
        real_vkEnumerateInstanceExtensionProperties =
            (PFN_vkEnumerateInstanceExtensionProperties)get_driver_proc("vkEnumerateInstanceExtensionProperties");
    }
    if (real_vkEnumerateInstanceExtensionProperties) {
        return real_vkEnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL shim_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    init_real_vulkan();
    if (!real_vkCreateInstance) {
        real_vkCreateInstance = (PFN_vkCreateInstance)get_driver_proc("vkCreateInstance");
    }
    if (!real_vkCreateInstance) {
        LOGE("[-] Failed to find real vkCreateInstance!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult res = real_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (res == VK_SUCCESS && pInstance && *pInstance) {
        add_live_instance(*pInstance);
        if (!real_vkGetDeviceProcAddr && real_vkGetInstanceProcAddr) {
            real_vkGetDeviceProcAddr =
                (PFN_vkGetDeviceProcAddr)real_vkGetInstanceProcAddr(*pInstance, "vkGetDeviceProcAddr");
        }
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL shim_vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator)
{
    init_real_vulkan();
    PFN_vkDestroyInstance real_fn = NULL;
    if (real_vkGetInstanceProcAddr && instance) {
        real_fn = (PFN_vkDestroyInstance)real_vkGetInstanceProcAddr(instance, "vkDestroyInstance");
    }
    if (!real_fn) {
        real_fn = (PFN_vkDestroyInstance)get_driver_proc("vkDestroyInstance");
    }
    remove_live_instance(instance);
    if (real_fn) {
        real_fn(instance, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL shim_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    init_real_vulkan();
    g_physical_device = physicalDevice;
    PFN_vkCreateDevice real_fn = (PFN_vkCreateDevice)get_driver_proc("vkCreateDevice");
    if (!real_fn) {
        LOGE("[-] Failed to find real vkCreateDevice!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = VK_ERROR_INITIALIZATION_FAILED;

    if (g_is_panvk_backend && !g_use_cpu_bcn) {
        // PanVK natively supports BC emulation, custom border colors, and memory budget
        res = real_fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }

    if (res != VK_SUCCESS) {
        // Filter out emulated extensions (like VK_EXT_custom_border_color, VK_EXT_memory_budget)
        // and emulated feature structs so Mali hardware vkCreateDevice succeeds without VK_ERROR_EXTENSION_NOT_PRESENT
        VkDeviceCreateInfo filteredInfo = *pCreateInfo;
        const char **filteredExts = NULL;
        if (pCreateInfo->enabledExtensionCount > 0 && pCreateInfo->ppEnabledExtensionNames) {
            filteredExts = (const char**)malloc(sizeof(char*) * pCreateInfo->enabledExtensionCount);
            uint32_t outCount = 0;
            for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
                const char *ext = pCreateInfo->ppEnabledExtensionNames[i];
                if (strcmp(ext, "VK_EXT_custom_border_color") == 0 ||
                    strcmp(ext, "VK_EXT_memory_budget") == 0) {
                    continue;
                }
                filteredExts[outCount++] = ext;
            }
            filteredInfo.enabledExtensionCount = outCount;
            filteredInfo.ppEnabledExtensionNames = filteredExts;
        }

        VkPhysicalDeviceFeatures localFeatures;
        if (pCreateInfo->pEnabledFeatures) {
            localFeatures = *pCreateInfo->pEnabledFeatures;
            localFeatures.textureCompressionBC = VK_FALSE;
            localFeatures.shaderClipDistance = VK_FALSE;
            localFeatures.shaderCullDistance = VK_FALSE;
            filteredInfo.pEnabledFeatures = &localFeatures;
        }

        res = real_fn(physicalDevice, &filteredInfo, pAllocator, pDevice);
        if (filteredExts) free(filteredExts);

        if (res != VK_SUCCESS) {
            res = real_fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
        }
    }

    if (res == VK_SUCCESS && pDevice && *pDevice) {
        g_staging.device = *pDevice;
        if (!real_vkGetDeviceProcAddr && real_vkGetInstanceProcAddr) {
            real_vkGetDeviceProcAddr =
                (PFN_vkGetDeviceProcAddr)real_vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr");
        }
        real_vkCreateImage = (PFN_vkCreateImage)real_vkGetDeviceProcAddr(*pDevice, "vkCreateImage");
        real_vkDestroyImage = (PFN_vkDestroyImage)real_vkGetDeviceProcAddr(*pDevice, "vkDestroyImage");
        real_vkCreateImageView = (PFN_vkCreateImageView)real_vkGetDeviceProcAddr(*pDevice, "vkCreateImageView");
        real_vkGetImageSubresourceLayout = (PFN_vkGetImageSubresourceLayout)real_vkGetDeviceProcAddr(*pDevice, "vkGetImageSubresourceLayout");
        real_vkCreateSampler = (PFN_vkCreateSampler)real_vkGetDeviceProcAddr(*pDevice, "vkCreateSampler");
        real_vkCmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)real_vkGetDeviceProcAddr(*pDevice, "vkCmdCopyBufferToImage");
        real_vkAllocateMemory = (PFN_vkAllocateMemory)real_vkGetDeviceProcAddr(*pDevice, "vkAllocateMemory");
        real_vkFreeMemory = (PFN_vkFreeMemory)real_vkGetDeviceProcAddr(*pDevice, "vkFreeMemory");
        real_vkMapMemory = (PFN_vkMapMemory)real_vkGetDeviceProcAddr(*pDevice, "vkMapMemory");
        real_vkUnmapMemory = (PFN_vkUnmapMemory)real_vkGetDeviceProcAddr(*pDevice, "vkUnmapMemory");
        real_vkBindBufferMemory = (PFN_vkBindBufferMemory)real_vkGetDeviceProcAddr(*pDevice, "vkBindBufferMemory");
        real_vkBindBufferMemory2 = (PFN_vkBindBufferMemory2)real_vkGetDeviceProcAddr(*pDevice, "vkBindBufferMemory2");
        real_vkDestroyBuffer = (PFN_vkDestroyBuffer)real_vkGetDeviceProcAddr(*pDevice, "vkDestroyBuffer");
        real_vkDeviceWaitIdle = (PFN_vkDeviceWaitIdle)real_vkGetDeviceProcAddr(*pDevice, "vkDeviceWaitIdle");
        real_vkFlushMappedMemoryRanges = (PFN_vkFlushMappedMemoryRanges)real_vkGetDeviceProcAddr(*pDevice, "vkFlushMappedMemoryRanges");
        real_vkQueueSubmit = (PFN_vkQueueSubmit)real_vkGetDeviceProcAddr(*pDevice, "vkQueueSubmit");

        start_mtk_ged_booster();
        if (g_use_cpu_bcn) {
            ensure_staging_ring(*pDevice, STAGING_BUFFER_DEFAULT_SIZE);
        }
        LOGI("[+] Vulkan Device created, MediaTek GED 836MHz GPU Boost active (cpu_bcn=%d).", (int)g_use_cpu_bcn);
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL shim_vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence)
{
    if (!real_vkQueueSubmit) {
        init_real_vulkan();
        real_vkQueueSubmit = (PFN_vkQueueSubmit)real_vkGetDeviceProcAddr(g_staging.device, "vkQueueSubmit");
    }
    // Periodically refresh MediaTek GED hardware GPU frequency boost so Mali-G615 never downclocks to 265MHz
    if (((++g_submit_counter) & 31U) == 0U) {
        mtk_ged_boost_gpu();
    }
    return real_vkQueueSubmit ? real_vkQueueSubmit(queue, submitCount, pSubmits, fence) : VK_ERROR_INITIALIZATION_FAILED;
}

// -------------------------------------------------------------
// Exported Dispatch Tables
// -------------------------------------------------------------
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    init_real_vulkan();
    if (instance) add_live_instance(instance);
    if (!pName) return NULL;

    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)shim_vkCreateInstance;
    if (strcmp(pName, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)shim_vkDestroyInstance;
    if (strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)shim_vkEnumerateInstanceExtensionProperties;
    if (strcmp(pName, "vkGetPhysicalDeviceProperties") == 0)
        return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceProperties;
    if (strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 || strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0)
        return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceProperties2;
    if (strcmp(pName, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)shim_vkCreateDevice;

    if (!g_is_panvk_backend || g_use_cpu_bcn) {
        if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0)
            return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceFormatProperties;
        if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties2") == 0 || strcmp(pName, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)
            return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceFormatProperties2;
        if (strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties") == 0)
            return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceImageFormatProperties;
        if (strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2") == 0 || strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0)
            return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceImageFormatProperties2;
        if (strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0)
            return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceFeatures;
        if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 || strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0)
            return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceFeatures2;
        if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0)
            return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceMemoryProperties;
        if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2") == 0 || strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0)
            return (PFN_vkVoidFunction)shim_vkGetPhysicalDeviceMemoryProperties2;
        if (strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0)
            return (PFN_vkVoidFunction)shim_vkEnumerateDeviceExtensionProperties;

        // Also allow device functions queried via vkGetInstanceProcAddr (used by Zink / LWJGL / VulkanMod)
        if (strcmp(pName, "vkCreateImage") == 0 ||
            strcmp(pName, "vkDestroyImage") == 0 ||
            strcmp(pName, "vkCreateImageView") == 0 ||
            strcmp(pName, "vkGetImageSubresourceLayout") == 0 ||
            strcmp(pName, "vkCreateSampler") == 0 ||
            strcmp(pName, "vkCmdCopyBufferToImage") == 0 ||
            strcmp(pName, "vkCmdCopyBufferToImage2") == 0 ||
            strcmp(pName, "vkCmdCopyBufferToImage2KHR") == 0 ||
            strcmp(pName, "vkAllocateMemory") == 0 ||
            strcmp(pName, "vkFreeMemory") == 0 ||
            strcmp(pName, "vkMapMemory") == 0 ||
            strcmp(pName, "vkUnmapMemory") == 0 ||
            strcmp(pName, "vkBindBufferMemory") == 0 ||
            strcmp(pName, "vkBindBufferMemory2") == 0 ||
            strcmp(pName, "vkBindBufferMemory2KHR") == 0 ||
            strcmp(pName, "vkDestroyBuffer") == 0 ||
            strcmp(pName, "vkQueueSubmit") == 0) {
            return vkGetDeviceProcAddr(VK_NULL_HANDLE, pName);
        }
    }

    if (real_vkGetInstanceProcAddr) {
        PFN_vkVoidFunction fn = real_vkGetInstanceProcAddr(instance ? instance : g_instance, pName);
        if (fn) return fn;
    }
    return (PFN_vkVoidFunction)get_driver_proc(pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    init_real_vulkan();
    if (!pName) return NULL;

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;

    if (!g_is_panvk_backend || g_use_cpu_bcn) {
        if (strcmp(pName, "vkCreateImage") == 0)
            return (PFN_vkVoidFunction)shim_vkCreateImage;
        if (strcmp(pName, "vkDestroyImage") == 0)
            return (PFN_vkVoidFunction)shim_vkDestroyImage;
        if (strcmp(pName, "vkCreateImageView") == 0)
            return (PFN_vkVoidFunction)shim_vkCreateImageView;
        if (strcmp(pName, "vkGetImageSubresourceLayout") == 0)
            return (PFN_vkVoidFunction)shim_vkGetImageSubresourceLayout;
        if (strcmp(pName, "vkCreateSampler") == 0)
            return (PFN_vkVoidFunction)shim_vkCreateSampler;
        if (strcmp(pName, "vkCmdCopyBufferToImage") == 0)
            return (PFN_vkVoidFunction)shim_vkCmdCopyBufferToImage;
        if (strcmp(pName, "vkCmdCopyBufferToImage2") == 0 || strcmp(pName, "vkCmdCopyBufferToImage2KHR") == 0)
            return (PFN_vkVoidFunction)shim_vkCmdCopyBufferToImage2;
        if (strcmp(pName, "vkAllocateMemory") == 0)
            return (PFN_vkVoidFunction)shim_vkAllocateMemory;
        if (strcmp(pName, "vkFreeMemory") == 0)
            return (PFN_vkVoidFunction)shim_vkFreeMemory;
        if (strcmp(pName, "vkMapMemory") == 0)
            return (PFN_vkVoidFunction)shim_vkMapMemory;
        if (strcmp(pName, "vkUnmapMemory") == 0)
            return (PFN_vkVoidFunction)shim_vkUnmapMemory;
        if (strcmp(pName, "vkBindBufferMemory") == 0)
            return (PFN_vkVoidFunction)shim_vkBindBufferMemory;
        if (strcmp(pName, "vkBindBufferMemory2") == 0 || strcmp(pName, "vkBindBufferMemory2KHR") == 0)
            return (PFN_vkVoidFunction)shim_vkBindBufferMemory2;
        if (strcmp(pName, "vkDestroyBuffer") == 0)
            return (PFN_vkVoidFunction)shim_vkDestroyBuffer;
        if (strcmp(pName, "vkQueueSubmit") == 0)
            return (PFN_vkVoidFunction)shim_vkQueueSubmit;
    }

    if (real_vkGetDeviceProcAddr && device) {
        return real_vkGetDeviceProcAddr(device, pName);
    }
    return (PFN_vkVoidFunction)get_driver_proc(pName);
}

// Direct Vulkan Loader Exports (when loaded directly as libvulkan.so by PojavLauncher / VulkanMod / LWJGL)
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    return shim_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties)
{
    return shim_vkEnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount,
    VkLayerProperties* pProperties)
{
    init_real_vulkan();
    PFN_vkEnumerateInstanceLayerProperties real_fn =
        (PFN_vkEnumerateInstanceLayerProperties)get_driver_proc("vkEnumerateInstanceLayerProperties");
    if (real_fn) return real_fn(pPropertyCount, pProperties);
    if (pPropertyCount) *pPropertyCount = 0;
    return VK_SUCCESS;
}

// Standard Khronos Vulkan ICD Loader Entry Points
VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion) {
    if (!pSupportedVersion) return VK_ERROR_INITIALIZATION_FAILED;
    init_real_vulkan();
    if (real_vk_icdNegotiateLoaderICDInterfaceVersion) {
        return real_vk_icdNegotiateLoaderICDInterfaceVersion(pSupportedVersion);
    }
    if (*pSupportedVersion > 5) {
        *pSupportedVersion = 5;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return vkGetInstanceProcAddr(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return NULL;
    init_real_vulkan();
    if (instance) add_live_instance(instance);
    if (strcmp(pName, "vkGetPhysicalDeviceProperties") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0 ||
        strcmp(pName, "vkCreateDevice") == 0) {
        return vkGetInstanceProcAddr(instance, pName);
    }
    if (!g_is_panvk_backend || g_use_cpu_bcn) {
        if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceFormatProperties2KHR") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2") == 0 ||
            strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0 ||
            strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) {
            return vkGetInstanceProcAddr(instance, pName);
        }
    }
    if (real_vk_icdGetPhysicalDeviceProcAddr) {
        PFN_vkVoidFunction fn = real_vk_icdGetPhysicalDeviceProcAddr(instance ? instance : g_instance, pName);
        if (fn) return fn;
    }
    return vkGetInstanceProcAddr(instance, pName);
}

// Android Hardware Vulkan HAL (HMI) Export for AdrenoTools / PojavLauncher / Zalith / FCL / Skyline
static int shim_hal_close(struct hw_device_t *device) {
    (void)device;
    return 0;
}

static hwvulkan_device_t g_shim_hal_device = {
    .common = {
        .tag = 0x48574456, // HARDWARE_DEVICE_TAG ('HWDV')
        .version = 0x0100, // HARDWARE_MAKE_API_VERSION(1, 0)
        .module = &HMI.common,
        .reserved = {0},
        .close = shim_hal_close
    },
    .EnumerateInstanceExtensionProperties = shim_vkEnumerateInstanceExtensionProperties,
    .CreateInstance = shim_vkCreateInstance,
    .GetInstanceProcAddr = vkGetInstanceProcAddr
};

static int shim_hal_open(const struct hw_module_t *module, const char *id, struct hw_device_t **device) {
    if (!id || strcmp(id, "vk0") != 0 || !device) {
        return -22; // -EINVAL
    }
    g_opened_as_hal = true;
    init_real_vulkan();
    g_shim_hal_device.common.module = (struct hw_module_t*)module;
    *device = &g_shim_hal_device.common;
    LOGI("[+] Android HAL HMI opened device 'vk0' (Mali-Adreno Vulkan Driver 24.2.0)");
    return 0;
}

static struct hw_module_methods_t g_shim_hal_methods = {
    .open = shim_hal_open
};

__attribute__((visibility("default"))) hwvulkan_module_t HMI = {
    .common = {
        .tag = 0x48574d54, // HARDWARE_MODULE_TAG ('HWMT')
        .module_api_version = 0x0100,
        .hal_api_version = 0x0100,
        .id = "vulkan",
        .name = "Qualcomm Adreno 750 / Mali-G615 Vulkan Driver",
        .author = "OpenDriver Mali Team",
        .methods = &g_shim_hal_methods,
        .dso = NULL,
        .reserved = {0}
    }
};
