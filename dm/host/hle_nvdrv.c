// nvdrv (the NVIDIA driver service): the host takes over NVN entirely and has no use for it; only the SDK movie library's hardware decoder (subsdk0's NVOMX → NvRmChannelOpen) opens /dev/nvhost-* directly.
// The original NvOsDrvOpen first connects to the nvdrv service; the host has no service process and the SDK aborts on the spot (Engage aborted when Unity created a decoder at startup).
// Cyber Sleuth's GL driver (NintendoSDK_OpenGL) renders through NVN but opens /dev/nvhost-ctrl-gpu for its own bookkeeping; failing the open makes it skip its allocator and crash on a null context.
// Bring-up stub: opens hand out fake fds and ioctls are logged (request + argument dump) and answered NV_OK, so we can see what the driver asks for and what it reads back.
#include "hle.h"

#define NV_ERR 3   // NvError: non-zero means failure (3 = not initialized)

// ---------- NvOs allocator: the GL driver's large CPU allocations need more than the sdk's 32 KB heap ----------
// Cyber Sleuth's subsdk0 (NintendoSDK_OpenGL) allocates its ~272 KB EGL context through NvOsAlloc, which lands in
// the sdk's NvOsHeapAllocator — a static dlmalloc mspace over a 32 KB buffer whose expansion path needs callbacks
// nothing in this game installs (NvOsHeapAllocatorSetCallbacks is a dead import, NvOsSetAllocHook is a no-op ret).
// On hardware this heap must have had a bigger backing; here NvOs allocations are served by the host allocator,
// and frees are routed by the block's owner so memory allocated inside the sdk's own mspace still goes to its free.
static void *(*g_nv_alloc)(size_t), *(*g_nv_alloc_al)(size_t, size_t), *(*g_nv_realloc)(void *, size_t);
static void (*g_nv_free)(void *);
static int (*g_nv_cfgstr)(const char *, char *, uint32_t), (*g_nv_syscfgstr)(const char *, char *, uint32_t);
static module_t *g_nv_sdk;
static pthread_once_t g_nv_once = PTHREAD_ONCE_INIT;
static void nv_alloc_init(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) g_nv_sdk = &g_mod[i];
    if (!g_nv_sdk) die("NvOs alloc: no sdk module");
    g_nv_alloc = module_sym(g_nv_sdk, "NvOsAlloc");
    g_nv_alloc_al = module_sym(g_nv_sdk, "NvOsAllocAlign");
    g_nv_realloc = module_sym(g_nv_sdk, "NvOsRealloc");
    g_nv_free = module_sym(g_nv_sdk, "NvOsFree");
    g_nv_cfgstr = module_sym(g_nv_sdk, "NvOsGetConfigString");
    g_nv_syscfgstr = module_sym(g_nv_sdk, "NvOsGetSysConfigString");
    if (!g_nv_alloc || !g_nv_free) die("NvOsAlloc/NvOsFree not found in the SDK");
}
static int nv_from_sdk_heap(void *p) { return p && g_nv_sdk && p >= (void *)g_nv_sdk->base && p < (void *)(g_nv_sdk->base + g_nv_sdk->size); }

HLE_FORCE("NvOsAlloc", "sdk heap is a fixed 32 KB mspace (Cyber Sleuth GL context needs ~272 KB); host malloc", void *, h_nvos_Alloc, size_t n) {
    pthread_once(&g_nv_once, nv_alloc_init);
    void *p = malloc(n);
    if (p) LOG("nvos alloc n=%#zx -> %p", n, p);
    return p;
}
HLE_FORCE("NvOsAllocAlign", "sdk heap is a fixed 32 KB mspace; host aligned alloc (NvOsAllocAlign(align, size))", void *, h_nvos_AllocAlign, size_t align, size_t n) {
    pthread_once(&g_nv_once, nv_alloc_init);
    void *p = NULL; if (align < sizeof(void *)) align = sizeof(void *);
    if (posix_memalign(&p, align, n)) p = NULL;
    if (p) LOG("nvos alloc_align align=%#zx n=%#zx -> %p", align, n, p);
    return p;
}
HLE_FORCE("NvOsFree", "host free, or the sdk's own free for mspace blocks", void, h_nvos_Free, void *p) {
    pthread_once(&g_nv_once, nv_alloc_init);
    if (nv_from_sdk_heap(p)) g_nv_free(p); else free(p);
}
HLE_FORCE("NvOsRealloc", "host realloc, or the sdk's for mspace blocks", void *, h_nvos_Realloc, void *p, size_t n) {
    pthread_once(&g_nv_once, nv_alloc_init);
    if (nv_from_sdk_heap(p)) return g_nv_realloc(p, n);
    return realloc(p, n);
}

