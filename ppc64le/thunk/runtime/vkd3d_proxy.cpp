/* HAND-MAINTAINED -- not generated.
 *
 * Guest-side runtime: proxy interning, reference counting, IUnknown, the three
 * float-class shapes, the eight flat d3d12.dll entry points, and the
 * cross-runtime interop exports dxvk's DXGI shim uses.
 *
 * Builds for x86-64 (the real guest) and, unchanged, for ppc64le so the
 * loopback test can drive it natively.  Nothing here is architecture-specific:
 * the marshalling is all "pack into uint64_t[VKD3D_THUNK_ARGS]", and both ABIs
 * agree on every struct that is passed by pointer.
 */
#include "vkd3d_proxy.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#define VKD3D_EXPORT __attribute__((visibility("default")))
/* The float stubs are reached only through the generated vtables inside this
 * library, so they are kept but not exported. */
#define VKD3D_FSTUB  __attribute__((visibility("hidden"), used))

/* ------------------------------------------------------------------ trace */

static bool trace_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("VKD3D_THUNK_TRACE");
        return e && e[0] == '1';
    }();
    return on;
}

#define TRACE(...) do { if (trace_enabled()) std::fprintf(stderr, "vkd3d_thunk: " __VA_ARGS__); } while (0)

/* ------------------------------------------------------------ IID lookup */

/* kVkdIids is generated, sorted by (w0, w1) -- the GUID's 16-byte memory image
 * read as two little-endian 64-bit words.  Both x86-64 and ppc64le are
 * little-endian here, so the guest's GUID bytes and the host's agree without
 * any byte swapping. */
extern "C" uint32_t vkd3d_iface_from_iid(const void* riid) {
    if (!riid)
        return VKD3D_IFACE_INVALID;

    uint64_t w[2];
    std::memcpy(w, riid, 16); /* memcpy: a REFIID from the guest need not be
                               * 8-byte aligned as far as we know. */

    uint32_t lo = 0, hi = kVkdIidCount;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const VkdIidEntry& e = kVkdIids[mid];
        if (e.w0 < w[0] || (e.w0 == w[0] && e.w1 < w[1]))
            lo = mid + 1;
        else if (e.w0 == w[0] && e.w1 == w[1])
            return e.iface;
        else
            hi = mid;
    }
    return VKD3D_IFACE_INVALID;
}

/* ------------------------------------------------------- interning table */

/* Identity key is (host interface pointer, interface id), NOT the host pointer
 * alone.
 *
 * Keying on the pointer alone is tempting -- a COM object that implements
 * several interfaces on one vtable chain returns the same `this` for most
 * QueryInterface calls -- and it is unsafe.  A proxy carries a *static
 * per-type* vtable sized to that interface's slot count.  If
 * QueryInterface(ID3D12GraphicsCommandList) returned the proxy interned earlier
 * as ID3D12CommandList (10 slots), the guest would call slot 47 through a
 * 10-entry array and jump off the end.  So one proxy per (object, interface).
 *
 * That still preserves COM identity, because COM only defines identity through
 * IUnknown: two proxies are the same object iff QI(IID_IUnknown) on each yields
 * the same pointer, which yields the same key, which yields the same proxy.
 * IUnknown is one of the generated interfaces (3 slots), so it interns like any
 * other.
 *
 * Sharded so that the object-create/destroy path does not serialise on one
 * lock.  D3D12 creates objects in bursts -- thousands of PSOs and resources at
 * level load -- but never per draw, so 64 shards of std::unordered_map is
 * comfortably enough; a lock-free open-addressing table was considered and
 * rejected as unreviewable for the gain. */

namespace {

struct Key {
    uint64_t host;
    uint32_t iface;
    bool operator==(const Key& o) const { return host == o.host && iface == o.iface; }
};

struct KeyHash {
    size_t operator()(const Key& k) const {
        uint64_t h = k.host ^ (uint64_t(k.iface) * 0x9e3779b97f4a7c15ull);
        h ^= h >> 29; h *= 0xbf58476d1ce4e5b9ull; h ^= h >> 32;
        return size_t(h);
    }
};

constexpr unsigned kShardBits = 6;
constexpr unsigned kShards    = 1u << kShardBits;

struct Shard {
    std::mutex                               m;
    std::unordered_map<Key, Proxy*, KeyHash> map;
};

Shard& shard_for(uint64_t host) {
    static Shard shards[kShards];
    /* COM objects are at least 8-byte aligned, so the low bits carry no
     * entropy; take bits above them. */
    uint64_t h = (host >> 4) ^ (host >> 20);
    return shards[h & (kShards - 1)];
}

std::atomic<uint32_t> g_live{0};

} /* namespace */

