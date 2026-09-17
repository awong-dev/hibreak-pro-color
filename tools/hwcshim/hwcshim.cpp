// hwcshim -- HWC2 interposer for Bigme's hwcomposer.mt6877.so.
//
// THE FIX (2026-09-17): on a GSI the HWC pre-converts the RGBA client target
// to gray8 on the CPU (OverlayEngine::processImage -> convertToGray) and
// commits that buffer, but the kernel's DRM_IOCTL_EINK_UPDATE path reads the
// fd as 4 bytes/pixel and converts to gray itself. Result: every 4 gray pixels
// collapse into one -> the frame tiled 4x. With commit_use_ct (default on) the
// commit's fd is swapped for the RGBA client target and the panel is correct.
//
// Installed as /vendor/lib64/hw/hwcomposer.mtk_common.so. libhardware tries
// `hwcomposer.<ro.hardware.hwcomposer>.so` (= mtk_common) before
// `hwcomposer.<ro.hardware>.so` (= mt6877), so this module wins without the
// original being renamed. It dlopens the real HWC, patches getFunction on the
// hwc2_device it returns, and hands back that same device -- so every real
// entry point keeps receiving the device pointer it expects.
//
// Two layers of visibility:
//
//   IN  -- what SurfaceFlinger hands the HWC over HWC2: layers, composition
//          types, the client target and its gralloc geometry/format.
//   OUT -- what the HWC hands the kernel. The real HWC calls its own exported
//          functions and libdrm/libc through the PLT (it is not linked
//          -Bsymbolic), so we rewrite its GOT slots for:
//            xrz::EffectHandler::convertToGray / imageSmoothing
//            OverlayEngine::convertLayerToGray / processImage
//            mtk_commit(drm_eink_update)       -> DRM_IOCTL_EINK_UPDATE
//            drmModeAddFB2WithModifiers / drmModeAtomicCommit / drmModeSetCrtc
//            ioctl
//
// Struct offsets marked BUILD-SPECIFIC were recovered by disassembling the
// HWC with BuildID 8a5cbea66efcb94a1a6cfdb53a239598 (firmware
// Bigme_HiBreak_V1.0_20251125). They are only used when the loaded library's
// BuildID matches; otherwise those peeks are skipped and only ABI-defined
// hooks stay active.
//
// Runtime knobs (all readable by the composer domain: `vendor.debug.hwc.`
// maps to vendor_mtk_hwc_debug_log_prop; set them from a root shell, then
// `pkill -f composer@2.3` -- init respawns it and SF reconnects):
//
//   vendor.debug.hwc.shim.log           0 quiet, 1 normal (default), 2 verbose
//   vendor.debug.hwc.shim.refresh_mode  -1 never call setLayerRefreshMode
//                                       (default); otherwise the EinkRefreshMode
//                                       to apply to every layer (178 = NORMAL)
//   vendor.debug.hwc.shim.preview       N -> ASCII-preview the next N gray
//                                       conversions and eink commits
//   vendor.debug.hwc.shim.disable       1 -> pure passthrough, no hooks
//   vendor.debug.hwc.shim.commit_stride  N  -> rewrite drm_eink_update.stride before the ioctl (0 = leave)
//   vendor.debug.hwc.shim.commit_format  N  -> rewrite .format (-1 = leave)
//   vendor.debug.hwc.shim.commit_mode    N  -> rewrite .mode   (-1 = leave)
//   vendor.debug.hwc.shim.commit_height  N  -> rewrite .height (0 = leave)
//   vendor.debug.hwc.shim.commit_use_ct  1 (DEFAULT; 0 = tiled baseline) -> commit the RGBA client target itself
//                                          instead of the HWC's CPU-converted gray8
//                                          buffer. The kernel (eink_pre_process ->
//                                          0x48c3f8c4) reads the fd at 4 B/px, pitch
//                                          = width*4, and runs its own rgb->gray; the
//                                          gray8 buffer is what produces the 4:1 tiling.
//   NOTE: `stride`/`height` in drm_eink_update are really the source WIDTH/HEIGHT
//   (the kernel clamps the update rect to x,y,x+w-1,y+h-1); +0x30/+0x34 are x/y.
//
// No STL, no libc++: the only DT_NEEDED are liblog, libdl, libc.

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <inttypes.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#define TAG "hwcshim"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGV(...) do { if (g_log >= 2) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__); } while (0)

static const char* REAL_HWC = "/vendor/lib64/hw/hwcomposer.mt6877.so";
static const char* REAL_HWC_BASENAME = "hwcomposer.mt6877.so";
static const uint8_t KNOWN_BUILD_ID[16] = {0x8a,0x5c,0xbe,0xa6,0x6e,0xfc,0xb9,0x4a,
                                           0x1a,0x6c,0xfd,0xb5,0x3a,0x23,0x95,0x98};

// ---------------------------------------------------------------- libhardware
// Minimal copies of hardware/libhardware/include/hardware/{hardware,hwcomposer2}.h
// (LP64 layout). The NDK does not ship them.

struct hw_module_t;
struct hw_device_t;
struct hw_module_methods_t {
    int (*open)(const hw_module_t*, const char*, hw_device_t**);
};
struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char* id;
    const char* name;
    const char* author;
    hw_module_methods_t* methods;
    void* dso;
    uint64_t reserved[32 - 7];
};
struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    hw_module_t* module;
    uint64_t reserved[12];
    int (*close)(hw_device_t*);
};
#define MAKE_TAG_CONSTANT(A,B,C,D) (((A) << 24) | ((B) << 16) | ((C) << 8) | (D))
#define HARDWARE_MODULE_TAG MAKE_TAG_CONSTANT('H','W','M','T')

typedef void (*hwc2_function_pointer_t)();
struct hwc2_device_t {
    hw_device_t common;
    void (*getCapabilities)(hwc2_device_t*, uint32_t* outCount, int32_t* outCapabilities);
    hwc2_function_pointer_t (*getFunction)(hwc2_device_t*, int32_t descriptor);
};

typedef uint64_t hwc2_display_t;
typedef uint64_t hwc2_layer_t;
struct native_handle_t { int version; int numFds; int numInts; int data[0]; };
typedef const native_handle_t* buffer_handle_t;
struct hwc_rect_t { int32_t left, top, right, bottom; };
struct hwc_frect_t { float left, top, right, bottom; };
struct hwc_region_t { size_t numRects; const hwc_rect_t* rects; };

enum {
    FN_ACCEPT_DISPLAY_CHANGES = 1, FN_CREATE_LAYER = 2, FN_DESTROY_LAYER = 4,
    FN_DUMP = 6, FN_GET_CHANGED_COMPOSITION_TYPES = 8, FN_GET_CLIENT_TARGET_SUPPORT = 9,
    FN_GET_DISPLAY_REQUESTS = 14, FN_PRESENT_DISPLAY = 20, FN_REGISTER_CALLBACK = 21,
    FN_SET_CLIENT_TARGET = 23, FN_SET_LAYER_BLEND_MODE = 27, FN_SET_LAYER_BUFFER = 28,
    FN_SET_LAYER_COMPOSITION_TYPE = 30, FN_SET_LAYER_DATASPACE = 31,
    FN_SET_LAYER_DISPLAY_FRAME = 32, FN_SET_LAYER_PLANE_ALPHA = 33,
    FN_SET_LAYER_SOURCE_CROP = 35, FN_SET_LAYER_TRANSFORM = 37, FN_SET_LAYER_Z_ORDER = 39,
    FN_SET_POWER_MODE = 41, FN_VALIDATE_DISPLAY = 43,
    // Bigme extension. Recovered from the vendor composer@2.3-service: its
    // initDispatch requests descriptor 71 into the slot used by
    // "HwcHal setLayerRefreshMode mode=%x"; the HWC side is
    // HWCMediator::layerStateSetRefreshMode(hwc2_device*, ulong, ulong, int).
    FN_BIGME_SET_LAYER_REFRESH_MODE = 71,
    FN_MAX_PROBE = 128,
};

