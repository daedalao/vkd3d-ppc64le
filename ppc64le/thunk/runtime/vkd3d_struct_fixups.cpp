/* HAND-MAINTAINED -- not generated.
 *
 * The struct fixups: the slots whose PARAMETER STRUCTS embed interface
 * pointers.
 *
 * Everywhere else in this thunk an aggregate crosses by pointer, untouched,
 * because both ABIs lay it out identically (ppc64le/layout/ proves that byte
 * for byte).  That is wrong for eight methods, because the bytes inside the
 * aggregate include an ID3D12Resource* or an ID3D12RootSignature*, and on this
 * side of the boundary those are guest Proxy* -- addresses of OUR objects, not
 * of vkd3d's.  So for exactly these slots the aggregate is COPIED to scratch,
 * the interface members of the copy are rewritten to host pointers, and the
 * copy is what crosses.  The guest's original is never written to.
 *
 * The copy is legal because every one of these slots is copy-in: D3D12 forbids
 * the runtime from retaining the pointer past the call (see
 * ppc64le/docs/struct-fixup-census.md, design rule 4), which is also why
 * CreatePipelineState's subobject stream may be copied.
 *
 * WIRING.  There is none at run time.  gen_thunk.py's FIXUP_SLOTS table makes
 * hand_written() name the symbols below, so the generated k<I>_target and
 * k<I>_vtbl_sysv arrays point straight here and the generic ms_abi forwarder
 * calls through target[] -- the same mechanism the three float-class shapes
 * have always used.  The declarations these definitions must match are
 * GENERATED into vkd3d_thunk_ids.h from the census, so a stub with the wrong
 * arity does not compile.
 *
 * THIS IS THE ONE FILE IN THE RUNTIME ALLOWED TO SEE d3d12 TYPES.  The
 * generated code and the rest of runtime/ stay header-free, exactly as before;
 * that is what lets them be compiled for the guest with no Windows SDK
 * anywhere.  The sizes of every load-bearing struct are static_assert'ed
 * against ppc64le/layout/x86_64.txt below, so a widl regeneration that changed
 * one stops the build here rather than at a game's first draw call.
 */
#define COM_NO_WINDOWS_H
#include <vkd3d_windows.h>
#include <vkd3d_dxgiformat.h>
#include <vkd3d_dxgibase.h>
#include <vkd3d_d3dcommon.h>
#include <vkd3d_d3d12.h>

#include "vkd3d_proxy.h"
#include "vkd3d_thunk_abi.h"

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define VKD3D_FIXUP __attribute__((visibility("hidden"), used))

/* ------------------------------------------------------------- layout ----
 *
 * Measured, not assumed.  Every number below is the `RECORD <name> size=`
 * line from ppc64le/layout/x86_64.txt, produced by ppc64le/layout/probe.c from
 * these same headers; ppc64le.txt carries the identical numbers, which is the
 * whole premise of passing these structs across at all.  The guest builds
 * them, the fixups copy them, native ppc64le vkd3d-proton reads them.
 */
static_assert(sizeof(D3D12_RESOURCE_BARRIER) == 32,
              "RECORD D3D12_RESOURCE_BARRIER size=32");
static_assert(sizeof(D3D12_TEXTURE_COPY_LOCATION) == 48,
              "RECORD D3D12_TEXTURE_COPY_LOCATION size=48");
static_assert(sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC) == 656,
              "RECORD D3D12_GRAPHICS_PIPELINE_STATE_DESC size=656");
static_assert(sizeof(D3D12_COMPUTE_PIPELINE_STATE_DESC) == 56,
              "RECORD D3D12_COMPUTE_PIPELINE_STATE_DESC size=56");
static_assert(sizeof(D3D12_PIPELINE_STATE_STREAM_DESC) == 16,
              "RECORD D3D12_PIPELINE_STATE_STREAM_DESC size=16");
static_assert(sizeof(D3D12_RENDER_PASS_RENDER_TARGET_DESC) == 88,
              "RECORD D3D12_RENDER_PASS_RENDER_TARGET_DESC size=88");
static_assert(sizeof(D3D12_RENDER_PASS_DEPTH_STENCIL_DESC) == 168,
              "RECORD D3D12_RENDER_PASS_DEPTH_STENCIL_DESC size=168");
static_assert(sizeof(D3D12_RENDER_PASS_ENDING_ACCESS) == 56,
              "RECORD D3D12_RENDER_PASS_ENDING_ACCESS size=56");
static_assert(sizeof(D3D12_BARRIER_GROUP) == 16,
              "RECORD D3D12_BARRIER_GROUP size=16");
static_assert(sizeof(D3D12_TEXTURE_BARRIER) == 64,
              "RECORD D3D12_TEXTURE_BARRIER size=64");
static_assert(sizeof(D3D12_BUFFER_BARRIER) == 40,
              "RECORD D3D12_BUFFER_BARRIER size=40");
static_assert(sizeof(D3D12_GLOBAL_BARRIER) == 16,
              "RECORD D3D12_GLOBAL_BARRIER size=16");
/* The subobject-stream walker's arithmetic is in terms of these two. */
static_assert(sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) == 4,
              "ENUM D3D12_PIPELINE_STATE_SUBOBJECT_TYPE size=4");