extern "C" uint32_t vkd3d_proxy_live_count(void) {
    return g_live.load(std::memory_order_relaxed);
}

/* ------------------------------------------------- guest calling convention */

/* Which of the generated vtable arrays new proxies are given.  The default is
 * MS-x64 on x86-64 because the deployment target is a PE game; a guest ELF
 * consumer -- every test in tests/, and the FEX ThunksDB direction -- must
 * select SysV.  Getting it wrong is a wild jump, so the mode is announced once,
 * unconditionally, when the first proxy is created. */
namespace {

uint32_t abi_default() {
#if defined(__x86_64__) && !defined(VKD3D_THUNK_DEFAULT_ABI_SYSV)
    return VKD3D_ABI_MS;
#else
    return VKD3D_ABI_SYSV;
#endif
}

std::atomic<uint32_t> g_abi{0xffffffffu};   /* resolved lazily */
std::atomic<bool>     g_abi_locked{false};  /* a proxy exists */

uint32_t abi_resolve() {
    uint32_t a = g_abi.load(std::memory_order_acquire);
    if (a != 0xffffffffu)
        return a;
    a = abi_default();
    if (const char* e = std::getenv("VKD3D_THUNK_ABI")) {
        if (std::strcmp(e, "sysv") == 0)
            a = VKD3D_ABI_SYSV;
        else if (std::strcmp(e, "ms") == 0)
            a = VKD3D_ABI_MS;
        else
            std::fprintf(stderr, "vkd3d_thunk: VKD3D_THUNK_ABI=%s is not 'ms' "
                         "or 'sysv'; keeping the default\n", e);
    }
    if (!(vkd3d_thunk_abi_available() & (1u << a))) {
        /* ppc64le has no ms_abi.  Say so rather than hand out a null vtable. */
        std::fprintf(stderr, "vkd3d_thunk: guest ABI %u is not available in "
                     "this build; using SysV\n", a);
        a = VKD3D_ABI_SYSV;
    }
    uint32_t exp = 0xffffffffu;
    if (g_abi.compare_exchange_strong(exp, a, std::memory_order_acq_rel))
        return a;
    return exp;
}

void abi_announce() {
    static std::once_flag once;
    std::call_once(once, [] {
        uint32_t a = abi_resolve();
        std::fprintf(stderr, "vkd3d_thunk: guest vtable ABI = %s\n",
                     a == VKD3D_ABI_MS ? "ms-x64 (PE callers)"
                                       : "sysv (ELF callers)");
    });
}

} /* namespace */

extern "C" uint32_t vkd3d_thunk_abi(void) {
    return abi_resolve();
}

extern "C" uint32_t vkd3d_thunk_set_abi(uint32_t abi) {
    if (abi >= VKD3D_ABI_COUNT || !(vkd3d_thunk_abi_available() & (1u << abi))) {
        std::fprintf(stderr, "vkd3d_thunk: ABI %u is not available\n", abi);
        return 0;
    }
    if (g_abi_locked.load(std::memory_order_acquire)) {
        /* Live proxies already carry vtables of the old convention; switching
         * now would leave them calling through the wrong one. */
        std::fprintf(stderr, "vkd3d_thunk: ABI cannot be changed after the "
                     "first proxy has been created\n");
        return 0;
    }
    g_abi.store(abi, std::memory_order_release);
    return 1;
}

/* The two spellings the boundary contract exports. */
extern "C" VKD3D_EXPORT void vkd3d_thunk_set_abi_sysv(void) {
    vkd3d_thunk_set_abi(VKD3D_ABI_SYSV);
}

extern "C" VKD3D_EXPORT int vkd3d_thunk_abi_is_ms(void) {
    return vkd3d_thunk_abi() == VKD3D_ABI_MS ? 1 : 0;
}

/* ------------------------------------------------ refusal, not a raw pointer */