typedef int32_t (*PFN_acceptDisplayChanges)(hwc2_device_t*, hwc2_display_t);
typedef int32_t (*PFN_createLayer)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t*);
typedef int32_t (*PFN_destroyLayer)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t);
typedef void    (*PFN_dump)(hwc2_device_t*, uint32_t* outSize, char* outBuffer);
typedef int32_t (*PFN_getChangedCompositionTypes)(hwc2_device_t*, hwc2_display_t, uint32_t*, hwc2_layer_t*, int32_t*);
typedef int32_t (*PFN_getClientTargetSupport)(hwc2_device_t*, hwc2_display_t, uint32_t, uint32_t, int32_t, int32_t);
typedef int32_t (*PFN_getDisplayRequests)(hwc2_device_t*, hwc2_display_t, int32_t*, uint32_t*, hwc2_layer_t*, int32_t*);
typedef int32_t (*PFN_presentDisplay)(hwc2_device_t*, hwc2_display_t, int32_t* outFence);
typedef int32_t (*PFN_setClientTarget)(hwc2_device_t*, hwc2_display_t, buffer_handle_t, int32_t, int32_t, hwc_region_t);
typedef int32_t (*PFN_setLayerBlendMode)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t);
typedef int32_t (*PFN_setLayerBuffer)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, buffer_handle_t, int32_t);
typedef int32_t (*PFN_setLayerCompositionType)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t);
typedef int32_t (*PFN_setLayerDataspace)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t);
typedef int32_t (*PFN_setLayerDisplayFrame)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, hwc_rect_t);
typedef int32_t (*PFN_setLayerPlaneAlpha)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, float);
typedef int32_t (*PFN_setLayerSourceCrop)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, hwc_frect_t);
typedef int32_t (*PFN_setLayerTransform)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t);
typedef int32_t (*PFN_setLayerZOrder)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, uint32_t);
typedef int32_t (*PFN_setPowerMode)(hwc2_device_t*, hwc2_display_t, int32_t);
typedef int32_t (*PFN_validateDisplay)(hwc2_device_t*, hwc2_display_t, uint32_t*, uint32_t*);
typedef int32_t (*PFN_bigmeSetLayerRefreshMode)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t);

// ---------------------------------------------------------------- state

static void* g_real_handle;
static hw_module_t* g_real_module;
static hwc2_device_t* g_dev;
static hwc2_function_pointer_t (*g_real_getFunction)(hwc2_device_t*, int32_t);
static hwc2_function_pointer_t g_real_fn[FN_MAX_PROBE];
static bool g_probed;
static bool g_build_id_ok;      // gate for BUILD-SPECIFIC struct peeks
static int  g_log = 1;
static bool g_disabled;
static int  g_refresh_mode = -1;
static int  g_refresh_mode_applied = -1;
static int  g_preview_budget;   // decremented per preview; refilled from prop
static int  g_commit_stride, g_commit_height;
static int  g_commit_format = -1, g_commit_mode = -1;
static int  g_commit_use_ct = 1;
static int  g_last_ct_fd = -1;      // OverlayPortParam+0x78 seen at processImage/convertLayerToGray IN

static int (*real_gralloc_extra_query)(buffer_handle_t, int, void*);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

struct BufInfo {
    bool valid;
    int32_t w, h, stride, vstride, alloc_size, format, usage, q17, ion_fd;
    uint32_t sf_info[8];
};

struct LayerState {
    bool used;
    hwc2_layer_t id;
    hwc2_display_t display;
    buffer_handle_t buffer;
    BufInfo buf;
    int32_t comp_requested, comp_changed_to;
    int32_t dataspace, transform, blend;
    float alpha;
    uint32_t z;
    hwc_rect_t frame;
    hwc_frect_t crop;
    int32_t refresh_mode;   // last value we pushed via descriptor 71
    uint32_t buffer_changes;
};
#define MAX_LAYERS 96
static LayerState g_layers[MAX_LAYERS];

struct Counters {
    uint64_t validate, present, set_client_target, create_layer, destroy_layer;
    uint64_t eink_update, eink_vsync, eink_other;
    uint64_t convert_to_gray, image_smoothing, convert_layer_to_gray, process_image;
    uint64_t addfb2, atomic_commit, set_crtc;
    uint64_t last_present_ns, present_max_us;
};
static Counters g_ct;

static buffer_handle_t g_ct_handle;
static BufInfo g_ct_buf;
static int32_t g_ct_dataspace;

// The last eink commit seen, kept for `dumpsys SurfaceFlinger` (our DUMP hook).
struct drm_eink_update {          // 68 bytes; DRM_IOWR(0x40 + 0x8f, 0x44) = 0xC04464CF
    uint32_t mode;
    int32_t  left, top, right, bottom;   // inclusive
    uint32_t dither;
    uint32_t rsvd0[4];
    int32_t  fd;
    uint32_t format;                     // 0 => HWC already produced gray8 in place
    uint32_t rsvd1[2];                   // all fields are 4-byte aligned in the HWC (68-byte copy)
    uint32_t stride;                     // pixels
    uint32_t height;
    uint32_t rsvd2;
};
static_assert(sizeof(drm_eink_update) == 0x44, "drm_eink_update must be 68 bytes");
#define DRM_IOCTL_EINK_UPDATE      0xC04464CFu
#define DRM_IOCTL_EINK_WAIT_VSYNC  0xC00464D1u
static drm_eink_update g_last_update;
static uint64_t g_last_update_seq;

// ---------------------------------------------------------------- helpers

static uint64_t now_ns() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int prop_int(const char* name, int def) {
    char v[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, v) <= 0 || !v[0]) return def;
    return (int)strtol(v, nullptr, 0);
}

static void refresh_knobs() {
    g_log = prop_int("vendor.debug.hwc.shim.log", 1);
    g_refresh_mode = prop_int("vendor.debug.hwc.shim.refresh_mode", -1);
    g_commit_stride = prop_int("vendor.debug.hwc.shim.commit_stride", 0);
    g_commit_height = prop_int("vendor.debug.hwc.shim.commit_height", 0);
    g_commit_format = prop_int("vendor.debug.hwc.shim.commit_format", -1);
    g_commit_mode   = prop_int("vendor.debug.hwc.shim.commit_mode", -1);
    g_commit_use_ct = prop_int("vendor.debug.hwc.shim.commit_use_ct", 1);
    int p = prop_int("vendor.debug.hwc.shim.preview", 0);
    if (p > 0) {
        g_preview_budget = p;
        __system_property_set("vendor.debug.hwc.shim.preview", "0");  // one-shot; may be denied, harmless
    }
}

// Log the first `burst` events, then every `every`th.
struct Limiter { uint64_t n; uint32_t burst, every; };
static bool lim(Limiter& l) {
    uint64_t n = __atomic_fetch_add(&l.n, 1, __ATOMIC_RELAXED);
    if (g_log >= 2) return true;
    return n < l.burst || (n % l.every) == 0;
}

static const char* hal_format_name(int32_t f) {
    switch (f) {
        case 1: return "RGBA_8888"; case 2: return "RGBX_8888"; case 3: return "RGB_888";
        case 4: return "RGB_565"; case 5: return "BGRA_8888"; case 0x16: return "RGBA_FP16";
        case 0x2b: return "RGBA_1010102"; case 0x23: return "YCbCr_420_888";
        case 0x11: return "YCrCb_420_SP"; case 0x14: return "YCbCr_422_I";
        case 0x35: return "BLOB"; case 0x22: return "IMPLEMENTATION_DEFINED";
        case 0x38: return "R_8"; case 0x36: return "Y8"; case 0x20203859: return "Y8(fourcc)";
        default: return "?";
    }
}
static const char* comp_name(int32_t t) {
    switch (t) { case 0: return "INVALID"; case 1: return "CLIENT"; case 2: return "DEVICE";
                 case 3: return "SOLID"; case 4: return "CURSOR"; case 5: return "SIDEBAND";
                 default: return "?"; }
}
static void fourcc_str(uint32_t f, char out[5]) {
    for (int i = 0; i < 4; i++) { char c = (char)((f >> (8*i)) & 0xff); out[i] = (c >= 32 && c < 127) ? c : '.'; }
    out[4] = 0;
}

// gralloc_extra_query ids. Recovered from the HWC's getPrivateHandle
// (0xade08): it queries 10,11,12,13,15,14,100,17 in that order and defaults
// 13 (vertical stride) to 11 (height) when zero. 16 = usage and 1 = ion fd are
// inferred from MediaTek's public gralloc_extra.h and are labelled as such.
static void query_buf(buffer_handle_t h, BufInfo* b) {
    memset(b, 0, sizeof(*b));
    if (!h || !real_gralloc_extra_query) return;
    int32_t v;
#define Q(id, field) do { v = 0; if (real_gralloc_extra_query(h, id, &v) == 0) b->field = v; } while (0)
    Q(10, w); Q(11, h); Q(12, stride); Q(13, vstride); Q(14, alloc_size); Q(15, format);
    Q(16, usage); Q(17, q17); Q(1, ion_fd);
#undef Q
    uint32_t big[1024]; memset(big, 0, sizeof(big));
    if (real_gralloc_extra_query(h, 100, big) == 0) memcpy(b->sf_info, big, sizeof(b->sf_info));
    b->valid = true;
}