static_assert(sizeof(void*) == 8, "the stream walker aligns to sizeof(void*)");

namespace {

/* ------------------------------------------------------- scratch sizes ----
 * Design rule 2: an inline stack array for the common case, the heap above it,
 * and abort-with-message rather than a short copy if the heap says no.  A
 * truncated array handed to vkd3d is a host-side overrun; there is no safe way
 * to continue, and the same policy already governs vkd3d_ifarray_in(). */
#define VKD3D_FIXUP_BARRIERS_INLINE        32u   /* ResourceBarrier */
#define VKD3D_FIXUP_RT_INLINE               8u   /* BeginRenderPass */
#define VKD3D_FIXUP_GROUPS_INLINE           4u   /* Barrier */
#define VKD3D_FIXUP_GROUP_BARRIERS_INLINE  16u   /* ... per group */
#define VKD3D_FIXUP_STREAM_INLINE        1024u   /* CreatePipelineState bytes */

/* CreatePipelineState's SizeInBytes comes from the guest and is only bounded
 * by SIZE_T.  A real pipeline stream is tens to a few hundred bytes; 1 MiB is
 * four orders of magnitude of headroom and still small enough that a corrupt
 * or hostile value cannot turn into a multi-gigabyte copy. */
#define VKD3D_FIXUP_STREAM_MAX  (1u << 20)

/* ------------------------------------------------------------ counters ----
 * Test hooks, and the evidence for the report.  Relaxed: they are diagnostics,
 * not synchronisation. */
std::atomic<uint64_t> g_foreign{0};   /* non-proxy members forwarded unchanged */
std::atomic<uint64_t> g_warns{0};     /* stderr lines emitted */
std::atomic<uint64_t> g_heap{0};      /* scratch copies that needed malloc */
std::atomic<uint64_t> g_refused{0};   /* calls answered without crossing */

/* Warning reasons, one once-per-slot flag each.  "Once per slot" and not once
 * per call: a game that hands us a foreign resource does it every frame. */
enum WarnReason {
    WARN_FOREIGN = 0,       /* a member that is not one of our proxies */
    WARN_UNKNOWN_UNION,     /* a union discriminant we do not know */
    WARN_REFUSED,           /* the call was answered with E_INVALIDARG */
    WARN_REASON_COUNT
};

std::atomic<uint32_t> g_warned[VKD3D_FIXUP_COUNT][WARN_REASON_COUNT];

void warn_once(uint32_t kind, WarnReason why, const char* fmt, ...)
        __attribute__((format(printf, 3, 4)));

void warn_once(uint32_t kind, WarnReason why, const char* fmt, ...) {
    uint32_t expected = 0;
    if (!g_warned[kind][why].compare_exchange_strong(expected, 1u))
        return;
    g_warns.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "vkd3d_thunk: FIXUP %s: ", kVkdFixupName[kind]);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

[[noreturn]] void fixup_oom(uint32_t kind, const char* what, size_t bytes) {
    std::fprintf(stderr, "vkd3d_thunk: FIXUP %s: out of memory for %zu bytes of "
                 "%s scratch.  A short copy would hand vkd3d an array it will "
                 "read off the end of, so this is fatal.\n",
                 kVkdFixupName[kind], bytes, what);
    std::fflush(stderr);
    std::abort();
}

/* --------------------------------------------------------------- unwrap ---
 *
 * Design rule 3, verbatim: NULL stays NULL; one of our live proxies becomes the
 * host pointer underneath it; anything else non-null is forwarded UNCHANGED
 * with a once-per-slot warning, because a raw host pointer arriving from
 * another runtime across the dxvk-dxgi seam is legal and refusing it would
 * break interop that works today.
 *
 * vkd3d_thunk_unwrap() and not vkd3d_proxy_unwrap(): the generated stubs may
 * trust their argument because its declared type says it came from us, but a
 * pointer found INSIDE a caller-built struct has no such provenance.  The
 * verifying form looks the (host, iface) key up in the interning table and
 * insists the entry is this exact object, so "not ours" is answered rather
 * than guessed.
 *
 * COST, MEASURED (x86-64 dev box, uncontended, 20M calls each): the verifying
 * form is 8.8 ns and the trusting one 1.1 ns, so +7.7 ns per interface member
 * -- roughly 250 ns added to a 32-barrier ResourceBarrier, against a FEX
 * crossing that costs orders of magnitude more.  Nothing here is optimised
 * because the measurement says there is nothing worth buying; what is not
 * measured is that shard lock under real multi-threaded command recording.
 * Any cheaper scheme (a per-proxy magic word, a lock-free read side) is a
 * change to the interning table's contract, not to this file. */
uint64_t unwrap(uint32_t kind, void* p) {
    if (!p)
        return 0;
    uint64_t host = vkd3d_thunk_unwrap(p);
    if (host)
        return host;
    g_foreign.fetch_add(1, std::memory_order_relaxed);
    warn_once(kind, WARN_FOREIGN,
              "%p is not one of our proxies; forwarding it into the struct "
              "UNCHANGED (a host pointer from another runtime is legal here)", p);
    return reinterpret_cast<uint64_t>(p);
}

/* The member form.  Takes the member by reference so the call site reads as
 * what it is -- `unwrap_member(kind, copy.Transition.pResource)` -- and cannot
 * accidentally rewrite the guest's original, which is a different object. */
template <typename T>
void unwrap_member(uint32_t kind, T*& member) {
    member = reinterpret_cast<T*>(static_cast<uintptr_t>(
        unwrap(kind, static_cast<void*>(member))));
}

/* --------------------------------------------------------------- scratch --
 *
 * A bump arena over either an inline buffer or one malloc.  One allocation for
 * a whole call, which matters for Barrier: a group array plus one nested array
 * per group would otherwise be up to five mallocs on a command-list slot. */
constexpr size_t round16(size_t n) { return (n + 15u) & ~size_t(15u); }

struct Arena {
    unsigned char* base = nullptr;
    size_t cap = 0, used = 0;
    void* heap = nullptr;
    uint32_t kind = 0;
    const char* what = "";