// ---------- NvOsHeapAllocator methods: same 32 KB cap, reached through allocator objects ----------
// The GL driver's GPU-visible buffers go through NvRmMemHandleAllocAttr → the allocator object in
// the surface attrs (NvOsGetCurrentAllocator's heap), whose method slots call the NvOsHeapAllocator*
// functions directly — the NvOsAlloc hooks never see them. Same redirection: host malloc serves them.
static void *(*g_nv_heap_alloc)(void *, size_t), *(*g_nv_heap_alloc_al)(void *, size_t, size_t);
static void *(*g_nv_heap_realloc)(void *, void *, size_t);
static void (*g_nv_heap_free)(void *, void *);
static int g_nv_heap_rdy;
static void nv_heap_init(void) {
    g_nv_heap_alloc = module_sym(g_nv_sdk, "NvOsHeapAllocatorAlloc");
    g_nv_heap_alloc_al = module_sym(g_nv_sdk, "NvOsHeapAllocatorAllocAlign");
    g_nv_heap_realloc = module_sym(g_nv_sdk, "NvOsHeapAllocatorRealloc");
    g_nv_heap_free = module_sym(g_nv_sdk, "NvOsHeapAllocatorFree");
    g_nv_heap_rdy = 1;
}
HLE_FORCE("NvOsHeapAllocatorAlloc", "heap object allocs hit the 32 KB mspace (GL buffers need MBs); host malloc", void *, h_nvos_HeapAlloc, void *a, size_t n) {
    pthread_once(&g_nv_once, nv_alloc_init);
    if (!g_nv_heap_rdy) nv_heap_init();
    void *p = malloc(n);
    if (!p && g_nv_heap_alloc) p = g_nv_heap_alloc(a, n);
    return p;
}
HLE_FORCE("NvOsHeapAllocatorAllocAlign", "heap object allocs hit the 32 KB mspace; host aligned alloc", void *, h_nvos_HeapAllocAlign, void *a, size_t align, size_t n) {
    pthread_once(&g_nv_once, nv_alloc_init);
    if (!g_nv_heap_rdy) nv_heap_init();
    if (align < sizeof(void *)) align = sizeof(void *);
    void *p = NULL;
    if (posix_memalign(&p, align, n) && g_nv_heap_alloc_al) p = g_nv_heap_alloc_al(a, align, n);
    if (p && n >= 0x40000) LOG("nvheap alloc_align a=%p align=%#zx n=%#zx -> %p", a, align, n, p);
    return p;
}
HLE_FORCE("NvOsHeapAllocatorFree", "host free, or the heap's own free for mspace blocks", void, h_nvos_HeapFree, void *a, void *p) {
    pthread_once(&g_nv_once, nv_alloc_init);
    if (!g_nv_heap_rdy) nv_heap_init();
    if (nv_from_sdk_heap(p)) { if (g_nv_heap_free) g_nv_heap_free(a, p); }
    else free(p);
}
HLE_FORCE("NvOsHeapAllocatorRealloc", "host realloc, or the heap's for mspace blocks", void *, h_nvos_HeapRealloc, void *a, void *p, size_t n) {
    pthread_once(&g_nv_once, nv_alloc_init);
    if (!g_nv_heap_rdy) nv_heap_init();
    if (nv_from_sdk_heap(p)) return g_nv_heap_realloc ? g_nv_heap_realloc(a, p, n) : NULL;
    return realloc(p, n);
}

// ---------- NvWsi winsys selection: NV_WINSYS picks the SDK's window-system backend ----------
// NintendoSDK_OpenGL enumerates displays through NvWsi, whose default "android" backend talks to
// nn::am/SurfaceComposer — service objects the host has none of (GetSelfController() == NULL →
// null-vtable crash inside eglInitialize). The SDK ships a self-contained "null" winsys (plain
// buffer queue, no services); answering the NV_WINSYS config string with "null" selects it.
static int nv_cfgstr(const char *name, char *buf, uint32_t len, int (*real)(const char *, char *, uint32_t)) {
    pthread_once(&g_nv_once, nv_alloc_init);
    if (name && !strcmp(name, "NV_WINSYS") && len >= 5) { memcpy(buf, "null", 5); LOG("nv config NV_WINSYS -> \"null\""); return 0; }
    return real ? real(name, buf, len) : 0x30006;
}
HLE_FORCE("NvOsGetConfigString", "NV_WINSYS -> \"null\" winsys (the android backend needs nn::am, which the host lacks)", int, h_nvos_CfgStr, const char *name, char *buf, uint32_t len) {
    return nv_cfgstr(name, buf, len, g_nv_cfgstr);
}
HLE_FORCE("NvOsGetSysConfigString", "NV_WINSYS -> \"null\" winsys", int, h_nvos_SysCfgStr, const char *name, char *buf, uint32_t len) {
    return nv_cfgstr(name, buf, len, g_nv_syscfgstr);
}