static void log_handle_raw(const char* what, buffer_handle_t h) {
    if (!h) { LOGI("%s: handle=NULL", what); return; }
    char line[1024]; int n = snprintf(line, sizeof(line), "%s: handle=%p ver=%d fds=%d ints=%d fd[",
                                      what, h, h->version, h->numFds, h->numInts);
    for (int i = 0; i < h->numFds && n < (int)sizeof(line) - 16; i++)
        n += snprintf(line + n, sizeof(line) - n, "%s%d", i ? "," : "", h->data[i]);
    n += snprintf(line + n, sizeof(line) - n, "] ints[");
    for (int i = 0; i < h->numInts && n < (int)sizeof(line) - 16; i++)
        n += snprintf(line + n, sizeof(line) - n, "%s%x", i ? "," : "", (unsigned)h->data[h->numFds + i]);
    snprintf(line + n, sizeof(line) - n, "]");
    LOGI("%s", line);
}

static void log_bufinfo(const char* what, const BufInfo* b) {
    if (!b->valid) { LOGI("%s: (no gralloc_extra)", what); return; }
    LOGI("%s: %dx%d stride=%d(px) vstride=%d alloc=%d fmt=0x%x(%s) usage=0x%x q17=0x%x ion_fd=%d "
         "bytes/row@32bpp=%d @8bpp=%d @2bpp=%d",
         what, b->w, b->h, b->stride, b->vstride, b->alloc_size, b->format, hal_format_name(b->format),
         b->usage, b->q17, b->ion_fd, b->stride * 4, b->stride, b->stride / 4);
    LOGI("%s: sf_info[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x", what,
         b->sf_info[0], b->sf_info[1], b->sf_info[2], b->sf_info[3],
         b->sf_info[4], b->sf_info[5], b->sf_info[6], b->sf_info[7]);
}

static bool bufinfo_differs(const BufInfo* a, const BufInfo* b) {
    return a->w != b->w || a->h != b->h || a->stride != b->stride || a->format != b->format ||
           a->usage != b->usage || a->alloc_size != b->alloc_size;
}

// ---------------------------------------------------------------- previews
//
// The diagnostic that answers the pitch question directly: render a buffer as
// ASCII at candidate pitches. The pitch that produces a coherent picture is
// the pitch the content was written at. `pitch_score` is the same idea as a
// number: mean |row[r] - row[r+1]| is small at the true pitch and large at a
// wrong one.

static double pitch_score(const uint8_t* base, size_t avail, uint32_t pitch, uint32_t width, uint32_t rows) {
    if (pitch == 0 || width == 0 || rows < 2) return -1;
    uint64_t acc = 0, cnt = 0;
    for (uint32_t r = 0; r + 1 < rows; r += 7) {
        size_t o0 = (size_t)r * pitch, o1 = (size_t)(r + 1) * pitch;
        if (o1 + width > avail) break;
        for (uint32_t x = 0; x < width; x += 13) { int d = (int)base[o0 + x] - (int)base[o1 + x]; acc += d < 0 ? -d : d; cnt++; }
    }
    return cnt ? (double)acc / cnt : -1;
}

static void preview_gray(const char* what, const uint8_t* base, size_t avail, uint32_t pitch, uint32_t width, uint32_t height) {
    static const char ramp[] = "@%#*+=-:. ";   // dark -> light
    const uint32_t cols = 82, rows = 41;
    uint32_t sx = width / cols ? width / cols : 1, sy = height / rows ? height / rows : 1;
    LOGI("%s: preview pitch=%u %ux%u (each char = %ux%u px)", what, pitch, width, height, sx, sy);
    char line[cols + 1];
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t y = r * sy; if (y >= height) break;
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t x = c * sx; size_t o = (size_t)y * pitch + x;
            line[c] = (o < avail) ? ramp[base[o] * 9 / 255] : '?';
        }
        line[cols] = 0;
        LOGI("|%s|", line);
    }
}

static void preview_rgba_lum(const char* what, const uint8_t* base, size_t avail, uint32_t pitch_bytes, uint32_t width, uint32_t height) {
    static const char ramp[] = "@%#*+=-:. ";
    const uint32_t cols = 82, rows = 41;
    uint32_t sx = width / cols ? width / cols : 1, sy = height / rows ? height / rows : 1;
    LOGI("%s: RGBA luminance preview pitch=%uB %ux%u", what, pitch_bytes, width, height);
    char line[cols + 1];
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t y = r * sy; if (y >= height) break;
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t x = c * sx; size_t o = (size_t)y * pitch_bytes + (size_t)x * 4;
            if (o + 3 >= avail) { line[c] = '?'; continue; }
            unsigned l = (base[o] * 77 + base[o+1] * 150 + base[o+2] * 29) >> 8;
            line[c] = ramp[l * 9 / 255];
        }
        line[cols] = 0;
        LOGI("|%s|", line);
    }
}

// Byte-pattern statistics over the first `bytes`: fraction of 0xFF at each
// residue mod 4. In-place RGBA->gray8 leaves alpha (=0xFF) at every 4th byte
// in the untouched tail; a fully-converted gray8 region shows no such stripe.
static void stripe_stats(const char* what, const uint8_t* base, size_t bytes) {
    uint64_t ff[4] = {0,0,0,0}, n[4] = {0,0,0,0};
    for (size_t i = 0; i < bytes; i += 1) { n[i & 3]++; if (base[i] == 0xff) ff[i & 3]++; }
    LOGI("%s: 0xFF fraction by byte%%4 over %zu B: %.2f %.2f %.2f %.2f", what, bytes,
         n[0] ? (double)ff[0]/n[0] : 0, n[1] ? (double)ff[1]/n[1] : 0,
         n[2] ? (double)ff[2]/n[2] : 0, n[3] ? (double)ff[3]/n[3] : 0);
}

static bool take_preview() {
    if (g_preview_budget <= 0) return false;
    g_preview_budget--;
    return true;
}

// ---------------------------------------------------------------- GOT hooking

struct HookSpec { const char* sym; void* hook; void** orig; int hits; };

struct GotCtx { HookSpec* specs; size_t n; int total; };

static int got_cb(struct dl_phdr_info* info, size_t, void* data) {
    GotCtx* ctx = (GotCtx*)data;
    if (!info->dlpi_name || !strstr(info->dlpi_name, REAL_HWC_BASENAME)) return 0;
    ElfW(Addr) bias = info->dlpi_addr;
    const ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn)*)(bias + info->dlpi_phdr[i].p_vaddr);
    if (!dyn) { LOGE("got: no PT_DYNAMIC in %s", info->dlpi_name); return 1; }

    const ElfW(Sym)* symtab = nullptr; const char* strtab = nullptr;
    const ElfW(Rela)* jmprel = nullptr; size_t jmprelsz = 0;
    const ElfW(Rela)* rela = nullptr; size_t relasz = 0;
    auto fix = [&](ElfW(Addr) p) -> ElfW(Addr) { return p < bias ? p + bias : p; };
    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB: symtab = (const ElfW(Sym)*)fix(d->d_un.d_ptr); break;
            case DT_STRTAB: strtab = (const char*)fix(d->d_un.d_ptr); break;
            case DT_JMPREL: jmprel = (const ElfW(Rela)*)fix(d->d_un.d_ptr); break;
            case DT_PLTRELSZ: jmprelsz = d->d_un.d_val; break;
            case DT_RELA: rela = (const ElfW(Rela)*)fix(d->d_un.d_ptr); break;
            case DT_RELASZ: relasz = d->d_un.d_val; break;
        }
    }
    if (!symtab || !strtab) { LOGE("got: no symtab/strtab"); return 1; }

    auto scan = [&](const ElfW(Rela)* r, size_t sz) {
        if (!r) return;
        for (size_t i = 0; i < sz / sizeof(ElfW(Rela)); i++) {
            uint32_t type = ELF64_R_TYPE(r[i].r_info);
            if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT) continue;
            const char* name = strtab + symtab[ELF64_R_SYM(r[i].r_info)].st_name;
            for (size_t s = 0; s < ctx->n; s++) {
                if (strcmp(name, ctx->specs[s].sym) != 0) continue;
                void** slot = (void**)(bias + r[i].r_offset);
                uintptr_t page = (uintptr_t)slot & ~(uintptr_t)(getpagesize() - 1);
                if (mprotect((void*)page, getpagesize(), PROT_READ | PROT_WRITE) != 0) {
                    LOGE("got: mprotect %p failed: %s", slot, strerror(errno)); continue;
                }
                if (!*ctx->specs[s].orig) *ctx->specs[s].orig = *slot;   // dlsym failed: fall back to slot
                *slot = ctx->specs[s].hook;
                ctx->specs[s].hits++; ctx->total++;
                LOGV("got: %s slot %p -> %p (orig %p)", name, slot, ctx->specs[s].hook, *ctx->specs[s].orig);
            }
        }
    };
    scan(jmprel, jmprelsz);
    scan(rela, relasz);
    return 1;
}