    ~Arena() { std::free(heap); }

    void init(uint32_t k, const char* w, void* inl, size_t inl_bytes,
              size_t need) {
        kind = k;
        what = w;
        if (need <= inl_bytes) {
            base = static_cast<unsigned char*>(inl);
            cap = inl_bytes;
        } else {
            heap = std::malloc(need);
            if (!heap)
                fixup_oom(k, w, need);
            g_heap.fetch_add(1, std::memory_order_relaxed);
            base = static_cast<unsigned char*>(heap);
            cap = need;
        }
        used = 0;
    }

    /* Every allocation is 16-byte aligned and 16-byte rounded, and the sizing
     * pass rounds the same way, so the arena cannot be short unless the caller
     * re-read a guest field that changed under it -- which D3D12 forbids for
     * the duration of the call.  If it ever does, this aborts rather than
     * overruns. */
    void* alloc(size_t bytes) {
        size_t take = round16(bytes);
        if (used + take > cap)
            fixup_oom(kind, what, used + take);
        void* p = base + used;
        used += take;
        return p;
    }
};

/* ------------------------------------------------------------- crossing ---
 * self->iface picks the interface, kVkdFixupSlot[kind] the slot: the same
 * derivation the float stubs use, so ResourceBarrier is slot 26 of whichever
 * of the eleven command-list interfaces the caller actually holds. */
uint64_t cross(Proxy* self, uint32_t kind, const uint64_t* args, unsigned n) {
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    for (unsigned i = 0; i < n && i < VKD3D_THUNK_ARGS; i++)
        a[i] = args[i];
    return vkd3d_thunk_call(self->iface, kVkdFixupSlot[kind], self->host, a);
}

/* HRESULT-returning refusal: no crossing, out-parameter nulled. */
uint64_t refuse(uint32_t kind, uint64_t ppv, const char* fmt, ...)
        __attribute__((format(printf, 3, 4)));

uint64_t refuse(uint32_t kind, uint64_t ppv, const char* fmt, ...) {
    g_refused.fetch_add(1, std::memory_order_relaxed);
    uint32_t expected = 0;
    if (g_warned[kind][WARN_REFUSED].compare_exchange_strong(expected, 1u)) {
        g_warns.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "vkd3d_thunk: FIXUP %s: ", kVkdFixupName[kind]);
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stderr, fmt, ap);
        va_end(ap);
        std::fprintf(stderr, "; returning E_INVALIDARG without crossing "
                             "(vkd3d would reject it too)\n");
    }
    if (ppv)
        *reinterpret_cast<void**>(static_cast<uintptr_t>(ppv)) = nullptr;
    return static_cast<uint64_t>(static_cast<uint32_t>(VKD3D_E_INVALIDARG));
}

/* --------------------------------------------------------- the riid dance --
 * Identical to what gen_thunk.py emits for every riid-driven void** out (read
 * ID3D12Device_8_CreateCommandQueue in the generated guest file): resolve the
 * IID FIRST so an unknown one costs nothing and never crosses, cross with a
 * private out-slot so the host never sees the guest's own pointer, wrap what
 * came back.  wrap() consumes the host reference the callee gave us. */
struct RiidOut {
    uint32_t iface;
    uint64_t host;
};

bool riid_begin(RiidOut* r, uint64_t riid, uint64_t ppv) {
    r->host = 0;
    r->iface = vkd3d_iface_from_iid(
        reinterpret_cast<const void*>(static_cast<uintptr_t>(riid)));
    if (r->iface == VKD3D_IFACE_INVALID) {
        if (ppv)
            *reinterpret_cast<void**>(static_cast<uintptr_t>(ppv)) = nullptr;
        return false;
    }
    return true;
}

void riid_end(const RiidOut* r, uint64_t ppv) {
    if (ppv)
        *reinterpret_cast<void**>(static_cast<uintptr_t>(ppv)) =
            vkd3d_proxy_wrap(r->host, r->iface);
}

} /* namespace */

/* ======================================================================== */
/* ID3D12GraphicsCommandList::ResourceBarrier -- the hottest call in D3D12    */
/* ======================================================================== */

extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_ResourceBarrier(
        Proxy* self, uint64_t count, uint64_t barriers) {
    const uint32_t kind = VKD3D_FIXUP_RESOURCE_BARRIER;
    const uint32_t n = static_cast<uint32_t>(count);
    const D3D12_RESOURCE_BARRIER* src =
        reinterpret_cast<const D3D12_RESOURCE_BARRIER*>(
            static_cast<uintptr_t>(barriers));

    if (!n || !src) {
        uint64_t a[2] = {count, barriers};
        return cross(self, kind, a, 2);
    }

    alignas(16) unsigned char inl[VKD3D_FIXUP_BARRIERS_INLINE *
                                  sizeof(D3D12_RESOURCE_BARRIER)];
    Arena ar;
    ar.init(kind, "resource barrier", inl, sizeof(inl),
            round16(size_t(n) * sizeof(D3D12_RESOURCE_BARRIER)));
    D3D12_RESOURCE_BARRIER* copy = static_cast<D3D12_RESOURCE_BARRIER*>(
        ar.alloc(size_t(n) * sizeof(D3D12_RESOURCE_BARRIER)));
    std::memcpy(copy, src, size_t(n) * sizeof(D3D12_RESOURCE_BARRIER));

    for (uint32_t i = 0; i < n; i++) {
        switch (copy[i].Type) {
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
            unwrap_member(kind, copy[i].Transition.pResource);
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
            unwrap_member(kind, copy[i].Aliasing.pResourceBefore);
            unwrap_member(kind, copy[i].Aliasing.pResourceAfter);
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_UAV:
            unwrap_member(kind, copy[i].UAV.pResource);
            break;
        default:
            /* An unknown discriminant means we do not know which union arm is
             * live, so we do not know which bytes are a pointer.  Forward the
             * barrier's bytes exactly as the guest wrote them and say so once:
             * guessing would corrupt whatever is really there. */
            warn_once(kind, WARN_UNKNOWN_UNION,
                      "barrier %u has D3D12_RESOURCE_BARRIER_TYPE %u, which "
                      "this build does not know; its bytes cross unchanged",
                      i, unsigned(copy[i].Type));
            break;
        }
    }

    uint64_t a[2] = {count, reinterpret_cast<uint64_t>(copy)};
    return cross(self, kind, a, 2);
}

/* ======================================================================== */
/* ID3D12GraphicsCommandList::CopyTextureRegion                              */
/* ======================================================================== */

extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_CopyTextureRegion(
        Proxy* self, uint64_t dst, uint64_t dst_x, uint64_t dst_y,
        uint64_t dst_z, uint64_t src, uint64_t src_box) {
    const uint32_t kind = VKD3D_FIXUP_COPY_TEXTURE_REGION;

    /* Two 48-byte structs on the stack; no allocation can be needed here.  The
     * union (SubresourceIndex or PlacedFootprint) is copied whole and never
     * interpreted -- only .pResource, which sits before it, is rewritten. */
    D3D12_TEXTURE_COPY_LOCATION dcopy, scopy;
    uint64_t dp = dst, sp = src;

    if (dst) {
        std::memcpy(&dcopy, reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(dst)), sizeof(dcopy));
        unwrap_member(kind, dcopy.pResource);
        dp = reinterpret_cast<uint64_t>(&dcopy);
    }
    if (src) {
        std::memcpy(&scopy, reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(src)), sizeof(scopy));
        unwrap_member(kind, scopy.pResource);
        sp = reinterpret_cast<uint64_t>(&scopy);
    }

    /* src_box is a D3D12_BOX: six UINTs, no interface anywhere.  It passes
     * through as the guest's own pointer. */
    uint64_t a[6] = {dp, dst_x, dst_y, dst_z, sp, src_box};
    return cross(self, kind, a, 6);
}

/* ======================================================================== */
/* The pipeline-state descriptors: one ID3D12RootSignature* each              */
/* ======================================================================== */

namespace {

/* D3D12_GRAPHICS_PIPELINE_STATE_DESC (656 B) and
 * D3D12_COMPUTE_PIPELINE_STATE_DESC (56 B) both begin with
 * `ID3D12RootSignature *pRootSignature`, and everything else in them is either
 * a scalar or a GUEST pointer the host reads directly (shader bytecode, the
 * input-element array, the cached blob).  Those are left byte-identical: they
 * point at guest memory, which under FEX's shared address space is exactly
 * what native vkd3d must dereference.
 *
 * pRootSignature == NULL is legal and common -- a shader can embed its own
 * root signature -- so a null there is not a diagnostic, it is a value. */
template <typename Desc>
uint64_t pipeline_desc_fixup(Proxy* self, uint32_t kind, const uint64_t* lead,
                             unsigned nlead, uint64_t desc, uint64_t riid,
                             uint64_t ppv) {
    RiidOut r;
    if (!riid_begin(&r, riid, ppv))
        return static_cast<uint64_t>(static_cast<uint32_t>(VKD3D_E_NOINTERFACE));

    Desc copy;
    uint64_t dp = desc;
    if (desc) {
        std::memcpy(&copy, reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(desc)), sizeof(copy));
        unwrap_member(kind, copy.pRootSignature);
        dp = reinterpret_cast<uint64_t>(&copy);
    }

    uint64_t a[VKD3D_THUNK_ARGS] = {};
    unsigned i = 0;
    for (; i < nlead; i++)
        a[i] = lead[i];
    a[i++] = dp;
    a[i++] = riid;
    a[i++] = ppv ? reinterpret_cast<uint64_t>(&r.host) : 0;