// ---------- NvWsi window surfaces: force the SDK's headless "null" backend ----------
// NvWsiContextCreate registers three winsys backends and installs "android" as the default in
// the context's ops slot (ctx+8); NV_WINSYS is only consulted when the display handle is the
// internal-display id, so our displays always get "android". That backend models the console's
// real ANativeWindow (nn::vi service parcels, magic "dnw_") — objects the host never made, so
// ops->validate_window rejects the host's handle and eglCreateWindowSurface fails. The "null"
// backend is a complete guest-side implementation (own window type + buffer queue, no
// services): the hook points the context's ops at its table so surfaces are real guest objects.
static int (*g_nv_wincreate)(void *, void *, void *, uint32_t, uint32_t, uint32_t, uint32_t, void *);
#define NV_NULL_OPS_OFF 0xb87970   // "null" winsys ops table, sdk image offset (Cyber Sleuth's sdk build)
HLE_FORCE("NvWsiWindowCreate", "use the SDK's headless null winsys backend (android backend needs service objects the host lacks)", int, h_nvwsi_WindowCreate, void *ctx, void *win, void *a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, void *out) {
    pthread_once(&g_nv_once, nv_alloc_init);
    if (!g_nv_wincreate) g_nv_wincreate = module_sym(g_nv_sdk, "NvWsiWindowCreate");
    ((void **)ctx)[1] = (uint8_t *)g_nv_sdk->base + NV_NULL_OPS_OFF;
    int r = g_nv_wincreate(ctx, win, a2, a3, a4, a5, a6, out);
    LOG("NvWsiWindowCreate -> %#x (null winsys)", r);
    return r;
}

// ---------- EGL window surfaces: hand the game a pbuffer ----------
// eglMakeCurrent on a window surface asks the winsys surface for its scanout parameters through
// a +0xb8 vmethod (attribute 0x2077); the "null" backend's surface does not answer it, so the
// bind fails. A pbuffer is a complete guest-side surface: create it once for the game's config,
// the context binds to real buffers, and eglSwapBuffers on a pbuffer is a no-op success.
static void *(*g_egl_pbuffer)(void *, void *, const int32_t *);
static void *(*g_egl_winsurf)(void *, void *, void *, const int32_t *);
static module_t *nv_mod(const char *name) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, name)) return &g_mod[i]; return NULL; }
HLE_FORCE("eglCreateWindowSurface", "window surface bind needs a scanout answer the null winsys lacks; a pbuffer binds offscreen", void *, h_egl_WinSurface, void *dpy, void *cfg, void *win, const int32_t *attribs) {
    if (!g_egl_pbuffer) {
        module_t *s0 = nv_mod("subsdk0");
        if (s0) { g_egl_pbuffer = module_sym(s0, "eglCreatePbufferSurface"); g_egl_winsurf = module_sym(s0, "eglCreateWindowSurface"); }
    }
    int32_t pb[] = { 0x3057, 1920, 0x3056, 1080, 0x3038 };   // EGL_WIDTH/EGL_HEIGHT/EGL_NONE
    void *s = g_egl_pbuffer ? g_egl_pbuffer(dpy, cfg, pb) : NULL;
    LOG("eglCreateWindowSurface(dpy=%p cfg=%p win=%p) -> pbuffer %p", dpy, cfg, win, s);
    if (!s && g_egl_winsurf) s = g_egl_winsurf(dpy, cfg, win, attribs);
    return s;
}

static struct { char path[48]; } nv_fds[64];
static int nv_nf, nv_dumps;
static uint64_t nv_as_next = 0x8000000;   // next fake GPU VA in the small-page region (as-gpu layout below)

static const char *nv_path(int fd) { int i = fd - 0x40; return (i >= 0 && i < nv_nf) ? nv_fds[i].path : "?"; }