extern "C" void vkd3d_thunk_refuse(const char* method, const char* why) {
    static std::mutex m;
    static std::unordered_map<std::string, int> seen;
    std::lock_guard<std::mutex> lk(m);
    if (seen[method]++)
        return;
    std::fprintf(stderr, "vkd3d_thunk: REFUSED %s -- %s\n", method, why);
}

/* ------------------------------- structs carrying interface pointers inside */

/* These slots cross RAW.  A D3D12_RESOURCE_BARRIER holds an ID3D12Resource*, a
 * D3D12_TEXTURE_COPY_LOCATION holds one, a D3D12_GRAPHICS_PIPELINE_STATE_DESC
 * holds an ID3D12RootSignature*, and structs pass by pointer with no repacking
 * -- the layout-identity result the whole design rests on -- so what native
 * vkd3d sees inside them is a guest Proxy*.  That is wrong, it is on the
 * hottest paths in the API, and it is the next work package.
 *
 * Until then it is LOUD.  The default is a line per call, because these calls
 * are per-draw and a quiet wrong answer is exactly what this project keeps
 * finding in review; VKD3D_THUNK_STRUCT_WARN=once|off exists for anyone who
 * needs to run through the noise, and VKD3D_THUNK_STRICT=1 aborts instead,
 * which is what the negative-control test drives. */
namespace {

enum StructWarnMode { WARN_EVERY, WARN_ONCE, WARN_OFF };

StructWarnMode struct_warn_mode() {
    static const StructWarnMode m = [] {
        const char* e = std::getenv("VKD3D_THUNK_STRUCT_WARN");
        if (!e || !e[0] || !std::strcmp(e, "every"))
            return WARN_EVERY;
        if (!std::strcmp(e, "once"))
            return WARN_ONCE;
        if (!std::strcmp(e, "off"))
            return WARN_OFF;
        std::fprintf(stderr, "vkd3d_thunk: VKD3D_THUNK_STRUCT_WARN=%s is not "
                     "'every', 'once' or 'off'; using 'every'\n", e);
        return WARN_EVERY;
    }();
    return m;
}

bool strict_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("VKD3D_THUNK_STRICT");
        return e && e[0] == '1';
    }();
    return on;
}

std::atomic<uint64_t> g_struct_warns{0};

} /* namespace */

extern "C" void vkd3d_thunk_struct_iface(const char* method, const char* type,
                                         int arg) {
    g_struct_warns.fetch_add(1, std::memory_order_relaxed);

    if (strict_enabled()) {
        std::fprintf(stderr, "vkd3d_thunk: STRUCT-IFACE %s arg %d is a %s, "
                     "whose interface members cross UNTRANSLATED -- aborting "
                     "because VKD3D_THUNK_STRICT=1\n", method, arg, type);
        std::fflush(stderr);
        std::abort();
    }

    StructWarnMode m = struct_warn_mode();
    if (m == WARN_OFF)
        return;
    if (m == WARN_ONCE) {
        static std::mutex mu;
        static std::unordered_map<std::string, int> seen;
        std::lock_guard<std::mutex> lk(mu);
        if (seen[method]++)
            return;
    }
    std::fprintf(stderr, "vkd3d_thunk: STRUCT-IFACE %s arg %d is a %s, whose "
                 "interface members cross UNTRANSLATED\n", method, arg, type);
}

extern "C" uint64_t vkd3d_thunk_struct_iface_count(void) {
    return g_struct_warns.load(std::memory_order_relaxed);
}

/* ------------------------------------------------- interface-pointer arrays */

extern "C" uint64_t vkd3d_ifarray_in(VkdIfArray* s, void* const* src, uint32_t n) {
    s->p = nullptr;
    s->n = 0;
    if (!src || !n)
        return 0;
    uint64_t* buf = s->inl;
    if (n > VKD3D_IFARRAY_INLINE) {
        buf = static_cast<uint64_t*>(std::malloc(size_t(n) * sizeof(uint64_t)));
        if (!buf) {
            /* Never a short buffer: vkd3d would read n elements off the end. */
            std::fprintf(stderr, "vkd3d_thunk: out of memory for a %u-element "
                         "interface array; passing null\n", n);
            return 0;
        }
        s->p = buf;
    }
    s->n = n;
    for (uint32_t i = 0; i < n; ++i)
        buf[i] = vkd3d_proxy_unwrap(src[i]);
    return reinterpret_cast<uint64_t>(buf);
}