// BuildID of the loaded real HWC, via its PT_NOTE.
static int buildid_cb(struct dl_phdr_info* info, size_t, void* data) {
    if (!info->dlpi_name || !strstr(info->dlpi_name, REAL_HWC_BASENAME)) return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type != PT_NOTE) continue;
        const uint8_t* p = (const uint8_t*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
        const uint8_t* end = p + info->dlpi_phdr[i].p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const ElfW(Nhdr)* nh = (const ElfW(Nhdr)*)p;
            const uint8_t* name = p + sizeof(*nh);
            const uint8_t* desc = name + ((nh->n_namesz + 3) & ~3u);
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && memcmp(name, "GNU", 4) == 0) {
                size_t n = nh->n_descsz < 20 ? nh->n_descsz : 20;
                memcpy(data, desc, n);
                ((uint8_t*)data)[20] = (uint8_t)nh->n_descsz;
                return 1;
            }
            p = desc + ((nh->n_descsz + 3) & ~3u);
        }
    }
    return 0;
}

// ---------------------------------------------------------------- OUT hooks

struct Rect { int32_t left, top, right, bottom; };   // android::Rect

// xrz::EffectHandler::convertToGray(void* src, void* dst, android::Rect, int pitchBytes, int)
// Static member; the HWC calls it with dst == src (in-place) and
// pitch = 4 * stride (see OverlayEngine::convertLayerToGray at 0xa56b0).
static int64_t (*real_convertToGray)(void*, void*, Rect, int, int);
static int64_t hook_convertToGray(void* src, void* dst, Rect r, int pitch, int flag) {
    static Limiter L = {0, 30, 300};
    uint32_t w = (uint32_t)(r.right - r.left), h = (uint32_t)(r.bottom - r.top);
    bool pv = take_preview();
    bool say = lim(L) || pv;
    if (say) LOGI("convertToGray IN : src=%p dst=%p rect=[%d,%d,%d,%d] (%ux%u) pitch=%dB flag=%d in_place=%d",
                  src, dst, r.left, r.top, r.right, r.bottom, w, h, pitch, flag, src == dst);
    size_t avail = (size_t)pitch * h;
    if (pv && src && pitch > 0) {
        preview_rgba_lum("convertToGray src", (const uint8_t*)src, avail, (uint32_t)pitch, w, h);
        stripe_stats("convertToGray src", (const uint8_t*)src, avail);
    }
    uint64_t t0 = now_ns();
    int64_t ret = real_convertToGray(src, dst, r, pitch, flag);
    uint64_t dt = now_ns() - t0;
    __atomic_fetch_add(&g_ct.convert_to_gray, 1, __ATOMIC_RELAXED);
    if (say && dst && pitch > 0) {
        const uint8_t* d = (const uint8_t*)dst;
        double s_w = pitch_score(d, avail, w, w, h);
        double s_pitch = pitch_score(d, avail, (uint32_t)pitch, w, h);
        double s_stride = pitch_score(d, avail, (uint32_t)pitch / 4, w, h);
        LOGI("convertToGray OUT: ret=%" PRId64 " %" PRIu64 "us  dst[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x "
             "row-continuity score (lower=coherent): pitch=w(%u):%.1f pitch/4(%u):%.1f pitch(%u):%.1f",
             ret, dt / 1000, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
             w, s_w, (unsigned)pitch / 4, s_stride, (unsigned)pitch, s_pitch);
        if (pv) {
            stripe_stats("convertToGray dst first quarter", d, avail / 4);
            stripe_stats("convertToGray dst last quarter", d + avail - avail / 4, avail / 4);
            preview_gray("convertToGray dst @pitch/4", d, avail, (uint32_t)pitch / 4, w, h);
            preview_gray("convertToGray dst @pitch", d, avail, (uint32_t)pitch, w, h);
        }
    }
    return ret;
}

// xrz::EffectHandler::imageSmoothing(void*, void*, void*, void*, android::Rect, int, int)
static int64_t (*real_imageSmoothing)(void*, void*, void*, void*, Rect, int, int);
static int64_t hook_imageSmoothing(void* a, void* b, void* c, void* d, Rect r, int x, int y) {
    static Limiter L = {0, 20, 300};
    if (lim(L)) LOGI("imageSmoothing: %p %p %p %p rect=[%d,%d,%d,%d] %d %d", a, b, c, d, r.left, r.top, r.right, r.bottom, x, y);
    int64_t ret = real_imageSmoothing(a, b, c, d, r, x, y);
    __atomic_fetch_add(&g_ct.image_smoothing, 1, __ATOMIC_RELAXED);
    return ret;
}

// BUILD-SPECIFIC peeks into OverlayPortParam / CommitInfo (see file header).
static void log_port_and_commit(const char* what, const uint8_t* param, const uint8_t* ci) {
    if (!g_build_id_ok || !param || !ci) return;
    const uint8_t* obj = *(const uint8_t* const*)(param + 0x178);
    int32_t src_fd = *(const int32_t*)(param + 0x78);
    int32_t ow = 0, oh = 0, os = 0;
    if (obj) { ow = *(const int32_t*)(obj + 0x48); oh = *(const int32_t*)(obj + 0x4c); os = *(const int32_t*)(obj + 0x50); }
    LOGI("%s: port.src_fd=%d port.obj=%p {w=%d h=%d stride=%d}  ci.mode=0x%x ci.rect=[%d,%d,%d,%d] ci.dither=%d "
         "ci.format=%d ci.flag5c=%d ci.antiflicker=%d",
         what, src_fd, obj, ow, oh, os,
         *(const uint32_t*)(ci + 0x08), *(const int32_t*)(ci + 0x0c), *(const int32_t*)(ci + 0x10),
         *(const int32_t*)(ci + 0x14), *(const int32_t*)(ci + 0x18), *(const int32_t*)(ci + 0x3c),
         *(const int32_t*)(ci + 0x54), ci[0x5c], ci[0x8c]);
}

static int64_t (*real_convertLayerToGray)(void*, void*, void*);
static void capture_ct_fd(const void* param) {
    if (g_build_id_ok && param) g_last_ct_fd = *(const int32_t*)((const uint8_t*)param + 0x78);
}

static int64_t hook_convertLayerToGray(void* self, void* param, void* ci) {
    static Limiter L = {0, 30, 300};
    capture_ct_fd(param);
    bool say = lim(L);
    if (say) log_port_and_commit("convertLayerToGray IN ", (const uint8_t*)param, (const uint8_t*)ci);
    int64_t ret = real_convertLayerToGray(self, param, ci);
    __atomic_fetch_add(&g_ct.convert_layer_to_gray, 1, __ATOMIC_RELAXED);
    if (say) { LOGI("convertLayerToGray OUT: ret=%" PRId64, ret); log_port_and_commit("convertLayerToGray OUT", (const uint8_t*)param, (const uint8_t*)ci); }
    return ret;
}

static int64_t (*real_processImage)(void*, void*, void*);
static int64_t hook_processImage(void* self, void* param, void* ci) {
    static Limiter L = {0, 30, 300};
    capture_ct_fd(param);
    bool say = lim(L);
    if (say) log_port_and_commit("processImage IN ", (const uint8_t*)param, (const uint8_t*)ci);
    int64_t ret = real_processImage(self, param, ci);
    __atomic_fetch_add(&g_ct.process_image, 1, __ATOMIC_RELAXED);
    if (say) { LOGI("processImage OUT: ret=%" PRId64, ret); log_port_and_commit("processImage OUT", (const uint8_t*)param, (const uint8_t*)ci); }
    return ret;
}

static void log_eink_update(const char* what, const drm_eink_update* u, int ret, uint64_t dt_us) {
    LOGI("%s: mode=0x%x area=[%d,%d,%d,%d] dither=%u fd=%d format=%u stride=%u height=%u rsvd0=%x,%x,%x,%x rsvd1=%x,%x rsvd2=%x ret=%d %" PRIu64 "us",
         what, u->mode, u->left, u->top, u->right, u->bottom, u->dither, u->fd, u->format, u->stride, u->height,
         u->rsvd0[0], u->rsvd0[1], u->rsvd0[2], u->rsvd0[3], u->rsvd1[0], u->rsvd1[1], u->rsvd2, ret, dt_us);
}

// Map the committed dma-buf and look at what the kernel is about to read.
static void preview_commit_buffer(const drm_eink_update* u) {
    if (u->fd < 0 || u->stride == 0 || u->height == 0) return;
    uint32_t w = (uint32_t)(u->right - u->left + 1);
    size_t want = (size_t)u->stride * u->height * 4;
    void* m = mmap(nullptr, want, PROT_READ, MAP_SHARED, u->fd, 0);
    if (m == MAP_FAILED) { want = (size_t)u->stride * u->height; m = mmap(nullptr, want, PROT_READ, MAP_SHARED, u->fd, 0); }
    if (m == MAP_FAILED) { LOGW("commit preview: mmap fd=%d failed: %s", u->fd, strerror(errno)); return; }
    const uint8_t* b = (const uint8_t*)m;
    LOGI("commit preview: mapped %zu B of fd=%d; row-continuity: @stride(%u):%.1f @stride*4(%u):%.1f @w(%u):%.1f",
         want, u->fd, u->stride, pitch_score(b, want, u->stride, w, u->height),
         u->stride * 4, pitch_score(b, want, u->stride * 4, w, u->height),
         w, pitch_score(b, want, w, w, u->height));
    stripe_stats("commit buffer head", b, want / 4);
    preview_gray("commit buffer @stride", b, want, u->stride, w, u->height);
    if (want >= (size_t)u->stride * 4 * u->height)
        preview_gray("commit buffer @stride*4", b, want, u->stride * 4, w, u->height);
    munmap(m, want);
}