    uint64_t hr = cross(self, kind, a, i);
    riid_end(&r, ppv);
    return hr;
}

} /* namespace */

extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_CreateGraphicsPipelineState(
        Proxy* self, uint64_t desc, uint64_t riid, uint64_t ppv) {
    return pipeline_desc_fixup<D3D12_GRAPHICS_PIPELINE_STATE_DESC>(
        self, VKD3D_FIXUP_CREATE_GRAPHICS_PIPELINE_STATE, nullptr, 0,
        desc, riid, ppv);
}

extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_CreateComputePipelineState(
        Proxy* self, uint64_t desc, uint64_t riid, uint64_t ppv) {
    return pipeline_desc_fixup<D3D12_COMPUTE_PIPELINE_STATE_DESC>(
        self, VKD3D_FIXUP_CREATE_COMPUTE_PIPELINE_STATE, nullptr, 0,
        desc, riid, ppv);
}

extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_LoadGraphicsPipeline(
        Proxy* self, uint64_t name, uint64_t desc, uint64_t riid, uint64_t ppv) {
    /* The name is an LPCWSTR into guest memory: a pointer, not a struct, and
     * the host only reads it. */
    uint64_t lead[1] = {name};
    return pipeline_desc_fixup<D3D12_GRAPHICS_PIPELINE_STATE_DESC>(
        self, VKD3D_FIXUP_LOAD_GRAPHICS_PIPELINE, lead, 1, desc, riid, ppv);
}

extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_LoadComputePipeline(
        Proxy* self, uint64_t name, uint64_t desc, uint64_t riid, uint64_t ppv) {
    uint64_t lead[1] = {name};
    return pipeline_desc_fixup<D3D12_COMPUTE_PIPELINE_STATE_DESC>(
        self, VKD3D_FIXUP_LOAD_COMPUTE_PIPELINE, lead, 1, desc, riid, ppv);
}

/* ======================================================================== */
/* The pipeline-state SUBOBJECT STREAM                                       */
/* ======================================================================== */