// GM20B (Tegra X1) gpu characteristics, 0xa0 bytes — the same values the Switch's nvdrv reports
struct nv_gc {
    uint32_t arch, impl, rev, num_gpc;
    uint64_t l2_cache_size, on_board_video_memory_size;
    uint32_t num_tpc_per_gpc, bus_type, big_page_size, compression_page_size;
    uint32_t pde_coverage_bit_count, available_big_page_sizes, gpc_mask;
    uint32_t sm_arch_sm_version, sm_arch_spa_version, sm_arch_warp_count;
    uint32_t gpu_va_bit_count, reserved;
    uint64_t flags;
    uint32_t twod_class, threed_class, compute_class, gpfifo_class;
    uint32_t inline_to_memory_class, dma_copy_class;
    uint32_t max_fbps_count, fbp_en_mask, max_ltc_per_fbp, max_lts_per_ltc;
    uint32_t max_tex_per_tpc, max_gpc_count, rop_l2_en_mask_0, rop_l2_en_mask_1;
    uint64_t chipname, gr_compbit_store_base_hw;
};
static const struct nv_gc NV_GC = {
    .arch = 0x120, .impl = 0xb, .rev = 0xa1, .num_gpc = 1,
    .l2_cache_size = 0x40000, .on_board_video_memory_size = 0,
    .num_tpc_per_gpc = 2, .bus_type = 0x20,
    .big_page_size = 0x20000, .compression_page_size = 0x20000,
    .pde_coverage_bit_count = 0x1b, .available_big_page_sizes = 0x30000, .gpc_mask = 1,
    .sm_arch_sm_version = 0x503, .sm_arch_spa_version = 0x503, .sm_arch_warp_count = 0x80,
    .gpu_va_bit_count = 0x28, .reserved = 0, .flags = 0x55,
    .twod_class = 0x902d, .threed_class = 0xb197, .compute_class = 0xb1c0, .gpfifo_class = 0xb06f,
    .inline_to_memory_class = 0xa140, .dma_copy_class = 0xb0b5,
    .max_fbps_count = 1, .fbp_en_mask = 0, .max_ltc_per_fbp = 2, .max_lts_per_ltc = 1,
    .max_tex_per_tpc = 0, .max_gpc_count = 1, .rop_l2_en_mask_0 = 0x21d70, .rop_l2_en_mask_1 = 0,
    .chipname = 0x6230326d67, .gr_compbit_store_base_hw = 0,
};

static void nv_dump(const void *p, size_t n) {
    if (nv_dumps > 300) return;
    nv_dumps++;
    const uint8_t *b = p; if (n > 192) n = 192;
    for (size_t i = 0; i < n; i += 16) {
        char line[80]; int o = snprintf(line, sizeof line, "  +%02zx:", i);
        for (size_t j = i; j < i + 16 && j < n; j++) o += snprintf(line + o, sizeof line - o, " %02x", b[j]);
        LOG("%s", line);
    }
}

HLE_FORCE("NvOsDrvOpen", "nvdrv open device: fake fd", int, h_nvos_Open, const char *path) {
    int fd = 0x40 + nv_nf;
    if (nv_nf < 64) { snprintf(nv_fds[nv_nf].path, 48, "%s", path ? path : "?"); nv_nf++; }
    LOG("nvdrv open %s -> fd=%d", path ? path : "(null)", fd);
    return fd;
}
HLE_FORCE("NvOsDrvClose", "nvdrv close device; nothing to do", void, h_nvos_Close, int fd) { (void)fd; }