// mtk_commit(drm_eink_update) -- 68-byte struct, passed by reference per AAPCS64.
static int (*real_mtk_commit)(const drm_eink_update*);
static int hook_mtk_commit(const drm_eink_update* orig) {
    static Limiter L = {0, 40, 120};
    refresh_knobs();
    drm_eink_update mod = *orig; const drm_eink_update* u = orig;
    bool rewritten = false;
    if (g_commit_stride > 0 && mod.stride != (uint32_t)g_commit_stride) { mod.stride = g_commit_stride; rewritten = true; }
    if (g_commit_height > 0 && mod.height != (uint32_t)g_commit_height) { mod.height = g_commit_height; rewritten = true; }
    if (g_commit_format >= 0 && mod.format != (uint32_t)g_commit_format) { mod.format = g_commit_format; rewritten = true; }
    if (g_commit_mode >= 0 && mod.mode != (uint32_t)g_commit_mode) { mod.mode = g_commit_mode; rewritten = true; }
    if (g_commit_use_ct && g_last_ct_fd >= 0 && mod.fd != g_last_ct_fd) { mod.fd = g_last_ct_fd; rewritten = true; }
    if (rewritten) u = &mod;
    bool pv = take_preview();
    if (pv) preview_commit_buffer(u);
    uint64_t t0 = now_ns();
    int ret = real_mtk_commit(u);
    uint64_t dt = (now_ns() - t0) / 1000;
    pthread_mutex_lock(&g_lock); g_last_update = *u; g_last_update_seq++; pthread_mutex_unlock(&g_lock);
    if (lim(L) || pv || rewritten) {
        if (rewritten) log_eink_update("EINK_UPDATE(mtk_commit) ORIGINAL", orig, 0, 0);
        log_eink_update(rewritten ? "EINK_UPDATE(mtk_commit) REWRITTEN" : "EINK_UPDATE(mtk_commit)", u, ret, dt);
    }
    return ret;
}

// ioctl -- catches the eink ioctls whether or not they go through mtk_commit.
static int (*real_ioctl)(int, int, ...);
static int hook_ioctl(int fd, int req, ...) {
    va_list ap; va_start(ap, req); void* arg = va_arg(ap, void*); va_end(ap);
    uint32_t r = (uint32_t)req;
    if (r == DRM_IOCTL_EINK_UPDATE) {
        static Limiter L = {0, 4, 600};
        uint64_t t0 = now_ns();
        int ret = real_ioctl(fd, req, arg);
        __atomic_fetch_add(&g_ct.eink_update, 1, __ATOMIC_RELAXED);
        if (lim(L)) log_eink_update("EINK_UPDATE(ioctl)", (const drm_eink_update*)arg, ret, (now_ns() - t0) / 1000);
        return ret;
    }
    if (r == DRM_IOCTL_EINK_WAIT_VSYNC) {
        int ret = real_ioctl(fd, req, arg);
        __atomic_fetch_add(&g_ct.eink_vsync, 1, __ATOMIC_RELAXED);
        LOGV("EINK_WAIT_VSYNC: id=%d ret=%d", arg ? *(int*)arg : -1, ret);
        return ret;
    }
    // Other MTK/eink private DRM ioctls (nr 0x8f..0x9f) -- count and show once.
    if ((r & 0xff00) == 0x6400 && (r & 0xff) >= 0x8f && (r & 0xff) <= 0x9f) {
        static Limiter L = {0, 16, 1000};
        int ret = real_ioctl(fd, req, arg);
        __atomic_fetch_add(&g_ct.eink_other, 1, __ATOMIC_RELAXED);
        if (lim(L)) LOGI("DRM private ioctl 0x%08x (nr 0x%x size %u) ret=%d", r, r & 0xff, (r >> 16) & 0x3fff, ret);
        return ret;
    }
    return real_ioctl(fd, req, arg);
}

// libdrm
static int (*real_drmModeAddFB2WithModifiers)(int, uint32_t, uint32_t, uint32_t, const uint32_t*, const uint32_t*, const uint32_t*, const uint64_t*, uint32_t*, uint32_t);
static int hook_drmModeAddFB2WithModifiers(int fd, uint32_t w, uint32_t h, uint32_t fmt, const uint32_t* handles,
                                           const uint32_t* pitches, const uint32_t* offsets, const uint64_t* mods,
                                           uint32_t* id, uint32_t flags) {
    static Limiter L = {0, 40, 200};
    int ret = real_drmModeAddFB2WithModifiers(fd, w, h, fmt, handles, pitches, offsets, mods, id, flags);
    __atomic_fetch_add(&g_ct.addfb2, 1, __ATOMIC_RELAXED);
    if (lim(L)) {
        char fc[5]; fourcc_str(fmt, fc);
        LOGI("drmModeAddFB2: %ux%u fmt=%s(0x%08x) handles=[%u,%u] pitches=[%u,%u] offsets=[%u,%u] mod=0x%" PRIx64 " flags=0x%x -> fb_id=%u ret=%d  (pitch/w=%.2f B/px)",
             w, h, fc, fmt, handles ? handles[0] : 0, handles ? handles[1] : 0, pitches ? pitches[0] : 0, pitches ? pitches[1] : 0,
             offsets ? offsets[0] : 0, offsets ? offsets[1] : 0, mods ? mods[0] : 0, flags, id ? *id : 0, ret,
             (pitches && w) ? (double)pitches[0] / w : 0.0);
    }
    return ret;
}

// libdrm's drmModeAtomicReq is private but stable: {cursor, size_items, items*}, item = {obj, prop, u64 value}.
struct AtomicItem { uint32_t object_id, property_id; uint64_t value; };
struct AtomicReq { uint32_t cursor, size_items; AtomicItem* items; };
static int (*real_drmModeAtomicCommit)(int, AtomicReq*, uint32_t, void*);
static int hook_drmModeAtomicCommit(int fd, AtomicReq* req, uint32_t flags, void* ud) {
    static Limiter L = {0, 20, 300};
    int ret = real_drmModeAtomicCommit(fd, req, flags, ud);
    __atomic_fetch_add(&g_ct.atomic_commit, 1, __ATOMIC_RELAXED);
    if (lim(L)) {
        char line[1536]; int n = snprintf(line, sizeof(line), "drmModeAtomicCommit flags=0x%x ret=%d items=%u:", flags, ret, req ? req->cursor : 0);
        for (uint32_t i = 0; req && i < req->cursor && n < (int)sizeof(line) - 48; i++)
            n += snprintf(line + n, sizeof(line) - n, " %u.%u=%" PRIu64, req->items[i].object_id, req->items[i].property_id, req->items[i].value);
        LOGI("%s", line);
    }
    return ret;
}

static int (*real_drmModeSetCrtc)(int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*, int, void*);
static int hook_drmModeSetCrtc(int fd, uint32_t crtc, uint32_t fb, uint32_t x, uint32_t y, uint32_t* conns, int count, void* mode) {
    int ret = real_drmModeSetCrtc(fd, crtc, fb, x, y, conns, count, mode);
    __atomic_fetch_add(&g_ct.set_crtc, 1, __ATOMIC_RELAXED);
    LOGI("drmModeSetCrtc crtc=%u fb=%u pos=%u,%u conns=%d ret=%d", crtc, fb, x, y, count, ret);
    return ret;
}