extern "C" uint64_t vkd3d_ifarray_out(VkdIfArray* s, uint32_t n) {
    s->p = nullptr;
    s->n = 0;
    if (!n)
        return 0;
    uint64_t* buf = s->inl;
    if (n > VKD3D_IFARRAY_INLINE) {
        buf = static_cast<uint64_t*>(std::calloc(n, sizeof(uint64_t)));
        if (!buf) {
            std::fprintf(stderr, "vkd3d_thunk: out of memory for a %u-element "
                         "interface array; passing null\n", n);
            return 0;
        }
        s->p = buf;
    } else {
        std::memset(buf, 0, size_t(n) * sizeof(uint64_t));
    }
    s->n = n;
    return reinterpret_cast<uint64_t>(buf);
}

extern "C" void vkd3d_ifarray_wrap_out(VkdIfArray* s, void** dst, uint32_t n,
                                       uint32_t iface) {
    if (!dst || !s->n)
        return;
    uint64_t* buf = s->p ? s->p : s->inl;
    if (n > s->n)
        n = s->n;   /* the scratch is the only thing the host could write */
    for (uint32_t i = 0; i < n; ++i)
        dst[i] = vkd3d_proxy_wrap(buf[i], iface); /* wrap(0) == nullptr */
}

extern "C" void vkd3d_ifarray_free(VkdIfArray* s) {
    if (s->p) {
        std::free(s->p);
        s->p = nullptr;
    }
    s->n = 0;
}

/* ------------------------------------------------------------- lifetime ---- */

extern "C" void vkd3d_proxy_host_release(uint64_t host) {
    if (!host)
        return;
    /* Release is slot 2 of every COM vtable without exception, so an object
     * whose interface we do not know can still be released as IUnknown.
     * VKD3D_IFACE_IUNKNOWN has exactly 3 slots, so slot 2 passes the host's
     * bounds check. */
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    vkd3d_thunk_call(VKD3D_IFACE_IUNKNOWN, 2, host, a);
}