namespace {

/* D3D12_PIPELINE_STATE_STREAM_DESC hides its contents behind a void*, so no
 * member scan can see the ID3D12RootSignature* inside it; gen_thunk.py names
 * the struct in OPAQUE_IFACE_STRUCTS for exactly that reason.
 *
 * The layout rules below are NOT invented here.  They are
 * vkd3d_pipeline_state_desc_from_d3d12_stream_desc() in libs/vkd3d/state.c,
 * which is the code that will parse this stream on the other side of the
 * boundary, transcribed:
 *
 *   - a subobject is `struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
 *     PAYLOAD data; }`, so the payload's offset is whatever the compiler gives
 *     that anonymous struct -- 4 for a UINT payload, 8 for a pointer-bearing
 *     one.  Never assume 4 or 8; ask the compiler, as the macro there does.
 *   - the next subobject starts at align(sizeof(that struct), sizeof(void*)).
 *   - a subobject that does not fit before stream_end is E_INVALIDARG.
 *   - a repeated type is E_INVALIDARG.
 *   - an unhandled type is E_INVALIDARG.
 *
 * If this walker and that function ever disagree about a size, the rewrite
 * lands on the wrong bytes, so the two are kept identical by construction:
 * one macro, one case per type, the same 28 types in the same order.
 */
struct SubobjectInfo {
    size_t   raw;          /* sizeof(struct {type; payload;}) */
    size_t   advance;      /* align(raw, sizeof(void*)) */
    size_t   payload_off;  /* offsetof(that struct, data) */
    bool     known;
    bool     root_sig;
};

constexpr size_t align_ptr(size_t n) {
    return (n + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
}

#define VKD3D_FIXUP_SUBOBJECT(type_enum, type_name)                           \
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##type_enum: {                   \
        struct Sub {                                                          \
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;                         \
            type_name data;                                                   \
        };                                                                    \
        info.raw = sizeof(Sub);                                               \
        info.advance = align_ptr(sizeof(Sub));                                \
        info.payload_off = offsetof(Sub, data);                               \
        info.known = true;                                                    \
        break;                                                                \
    }

SubobjectInfo subobject_info(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t) {
    SubobjectInfo info = {0, 0, 0, false, false};
    switch (t) {
    VKD3D_FIXUP_SUBOBJECT(ROOT_SIGNATURE, ID3D12RootSignature*)
    VKD3D_FIXUP_SUBOBJECT(VS, D3D12_SHADER_BYTECODE)
    VKD3D_FIXUP_SUBOBJECT(PS, D3D12_SHADER_BYTECODE)
    VKD3D_FIXUP_SUBOBJECT(DS, D3D12_SHADER_BYTECODE)
    VKD3D_FIXUP_SUBOBJECT(HS, D3D12_SHADER_BYTECODE)
    VKD3D_FIXUP_SUBOBJECT(GS, D3D12_SHADER_BYTECODE)
    VKD3D_FIXUP_SUBOBJECT(CS, D3D12_SHADER_BYTECODE)
    VKD3D_FIXUP_SUBOBJECT(AS, D3D12_SHADER_BYTECODE)
    VKD3D_FIXUP_SUBOBJECT(MS, D3D12_SHADER_BYTECODE)
    VKD3D_FIXUP_SUBOBJECT(STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC)
    VKD3D_FIXUP_SUBOBJECT(BLEND, D3D12_BLEND_DESC)
    VKD3D_FIXUP_SUBOBJECT(SAMPLE_MASK, UINT)
    VKD3D_FIXUP_SUBOBJECT(RASTERIZER, D3D12_RASTERIZER_DESC)
    VKD3D_FIXUP_SUBOBJECT(DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC)
    VKD3D_FIXUP_SUBOBJECT(INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC)
    VKD3D_FIXUP_SUBOBJECT(IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)
    VKD3D_FIXUP_SUBOBJECT(PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE)
    VKD3D_FIXUP_SUBOBJECT(RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY)
    VKD3D_FIXUP_SUBOBJECT(DEPTH_STENCIL_FORMAT, DXGI_FORMAT)
    VKD3D_FIXUP_SUBOBJECT(SAMPLE_DESC, DXGI_SAMPLE_DESC)
    VKD3D_FIXUP_SUBOBJECT(NODE_MASK, UINT)
    VKD3D_FIXUP_SUBOBJECT(CACHED_PSO, D3D12_CACHED_PIPELINE_STATE)
    VKD3D_FIXUP_SUBOBJECT(FLAGS, D3D12_PIPELINE_STATE_FLAGS)
    VKD3D_FIXUP_SUBOBJECT(DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1)
    VKD3D_FIXUP_SUBOBJECT(VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC)
    VKD3D_FIXUP_SUBOBJECT(DEPTH_STENCIL2, D3D12_DEPTH_STENCIL_DESC2)
    VKD3D_FIXUP_SUBOBJECT(RASTERIZER1, D3D12_RASTERIZER_DESC1)
    VKD3D_FIXUP_SUBOBJECT(RASTERIZER2, D3D12_RASTERIZER_DESC2)
    default:
        break;
    }
    info.root_sig = (t == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE);
    return info;
}

#undef VKD3D_FIXUP_SUBOBJECT

uint64_t pipeline_stream_fixup(Proxy* self, uint32_t kind, const uint64_t* lead,
                               unsigned nlead, uint64_t descp, uint64_t riid,
                               uint64_t ppv) {
    RiidOut r;
    if (!riid_begin(&r, riid, ppv))
        return static_cast<uint64_t>(static_cast<uint32_t>(VKD3D_E_NOINTERFACE));

    const D3D12_PIPELINE_STATE_STREAM_DESC* src =
        reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DESC*>(
            static_cast<uintptr_t>(descp));

    uint64_t a[VKD3D_THUNK_ARGS] = {};
    unsigned i = 0;
    for (; i < nlead; i++)
        a[i] = lead[i];

    if (!src || !src->SizeInBytes) {
        /* Nothing to walk.  vkd3d treats an empty stream as "all defaults",
         * which is its business, not ours; pass the descriptor through. */
        a[i++] = descp;
        a[i++] = riid;
        a[i++] = ppv ? reinterpret_cast<uint64_t>(&r.host) : 0;
        uint64_t hr = cross(self, kind, a, i);
        riid_end(&r, ppv);
        return hr;
    }

    if (!src->pPipelineStateSubobjectStream)
        /* A nonzero SizeInBytes with no stream is a caller bug that vkd3d
         * would walk straight off a null pointer.  Refused here instead. */
        return refuse(kind, ppv, "SizeInBytes is %zu with a NULL subobject "
                      "stream", size_t(src->SizeInBytes));

    if (src->SizeInBytes > VKD3D_FIXUP_STREAM_MAX)
        return refuse(kind, ppv, "subobject stream is %zu bytes, over the %u-byte "
                      "sanity cap", size_t(src->SizeInBytes),
                      unsigned(VKD3D_FIXUP_STREAM_MAX));

    const size_t bytes = size_t(src->SizeInBytes);

    /* alignas(void*) is the alignment the stream's own rule demands (design
     * rule 5); malloc's is stricter still. */
    alignas(16) unsigned char inl[VKD3D_FIXUP_STREAM_INLINE];
    Arena ar;
    ar.init(kind, "pipeline subobject stream", inl, sizeof(inl), round16(bytes));
    unsigned char* base = static_cast<unsigned char*>(ar.alloc(bytes));
    std::memcpy(base, src->pPipelineStateSubobjectStream, bytes);

    unsigned char* p = base;
    unsigned char* const end = base + bytes;
    uint64_t defined = 0;

    while (p < end) {
        if (size_t(end - p) < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))
            return refuse(kind, ppv, "subobject stream ends %zu bytes into a "
                          "subobject type token", size_t(end - p));

        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t;
        std::memcpy(&t, p, sizeof(t));

        SubobjectInfo info = subobject_info(t);
        if (!info.known)
            return refuse(kind, ppv, "subobject type %u is not one this build "
                          "knows (vkd3d's stream walker would reject it too)",
                          unsigned(t));

        const unsigned bit = unsigned(t);
        if (bit >= 64)
            return refuse(kind, ppv, "subobject type %u is out of range", bit);
        if (defined & (1ull << bit))
            return refuse(kind, ppv, "subobject type %u appears twice", bit);
        defined |= 1ull << bit;

        if (size_t(end - p) < info.raw)
            return refuse(kind, ppv, "subobject type %u needs %zu bytes and only "
                          "%zu remain", bit, info.raw, size_t(end - p));

        if (info.root_sig) {
            ID3D12RootSignature** rs =
                reinterpret_cast<ID3D12RootSignature**>(p + info.payload_off);
            unwrap_member(kind, *rs);
        }
        /* Every other payload is left byte-identical.  They hold guest
         * pointers -- bytecode, input-element arrays, cached blobs -- that the
         * host dereferences directly, so touching them would be the bug. */

        p += info.advance;
    }