static void install_out_hooks() {
    real_gralloc_extra_query = (int (*)(buffer_handle_t, int, void*))dlsym(g_real_handle, "gralloc_extra_query");
    if (!real_gralloc_extra_query) LOGW("gralloc_extra_query not resolvable; buffer geometry logging disabled");

    HookSpec specs[] = {
        {"_ZN3xrz13EffectHandler13convertToGrayEPvS1_N7android4RectEii", (void*)hook_convertToGray, (void**)&real_convertToGray, 0},
        {"_ZN3xrz13EffectHandler14imageSmoothingEPvS1_S1_S1_N7android4RectEii", (void*)hook_imageSmoothing, (void**)&real_imageSmoothing, 0},
        {"_ZN13OverlayEngine18convertLayerToGrayEP16OverlayPortParamP10CommitInfo", (void*)hook_convertLayerToGray, (void**)&real_convertLayerToGray, 0},
        {"_ZN13OverlayEngine12processImageEP16OverlayPortParamP10CommitInfo", (void*)hook_processImage, (void**)&real_processImage, 0},
        {"_Z10mtk_commit15drm_eink_update", (void*)hook_mtk_commit, (void**)&real_mtk_commit, 0},
        {"ioctl", (void*)hook_ioctl, (void**)&real_ioctl, 0},
        {"drmModeAddFB2WithModifiers", (void*)hook_drmModeAddFB2WithModifiers, (void**)&real_drmModeAddFB2WithModifiers, 0},
        {"drmModeAtomicCommit", (void*)hook_drmModeAtomicCommit, (void**)&real_drmModeAtomicCommit, 0},
        {"drmModeSetCrtc", (void*)hook_drmModeSetCrtc, (void**)&real_drmModeSetCrtc, 0},
    };
    const size_t n = sizeof(specs) / sizeof(specs[0]);
    // Originals via dlsym on the real handle (searches it and its DT_NEEDED
    // closure), never via the GOT slot, so BIND_NOW vs lazy does not matter.
    for (size_t i = 0; i < n; i++) {
        *specs[i].orig = dlsym(g_real_handle, specs[i].sym);
        if (!*specs[i].orig) LOGW("dlsym(%s) failed: %s", specs[i].sym, dlerror());
    }
    GotCtx ctx = {specs, n, 0};
    dl_iterate_phdr(got_cb, &ctx);
    for (size_t i = 0; i < n; i++)
        LOGI("hook %-70s slots=%d orig=%p", specs[i].sym, specs[i].hits, *specs[i].orig);
    LOGI("GOT hooks installed: %d slots", ctx.total);
}

// ---------------------------------------------------------------- IN hooks (HWC2)

static LayerState* find_layer(hwc2_layer_t id, bool create) {
    for (int i = 0; i < MAX_LAYERS; i++) if (g_layers[i].used && g_layers[i].id == id) return &g_layers[i];
    if (!create) return nullptr;
    for (int i = 0; i < MAX_LAYERS; i++) if (!g_layers[i].used) { memset(&g_layers[i], 0, sizeof(LayerState)); g_layers[i].used = true; g_layers[i].id = id; g_layers[i].refresh_mode = -1; return &g_layers[i]; }
    return nullptr;
}

static LayerState* get_layer(hwc2_display_t display, hwc2_layer_t id) {
    LayerState* l = find_layer(id, true);
    if (l) l->display = display;
    return l;
}

static void push_refresh_mode(hwc2_display_t display, LayerState* l, int mode, const char* why) {
    PFN_bigmeSetLayerRefreshMode fn = (PFN_bigmeSetLayerRefreshMode)g_real_fn[FN_BIGME_SET_LAYER_REFRESH_MODE];
    if (!fn) return;
    int32_t err = fn(g_dev, display, l->id, mode);
    l->refresh_mode = mode;
    LOGI("setLayerRefreshMode(display=%" PRIu64 " layer=%" PRIu64 " mode=%d) [%s] -> %d", display, l->id, mode, why, err);
}

static int32_t hook_createLayer(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t* out) {
    int32_t err = ((PFN_createLayer)g_real_fn[FN_CREATE_LAYER])(dev, display, out);
    __atomic_fetch_add(&g_ct.create_layer, 1, __ATOMIC_RELAXED);
    if (err == 0 && out) {
        pthread_mutex_lock(&g_lock);
        LayerState* l = find_layer(*out, true);
        if (l) { l->display = display; l->comp_changed_to = -1; }
        LOGV("createLayer display=%" PRIu64 " -> layer=%" PRIu64, display, *out);
        if (l && g_refresh_mode >= 0) push_refresh_mode(display, l, g_refresh_mode, "create");
        pthread_mutex_unlock(&g_lock);
    }
    return err;
}

static int32_t hook_destroyLayer(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t layer) {
    pthread_mutex_lock(&g_lock);
    LayerState* l = find_layer(layer, false); if (l) l->used = false;
    pthread_mutex_unlock(&g_lock);
    __atomic_fetch_add(&g_ct.destroy_layer, 1, __ATOMIC_RELAXED);
    LOGV("destroyLayer display=%" PRIu64 " layer=%" PRIu64, display, layer);
    return ((PFN_destroyLayer)g_real_fn[FN_DESTROY_LAYER])(dev, display, layer);
}

static int32_t hook_setLayerBuffer(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t layer, buffer_handle_t buf, int32_t fence) {
    pthread_mutex_lock(&g_lock);
    LayerState* l = get_layer(display, layer);
    if (l) {
        BufInfo bi; query_buf(buf, &bi);
        bool changed = !l->buf.valid || bufinfo_differs(&l->buf, &bi);
        if (changed && buf) {
            char what[64]; snprintf(what, sizeof(what), "layer %" PRIu64 " buffer", layer);
            log_bufinfo(what, &bi);
            if (g_log >= 2 || l->buffer_changes < 2) log_handle_raw(what, buf);
            l->buffer_changes++;
        }
        l->buffer = buf; l->buf = bi;
    }
    pthread_mutex_unlock(&g_lock);
    return ((PFN_setLayerBuffer)g_real_fn[FN_SET_LAYER_BUFFER])(dev, display, layer, buf, fence);
}

static int32_t hook_setLayerCompositionType(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t layer, int32_t type) {
    pthread_mutex_lock(&g_lock);
    LayerState* l = get_layer(display, layer); if (l) l->comp_requested = type;
    pthread_mutex_unlock(&g_lock);
    return ((PFN_setLayerCompositionType)g_real_fn[FN_SET_LAYER_COMPOSITION_TYPE])(dev, display, layer, type);
}
static int32_t hook_setLayerDataspace(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t layer, int32_t ds) {
    pthread_mutex_lock(&g_lock); LayerState* l = get_layer(display, layer); if (l) l->dataspace = ds; pthread_mutex_unlock(&g_lock);
    return ((PFN_setLayerDataspace)g_real_fn[FN_SET_LAYER_DATASPACE])(dev, display, layer, ds);
}
static int32_t hook_setLayerDisplayFrame(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t layer, hwc_rect_t f) {
    pthread_mutex_lock(&g_lock); LayerState* l = get_layer(display, layer); if (l) l->frame = f; pthread_mutex_unlock(&g_lock);
    return ((PFN_setLayerDisplayFrame)g_real_fn[FN_SET_LAYER_DISPLAY_FRAME])(dev, display, layer, f);
}
static int32_t hook_setLayerSourceCrop(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t layer, hwc_frect_t c) {
    pthread_mutex_lock(&g_lock); LayerState* l = get_layer(display, layer); if (l) l->crop = c; pthread_mutex_unlock(&g_lock);
    return ((PFN_setLayerSourceCrop)g_real_fn[FN_SET_LAYER_SOURCE_CROP])(dev, display, layer, c);
}
static int32_t hook_setLayerTransform(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t layer, int32_t t) {
    pthread_mutex_lock(&g_lock); LayerState* l = get_layer(display, layer); if (l) l->transform = t; pthread_mutex_unlock(&g_lock);
    return ((PFN_setLayerTransform)g_real_fn[FN_SET_LAYER_TRANSFORM])(dev, display, layer, t);
}
static int32_t hook_setLayerBlendMode(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t layer, int32_t b) {
    pthread_mutex_lock(&g_lock); LayerState* l = get_layer(display, layer); if (l) l->blend = b; pthread_mutex_unlock(&g_lock);
    return ((PFN_setLayerBlendMode)g_real_fn[FN_SET_LAYER_BLEND_MODE])(dev, display, layer, b);
}
static int32_t hook_setLayerPlaneAlpha(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t layer, float a) {
    pthread_mutex_lock(&g_lock); LayerState* l = get_layer(display, layer); if (l) l->alpha = a; pthread_mutex_unlock(&g_lock);
    return ((PFN_setLayerPlaneAlpha)g_real_fn[FN_SET_LAYER_PLANE_ALPHA])(dev, display, layer, a);
}
static int32_t hook_setLayerZOrder(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t layer, uint32_t z) {
    pthread_mutex_lock(&g_lock); LayerState* l = get_layer(display, layer); if (l) l->z = z; pthread_mutex_unlock(&g_lock);
    return ((PFN_setLayerZOrder)g_real_fn[FN_SET_LAYER_Z_ORDER])(dev, display, layer, z);
}