extern "C" void* vkd3d_proxy_wrap(uint64_t host, uint32_t iface) {
    if (!host)
        return nullptr;
    if (iface >= VKD3D_IFACE_COUNT) {
        /* Cannot represent it; the caller's host reference would leak, so drop
         * it here rather than silently. */
        std::fprintf(stderr, "vkd3d_thunk: wrap of unknown iface %u, releasing "
                     "host %p\n", iface, (void*) host);
        vkd3d_proxy_host_release(host);
        return nullptr;
    }

    /* Fixes the calling convention for the whole process and says which one it
     * picked.  Cheap: std::once_flag on a path that already allocates. */
    abi_announce();
    g_abi_locked.store(true, std::memory_order_release);

    Shard& sh = shard_for(host);
    Key key{host, iface};
    Proxy* p;
    bool surplus = false;

    {
        std::lock_guard<std::mutex> lk(sh.m);
        auto it = sh.map.find(key);
        if (it != sh.map.end()) {
            p = it->second;
            /* Incrementing under the shard lock is what makes resurrection
             * safe: vkd3d_proxy_release performs its 1->0 transition under this
             * same lock and erases the entry before dropping it, so a proxy
             * found here is alive and stays alive. */
            p->refs.fetch_add(1, std::memory_order_acq_rel);
            surplus = true;
        } else {
            p = new Proxy{vkd3d_thunk_vtable_for(iface, abi_resolve()),
                          host, {1}, iface};
            sh.map.emplace(key, p);
            g_live.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (surplus) {
        /* wrap() consumes a host reference; this pointer was already interned,
         * so the caller's reference is one too many.  Dropped outside the lock
         * -- never call across the boundary holding a guest lock. */
        vkd3d_proxy_host_release(host);
        TRACE("wrap hit    host=%p iface=%s proxy=%p refs=%u\n", (void*) host,
              kVkdIfaceName[iface], (void*) p, p->refs.load());
    } else {
        TRACE("wrap new    host=%p iface=%s proxy=%p\n", (void*) host,
              kVkdIfaceName[iface], (void*) p);
    }
    return p;
}

extern "C" uint64_t vkd3d_proxy_unwrap(void* proxy) {
    return proxy ? static_cast<Proxy*>(proxy)->host : 0;
}

/* ------------------------------------------------------ IUnknown, guest-side */

extern "C" uint32_t vkd3d_proxy_addref(Proxy* self) {
    if (!self)
        return 0;
    return self->refs.fetch_add(1, std::memory_order_acq_rel) + 1;
}

extern "C" uint32_t vkd3d_proxy_release(Proxy* self) {
    if (!self)
        return 0;

    /* Fast path, and the only path that runs per frame: decrement without a
     * lock as long as this is provably not the last reference. */
    uint32_t cur = self->refs.load(std::memory_order_relaxed);
    while (cur > 1) {
        if (self->refs.compare_exchange_weak(cur, cur - 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed))
            return cur - 1; /* no lock, no crossing, no table */
    }

    /* This may be the last reference.  From here the count is manipulated ONLY
     * under the shard lock, which is what makes the destroying thread unique.
     *
     * The obvious version -- decrement, and if it hit zero take the lock and
     * re-check -- is WRONG, and the threaded test finds it in well under a
     * second (it was found that way in the D3D11 project: "free(): double free
     * detected in tcache 2").  Thread A decrements to zero; thread B resurrects
     * through vkd3d_proxy_wrap and then releases, sees zero itself, erases and
     * frees the proxy; thread A finally takes the lock and reads the refcount
     * of an object B already freed, then frees it a second time.  Doing the
     * 1->0 transition inside the lock removes the window: exactly one thread
     * can observe it, and it erases the map entry before releasing the lock, so
     * no later wrap() can find the proxy it is about to destroy. */
    Shard& sh = shard_for(self->host);
    Key key{self->host, self->iface};
    {
        std::lock_guard<std::mutex> lk(sh.m);
        uint32_t prev = self->refs.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 0) {
            self->refs.fetch_add(1, std::memory_order_relaxed); /* undo */
            std::fprintf(stderr, "vkd3d_thunk: Release on proxy %p at refcount 0\n",
                         (void*) self);
            return 0;
        }
        if (prev != 1)
            return prev - 1; /* resurrected while we waited for the lock */

        auto it = sh.map.find(key);
        if (it != sh.map.end() && it->second == self)
            sh.map.erase(it);
        /* Erase strictly before the host release below.  The host object cannot
         * be freed while we hold a reference, so no other host object can be
         * allocated at this address until after the erase -- which is what stops
         * a recycled host pointer from colliding with a stale entry. */
    }

    uint64_t host = self->host;
    TRACE("release end host=%p iface=%s proxy=%p\n", (void*) host,
          kVkdIfaceName[self->iface], (void*) self);
    g_live.fetch_sub(1, std::memory_order_relaxed);
    delete self;
    vkd3d_proxy_host_release(host); /* outside the lock: never cross holding one */
    return 0;
}

extern "C" int32_t vkd3d_proxy_qi(Proxy* self, const void* riid, void** ppvObject) {
    if (!ppvObject)
        return VKD3D_E_POINTER;
    *ppvObject = nullptr;
    if (!self || !riid)
        return VKD3D_E_POINTER;

    uint32_t want = vkd3d_iface_from_iid(riid);
    if (want == VKD3D_IFACE_INVALID) {
        /* We have no vtable for it, so we could not hand the guest a usable
         * pointer even if vkd3d supported it.  Answering here rather than after
         * a crossing gives the same result for less; games query vendor-private
         * IIDs constantly.  VKD3D_THUNK_TRACE=1 shows what was asked for. */
        const uint32_t* g = static_cast<const uint32_t*>(riid);
        TRACE("QI unknown iid %08x-... on %s\n", g[0], kVkdIfaceName[self->iface]);
        return VKD3D_E_NOINTERFACE;
    }

    if (want == self->iface) {
        /* COM requires QueryInterface for a given IID to return the same
         * pointer every time, so this cannot disagree with the host. */
        vkd3d_proxy_addref(self);
        *ppvObject = self;
        return VKD3D_S_OK;
    }

    uint64_t out = 0;
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    a[0] = reinterpret_cast<uint64_t>(riid);
    a[1] = reinterpret_cast<uint64_t>(&out);
    /* Both pointers are guest addresses the host writes through directly.  That
     * is only legal because FEX shares the address space; if the boundary ever
     * becomes a copying one, these two are the marshalling points. */
    int32_t hr = int32_t(vkd3d_thunk_call(self->iface, 0, self->host, a));
    if (hr < 0)
        return hr;
    if (!out)
        return VKD3D_E_NOINTERFACE;

    *ppvObject = vkd3d_proxy_wrap(out, want); /* consumes the host reference */
    return *ppvObject ? VKD3D_S_OK : VKD3D_E_NOINTERFACE;
}

/* ------------------------------------------- the three float-class shapes */

/* The generic path packs every argument into a uint64_t, which puts a
 * float-class value in a GPR.  Both ABIs pass it in an FP register -- xmm0-7 on
 * x86-64 SysV, f1-f13 on ELFv2 -- so the callee would read an unrelated
 * register.  These are the only three prototypes in the whole D3D12 surface (23
 * vtable slots, because every ID3D12GraphicsCommandList version inherits them),
 * and both halves are hand-written with the exact prototype so the compiler
 * places the arguments.
 *
 * Note ClearDepthStencilView's first parameter: a by-value
 * D3D12_CPU_DESCRIPTOR_HANDLE.  It is declared here with its real 8-byte shape,
 * so it occupies an INTEGER position and `depth` is the fourth argument -- which
 * is why an MS-x64 caller puts it in XMM3 and a SysV caller in XMM0.  The bit
 * copy into args[] goes through vkd3d_agg_bits(). */

extern "C" VKD3D_FSTUB void vkd3d_fstub_ClearDepthStencilView(
        Proxy* self, VkdCpuDescriptorHandle dsv, uint32_t flags, float depth,
        uint8_t stencil, uint32_t rect_count, const void* rects) {
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    a[0] = vkd3d_agg_bits(dsv);
    a[1] = flags;
    a[2] = stencil;
    a[3] = rect_count;
    a[4] = reinterpret_cast<uint64_t>(rects);
    float f[VKD3D_THUNK_FLOAT_ARGS] = {depth, 0.0f, 0.0f};
    vkd3d_thunk_call_float(self->iface,
                           kVkdFloatSlot[VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW],
                           VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW, self->host,
                           a, f, nullptr);
}

extern "C" VKD3D_FSTUB void vkd3d_fstub_OMSetDepthBounds(
        Proxy* self, float min_depth, float max_depth) {
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    float f[VKD3D_THUNK_FLOAT_ARGS] = {min_depth, max_depth, 0.0f};
    vkd3d_thunk_call_float(self->iface,
                           kVkdFloatSlot[VKD3D_FSHAPE_OM_SET_DEPTH_BOUNDS],
                           VKD3D_FSHAPE_OM_SET_DEPTH_BOUNDS, self->host,
                           a, f, nullptr);
}

extern "C" VKD3D_FSTUB void vkd3d_fstub_RSSetDepthBias(
        Proxy* self, float bias, float clamp, float slope_scaled) {
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    float f[VKD3D_THUNK_FLOAT_ARGS] = {bias, clamp, slope_scaled};
    vkd3d_thunk_call_float(self->iface,
                           kVkdFloatSlot[VKD3D_FSHAPE_RS_SET_DEPTH_BIAS],
                           VKD3D_FSHAPE_RS_SET_DEPTH_BIAS, self->host,
                           a, f, nullptr);
}

/* ------------------------------------------- cross-runtime interop exports */

/* dxvk's DXGI shim holds one of our ID3D12CommandQueue proxies and must hand
 * NATIVE dxvk-dxgi the HOST pointer underneath it (CreateSwapChainForHwnd ->
 * IDXGIVkSwapChainFactory).  Unlike vkd3d_proxy_unwrap(), which is called from
 * generated stubs whose argument is ours by construction, this one is called
 * with a pointer from ANOTHER project's code: it looks the pointer up in the
 * interning table and returns 0 rather than guessing. */
extern "C" VKD3D_EXPORT uint64_t vkd3d_thunk_unwrap(void* maybe_proxy) {
    if (!maybe_proxy)
        return 0;
    Proxy* p = static_cast<Proxy*>(maybe_proxy);
    /* p->host and p->iface are read only to derive a table key.  If the caller
     * handed us something that is not one of ours they are meaningless -- but
     * they are never dereferenced, and the lookup below has to map that key
     * back to THIS exact object before anything is returned.  A pointer that is
     * not in the table gets 0, which is the promise this entry point makes. */
    const uint64_t host  = p->host;
    const uint32_t iface = p->iface;
    if (!host || iface >= VKD3D_IFACE_COUNT)
        return 0;

    Shard& sh = shard_for(host);
    std::lock_guard<std::mutex> lk(sh.m);
    auto it = sh.map.find(Key{host, iface});
    if (it == sh.map.end() || it->second != p)
        return 0;
    return host;
}

extern "C" VKD3D_EXPORT void* vkd3d_thunk_wrap(uint64_t host, uint32_t iface_id) {
    /* Same ownership rule as QueryInterface: this consumes one host reference. */
    return vkd3d_proxy_wrap(host, iface_id);
}

extern "C" VKD3D_EXPORT uint32_t vkd3d_thunk_interop_version(void) {
    return 1;
}

/* -------------------------------------------------- the eight flat exports */

/* These are the only symbols the PE shim (or a guest ELF consumer) links
 * against; everything else is vtable dispatch.  Types are spelled as opaque
 * pointers and fixed-width integers: the guest side has no d3d12.h, and does
 * not need one, because every one of these arguments is integer-class and the
 * descriptor structs behind the pointers are layout-identical on both sides. */

extern "C" VKD3D_EXPORT int32_t D3D12CreateDevice(
        void* adapter, uint32_t minimum_feature_level, const void* riid,
        void** device) {
    if (!device)
        return VKD3D_E_POINTER;
    *device = nullptr;

    uint32_t want = vkd3d_iface_from_iid(riid);
    if (want == VKD3D_IFACE_INVALID)
        return VKD3D_E_NOINTERFACE;   /* no vtable, so nothing usable to return */

    if (adapter) {
        /* The native build ignores the adapter entirely (libs/d3d12core/main.c,
         * "FIXME("Ignoring adapter.")"): device selection is vkd3d's own
         * enumeration, steered by VKD3D_FILTER_DEVICE_NAME / VKD3D_VULKAN_DEVICE.
         * Forwarding the guest's IDXGIAdapter would hand native code a pointer
         * from a DXGI implementation this thunk knows nothing about, so v1
         * passes NULL and says so.  See d3d12-boundary-analysis.md §1. */
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "vkd3d_thunk: FIXME: D3D12CreateDevice adapter "
                         "ignored (native vkd3d ignores it too; device selection "
                         "is VKD3D_FILTER_DEVICE_NAME / VKD3D_VULKAN_DEVICE)\n");
        });
    }

    uint64_t out = 0;
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    a[0] = 0;                                       /* adapter, forced NULL */
    a[1] = minimum_feature_level;
    a[2] = reinterpret_cast<uint64_t>(riid);
    a[3] = reinterpret_cast<uint64_t>(&out);

    int32_t hr = int32_t(vkd3d_thunk_call_entry(VKD3D_ENTRY_CREATE_DEVICE, a));
    if (hr < 0 || !out)
        return hr < 0 ? hr : VKD3D_E_FAIL;
    *device = vkd3d_proxy_wrap(out, want);
    return *device ? VKD3D_S_OK : VKD3D_E_NOINTERFACE;
}