    D3D12_PIPELINE_STATE_STREAM_DESC dcopy;
    dcopy.SizeInBytes = src->SizeInBytes;
    dcopy.pPipelineStateSubobjectStream = base;

    a[i++] = reinterpret_cast<uint64_t>(&dcopy);
    a[i++] = riid;
    a[i++] = ppv ? reinterpret_cast<uint64_t>(&r.host) : 0;

    uint64_t hr = cross(self, kind, a, i);
    riid_end(&r, ppv);
    return hr;
}

} /* namespace */

extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_CreatePipelineState(
        Proxy* self, uint64_t desc, uint64_t riid, uint64_t ppv) {
    return pipeline_stream_fixup(self, VKD3D_FIXUP_CREATE_PIPELINE_STATE,
                                 nullptr, 0, desc, riid, ppv);
}

extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_LoadPipeline(
        Proxy* self, uint64_t name, uint64_t desc, uint64_t riid, uint64_t ppv) {
    uint64_t lead[1] = {name};
    return pipeline_stream_fixup(self, VKD3D_FIXUP_LOAD_PIPELINE, lead, 1,
                                 desc, riid, ppv);
}

/* ======================================================================== */
/* ID3D12GraphicsCommandList4::BeginRenderPass                               */
/* ======================================================================== */

namespace {

/* D3D12_RENDER_PASS_ENDING_ACCESS is a discriminated union whose ONLY arm in
 * these headers is D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_PARAMETERS, and the
 * only Type that selects it is ..._RESOLVE.  The PRESERVE_LOCAL_* types (4, 5,
 * 6) exist in this header's enum but carry no union arm here, so there is
 * nothing to rewrite for them -- checked against the enum in
 * ppc64le/idl/gen/vkd3d_d3d12.h rather than remembered.  If a newer widl adds
 * a PreserveLocal arm it carries UINTs, not interfaces, and this stays right.
 *
 * .Resolve.pSubresourceParameters points at an array of
 * D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS: four UINTs
 * and a D3D12_RECT, no interface anywhere.  It is forwarded UNCHANGED, still
 * pointing at the guest's array, which is what the host must read.
 *
 * BeginningAccess's only arm is a D3D12_CLEAR_VALUE.  No interfaces. */
void fixup_ending_access(uint32_t kind, D3D12_RENDER_PASS_ENDING_ACCESS& e) {
    if (e.Type != D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE)
        return;
    unwrap_member(kind, e.Resolve.pSrcResource);
    unwrap_member(kind, e.Resolve.pDstResource);
}

} /* namespace */

extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_BeginRenderPass(
        Proxy* self, uint64_t count, uint64_t render_targets,
        uint64_t depth_stencil, uint64_t flags) {
    const uint32_t kind = VKD3D_FIXUP_BEGIN_RENDER_PASS;
    const uint32_t n = static_cast<uint32_t>(count);

    alignas(16) unsigned char inl[VKD3D_FIXUP_RT_INLINE *
                                  sizeof(D3D12_RENDER_PASS_RENDER_TARGET_DESC)];
    Arena ar;
    uint64_t rtp = render_targets;

    if (n && render_targets) {
        ar.init(kind, "render pass render targets", inl, sizeof(inl),
                round16(size_t(n) *
                        sizeof(D3D12_RENDER_PASS_RENDER_TARGET_DESC)));
        D3D12_RENDER_PASS_RENDER_TARGET_DESC* copy =
            static_cast<D3D12_RENDER_PASS_RENDER_TARGET_DESC*>(ar.alloc(
                size_t(n) * sizeof(D3D12_RENDER_PASS_RENDER_TARGET_DESC)));
        std::memcpy(copy, reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(render_targets)),
                    size_t(n) * sizeof(D3D12_RENDER_PASS_RENDER_TARGET_DESC));
        for (uint32_t i = 0; i < n; i++)
            fixup_ending_access(kind, copy[i].EndingAccess);
        rtp = reinterpret_cast<uint64_t>(copy);
    }

    /* The depth-stencil descriptor is nullable, and carries TWO ending
     * accesses -- depth and stencil -- either of which can be a resolve. */
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds;
    uint64_t dsp = depth_stencil;
    if (depth_stencil) {
        std::memcpy(&ds, reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(depth_stencil)), sizeof(ds));
        fixup_ending_access(kind, ds.DepthEndingAccess);
        fixup_ending_access(kind, ds.StencilEndingAccess);
        dsp = reinterpret_cast<uint64_t>(&ds);
    }

    uint64_t a[4] = {count, rtp, dsp, flags};
    return cross(self, kind, a, 4);
}