static int32_t hook_setClientTarget(hwc2_device_t* dev, hwc2_display_t display, buffer_handle_t target, int32_t fence, int32_t dataspace, hwc_region_t damage) {
    static Limiter L = {0, 20, 300};
    BufInfo bi; query_buf(target, &bi);
    pthread_mutex_lock(&g_lock);
    bool changed = target != g_ct_handle || !g_ct_buf.valid || bufinfo_differs(&g_ct_buf, &bi) || dataspace != g_ct_dataspace;
    g_ct_handle = target; g_ct_buf = bi; g_ct_dataspace = dataspace;
    pthread_mutex_unlock(&g_lock);
    __atomic_fetch_add(&g_ct.set_client_target, 1, __ATOMIC_RELAXED);
    bool say = lim(L);
    if (changed || say) {
        LOGI("setClientTarget display=%" PRIu64 " handle=%p fence=%d dataspace=0x%x damage=%zu rects",
             display, target, fence, dataspace, damage.numRects);
        if (damage.numRects && damage.rects)
            LOGI("  damage[0]=[%d,%d,%d,%d]", damage.rects[0].left, damage.rects[0].top, damage.rects[0].right, damage.rects[0].bottom);
        log_bufinfo("client target", &bi);
        if (changed) log_handle_raw("client target", target);
    }
    return ((PFN_setClientTarget)g_real_fn[FN_SET_CLIENT_TARGET])(dev, display, target, fence, dataspace, damage);
}

static int32_t hook_getClientTargetSupport(hwc2_device_t* dev, hwc2_display_t display, uint32_t w, uint32_t h, int32_t format, int32_t dataspace) {
    int32_t err = ((PFN_getClientTargetSupport)g_real_fn[FN_GET_CLIENT_TARGET_SUPPORT])(dev, display, w, h, format, dataspace);
    LOGI("getClientTargetSupport display=%" PRIu64 " %ux%u fmt=0x%x(%s) dataspace=0x%x -> %d", display, w, h, format, hal_format_name(format), dataspace, err);
    return err;
}

static void log_layer_table(hwc2_display_t display, const char* when) {
    pthread_mutex_lock(&g_lock);
    int count = 0;
    for (int i = 0; i < MAX_LAYERS; i++) if (g_layers[i].used && g_layers[i].display == display) count++;
    LOGI("--- %s: display %" PRIu64 " %d layers; client target %p %dx%d s=%d fmt=0x%x(%s) ds=0x%x", when, display, count,
         g_ct_handle, g_ct_buf.w, g_ct_buf.h, g_ct_buf.stride, g_ct_buf.format, hal_format_name(g_ct_buf.format), g_ct_dataspace);
    for (int i = 0; i < MAX_LAYERS; i++) {
        const LayerState* l = &g_layers[i];
        if (!l->used || l->display != display) continue;
        LOGI("  L%-4" PRIu64 " z=%-3u req=%-6s got=%-6s buf=%p %dx%d s=%d fmt=0x%x(%s) u=0x%x ds=0x%x tr=%d bl=%d a=%.2f "
             "frame=[%d,%d,%d,%d] crop=[%.0f,%.0f,%.0f,%.0f] rm=%d",
             l->id, l->z, comp_name(l->comp_requested), l->comp_changed_to < 0 ? "-" : comp_name(l->comp_changed_to),
             l->buffer, l->buf.w, l->buf.h, l->buf.stride, l->buf.format, hal_format_name(l->buf.format), l->buf.usage,
             l->dataspace, l->transform, l->blend, l->alpha,
             l->frame.left, l->frame.top, l->frame.right, l->frame.bottom,
             l->crop.left, l->crop.top, l->crop.right, l->crop.bottom, l->refresh_mode);
    }
    pthread_mutex_unlock(&g_lock);
}

static int32_t hook_validateDisplay(hwc2_device_t* dev, hwc2_display_t display, uint32_t* outNumTypes, uint32_t* outNumRequests) {
    static Limiter L = {0, 12, 240};
    refresh_knobs();
    // Apply/refresh the Bigme per-layer refresh mode if the knob changed.
    if (g_refresh_mode != g_refresh_mode_applied) {
        pthread_mutex_lock(&g_lock);
        if (g_refresh_mode >= 0)
            for (int i = 0; i < MAX_LAYERS; i++) if (g_layers[i].used) push_refresh_mode(g_layers[i].display, &g_layers[i], g_refresh_mode, "knob");
        g_refresh_mode_applied = g_refresh_mode;
        pthread_mutex_unlock(&g_lock);
        LOGI("refresh_mode knob now %d", g_refresh_mode);
    }
    bool say = lim(L);
    if (say) log_layer_table(display, "validateDisplay IN");
    int32_t err = ((PFN_validateDisplay)g_real_fn[FN_VALIDATE_DISPLAY])(dev, display, outNumTypes, outNumRequests);
    __atomic_fetch_add(&g_ct.validate, 1, __ATOMIC_RELAXED);
    if (say) LOGI("validateDisplay OUT: err=%d (5=HAS_CHANGES) numTypes=%u numRequests=%u", err, outNumTypes ? *outNumTypes : 0, outNumRequests ? *outNumRequests : 0);
    return err;
}

static int32_t hook_getChangedCompositionTypes(hwc2_device_t* dev, hwc2_display_t display, uint32_t* outNum, hwc2_layer_t* outLayers, int32_t* outTypes) {
    static Limiter L = {0, 12, 240};
    int32_t err = ((PFN_getChangedCompositionTypes)g_real_fn[FN_GET_CHANGED_COMPOSITION_TYPES])(dev, display, outNum, outLayers, outTypes);
    if (err == 0 && outNum && outLayers && outTypes) {
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAX_LAYERS; i++) if (g_layers[i].used && g_layers[i].display == display) g_layers[i].comp_changed_to = -1;
        for (uint32_t i = 0; i < *outNum; i++) { LayerState* l = find_layer(outLayers[i], false); if (l) l->comp_changed_to = outTypes[i]; }
        pthread_mutex_unlock(&g_lock);
        if (lim(L)) {
            char line[1024]; int n = snprintf(line, sizeof(line), "changedCompositionTypes display=%" PRIu64 " n=%u:", display, *outNum);
            for (uint32_t i = 0; i < *outNum && n < (int)sizeof(line) - 40; i++)
                n += snprintf(line + n, sizeof(line) - n, " L%" PRIu64 "->%s", outLayers[i], comp_name(outTypes[i]));
            LOGI("%s", line);
        }
    }
    return err;
}

static int32_t hook_presentDisplay(hwc2_device_t* dev, hwc2_display_t display, int32_t* outFence) {
    static Limiter L = {0, 12, 240};
    uint64_t t0 = now_ns();
    int32_t err = ((PFN_presentDisplay)g_real_fn[FN_PRESENT_DISPLAY])(dev, display, outFence);
    uint64_t dt = (now_ns() - t0) / 1000;
    __atomic_fetch_add(&g_ct.present, 1, __ATOMIC_RELAXED);
    g_ct.last_present_ns = t0;
    if (dt > g_ct.present_max_us) g_ct.present_max_us = dt;
    if (lim(L)) LOGI("presentDisplay display=%" PRIu64 " err=%d fence=%d %" PRIu64 "us  [totals: validate=%" PRIu64 " present=%" PRIu64 " eink_update=%" PRIu64 " c2g=%" PRIu64 " cl2g=%" PRIu64 " pimg=%" PRIu64 " addfb2=%" PRIu64 " atomic=%" PRIu64 "]",
                    display, err, outFence ? *outFence : -1, dt, g_ct.validate, g_ct.present, g_ct.eink_update,
                    g_ct.convert_to_gray, g_ct.convert_layer_to_gray, g_ct.process_image, g_ct.addfb2, g_ct.atomic_commit);
    return err;
}

static int32_t hook_setPowerMode(hwc2_device_t* dev, hwc2_display_t display, int32_t mode) {
    LOGI("setPowerMode display=%" PRIu64 " mode=%d", display, mode);
    return ((PFN_setPowerMode)g_real_fn[FN_SET_POWER_MODE])(dev, display, mode);
}