/* The riid-driven entries all have the same shape: resolve the IID first, refuse
 * an unknown one without crossing (exactly QueryInterface's policy), cross with
 * a private out-slot, wrap the result. */
static int32_t vkd3d_riid_entry(uint32_t entry, const uint64_t* lead,
                                unsigned nlead, const void* riid, void** out_p) {
    if (!out_p)
        return VKD3D_E_POINTER;
    *out_p = nullptr;

    uint32_t want = vkd3d_iface_from_iid(riid);
    if (want == VKD3D_IFACE_INVALID)
        return VKD3D_E_NOINTERFACE;

    uint64_t out = 0;
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    unsigned i = 0;
    for (; i < nlead; i++)
        a[i] = lead[i];
    a[i++] = reinterpret_cast<uint64_t>(riid);
    a[i++] = reinterpret_cast<uint64_t>(&out);

    int32_t hr = int32_t(vkd3d_thunk_call_entry(entry, a));
    if (hr < 0 || !out)
        return hr < 0 ? hr : VKD3D_E_FAIL;
    *out_p = vkd3d_proxy_wrap(out, want);
    return *out_p ? VKD3D_S_OK : VKD3D_E_NOINTERFACE;
}

extern "C" VKD3D_EXPORT int32_t D3D12GetDebugInterface(const void* riid,
                                                       void** debug) {
    return vkd3d_riid_entry(VKD3D_ENTRY_GET_DEBUG_INTERFACE, nullptr, 0, riid,
                            debug);
}