/* ======================================================================== */
/* ID3D12GraphicsCommandList7::Barrier (enhanced barriers)                   */
/* ======================================================================== */

namespace {

constexpr size_t kBarrierArenaInline =
    round16(VKD3D_FIXUP_GROUPS_INLINE * sizeof(D3D12_BARRIER_GROUP)) +
    VKD3D_FIXUP_GROUPS_INLINE *
        round16(VKD3D_FIXUP_GROUP_BARRIERS_INLINE * sizeof(D3D12_TEXTURE_BARRIER));

size_t barrier_group_bytes(const D3D12_BARRIER_GROUP& g) {
    if (!g.NumBarriers)
        return 0;
    switch (g.Type) {
    case D3D12_BARRIER_TYPE_TEXTURE:
        return g.pTextureBarriers
                   ? round16(size_t(g.NumBarriers) * sizeof(D3D12_TEXTURE_BARRIER))
                   : 0;
    case D3D12_BARRIER_TYPE_BUFFER:
        return g.pBufferBarriers
                   ? round16(size_t(g.NumBarriers) * sizeof(D3D12_BUFFER_BARRIER))
                   : 0;
    default:
        /* GLOBAL groups carry no interface pointer, and an unknown Type is a
         * union we cannot read; both pass through pointing at guest memory. */
        return 0;
    }
}

} /* namespace */

extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_Barrier(
        Proxy* self, uint64_t count, uint64_t groups) {
    const uint32_t kind = VKD3D_FIXUP_BARRIER;
    const uint32_t n = static_cast<uint32_t>(count);
    const D3D12_BARRIER_GROUP* src = reinterpret_cast<const D3D12_BARRIER_GROUP*>(
        static_cast<uintptr_t>(groups));

    if (!n || !src) {
        uint64_t a[2] = {count, groups};
        return cross(self, kind, a, 2);
    }

    /* Sizing pass: the group array plus every nested array that needs
     * rewriting, so the whole call takes at most ONE allocation. */
    size_t need = round16(size_t(n) * sizeof(D3D12_BARRIER_GROUP));
    for (uint32_t i = 0; i < n; i++)
        need += barrier_group_bytes(src[i]);

    alignas(16) unsigned char inl[kBarrierArenaInline];
    Arena ar;
    ar.init(kind, "barrier groups", inl, sizeof(inl), need);

    D3D12_BARRIER_GROUP* copy = static_cast<D3D12_BARRIER_GROUP*>(
        ar.alloc(size_t(n) * sizeof(D3D12_BARRIER_GROUP)));
    std::memcpy(copy, src, size_t(n) * sizeof(D3D12_BARRIER_GROUP));

    for (uint32_t i = 0; i < n; i++) {
        const uint32_t m = copy[i].NumBarriers;
        switch (copy[i].Type) {
        case D3D12_BARRIER_TYPE_TEXTURE:
            if (m && copy[i].pTextureBarriers) {
                D3D12_TEXTURE_BARRIER* tb = static_cast<D3D12_TEXTURE_BARRIER*>(
                    ar.alloc(size_t(m) * sizeof(D3D12_TEXTURE_BARRIER)));
                std::memcpy(tb, copy[i].pTextureBarriers,
                            size_t(m) * sizeof(D3D12_TEXTURE_BARRIER));
                for (uint32_t j = 0; j < m; j++)
                    unwrap_member(kind, tb[j].pResource);
                copy[i].pTextureBarriers = tb;
            }
            break;
        case D3D12_BARRIER_TYPE_BUFFER:
            if (m && copy[i].pBufferBarriers) {
                D3D12_BUFFER_BARRIER* bb = static_cast<D3D12_BUFFER_BARRIER*>(
                    ar.alloc(size_t(m) * sizeof(D3D12_BUFFER_BARRIER)));
                std::memcpy(bb, copy[i].pBufferBarriers,
                            size_t(m) * sizeof(D3D12_BUFFER_BARRIER));
                for (uint32_t j = 0; j < m; j++)
                    unwrap_member(kind, bb[j].pResource);
                copy[i].pBufferBarriers = bb;
            }
            break;
        case D3D12_BARRIER_TYPE_GLOBAL:
            /* D3D12_GLOBAL_BARRIER is four enums.  The guest's array crosses
             * as it stands. */
            break;
        default:
            warn_once(kind, WARN_UNKNOWN_UNION,
                      "group %u has D3D12_BARRIER_TYPE %u, which this build "
                      "does not know; its barrier array crosses unchanged",
                      i, unsigned(copy[i].Type));
            break;
        }
    }

    uint64_t a[2] = {count, reinterpret_cast<uint64_t>(copy)};
    return cross(self, kind, a, 2);
}

/* ======================================================================== */
/* Test / diagnostic hooks                                                   */
/* ======================================================================== */

extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_foreign_count(void) {
    return g_foreign.load(std::memory_order_relaxed);
}
extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_warn_count(void) {
    return g_warns.load(std::memory_order_relaxed);
}
extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_heap_count(void) {
    return g_heap.load(std::memory_order_relaxed);
}
extern "C" VKD3D_FIXUP uint64_t vkd3d_fixup_refused_count(void) {
    return g_refused.load(std::memory_order_relaxed);
}