// Append our state to the HWC's dump so it shows up in `dumpsys SurfaceFlinger`.
static int render_summary(char* buf, size_t cap) {
    pthread_mutex_lock(&g_lock);
    drm_eink_update u = g_last_update; uint64_t seq = g_last_update_seq;
    pthread_mutex_unlock(&g_lock);
    return snprintf(buf, cap,
        "\n[hwcshim] build_id_ok=%d log=%d refresh_mode=%d(applied %d) fn71=%p\n"
        "[hwcshim] counts: validate=%" PRIu64 " present=%" PRIu64 " (max %" PRIu64 "us) setCT=%" PRIu64 " layers +%" PRIu64 "/-%" PRIu64 "\n"
        "[hwcshim] out: eink_update=%" PRIu64 " vsync=%" PRIu64 " other=%" PRIu64 " convertToGray=%" PRIu64 " imageSmoothing=%" PRIu64
        " convertLayerToGray=%" PRIu64 " processImage=%" PRIu64 " addfb2=%" PRIu64 " atomic=%" PRIu64 " setcrtc=%" PRIu64 "\n"
        "[hwcshim] client target: %dx%d stride=%d fmt=0x%x(%s) usage=0x%x alloc=%d ds=0x%x\n"
        "[hwcshim] last EINK_UPDATE #%" PRIu64 ": mode=0x%x area=[%d,%d,%d,%d] dither=%u fd=%d format=%u stride=%u height=%u\n",
        g_build_id_ok, g_log, g_refresh_mode, g_refresh_mode_applied, (void*)g_real_fn[FN_BIGME_SET_LAYER_REFRESH_MODE],
        g_ct.validate, g_ct.present, g_ct.present_max_us, g_ct.set_client_target, g_ct.create_layer, g_ct.destroy_layer,
        g_ct.eink_update, g_ct.eink_vsync, g_ct.eink_other, g_ct.convert_to_gray, g_ct.image_smoothing,
        g_ct.convert_layer_to_gray, g_ct.process_image, g_ct.addfb2, g_ct.atomic_commit, g_ct.set_crtc,
        g_ct_buf.w, g_ct_buf.h, g_ct_buf.stride, g_ct_buf.format, hal_format_name(g_ct_buf.format), g_ct_buf.usage, g_ct_buf.alloc_size, g_ct_dataspace,
        seq, u.mode, u.left, u.top, u.right, u.bottom, u.dither, u.fd, u.format, u.stride, u.height);
}

static void hook_dump(hwc2_device_t* dev, uint32_t* outSize, char* outBuffer) {
    PFN_dump real = (PFN_dump)g_real_fn[FN_DUMP];
    char ours[2048]; int ourLen = render_summary(ours, sizeof(ours));
    if (ourLen < 0) ourLen = 0; if (ourLen >= (int)sizeof(ours)) ourLen = sizeof(ours) - 1;
    if (!outBuffer) {
        uint32_t realLen = 0; if (real) real(dev, &realLen, nullptr);
        *outSize = realLen + (uint32_t)ourLen;
        return;
    }
    uint32_t cap = *outSize, realLen = 0;
    if (real) { realLen = cap > (uint32_t)ourLen ? cap - (uint32_t)ourLen : 0; real(dev, &realLen, outBuffer); if (realLen > cap) realLen = cap; }
    uint32_t room = cap - realLen; uint32_t n = (uint32_t)ourLen < room ? (uint32_t)ourLen : room;
    memcpy(outBuffer + realLen, ours, n);
    *outSize = realLen + n;
}

// ---------------------------------------------------------------- getFunction

static hwc2_function_pointer_t shim_getFunction(hwc2_device_t* dev, int32_t desc) {
    hwc2_function_pointer_t real = g_real_getFunction(dev, desc);
    if (desc >= 0 && desc < FN_MAX_PROBE) g_real_fn[desc] = real;
    LOGV("getFunction(%d) -> %p", desc, (void*)real);
    if (g_disabled || !real) return real;
    switch (desc) {
        case FN_CREATE_LAYER:                 return (hwc2_function_pointer_t)hook_createLayer;
        case FN_DESTROY_LAYER:                return (hwc2_function_pointer_t)hook_destroyLayer;
        case FN_DUMP:                         return (hwc2_function_pointer_t)hook_dump;
        case FN_GET_CHANGED_COMPOSITION_TYPES:return (hwc2_function_pointer_t)hook_getChangedCompositionTypes;
        case FN_GET_CLIENT_TARGET_SUPPORT:    return (hwc2_function_pointer_t)hook_getClientTargetSupport;
        case FN_PRESENT_DISPLAY:              return (hwc2_function_pointer_t)hook_presentDisplay;
        case FN_SET_CLIENT_TARGET:            return (hwc2_function_pointer_t)hook_setClientTarget;
        case FN_SET_LAYER_BLEND_MODE:         return (hwc2_function_pointer_t)hook_setLayerBlendMode;
        case FN_SET_LAYER_BUFFER:             return (hwc2_function_pointer_t)hook_setLayerBuffer;
        case FN_SET_LAYER_COMPOSITION_TYPE:   return (hwc2_function_pointer_t)hook_setLayerCompositionType;
        case FN_SET_LAYER_DATASPACE:          return (hwc2_function_pointer_t)hook_setLayerDataspace;
        case FN_SET_LAYER_DISPLAY_FRAME:      return (hwc2_function_pointer_t)hook_setLayerDisplayFrame;
        case FN_SET_LAYER_PLANE_ALPHA:        return (hwc2_function_pointer_t)hook_setLayerPlaneAlpha;
        case FN_SET_LAYER_SOURCE_CROP:        return (hwc2_function_pointer_t)hook_setLayerSourceCrop;
        case FN_SET_LAYER_TRANSFORM:          return (hwc2_function_pointer_t)hook_setLayerTransform;
        case FN_SET_LAYER_Z_ORDER:            return (hwc2_function_pointer_t)hook_setLayerZOrder;
        case FN_SET_POWER_MODE:               return (hwc2_function_pointer_t)hook_setPowerMode;
        case FN_VALIDATE_DISPLAY:             return (hwc2_function_pointer_t)hook_validateDisplay;
        default:                              return real;
    }
}

static void probe_functions(hwc2_device_t* dev) {
    char line[1024]; int n = snprintf(line, sizeof(line), "real HWC descriptors present:");
    int extra = 0;
    for (int d = 1; d < FN_MAX_PROBE; d++) {
        hwc2_function_pointer_t f = g_real_getFunction(dev, d);
        g_real_fn[d] = f;
        if (!f) continue;
        if (d > 61) { n += snprintf(line + n, sizeof(line) - n, " %d", d); extra++; }
    }
    LOGI("%s%s (beyond composer-2.3's 61: %d; 71 = Bigme setLayerRefreshMode is %s)", line, extra ? "" : " (none beyond 61)", extra,
         g_real_fn[FN_BIGME_SET_LAYER_REFRESH_MODE] ? "PRESENT" : "ABSENT");
    g_probed = true;
}

// ---------------------------------------------------------------- module entry

static int shim_open(const hw_module_t* /*module*/, const char* name, hw_device_t** device) {
    refresh_knobs();
    g_disabled = prop_int("vendor.debug.hwc.shim.disable", 0) != 0;
    LOGI("hwcshim loading real HWC %s (name=%s, log=%d, refresh_mode=%d, disabled=%d)", REAL_HWC, name ? name : "?", g_log, g_refresh_mode, g_disabled);

    g_real_handle = dlopen(REAL_HWC, RTLD_NOW | RTLD_LOCAL);
    if (!g_real_handle) { LOGE("dlopen(%s) failed: %s", REAL_HWC, dlerror()); return -ENOENT; }
    g_real_module = (hw_module_t*)dlsym(g_real_handle, "HMI");
    if (!g_real_module) { LOGE("real HMI not found: %s", dlerror()); return -EINVAL; }
    LOGI("real module id=%s name=%s author=%s api=0x%x hal=0x%x", g_real_module->id, g_real_module->name,
         g_real_module->author, g_real_module->module_api_version, g_real_module->hal_api_version);

    uint8_t bid[21] = {0};
    if (dl_iterate_phdr(buildid_cb, bid) == 1) {
        char hex[41]; for (int i = 0; i < 16; i++) snprintf(hex + 2*i, 3, "%02x", bid[i]); hex[32] = 0;
        g_build_id_ok = bid[20] >= 16 && memcmp(bid, KNOWN_BUILD_ID, 16) == 0;
        LOGI("real HWC BuildID %s -> struct peeks %s", hex, g_build_id_ok ? "ENABLED" : "DISABLED (unknown build)");
    } else {
        LOGW("could not read real HWC BuildID; struct peeks disabled");
    }

    hw_device_t* dev = nullptr;
    int err = g_real_module->methods->open(g_real_module, name, &dev);
    if (err != 0 || !dev) { LOGE("real open failed: %d", err); return err ? err : -EINVAL; }
    g_dev = (hwc2_device_t*)dev;
    LOGI("real device version=0x%x (major %u)", dev->version, (dev->version >> 24) & 0xf);

    if (!g_disabled) {
        g_real_getFunction = g_dev->getFunction;
        g_dev->getFunction = shim_getFunction;
        probe_functions(g_dev);
        install_out_hooks();
    }
    *device = dev;
    return 0;
}

static hw_module_methods_t shim_methods = { shim_open };

extern "C" __attribute__((visibility("default"))) hw_module_t HMI = {
    HARDWARE_MODULE_TAG,
    /*module_api_version*/ 0x0300,     // HWC_MODULE_API_VERSION_0_1 semantics are unused by hwc2 loaders
    /*hal_api_version*/    0x0100,
    "hwcomposer",
    "hwcshim interposer for hwcomposer.mt6877.so",
    "hibreak",
    &shim_methods,
    nullptr,
    {0},
};