extern "C" VKD3D_EXPORT int32_t D3D12GetInterface(const void* rclsid,
                                                  const void* riid, void** out) {
    uint64_t lead[1] = {reinterpret_cast<uint64_t>(rclsid)};
    return vkd3d_riid_entry(VKD3D_ENTRY_GET_INTERFACE, lead, 1, riid, out);
}

extern "C" VKD3D_EXPORT int32_t D3D12CreateRootSignatureDeserializer(
        const void* data, uint64_t data_size, const void* riid,
        void** deserializer) {
    uint64_t lead[2] = {reinterpret_cast<uint64_t>(data), data_size};
    return vkd3d_riid_entry(VKD3D_ENTRY_CREATE_ROOT_SIG_DESERIALIZER, lead, 2,
                            riid, deserializer);
}

extern "C" VKD3D_EXPORT int32_t D3D12CreateVersionedRootSignatureDeserializer(
        const void* data, uint64_t data_size, const void* riid,
        void** deserializer) {
    uint64_t lead[2] = {reinterpret_cast<uint64_t>(data), data_size};
    return vkd3d_riid_entry(VKD3D_ENTRY_CREATE_VERSIONED_RS_DESER, lead, 2,
                            riid, deserializer);
}

/* The two serialisers return ID3DBlob** out-parameters.  ID3DBlob is ID3D10Blob
 * -- the census carries it under that name -- so the interface is known
 * statically and there is no IID to resolve. */