static int h_nvos_Ioctl_(int fd, uint32_t req, void *arg, size_t n) {
    // /dev/nvhost-ctrl-gpu ioctls (nvdrv 'G' group); arg layouts from the nvgpu uapi the SDK's NvRm library uses
    if (arg) switch (req) {
    case 0xc0b04705: {   // GET_CHARACTERISTICS: {u64 buf_size, u64 buf_addr, gc inline at +0x10}
        if (n >= 0xb0) memcpy((uint8_t *)arg + 0x10, &NV_GC, sizeof NV_GC);
        void *dst = *(void **)((uint8_t *)arg + 8);
        if (dst) memcpy(dst, &NV_GC, sizeof NV_GC);
        return 0;
    }
    case 0xc0184706: {   // GET_TPC_MASKS: {u32 mask_buf_size, pad, u64 mask_buf_addr, u32 tpc_mask, pad}
        *(uint32_t *)((uint8_t *)arg + 0x10) = 3;
        void *dst = *(void **)((uint8_t *)arg + 8);
        if (dst) *(uint32_t *)dst = 3;
        return 0;
    }
    case 0x80044701: *(uint32_t *)arg = 1; return 0;                              // ZCULL_GET_CTX_SIZE
    case 0x80284702: {                                                          // ZCULL_GET_INFO
        static const uint32_t z[10] = { 0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xc0, 0x20, 0x40, 0x10 };
        memcpy(arg, z, sizeof z); return 0;
    }
    case 0x80084714: *(uint32_t *)arg = 7; *((uint32_t *)arg + 1) = 1; return 0;  // ZBC_GET_ACTIVE_SLOT_MASK
    case 0x80044715: *(uint32_t *)arg = 100; return 0;                            // PMU_GET_GPU_LOAD
    case 0x40084707: return 0;                                                  // FLUSH_L2 (no-op)
    case 0xc010471c: *(uint64_t *)arg = guest_ns(); return 0;                     // GET_GPU_TIME
    case 0x40284109: return 0;                                                  // AS_ALLOC_AS_EX (defaults: small 0x8000000..4G, big 4G..128G)
    case 0x40044101: return 0;                                                  // AS_BIND_CHANNEL
    case 0xc0404108: {                                                          // AS_GET_VA_REGIONS: {u64 buf_addr, u32 buf_size, u32 rsvd, region[2]}
        struct { uint64_t off; uint32_t psz, r; uint64_t pages; } *rg = (void *)((uint8_t *)arg + 0x10);
        *(uint32_t *)((uint8_t *)arg + 8) = 0x40;
        rg[0].off = 0x8000000;    rg[0].psz = 0x1000;  rg[0].r = 0; rg[0].pages = 0x3ff800;
        rg[1].off = 0x400000000;  rg[1].psz = 0x20000; rg[1].r = 0; rg[1].pages = 0xe0000;
        return 0;
    }
    case 0xc0184102: {                                                          // AS_ALLOC_SPACE: {u64 pages, u64 page_size, u32 flags, u32 rsvd, u64 offset}
        uint64_t pages = *(uint64_t *)arg, psz = *(uint64_t *)((uint8_t *)arg + 8);
        uint32_t flags = *(uint32_t *)((uint8_t *)arg + 0x10);
        uint64_t off;
        if (flags & 1) off = *(uint64_t *)((uint8_t *)arg + 0x18);                // fixed offset requested
        else { off = nv_as_next; nv_as_next += ((pages ? pages : 1) * (psz ? psz : 0x1000) + 0x1ffff) & ~0x1ffffULL; }
        *(uint64_t *)((uint8_t *)arg + 0x18) = off;
        return 0;
    }
    }
    LOG("nvdrv ioctl fd=%d(%s) req=%#x dir=%d size=%#x type=%c nr=%d n=%zu",
        fd, nv_path(fd), req, req >> 30, (req >> 16) & 0x3fff, (req >> 8) & 0xff, req & 0xff, n);
    if (arg) nv_dump(arg, n);
    return 0;   // NV_OK
}
HLE_FORCE("NvOsDrvIoctl", "nvdrv ioctl: logged, returns success", int, h_nvos_Ioctl, int fd, uint32_t req, void *arg, size_t n) { return h_nvos_Ioctl_(fd, req, arg, n); }
HLE_FORCE("NvOsDrvIoctl2", "nvdrv ioctl2: logged, returns success", int, h_nvos_Ioctl2, int fd, uint32_t req, void *arg, size_t n) { return h_nvos_Ioctl_(fd, req, arg, n); }
HLE_FORCE("NvOsDrvIoctl3", "nvdrv ioctl3: logged, returns success", int, h_nvos_Ioctl3, int fd, uint32_t req, void *arg, size_t n) { return h_nvos_Ioctl_(fd, req, arg, n); }
HLE_FORCE("NvOsDrvMapSharedMem", "nvdrv shared memory map: returns an error", int, h_nvos_MapShared, int fd) { LOG("nvdrv mapshared fd=%d", fd); return NV_ERR; }
HLE_FORCE("_Z17NvOsDrvQueryEventijPN2nn2os15SystemEventTypeE", "nvdrv query event: bound to a kernel event, never signaled (error notifier)", int, h_nvos_QueryEvent, int fd, uint32_t id, void *ev) { (void)fd; (void)id; hle_bind_system_event(ev, 0); return 0; }