extern "C" VKD3D_EXPORT int32_t D3D12SerializeRootSignature(
        const void* root_signature_desc, uint32_t version, void** blob,
        void** error_blob) {
    uint64_t hblob = 0, herr = 0;
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    a[0] = reinterpret_cast<uint64_t>(root_signature_desc);
    a[1] = version;
    a[2] = blob ? reinterpret_cast<uint64_t>(&hblob) : 0;
    a[3] = error_blob ? reinterpret_cast<uint64_t>(&herr) : 0;

    int32_t hr = int32_t(vkd3d_thunk_call_entry(VKD3D_ENTRY_SERIALIZE_ROOT_SIG, a));
    if (blob)
        *blob = vkd3d_proxy_wrap(hblob, VKD3D_IFACE_ID3D10BLOB);
    if (error_blob)
        *error_blob = vkd3d_proxy_wrap(herr, VKD3D_IFACE_ID3D10BLOB);
    return hr;
}

extern "C" VKD3D_EXPORT int32_t D3D12SerializeVersionedRootSignature(
        const void* desc, void** blob, void** error_blob) {
    uint64_t hblob = 0, herr = 0;
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    a[0] = reinterpret_cast<uint64_t>(desc);
    a[1] = blob ? reinterpret_cast<uint64_t>(&hblob) : 0;
    a[2] = error_blob ? reinterpret_cast<uint64_t>(&herr) : 0;

    int32_t hr = int32_t(vkd3d_thunk_call_entry(
            VKD3D_ENTRY_SERIALIZE_VERSIONED_ROOT_SIG, a));
    if (blob)
        *blob = vkd3d_proxy_wrap(hblob, VKD3D_IFACE_ID3D10BLOB);
    if (error_blob)
        *error_blob = vkd3d_proxy_wrap(herr, VKD3D_IFACE_ID3D10BLOB);
    return hr;
}

extern "C" VKD3D_EXPORT int32_t D3D12EnableExperimentalFeatures(
        uint32_t feature_count, const void* iids, void* configurations,
        uint32_t* configuration_sizes) {
    /* Data only -- an array of IIDs the caller wants enabled, plus opaque
     * configuration blobs.  No interface pointers in either direction. */
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    a[0] = feature_count;
    a[1] = reinterpret_cast<uint64_t>(iids);
    a[2] = reinterpret_cast<uint64_t>(configurations);
    a[3] = reinterpret_cast<uint64_t>(configuration_sizes);
    return int32_t(vkd3d_thunk_call_entry(VKD3D_ENTRY_ENABLE_EXPERIMENTAL_FEATURES, a));
}
