/* HAND-MAINTAINED -- test rig, not shipped.
 *
 * Loopback: the guest runtime and the host runtime in one process, with
 * vkd3d_thunk_call wired straight to vkd3d_host_dispatch instead of through a
 * FEX thunk.
 *
 * WHAT THIS DOES AND DOES NOT PROVE.  It exercises the real proxy interning,
 * the real reference counting, the real IID table, the real generated vtables
 * and slot numbers, the real host dispatcher and the real fence/event pump --
 * against mock COM objects whose vtables are hand-built arrays, so a method can
 * sit at slot 76 without declaring 76 dummy virtuals and the test drives the
 * REAL slot numbers (generated as VKD3D_SLOT_* constants).  It does NOT prove
 * the cross-ABI half: this binary is one architecture throughout.  That is what
 * tests/msabi_caller.cpp is for.  It also does not dlopen real vkd3d-proton;
 * that is the POWER box's job.
 *
 *   ./loopback                 the whole suite
 *   ./loopback --struct-abort  one VKD3D_SLOT_STRUCT_IFACE call and nothing
 *                              else, for the VKD3D_THUNK_STRICT=1 abort check
 */
#include "vkd3d_proxy.h"
#include "vkd3d_thunk_ids.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <thread>
#include <utility>
#include <vector>

/* The flat exports the guest runtime defines, and the host runtime's pump
 * counters.  Declared here rather than in a header: the guest half deliberately
 * has no d3d12.h, and these are the only symbols a consumer links against. */
extern "C" {
int32_t D3D12CreateDevice(void* adapter, uint32_t minimum_feature_level,
                          const void* riid, void** device);
int32_t D3D12SerializeRootSignature(const void* desc, uint32_t version,
                                    void** blob, void** error_blob);
uint32_t vkd3d_host_pump_registered(void);
uint32_t vkd3d_host_pump_fired(void);
}

/* ------------------------------------------------------- the loopback ---- */

static std::atomic<uint64_t> g_crossings{0};

/* Recorded flat-entry traffic.  The eight dlsym'd entries are answered here
 * rather than by vkd3d_host_entry(): the host half's dlopen is exercised for
 * real on the POWER box against real vkd3d-proton, and what these tests are
 * about is the GUEST half of the entries -- the forced-null adapter, the IID
 * resolution, the blob wrapping.  The pump ops go to the real host runtime. */
struct EntryTrace {
    uint32_t entry = 0xffffffffu;
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    uint64_t give_out = 0;      /* what to write into the out-slot */
    uint64_t give_out2 = 0;
    int32_t  hr = VKD3D_S_OK;
    unsigned calls = 0;
};
static EntryTrace g_entry;

extern "C" uint64_t vkd3d_thunk_call(uint32_t iface, uint32_t slot, uint64_t host,
                                     uint64_t* args) {
    g_crossings.fetch_add(1, std::memory_order_relaxed);
    return vkd3d_host_dispatch(iface, slot, host, args);
}

extern "C" uint64_t vkd3d_thunk_call_float(uint32_t iface, uint32_t slot,
                                           uint32_t shape, uint64_t host,
                                           uint64_t* args, const float* fin,
                                           float* fout) {
    g_crossings.fetch_add(1, std::memory_order_relaxed);
    return vkd3d_host_dispatch_float(iface, slot, shape, host, args, fin, fout);
}

extern "C" uint32_t vkd3d_thunk_call_entry(uint32_t entry, uint64_t* args) {
    g_crossings.fetch_add(1, std::memory_order_relaxed);
    if (entry >= VKD3D_ENTRY_COUNT_DLSYM)
        return vkd3d_host_entry(entry, args);   /* the pump ops, for real */

    g_entry.entry = entry;
    g_entry.calls++;
    std::memcpy(g_entry.a, args, sizeof(g_entry.a));
    switch (entry) {
    case VKD3D_ENTRY_CREATE_DEVICE:
        if (args[3])
            *reinterpret_cast<uint64_t*>(args[3]) = g_entry.give_out;
        break;
    case VKD3D_ENTRY_SERIALIZE_ROOT_SIG:
        if (args[2]) *reinterpret_cast<uint64_t*>(args[2]) = g_entry.give_out;
        if (args[3]) *reinterpret_cast<uint64_t*>(args[3]) = g_entry.give_out2;
        break;
    default:
        break;
    }
    return uint32_t(g_entry.hr);
}

extern "C" uint32_t vkd3d_thunk_host_probe(void) {
    return vkd3d_host_probe();
}

/* ------------------------------------------------------------ harness ---- */

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
           std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

#define NOTE(...) do { std::printf("  "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

/* ---------------------------------------------------------- mock objects -- */

/* A host COM object with two distinct interface pointers, the way a C++ class
 * inheriting two interfaces really behaves: QueryInterface(IID_IUnknown)
 * returns an address that is NOT the address of the other interface.  That is
 * the case proxy interning has to get right. */

struct MockObject;

struct MockSub {
    void**      vptr;   /* must be first: the dispatcher reads *(void***)obj */
    MockObject* owner;
    uint32_t    iface;
};

/* The two by-value descriptor-handle shapes, declared here with their real
 * 8-byte layout so the mock receives them the way native vkd3d would. */
struct MockCpuHandle { uint64_t ptr; };
struct MockGpuHandle { uint64_t ptr; };

struct MockObject {
    MockSub  primary;
    MockSub  unknown;
    std::atomic<int> refs{1};

    uint64_t last_a[VKD3D_THUNK_ARGS] = {};
    unsigned last_argc = 0;
    float    last_f[4] = {};
    uint64_t seen_array[64] = {};
    uint32_t seen_count = 0;
    MockGpuHandle last_gpu{0};
    MockCpuHandle last_cpu{0};
    uint64_t agg_ret_slot = 0;
    int      qi_calls = 0;
};

static const uint8_t kIidIUnknown[16] = {
    0,0,0,0, 0,0, 0,0, 0xc0,0,0,0,0,0,0,0x46
};

static uint32_t iid_of(uint32_t iface, uint8_t out[16]) {
    for (uint32_t i = 0; i < kVkdIidCount; i++) {
        if (kVkdIids[i].iface == iface) {
            std::memcpy(out, &kVkdIids[i].w0, 8);
            std::memcpy(out + 8, &kVkdIids[i].w1, 8);
            return 1;
        }
    }
    return 0;
}

static int32_t mock_qi(void* self, const void* riid, void** ppv) {
    MockSub* sub = static_cast<MockSub*>(self);
    MockObject* o = sub->owner;
    o->qi_calls++;
    *ppv = nullptr;

    uint8_t want[16];
    if (!std::memcmp(riid, kIidIUnknown, 16)) {
        o->refs.fetch_add(1);
        *ppv = &o->unknown;
        return VKD3D_S_OK;
    }
    if (iid_of(o->primary.iface, want) && !std::memcmp(riid, want, 16)) {
        o->refs.fetch_add(1);
        *ppv = &o->primary;
        return VKD3D_S_OK;
    }
    return VKD3D_E_NOINTERFACE;
}

static uint32_t mock_addref(void* self) {
    return static_cast<MockSub*>(self)->owner->refs.fetch_add(1) + 1;
}

static uint32_t mock_release(void* self) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    int prev = o->refs.fetch_sub(1);
    if (prev <= 0) {
        std::printf("  FAIL host refcount went negative\n");
        g_fail++;
    }
    return uint32_t(prev - 1);
}

/* A generic recorder at an arbitrary slot, wide enough for the widest slot in
 * the surface (10 parameters). */
static uint64_t mock_generic(void* self, uint64_t a0, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                             uint64_t a7, uint64_t a8, uint64_t a9) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_a[0] = a0; o->last_a[1] = a1; o->last_a[2] = a2; o->last_a[3] = a3;
    o->last_a[4] = a4; o->last_a[5] = a5; o->last_a[6] = a6; o->last_a[7] = a7;
    o->last_a[8] = a8; o->last_a[9] = a9;
    o->last_argc = 10;
    return 0xfeedfacecafebeefull;
}

/* ---- the D3D12-specific shapes ---------------------------------------- */

/* Aggregate return.  The widl C vtable form: an explicit __ret pointer right
 * after This, returned back.  A native implementation writes through __ret and
 * returns it; so does this. */
static void* mock_get_cpu_handle(void* self, MockCpuHandle* ret) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->agg_ret_slot = reinterpret_cast<uint64_t>(ret);
    ret->ptr = 0x00c0ffee12345678ull;
    return ret;
}

static void* mock_get_gpu_handle(void* self, MockGpuHandle* ret) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->agg_ret_slot = reinterpret_cast<uint64_t>(ret);
    ret->ptr = 0x00dec0de87654321ull;
    return ret;
}

/* By-value 8-byte aggregate: SetGraphicsRootDescriptorTable(UINT, handle). */
static void mock_set_root_table(void* self, uint64_t index, MockGpuHandle h) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_a[0] = index;
    o->last_gpu  = h;
}

/* ClearRenderTargetView(handle, const FLOAT[4], UINT, const D3D12_RECT*) --
 * a by-value CPU handle and a POINTER to floats, which rides the integer path
 * exactly because an array declarator is a pointer. */
static void mock_clear_rtv(void* self, MockCpuHandle rtv, const float* colour,
                           uint64_t rect_count, uint64_t rects) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_cpu  = rtv;
    o->last_a[1] = reinterpret_cast<uint64_t>(colour);
    o->last_a[2] = rect_count;
    o->last_a[3] = rects;
    if (colour)
        std::memcpy(o->last_f, colour, sizeof(o->last_f));
}

/* Float-class shapes, with their true D3D12 prototypes. */
static void mock_clear_dsv(void* self, MockCpuHandle dsv, uint32_t flags,
                           float depth, uint8_t stencil, uint32_t rect_count,
                           const void* rects) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_cpu  = dsv;
    o->last_a[1] = flags;
    o->last_a[2] = stencil;
    o->last_a[3] = rect_count;
    o->last_a[4] = reinterpret_cast<uint64_t>(rects);
    o->last_f[0] = depth;
}

static void mock_depth_bounds(void* self, float lo, float hi) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_f[0] = lo;
    o->last_f[1] = hi;
}

static void mock_depth_bias(void* self, float bias, float clamp, float slope) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_f[0] = bias;
    o->last_f[1] = clamp;
    o->last_f[2] = slope;
}

/* riid-driven void** out. */
static MockObject* g_out_object = nullptr;

static uint64_t mock_riid_out(void* self, uint64_t desc, uint64_t riid,
                              uint64_t ppOut) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_a[0] = desc;
    o->last_a[1] = riid;
    o->last_a[2] = ppOut;
    if (ppOut && g_out_object) {
        g_out_object->refs.fetch_add(1);   /* COM: an OUT parameter is a ref */
        *reinterpret_cast<uint64_t*>(ppOut) =
            reinterpret_cast<uint64_t>(&g_out_object->primary);
    }
    return uint64_t(uint32_t(VKD3D_S_OK));
}

/* ID3D12DeviceChild::GetDevice(riid, void**) -- two arguments. */
static uint64_t mock_get_device(void* self, uint64_t riid, uint64_t ppOut) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_a[0] = riid;
    o->last_a[1] = ppOut;
    if (ppOut && g_out_object) {
        g_out_object->refs.fetch_add(1);
        *reinterpret_cast<uint64_t*>(ppOut) =
            reinterpret_cast<uint64_t>(&g_out_object->primary);
    }
    return uint64_t(uint32_t(VKD3D_S_OK));
}

/* CreateCommittedResource3: the widest slot in the surface, 10 parameters, the
 * last two riid-driven. */
static uint64_t mock_create_res3(void* self, uint64_t a0, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5,
                                 uint64_t a6, uint64_t a7, uint64_t a8,
                                 uint64_t a9) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_a[0]=a0; o->last_a[1]=a1; o->last_a[2]=a2; o->last_a[3]=a3;
    o->last_a[4]=a4; o->last_a[5]=a5; o->last_a[6]=a6; o->last_a[7]=a7;
    o->last_a[8]=a8; o->last_a[9]=a9;
    o->last_argc = 10;
    if (a9 && g_out_object) {
        g_out_object->refs.fetch_add(1);
        *reinterpret_cast<uint64_t*>(a9) =
            reinterpret_cast<uint64_t>(&g_out_object->primary);
    }
    return uint64_t(uint32_t(VKD3D_S_OK));
}

/* IN array: ExecuteCommandLists(UINT count, ID3D12CommandList* const*). */
static uint64_t mock_execute(void* self, uint64_t count, uint64_t pp) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_a[0] = count;
    o->last_a[1] = pp;
    o->seen_count = 0;
    if (pp) {
        const uint64_t* v = reinterpret_cast<const uint64_t*>(pp);
        for (uint64_t i = 0; i < count && i < 64; i++)
            o->seen_array[o->seen_count++] = v[i];
    }
    return 0;
}

/* ID3D12Resource::Map(UINT, const D3D12_RANGE*, void **data) -- the one raw
 * void** in the surface: mapped memory, not an interface. */
static uint64_t g_mapped_storage = 0;
static uint64_t mock_map(void* self, uint64_t sub, uint64_t range, uint64_t pp) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_a[0] = sub;
    o->last_a[1] = range;
    o->last_a[2] = pp;
    if (pp)
        *reinterpret_cast<uint64_t*>(pp) =
            reinterpret_cast<uint64_t>(&g_mapped_storage);
    return uint64_t(uint32_t(VKD3D_S_OK));
}

/* ID3D12Fence::SetEventOnCompletion(UINT64 value, HANDLE event) -- what the
 * pump override calls after substituting an eventfd. */
static uint64_t mock_set_event(void* self, uint64_t value, uint64_t handle) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_a[0] = value;
    o->last_a[1] = handle;
    o->last_argc = 2;
    return uint64_t(uint32_t(VKD3D_S_OK));
}

/* ID3D12Device1::SetEventOnMultipleFenceCompletion(fences, values, count,
 * flags, HANDLE event) -- an interface ARRAY and the pump override in the same
 * slot. */
static uint64_t mock_multi_fence(void* self, uint64_t fences, uint64_t values,
                                 uint64_t count, uint64_t flags, uint64_t event) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->last_a[0] = fences;
    o->last_a[1] = values;
    o->last_a[2] = count;
    o->last_a[3] = flags;
    o->last_a[4] = event;
    o->last_argc = 5;
    o->seen_count = 0;
    if (fences) {
        const uint64_t* v = reinterpret_cast<const uint64_t*>(fences);
        for (uint64_t i = 0; i < count && i < 64; i++)
            o->seen_array[o->seen_count++] = v[i];
    }
    return uint64_t(uint32_t(VKD3D_S_OK));
}

/* ---- mock vtables, one per role ---------------------------------------- */

#define MOCK_VT_SLOTS 128

enum MockRole {
    VT_GENERIC = 0, VT_DEVICE, VT_DEVICE1, VT_DEVICE10, VT_HEAP, VT_LIST,
    VT_QUEUE, VT_FENCE, VT_RESOURCE, VT_UNKNOWN, VT_ROLE_COUNT
};

static void* g_vt[VT_ROLE_COUNT][MOCK_VT_SLOTS];

static void build_mock_vtables() {
    for (int r = 0; r < VT_ROLE_COUNT; r++) {
        for (int i = 0; i < MOCK_VT_SLOTS; i++)
            g_vt[r][i] = (void*) mock_generic;
        g_vt[r][0] = (void*) mock_qi;
        g_vt[r][1] = (void*) mock_addref;
        g_vt[r][2] = (void*) mock_release;
    }

    g_vt[VT_DEVICE][VKD3D_SLOT_ID3D12DEVICE_CREATECOMMANDQUEUE] = (void*) mock_riid_out;
    g_vt[VT_DEVICE][VKD3D_SLOT_ID3D12DEVICE_CREATECOMMITTEDRESOURCE] = (void*) mock_generic;

    g_vt[VT_DEVICE10][VKD3D_SLOT_ID3D12DEVICE10_CREATECOMMITTEDRESOURCE3] =
        (void*) mock_create_res3;

    g_vt[VT_HEAP][VKD3D_SLOT_ID3D12DESCRIPTORHEAP_GETCPUDESCRIPTORHANDLEFORHEAPSTART] =
        (void*) mock_get_cpu_handle;
    g_vt[VT_HEAP][VKD3D_SLOT_ID3D12DESCRIPTORHEAP_GETGPUDESCRIPTORHANDLEFORHEAPSTART] =
        (void*) mock_get_gpu_handle;

    g_vt[VT_LIST][VKD3D_SLOT_ID3D12GRAPHICSCOMMANDLIST_SETGRAPHICSROOTDESCRIPTORTABLE] =
        (void*) mock_set_root_table;
    g_vt[VT_LIST][VKD3D_SLOT_ID3D12GRAPHICSCOMMANDLIST_CLEARRENDERTARGETVIEW] =
        (void*) mock_clear_rtv;
    g_vt[VT_LIST][kVkdFloatSlot[VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW]] =
        (void*) mock_clear_dsv;
    g_vt[VT_LIST][kVkdFloatSlot[VKD3D_FSHAPE_OM_SET_DEPTH_BOUNDS]] =
        (void*) mock_depth_bounds;
    g_vt[VT_LIST][kVkdFloatSlot[VKD3D_FSHAPE_RS_SET_DEPTH_BIAS]] =
        (void*) mock_depth_bias;

    g_vt[VT_QUEUE][VKD3D_SLOT_ID3D12COMMANDQUEUE_EXECUTECOMMANDLISTS] =
        (void*) mock_execute;

    g_vt[VT_FENCE][VKD3D_SLOT_ID3D12FENCE_SETEVENTONCOMPLETION] =
        (void*) mock_set_event;

    g_vt[VT_DEVICE1][VKD3D_SLOT_ID3D12DEVICE1_SETEVENTONMULTIPLEFENCECOMPLETION] =
        (void*) mock_multi_fence;

    g_vt[VT_RESOURCE][VKD3D_SLOT_ID3D12RESOURCE_MAP] = (void*) mock_map;
    g_vt[VT_RESOURCE][VKD3D_SLOT_ID3D12RESOURCE_GETDEVICE] = (void*) mock_get_device;
}

static void mock_init(MockObject* o, uint32_t iface, MockRole role) {
    o->primary.vptr = g_vt[role]; o->primary.owner = o; o->primary.iface = iface;
    o->unknown.vptr = g_vt[VT_UNKNOWN]; o->unknown.owner = o;
    o->unknown.iface = VKD3D_IFACE_IUNKNOWN;
    o->refs.store(1);
}

static Proxy* as_proxy(void* p) { return static_cast<Proxy*>(p); }
static uint64_t vslot(void* p, uint32_t slot) {
    return reinterpret_cast<const uint64_t*>(as_proxy(p)->vtbl)[slot];
}
static uint64_t host_of(MockObject& o) {
    return reinterpret_cast<uint64_t>(&o.primary);
}

typedef uint32_t (*ReleaseFn)(void*);
static void release(void* p) {
    if (p) reinterpret_cast<ReleaseFn>(vslot(p, 2))(p);
}

/* --------------------------------------------------------------- tests --- */

static void test_interning() {
    std::printf("[interning and identity]\n");
    MockObject o;
    mock_init(&o, VKD3D_IFACE_ID3D12DEVICE, VT_DEVICE);

    /* Producing a host pointer gives you a reference; wrap() consumes it. */
    mock_addref(&o.primary);
    void* p1 = vkd3d_proxy_wrap(host_of(o), VKD3D_IFACE_ID3D12DEVICE);
    CHECK(p1 != nullptr, "wrap returned null");
    CHECK(o.refs.load() == 2, "host refs %d, want 2", o.refs.load());

    /* Same host pointer again -> same proxy, surplus host reference dropped. */
    mock_addref(&o.primary);
    void* p2 = vkd3d_proxy_wrap(host_of(o), VKD3D_IFACE_ID3D12DEVICE);
    CHECK(p1 == p2, "interning failed: %p vs %p", p1, p2);
    CHECK(o.refs.load() == 2, "surplus host ref not dropped: %d", o.refs.load());
    CHECK(as_proxy(p1)->refs.load() == 2, "guest refs %u, want 2",
          as_proxy(p1)->refs.load());

    /* The key is (host, iface): the SAME host pointer as a different interface
     * must be a DIFFERENT proxy, because a proxy carries a static per-type
     * vtable and a shorter interface's array would be indexed off the end. */
    mock_addref(&o.primary);
    void* pchild = vkd3d_proxy_wrap(host_of(o), VKD3D_IFACE_ID3D12DEVICECHILD);
    CHECK(pchild != p1, "(host, iface) keying broken: one proxy for two interfaces");
    CHECK(as_proxy(pchild)->host == as_proxy(p1)->host, "different host pointer");
    CHECK(as_proxy(pchild)->vtbl != as_proxy(p1)->vtbl, "same vtable for two interfaces");
    release(pchild);

    /* AddRef/Release must not cross. */
    uint64_t before = g_crossings.load();
    typedef uint32_t (*AddRefFn)(void*);
    AddRefFn addref = reinterpret_cast<AddRefFn>(vslot(p1, 1));
    for (int i = 0; i < 100; i++) addref(p1);
    for (int i = 0; i < 100; i++) release(p1);
    CHECK(g_crossings.load() == before, "AddRef/Release crossed the boundary %llu times",
          (unsigned long long) (g_crossings.load() - before));
    CHECK(o.refs.load() == 2, "host refcount disturbed by guest AddRef/Release: %d",
          o.refs.load());

    /* QueryInterface to IUnknown: a different host pointer, so a different
     * proxy, interned under its own key. */
    typedef int32_t (*QIFn)(void*, const void*, void**);
    QIFn qi = reinterpret_cast<QIFn>(vslot(p1, 0));
    void* u1 = nullptr;
    CHECK(qi(p1, kIidIUnknown, &u1) == VKD3D_S_OK, "QI(IUnknown) failed");
    CHECK(u1 != nullptr && u1 != p1, "QI(IUnknown) gave %p (p1=%p)", u1, p1);
    CHECK(u1 && as_proxy(u1)->host == reinterpret_cast<uint64_t>(&o.unknown),
          "QI(IUnknown) proxy points at the wrong host sub-object");

    /* COM identity: any interface of the same object must land on that proxy. */
    void* u2 = nullptr;
    QIFn qi_u = reinterpret_cast<QIFn>(vslot(u1, 0));
    CHECK(qi_u(u1, kIidIUnknown, &u2) == VKD3D_S_OK, "QI on IUnknown proxy failed");
    CHECK(u1 == u2, "COM identity broken: %p vs %p", u1, u2);

    /* QI for its own IID short-circuits without crossing. */
    uint8_t own[16];
    iid_of(VKD3D_IFACE_ID3D12DEVICE, own);
    before = g_crossings.load();
    void* p3 = nullptr;
    CHECK(qi(p1, own, &p3) == VKD3D_S_OK, "QI(self) failed");
    CHECK(p3 == p1, "QI(self) gave a different proxy");
    CHECK(g_crossings.load() == before, "QI(self) crossed the boundary");

    /* An IID we have no vtable for is refused without crossing. */
    uint8_t junk[16];
    std::memset(junk, 0x5a, sizeof(junk));
    before = g_crossings.load();
    void* pj = reinterpret_cast<void*>(0x1234);
    CHECK(qi(p1, junk, &pj) == VKD3D_E_NOINTERFACE, "unknown IID not refused");
    CHECK(pj == nullptr, "unknown IID left *ppv non-null");
    CHECK(g_crossings.load() == before, "unknown IID crossed the boundary");

    /* Tear down: only the final Release of each proxy crosses. */
    release(p3);
    release(p2);
    before = g_crossings.load();
    release(p1);
    CHECK(g_crossings.load() == before + 1, "final Release crossed %llu times",
          (unsigned long long) (g_crossings.load() - before));

    release(u2);
    release(u1);

    CHECK(o.refs.load() == 1, "host refs %d after teardown, want 1 (our own)",
          o.refs.load());
    CHECK(vkd3d_proxy_live_count() == 0, "%u proxies still interned",
          vkd3d_proxy_live_count());
}

static void test_generic_dispatch() {
    std::printf("[generic dispatch, slot indexing, arity 10]\n");
    MockObject dev, out, sess;
    mock_init(&dev, VKD3D_IFACE_ID3D12DEVICE10, VT_DEVICE10);
    mock_init(&out, VKD3D_IFACE_ID3D12RESOURCE, VT_RESOURCE);
    mock_init(&sess, VKD3D_IFACE_ID3D12PROTECTEDRESOURCESESSION, VT_GENERIC);
    g_out_object = &out;

    void* p = vkd3d_proxy_wrap(host_of(dev), VKD3D_IFACE_ID3D12DEVICE10);
    void* psess = vkd3d_proxy_wrap(host_of(sess),
                                   VKD3D_IFACE_ID3D12PROTECTEDRESOURCESESSION);

    /* ID3D12Device10::CreateCommittedResource3 is the widest slot in the whole
     * surface: ten parameters, of which the last two are the riid-driven out.
     * On MS-x64 six of them go on the stack. */
    typedef uint64_t (*Fn10)(void*, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t);
    Fn10 create3 = reinterpret_cast<Fn10>(
        vslot(p, VKD3D_SLOT_ID3D12DEVICE10_CREATECOMMITTEDRESOURCE3));

    uint8_t iid_res[16];
    iid_of(VKD3D_IFACE_ID3D12RESOURCE, iid_res);
    void* res = nullptr;
    /* Argument 5 is an IN ID3D12ProtectedResourceSession*, so it is a proxy on
     * the way in and must arrive as the host pointer -- an interface parameter
     * in the middle of the widest signature in the surface. */
    uint64_t hr = create3(p, 0x1111111111111111ull, 0x2222222222222222ull,
                          0x3333333333333333ull, 0x4444444444444444ull,
                          0x5555555555555555ull,
                          reinterpret_cast<uint64_t>(psess),
                          0x7777777777777777ull, 0x8888888888888888ull,
                          reinterpret_cast<uint64_t>(iid_res),
                          reinterpret_cast<uint64_t>(&res));
    CHECK(int32_t(uint32_t(hr)) == VKD3D_S_OK, "CreateCommittedResource3 -> 0x%08x",
          unsigned(hr));
    bool ordered = true;
    for (int i = 0; i < 8; i++) {
        if (i == 5)
            continue;
        if (dev.last_a[i] != 0x1111111111111111ull * uint64_t(i + 1))
            ordered = false;
    }
    CHECK(dev.last_a[5] == host_of(sess),
          "the IN interface pointer at argument 5 was not unwrapped (0x%llx)",
          (unsigned long long) dev.last_a[5]);
    CHECK(ordered, "argument order/values mangled at arity 10: "
          "%llx %llx %llx %llx %llx %llx %llx %llx",
          (unsigned long long) dev.last_a[0], (unsigned long long) dev.last_a[1],
          (unsigned long long) dev.last_a[2], (unsigned long long) dev.last_a[3],
          (unsigned long long) dev.last_a[4], (unsigned long long) dev.last_a[5],
          (unsigned long long) dev.last_a[6], (unsigned long long) dev.last_a[7]);
    CHECK(dev.last_a[8] == reinterpret_cast<uint64_t>(iid_res),
          "the riid did not arrive unaltered");
    CHECK(dev.last_a[9] != reinterpret_cast<uint64_t>(&res),
          "the riid out-parameter handed the guest's own slot to the host");
    CHECK(res && as_proxy(res)->host == host_of(out),
          "arity-10 riid out did not wrap the host pointer");
    CHECK(res && as_proxy(res)->iface == VKD3D_IFACE_ID3D12RESOURCE,
          "arity-10 riid out wrapped as the wrong interface");
    release(res);

    /* Out-of-range slot, unknown interface and null object are refused. */
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    CHECK(vkd3d_host_dispatch(VKD3D_IFACE_COUNT, 0, host_of(dev), a) == 0, "bad iface");
    CHECK(vkd3d_host_dispatch(VKD3D_IFACE_ID3D12DEVICE10, 9999, host_of(dev), a) == 0,
          "bad slot");
    CHECK(vkd3d_host_dispatch(VKD3D_IFACE_ID3D12DEVICE10, 3, 0, a) == 0, "null obj");

    release(psess);
    release(p);
    g_out_object = nullptr;
    CHECK(vkd3d_proxy_live_count() == 0, "leak");
}

static void test_aggregate_return() {
    std::printf("[aggregate returns: the explicit __ret pointer]\n");
    MockObject heap;
    mock_init(&heap, VKD3D_IFACE_ID3D12DESCRIPTORHEAP, VT_HEAP);
    void* p = vkd3d_proxy_wrap(host_of(heap), VKD3D_IFACE_ID3D12DESCRIPTORHEAP);

    /* This is the shape an MSVC-compiled caller emits and the shape the widl C
     * vtable declares: (this, retptr, args...) -> retptr.  No special ABI, no
     * hidden register -- which is the whole D3D12 aggregate-return finding. */
    typedef MockCpuHandle* (*AggCpu)(void*, MockCpuHandle*);
    AggCpu get_cpu = reinterpret_cast<AggCpu>(
        vslot(p, VKD3D_SLOT_ID3D12DESCRIPTORHEAP_GETCPUDESCRIPTORHANDLEFORHEAPSTART));

    MockCpuHandle out{0};
    MockCpuHandle* ret = get_cpu(p, &out);
    CHECK(out.ptr == 0x00c0ffee12345678ull,
          "the mock's write through __ret did not reach the caller: 0x%llx",
          (unsigned long long) out.ptr);
    CHECK(ret == &out, "the returned pointer is not __ret (%p vs %p)",
          (void*) ret, (void*) &out);
    CHECK(heap.agg_ret_slot == reinterpret_cast<uint64_t>(&out),
          "the host received a different __ret pointer than the caller passed");

    typedef MockGpuHandle* (*AggGpu)(void*, MockGpuHandle*);
    AggGpu get_gpu = reinterpret_cast<AggGpu>(
        vslot(p, VKD3D_SLOT_ID3D12DESCRIPTORHEAP_GETGPUDESCRIPTORHANDLEFORHEAPSTART));
    MockGpuHandle gout{0};
    MockGpuHandle* gret = get_gpu(p, &gout);
    CHECK(gout.ptr == 0x00dec0de87654321ull && gret == &gout,
          "GPU-handle aggregate return did not round-trip");

    release(p);
    CHECK(vkd3d_proxy_live_count() == 0, "leak");
}

static void test_byval_aggregate() {
    std::printf("[by-value 8-byte descriptor handles]\n");
    MockObject list;
    mock_init(&list, VKD3D_IFACE_ID3D12GRAPHICSCOMMANDLIST, VT_LIST);
    void* p = vkd3d_proxy_wrap(host_of(list), VKD3D_IFACE_ID3D12GRAPHICSCOMMANDLIST);

    /* SetGraphicsRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE): the
     * handle is an 8-byte aggregate passed BY VALUE.  MS-x64 puts it in one
     * GPR, SysV classifies it as one INTEGER eightbyte, ELFv2 passes it in a
     * GPR -- so one uint64_t slot is correct transport for all three, and the
     * assertion is that the exact 64-bit pattern arrives. */
    typedef void (*TableFn)(void*, uint64_t, MockGpuHandle);
    TableFn set_table = reinterpret_cast<TableFn>(
        vslot(p, VKD3D_SLOT_ID3D12GRAPHICSCOMMANDLIST_SETGRAPHICSROOTDESCRIPTORTABLE));
    MockGpuHandle h{0xfeedfacedeadbeefull};
    set_table(p, 3, h);
    CHECK(list.last_a[0] == 3, "root parameter index arrived as %llu",
          (unsigned long long) list.last_a[0]);
    CHECK(list.last_gpu.ptr == 0xfeedfacedeadbeefull,
          "by-value GPU handle arrived as 0x%llx",
          (unsigned long long) list.last_gpu.ptr);

    /* ClearRenderTargetView(handle, const FLOAT[4], UINT, const D3D12_RECT*):
     * a by-value CPU handle, and a float ARRAY, which is a pointer and rides
     * the integer path -- the case a naive float scan gets wrong. */
    typedef void (*ClearRtv)(void*, MockCpuHandle, const float*, uint64_t, uint64_t);
    ClearRtv clear = reinterpret_cast<ClearRtv>(
        vslot(p, VKD3D_SLOT_ID3D12GRAPHICSCOMMANDLIST_CLEARRENDERTARGETVIEW));
    const float colour[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    MockCpuHandle rtv{0x0badc0de00001000ull};
    clear(p, rtv, colour, 0, 0);
    CHECK(list.last_cpu.ptr == 0x0badc0de00001000ull,
          "by-value CPU handle arrived as 0x%llx",
          (unsigned long long) list.last_cpu.ptr);
    CHECK(list.last_a[1] == reinterpret_cast<uint64_t>(colour),
          "the FLOAT[4] pointer was not passed through");
    CHECK(list.last_f[0] == 0.25f && list.last_f[3] == 1.0f,
          "the colour array the host read back is wrong");

    release(p);
    CHECK(vkd3d_proxy_live_count() == 0, "leak");
}

static void test_float_shapes() {
    std::printf("[float-class shapes]\n");
    MockObject list;
    mock_init(&list, VKD3D_IFACE_ID3D12GRAPHICSCOMMANDLIST9, VT_LIST);
    /* Deliberately a DERIVED interface: the float slots are inherited, and the
     * shape table asserts every version agrees on the slot number. */
    void* p = vkd3d_proxy_wrap(host_of(list), VKD3D_IFACE_ID3D12GRAPHICSCOMMANDLIST9);

    typedef void (*ClearDsv)(void*, MockCpuHandle, uint32_t, float, uint8_t,
                             uint32_t, const void*);
    ClearDsv clear = reinterpret_cast<ClearDsv>(
        vslot(p, kVkdFloatSlot[VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW]));
    MockCpuHandle dsv{0x1234000000005678ull};
    clear(p, dsv, 3u, 0.375f, 0xa5, 2u, reinterpret_cast<const void*>(0x99));
    CHECK(list.last_f[0] == 0.375f, "depth arrived as %.9g", list.last_f[0]);
    CHECK(list.last_cpu.ptr == 0x1234000000005678ull,
          "the by-value handle in a float-class slot arrived as 0x%llx",
          (unsigned long long) list.last_cpu.ptr);
    CHECK(list.last_a[1] == 3u && list.last_a[2] == 0xa5 && list.last_a[3] == 2u &&
          list.last_a[4] == 0x99, "the integer arguments around the float are wrong");

    typedef void (*Bounds)(void*, float, float);
    Bounds bounds = reinterpret_cast<Bounds>(
        vslot(p, kVkdFloatSlot[VKD3D_FSHAPE_OM_SET_DEPTH_BOUNDS]));
    bounds(p, 0.125f, 0.875f);
    CHECK(list.last_f[0] == 0.125f && list.last_f[1] == 0.875f,
          "depth bounds arrived as %.9g / %.9g", list.last_f[0], list.last_f[1]);

    typedef void (*Bias)(void*, float, float, float);
    Bias bias = reinterpret_cast<Bias>(
        vslot(p, kVkdFloatSlot[VKD3D_FSHAPE_RS_SET_DEPTH_BIAS]));
    bias(p, -1.5f, 2.25f, 4.5f);
    CHECK(list.last_f[0] == -1.5f && list.last_f[1] == 2.25f &&
          list.last_f[2] == 4.5f, "depth bias arrived as %.9g / %.9g / %.9g",
          list.last_f[0], list.last_f[1], list.last_f[2]);

    release(p);
    CHECK(vkd3d_proxy_live_count() == 0, "leak");
}

static void test_marshalling() {
    std::printf("[interface-pointer marshalling]\n");
    MockObject dev, queue, out, res;
    mock_init(&dev, VKD3D_IFACE_ID3D12DEVICE, VT_DEVICE);
    mock_init(&queue, VKD3D_IFACE_ID3D12COMMANDQUEUE, VT_QUEUE);
    mock_init(&out, VKD3D_IFACE_ID3D12COMMANDQUEUE, VT_QUEUE);
    mock_init(&res, VKD3D_IFACE_ID3D12RESOURCE, VT_RESOURCE);
    g_out_object = &out;

    void* pdev = vkd3d_proxy_wrap(host_of(dev), VKD3D_IFACE_ID3D12DEVICE);
    void* pq   = vkd3d_proxy_wrap(host_of(queue), VKD3D_IFACE_ID3D12COMMANDQUEUE);
    void* pres = vkd3d_proxy_wrap(host_of(res), VKD3D_IFACE_ID3D12RESOURCE);

    /* ---- riid-driven void** out: CreateCommandQueue(desc, riid, void**). */
    typedef uint64_t (*Fn3)(void*, uint64_t, uint64_t, uint64_t);
    Fn3 create_q = reinterpret_cast<Fn3>(
        vslot(pdev, VKD3D_SLOT_ID3D12DEVICE_CREATECOMMANDQUEUE));
    uint8_t iid_q[16];
    iid_of(VKD3D_IFACE_ID3D12COMMANDQUEUE, iid_q);
    void* newq = reinterpret_cast<void*>(0xdeadbeefull);
    int32_t hr = int32_t(create_q(pdev, 0x1234,
                                  reinterpret_cast<uint64_t>(iid_q),
                                  reinterpret_cast<uint64_t>(&newq)));
    CHECK(hr == VKD3D_S_OK, "CreateCommandQueue -> 0x%08x", unsigned(hr));
    CHECK(dev.last_a[2] != reinterpret_cast<uint64_t>(&newq),
          "the out-parameter handed the guest's own slot straight to the host");
    CHECK(newq && newq != reinterpret_cast<void*>(0xdeadbeefull),
          "the out-parameter was not written");
    CHECK(newq && as_proxy(newq)->host == host_of(out),
          "the out-parameter wrapped the wrong host pointer");
    CHECK(newq && as_proxy(newq)->iface == VKD3D_IFACE_ID3D12COMMANDQUEUE,
          "the out-parameter wrapped as the wrong interface");

    /* Interning: a second call returning the same host pointer must give the
     * same proxy, and the surplus host reference must be dropped. */
    int refs_before = out.refs.load();
    void* q2 = nullptr;
    create_q(pdev, 0, reinterpret_cast<uint64_t>(iid_q),
             reinterpret_cast<uint64_t>(&q2));
    CHECK(q2 == newq, "the riid out did not intern: %p vs %p", q2, newq);
    CHECK(out.refs.load() == refs_before, "surplus host reference leaked: %d -> %d",
          refs_before, out.refs.load());
    release(q2);

    /* An IID with no vtable is refused with ZERO crossings -- the same policy
     * QueryInterface uses; the alternative is handing back a raw host pointer
     * the guest cannot call. */
    uint8_t junk[16];
    std::memset(junk, 0x33, sizeof(junk));
    uint64_t before = g_crossings.load();
    void* nothing = reinterpret_cast<void*>(0x99);
    hr = int32_t(create_q(pdev, 0, reinterpret_cast<uint64_t>(junk),
                          reinterpret_cast<uint64_t>(&nothing)));
    CHECK(hr == VKD3D_E_NOINTERFACE, "unknown riid gave 0x%08x", unsigned(hr));
    CHECK(nothing == nullptr, "unknown riid left *ppv non-null");
    CHECK(g_crossings.load() == before, "unknown riid crossed the boundary");

    /* ---- GetDevice(riid, void**) on a device child interns back. */
    g_out_object = &dev;
    typedef uint64_t (*Fn2)(void*, uint64_t, uint64_t);
    Fn2 get_device = reinterpret_cast<Fn2>(
        vslot(pres, VKD3D_SLOT_ID3D12RESOURCE_GETDEVICE));
    uint8_t iid_dev[16];
    iid_of(VKD3D_IFACE_ID3D12DEVICE, iid_dev);
    void* devback = nullptr;
    get_device(pres, reinterpret_cast<uint64_t>(iid_dev),
               reinterpret_cast<uint64_t>(&devback));
    CHECK(devback == pdev, "GetDevice gave %p, want the device proxy %p",
          devback, pdev);
    release(devback);
    g_out_object = &out;

    /* ---- IN array + count: ExecuteCommandLists(count, lists).  20 elements,
     * past VKD3D_IFARRAY_INLINE, so the heap path is exercised, and one null
     * element, which must stay null rather than becoming a wild pointer. */
    const uint32_t kN = 20;
    std::vector<MockObject> lists(kN);
    std::vector<void*> proxies(kN);
    for (uint32_t i = 0; i < kN; i++) {
        mock_init(&lists[i], VKD3D_IFACE_ID3D12COMMANDLIST, VT_GENERIC);
        proxies[i] = vkd3d_proxy_wrap(host_of(lists[i]), VKD3D_IFACE_ID3D12COMMANDLIST);
    }
    std::vector<void*> call_args(proxies);
    call_args[7] = nullptr;     /* a null element must stay null */

    Fn2 execute = reinterpret_cast<Fn2>(
        vslot(pq, VKD3D_SLOT_ID3D12COMMANDQUEUE_EXECUTECOMMANDLISTS));
    execute(pq, kN, reinterpret_cast<uint64_t>(call_args.data()));
    CHECK(queue.last_a[1] != reinterpret_cast<uint64_t>(call_args.data()),
          "the IN array passed the guest's proxy array straight through");
    CHECK(queue.seen_count == kN, "the mock recorded %u elements, want %u",
          queue.seen_count, kN);
    bool arr_ok = true;
    for (uint32_t i = 0; i < queue.seen_count; i++) {
        uint64_t want = (i == 7) ? 0 : host_of(lists[i]);
        if (queue.seen_array[i] != want)
            arr_ok = false;
    }
    CHECK(arr_ok, "an IN array element was not unwrapped to its host pointer "
          "(or a null element did not stay null)");

    /* A null array with a non-zero count must not be dereferenced. */
    execute(pq, 4, 0);
    CHECK(queue.last_a[1] == 0, "a null IN array turned into a pointer");

    for (uint32_t i = 0; i < kN; i++)
        release(proxies[i]);

    /* ---- raw void** (memory, not an interface): ID3D12Resource::Map. */
    void* mapped = nullptr;
    Fn3 map = reinterpret_cast<Fn3>(vslot(pres, VKD3D_SLOT_ID3D12RESOURCE_MAP));
    hr = int32_t(map(pres, 0, 0, reinterpret_cast<uint64_t>(&mapped)));
    CHECK(hr == VKD3D_S_OK, "Map -> 0x%08x", unsigned(hr));
    CHECK(mapped == &g_mapped_storage,
          "Map's void** was not passed through unchanged (%p)", mapped);
    CHECK(res.last_a[2] == reinterpret_cast<uint64_t>(&mapped),
          "Map's void** was redirected through a private slot; it is memory, "
          "not an interface");

    for (void* p : {pres, pq, pdev})
        release(p);
    release(newq);
    g_out_object = nullptr;

    CHECK(vkd3d_proxy_live_count() == 0, "%u proxies leaked",
          vkd3d_proxy_live_count());
    CHECK(out.refs.load() == 1, "host refcount on the OUT object ended at %d, want 1",
          out.refs.load());
}

static void test_refused() {
    std::printf("[refused slots: no crossing, poison return]\n");
    MockObject wg, dn;
    mock_init(&wg, VKD3D_IFACE_ID3D12WORKGRAPHPROPERTIES, VT_GENERIC);
    mock_init(&dn, VKD3D_IFACE_ID3DDESTRUCTIONNOTIFIER, VT_GENERIC);
    void* pwg = vkd3d_proxy_wrap(host_of(wg), VKD3D_IFACE_ID3D12WORKGRAPHPROPERTIES);
    void* pdn = vkd3d_proxy_wrap(host_of(dn), VKD3D_IFACE_ID3DDESTRUCTIONNOTIFIER);

    /* GetNodeIndex(UINT, D3D12_NODE_ID) takes a 16-byte aggregate BY VALUE,
     * which no single uint64_t slot can carry under any of the three ABIs.  It
     * must not cross at all. */
    uint64_t before = g_crossings.load();
    typedef uint64_t (*Fn2)(void*, uint64_t, uint64_t);
    Fn2 node_index = reinterpret_cast<Fn2>(
        vslot(pwg, VKD3D_SLOT_ID3D12WORKGRAPHPROPERTIES_GETNODEINDEX));
    uint64_t r = node_index(pwg, 0, 0);
    CHECK(r == 0, "the refused UINT slot returned %llu, want 0",
          (unsigned long long) r);
    CHECK(g_crossings.load() == before, "a refused slot crossed the boundary");
    CHECK(wg.last_argc == 0, "a refused slot reached the mock");

    /* RegisterDestructionCallback would hand vkd3d a GUEST function pointer to
     * call from a host thread.  HRESULT-returning, so the poison is E_NOTIMPL. */
    typedef uint64_t (*Fn3)(void*, uint64_t, uint64_t, uint64_t);
    Fn3 reg = reinterpret_cast<Fn3>(vslot(pdn, 3));
    int32_t hr = int32_t(uint32_t(reg(pdn, 0x1000, 0, 0)));
    CHECK(hr == VKD3D_E_NOTIMPL, "the refused HRESULT slot returned 0x%08x",
          unsigned(hr));
    CHECK(g_crossings.load() == before, "a refused slot crossed the boundary");

    release(pwg);
    release(pdn);
    CHECK(vkd3d_proxy_live_count() == 0, "leak");
}

/* One VKD3D_SLOT_STRUCT_IFACE call: ResourceBarrier, whose
 * D3D12_RESOURCE_BARRIER contains an ID3D12Resource* that crosses raw. */
static void drive_struct_iface_slot(MockObject& list, void* p) {
    typedef uint64_t (*Fn2)(void*, uint64_t, uint64_t);
    Fn2 barrier = reinterpret_cast<Fn2>(
        vslot(p, VKD3D_SLOT_ID3D12GRAPHICSCOMMANDLIST_RESOURCEBARRIER));
    uint64_t fake_barriers = 0xabcdef00ull;
    barrier(p, 1, fake_barriers);
    (void) list;
}

static void test_struct_iface() {
    std::printf("[structs carrying interface pointers: loud, not silent]\n");
    MockObject list;
    mock_init(&list, VKD3D_IFACE_ID3D12GRAPHICSCOMMANDLIST, VT_LIST);
    void* p = vkd3d_proxy_wrap(host_of(list), VKD3D_IFACE_ID3D12GRAPHICSCOMMANDLIST);

    uint64_t warns_before = vkd3d_thunk_struct_iface_count();
    uint64_t cross_before = g_crossings.load();
    drive_struct_iface_slot(list, p);
    CHECK(vkd3d_thunk_struct_iface_count() == warns_before + 1,
          "a STRUCT_IFACE slot did not warn");
    CHECK(g_crossings.load() == cross_before + 1,
          "a STRUCT_IFACE slot did not cross (it is supposed to, loudly)");
    CHECK(list.last_a[1] == 0xabcdef00ull,
          "the struct pointer was not passed through unchanged");
    CHECK(kVkdSlotFlags[VKD3D_IFACE_ID3D12GRAPHICSCOMMANDLIST]
                       [VKD3D_SLOT_ID3D12GRAPHICSCOMMANDLIST_RESOURCEBARRIER]
          & VKD3D_SLOT_STRUCT_IFACE, "ResourceBarrier is not flagged STRUCT_IFACE");
    CHECK(vkd3d_slot_untranslated(
              kVkdSlotFlags[VKD3D_IFACE_ID3D12GRAPHICSCOMMANDLIST]
                           [VKD3D_SLOT_ID3D12GRAPHICSCOMMANDLIST_RESOURCEBARRIER]),
          "STRICT would not report ResourceBarrier");
    /* Map is RAW_VOID and deliberately NOT reported: mapped memory under a
     * shared address space is correct by design.  This is the one place this
     * project diverges from the D3D11 reference's STRICT policy. */
    CHECK(!vkd3d_slot_untranslated(
              kVkdSlotFlags[VKD3D_IFACE_ID3D12RESOURCE][VKD3D_SLOT_ID3D12RESOURCE_MAP]),
          "STRICT reports ID3D12Resource::Map, which is correct by design");

    release(p);
    CHECK(vkd3d_proxy_live_count() == 0, "leak");
}

static void test_flat_entries() {
    std::printf("[the flat d3d12.dll entry points]\n");
    MockObject dev, blob, errblob;
    mock_init(&dev, VKD3D_IFACE_ID3D12DEVICE, VT_DEVICE);
    mock_init(&blob, VKD3D_IFACE_ID3D10BLOB, VT_GENERIC);
    mock_init(&errblob, VKD3D_IFACE_ID3D10BLOB, VT_GENERIC);

    uint8_t iid_dev[16];
    iid_of(VKD3D_IFACE_ID3D12DEVICE, iid_dev);

    /* An adapter the caller passes is FORCED TO NULL: the native build ignores
     * it (libs/d3d12core/main.c, "Ignoring adapter."), and forwarding a pointer
     * from a DXGI implementation this thunk knows nothing about would be worse
     * than dropping it.  The FIXME goes to stderr once. */
    g_entry.give_out = host_of(dev);
    g_entry.hr = VKD3D_S_OK;
    void* device = nullptr;
    void* fake_adapter = reinterpret_cast<void*>(0x4321);
    int32_t hr = D3D12CreateDevice(fake_adapter, 0xb000, iid_dev, &device);
    CHECK(hr == VKD3D_S_OK, "D3D12CreateDevice -> 0x%08x", unsigned(hr));
    CHECK(g_entry.entry == VKD3D_ENTRY_CREATE_DEVICE, "wrong entry id");
    CHECK(g_entry.a[0] == 0, "the adapter was not forced to NULL (0x%llx)",
          (unsigned long long) g_entry.a[0]);
    CHECK(g_entry.a[1] == 0xb000, "the feature level was mangled");
    CHECK(g_entry.a[3] != reinterpret_cast<uint64_t>(&device),
          "the out-parameter handed the guest's own slot to the host");
    CHECK(device && as_proxy(device)->host == host_of(dev),
          "D3D12CreateDevice did not wrap the device");
    CHECK(device && as_proxy(device)->iface == VKD3D_IFACE_ID3D12DEVICE,
          "D3D12CreateDevice wrapped the wrong interface");
    release(device);

    /* An IID with no vtable is refused with ZERO crossings. */
    uint8_t junk[16];
    std::memset(junk, 0x71, sizeof(junk));
    uint64_t before = g_crossings.load();
    void* nothing = reinterpret_cast<void*>(0x5);
    hr = D3D12CreateDevice(nullptr, 0xb000, junk, &nothing);
    CHECK(hr == VKD3D_E_NOINTERFACE, "unknown IID gave 0x%08x", unsigned(hr));
    CHECK(nothing == nullptr, "unknown IID left the out-parameter non-null");
    CHECK(g_crossings.load() == before, "unknown IID crossed the boundary");

    /* The serialisers: two statically-typed ID3DBlob outs, which the census
     * carries as ID3D10Blob, so there is no IID to resolve. */
    g_entry.give_out  = host_of(blob);
    g_entry.give_out2 = host_of(errblob);
    void* pblob = nullptr;
    void* perr  = nullptr;
    hr = D3D12SerializeRootSignature(reinterpret_cast<const void*>(0x11), 1,
                                     &pblob, &perr);
    CHECK(hr == VKD3D_S_OK, "D3D12SerializeRootSignature -> 0x%08x", unsigned(hr));
    CHECK(g_entry.a[0] == 0x11 && g_entry.a[1] == 1, "the desc/version were mangled");
    CHECK(pblob && as_proxy(pblob)->iface == VKD3D_IFACE_ID3D10BLOB,
          "the blob was not wrapped as ID3D10Blob");
    CHECK(perr && as_proxy(perr)->host == host_of(errblob),
          "the error blob was not wrapped");
    release(pblob);
    release(perr);

    g_entry.give_out = g_entry.give_out2 = 0;
    CHECK(vkd3d_proxy_live_count() == 0, "leak");
}

static void test_interop_exports() {
    std::printf("[cross-runtime interop exports]\n");
    MockObject q;
    mock_init(&q, VKD3D_IFACE_ID3D12COMMANDQUEUE, VT_QUEUE);
    void* p = vkd3d_proxy_wrap(host_of(q), VKD3D_IFACE_ID3D12COMMANDQUEUE);

    CHECK(vkd3d_thunk_interop_version() == 1, "interop version is not 1");
    CHECK(vkd3d_thunk_unwrap(p) == host_of(q),
          "unwrap did not return the host pointer under the proxy");
    CHECK(vkd3d_thunk_unwrap(nullptr) == 0, "unwrap(null) is not 0");

    /* Something that is NOT one of ours must come back 0, never a guess.  A
     * MockSub has the same first two words as a Proxy, which is exactly the
     * confusable case dxvk's DXGI shim could hand us. */
    CHECK(vkd3d_thunk_unwrap(&q.primary) == 0,
          "unwrap guessed at a pointer that is not one of our proxies");

    /* wrap() through the interop entry point interns like any other. */
    mock_addref(&q.primary);
    void* again = vkd3d_thunk_wrap(host_of(q), VKD3D_IFACE_ID3D12COMMANDQUEUE);
    CHECK(again == p, "the interop wrap did not intern");
    release(again);

    release(p);
    CHECK(vkd3d_proxy_live_count() == 0, "leak");
}

static void test_threaded() {
    std::printf("[threaded wrap/release stress]\n");
    MockObject o;
    mock_init(&o, VKD3D_IFACE_ID3D12RESOURCE, VT_RESOURCE);

    const int kThreads = 8, kIters = 20000;
    std::vector<std::thread> ts;
    std::atomic<int> mismatched{0};
    for (int t = 0; t < kThreads; t++) {
        ts.emplace_back([&] {
            for (int i = 0; i < kIters; i++) {
                /* Model "the host handed us a reference", then wrap. */
                mock_addref(&o.primary);
                void* p = vkd3d_proxy_wrap(host_of(o), VKD3D_IFACE_ID3D12RESOURCE);
                if (!p) { mismatched++; continue; }
                if (as_proxy(p)->host != host_of(o))
                    mismatched++;
                vkd3d_proxy_addref(as_proxy(p));
                vkd3d_proxy_release(as_proxy(p));
                vkd3d_proxy_release(as_proxy(p));
            }
        });
    }
    for (auto& t : ts) t.join();

    CHECK(mismatched.load() == 0, "%d bad proxies under contention", mismatched.load());
    CHECK(o.refs.load() == 1, "host refcount ended at %d, want 1", o.refs.load());
    CHECK(vkd3d_proxy_live_count() == 0, "%u proxies leaked", vkd3d_proxy_live_count());
    NOTE("%d threads x %d wrap/release, host refcount balanced", kThreads, kIters);
}

/* ---------------------------------------------------------------- the pump */

static void test_pump() {
    std::printf("[fence/event pump]\n");
    MockObject fence;
    mock_init(&fence, VKD3D_IFACE_ID3D12FENCE, VT_FENCE);
    void* p = vkd3d_proxy_wrap(host_of(fence), VKD3D_IFACE_ID3D12FENCE);

    typedef uint64_t (*Fn2)(void*, uint64_t, uint64_t);
    Fn2 set_event = reinterpret_cast<Fn2>(
        vslot(p, VKD3D_SLOT_ID3D12FENCE_SETEVENTONCOMPLETION));

    /* hEvent == NULL is the D3D12 blocking-wait contract: it must reach vkd3d
     * unchanged, because vkd3d blocks the calling thread itself. */
    set_event(p, 41, 0);
    CHECK(fence.last_a[0] == 41, "the fence value was mangled");
    CHECK(fence.last_a[1] == 0, "a NULL event did not reach the mock as NULL "
          "(got 0x%llx)", (unsigned long long) fence.last_a[1]);

    /* A non-NULL event is a guest Win32 HANDLE (an ntsync object on the
     * deployment box).  It must NOT reach vkd3d, which would read its bits as
     * an eventfd; the override substitutes a pooled eventfd and keeps the
     * HANDLE as the completion cookie. */
    const uint64_t kCookie = 0x00c0ffee0000deadull;
    set_event(p, 42, kCookie);
    CHECK(fence.last_a[0] == 42, "the fence value was mangled");
    CHECK(fence.last_a[1] != kCookie,
          "the guest HANDLE was passed straight to vkd3d");
    int fd = int(fence.last_a[1]);
    CHECK(fd > 0, "the pump did not substitute a usable eventfd (got %d)", fd);
    CHECK(vkd3d_host_pump_registered() >= 1, "the pump recorded no registration");

    if (fd > 0) {
        /* Signal it the way vkd3d's fence worker would. */
        uint64_t one = 1;
        ssize_t w = write(fd, &one, sizeof(one));
        CHECK(w == ssize_t(sizeof(one)), "could not signal the eventfd");

        auto fut = std::async(std::launch::async, [] {
            uint64_t a[VKD3D_THUNK_ARGS] = {};
            uint32_t r = vkd3d_thunk_call_entry(VKD3D_ENTRY_PUMP_WAIT, a);
            return std::make_pair(r, a[1]);
        });
        if (fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
            auto got = fut.get();
            CHECK(got.first == 1, "PUMP_WAIT returned %u, want 1", got.first);
            CHECK(got.second == kCookie,
                  "PUMP_WAIT returned cookie 0x%llx, want 0x%llx",
                  (unsigned long long) got.second, (unsigned long long) kCookie);
        } else {
            g_fail++;
            std::printf("  FAIL PUMP_WAIT did not return within 5s\n");
            uint64_t a[VKD3D_THUNK_ARGS] = {};
            vkd3d_thunk_call_entry(VKD3D_ENTRY_PUMP_SHUTDOWN, a);
            fut.wait();
        }
        CHECK(vkd3d_host_pump_fired() >= 1, "the reaper saw no eventfd fire");
    }

    /* The same override on ID3D12Device1::SetEventOnMultipleFenceCompletion,
     * which is also an ARRAY_SPECS case: the fence array is unwrapped guest-side
     * and only the event is substituted host-side. */
    MockObject dev1;
    mock_init(&dev1, VKD3D_IFACE_ID3D12DEVICE1, VT_DEVICE1);
    void* pdev1 = vkd3d_proxy_wrap(host_of(dev1), VKD3D_IFACE_ID3D12DEVICE1);
    typedef uint64_t (*Fn5)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    Fn5 multi = reinterpret_cast<Fn5>(
        vslot(pdev1, VKD3D_SLOT_ID3D12DEVICE1_SETEVENTONMULTIPLEFENCECOMPLETION));
    const uint64_t kCookie2 = 0x00abcdef00001234ull;
    void* fences[1] = {p};
    uint64_t values[1] = {77};
    multi(pdev1, reinterpret_cast<uint64_t>(fences),
          reinterpret_cast<uint64_t>(values), 1, 0, kCookie2);
    CHECK(dev1.last_a[0] != reinterpret_cast<uint64_t>(fences),
          "the fence array passed the guest's proxy array straight through");
    CHECK(dev1.seen_count == 1 && dev1.seen_array[0] == host_of(fence),
          "the fence array element was not unwrapped to its host pointer");
    CHECK(dev1.last_a[2] == 1 && dev1.last_a[1] == reinterpret_cast<uint64_t>(values),
          "the count or the value array was mangled");
    CHECK(dev1.last_a[4] != kCookie2,
          "the guest HANDLE was passed straight to vkd3d by the multi-fence "
          "override");
    int fd2 = int(dev1.last_a[4]);
    CHECK(fd2 > 0, "the multi-fence override did not substitute an eventfd");
    if (fd2 > 0) {
        uint64_t one = 1;
        ssize_t w = write(fd2, &one, sizeof(one));
        CHECK(w == ssize_t(sizeof(one)), "could not signal the eventfd");
        auto fut = std::async(std::launch::async, [] {
            uint64_t a[VKD3D_THUNK_ARGS] = {};
            uint32_t r = vkd3d_thunk_call_entry(VKD3D_ENTRY_PUMP_WAIT, a);
            return std::make_pair(r, a[1]);
        });
        if (fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
            auto got = fut.get();
            CHECK(got.first == 1 && got.second == kCookie2,
                  "the multi-fence completion came back as (%u, 0x%llx)",
                  got.first, (unsigned long long) got.second);
        } else {
            g_fail++;
            std::printf("  FAIL the multi-fence PUMP_WAIT did not return within 5s\n");
            uint64_t a[VKD3D_THUNK_ARGS] = {};
            vkd3d_thunk_call_entry(VKD3D_ENTRY_PUMP_SHUTDOWN, a);
            fut.wait();
        }
    }
    release(pdev1);

    /* Shutdown wakes a parked waiter with 0. */
    auto fut2 = std::async(std::launch::async, [] {
        uint64_t a[VKD3D_THUNK_ARGS] = {};
        return vkd3d_thunk_call_entry(VKD3D_ENTRY_PUMP_WAIT, a);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    uint64_t sd[VKD3D_THUNK_ARGS] = {};
    CHECK(vkd3d_thunk_call_entry(VKD3D_ENTRY_PUMP_SHUTDOWN, sd) == 1,
          "PUMP_SHUTDOWN did not report success");
    if (fut2.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        CHECK(fut2.get() == 0, "PUMP_WAIT did not return 0 on shutdown");
    } else {
        g_fail++;
        std::printf("  FAIL PUMP_WAIT did not wake on shutdown\n");
        fut2.wait();
    }

    release(p);
    CHECK(vkd3d_proxy_live_count() == 0, "leak");
}

/* --------------------------------------------- the three vtables are separate */

static void test_abi_tables() {
    std::printf("[guest ABI vtables]\n");
    uint32_t avail = vkd3d_thunk_abi_available();
    CHECK(avail & (1u << VKD3D_ABI_SYSV), "SysV vtables missing");
#if defined(__x86_64__)
    CHECK(avail & (1u << VKD3D_ABI_MS), "x86-64 build has no MS-x64 vtables");
    const void** target = const_cast<const void**>(
        vkd3d_thunk_vtable(VKD3D_IFACE_ID3D12DEVICE));
    const void* const* sysv =
        vkd3d_thunk_vtable_for(VKD3D_IFACE_ID3D12DEVICE, VKD3D_ABI_SYSV);
    const void* const* ms =
        vkd3d_thunk_vtable_for(VKD3D_IFACE_ID3D12DEVICE, VKD3D_ABI_MS);
    CHECK(target != sysv && target != ms && sysv != ms,
          "the three per-interface arrays are not distinct");
    /* An override installed for PE callers must NOT reach the SysV vtable.
     * That is the structural property the next work package's struct fixups
     * will rely on. */
    uint32_t s = VKD3D_SLOT_ID3D12DEVICE_GETDESCRIPTORHANDLEINCREMENTSIZE;
    const void* saved = target[s];
    target[s] = (const void*) 0x1234;
    CHECK(sysv[s] != (const void*) 0x1234,
          "patching the PE-facing table also changed the SysV vtable");
    target[s] = saved;
#else
    CHECK(!(avail & (1u << VKD3D_ABI_MS)),
          "non-x86-64 build claims MS-x64 vtables it cannot have");
    CHECK(vkd3d_thunk_vtable_for(VKD3D_IFACE_ID3D12DEVICE, VKD3D_ABI_MS) == nullptr,
          "an MS vtable was returned on a target with no ms_abi");
#endif
    /* Once a proxy has existed the mode is frozen. */
    CHECK(vkd3d_thunk_set_abi(VKD3D_ABI_SYSV) == 0,
          "the ABI was changeable after proxies had been created");
    CHECK(vkd3d_thunk_abi_is_ms() == (vkd3d_thunk_abi() == VKD3D_ABI_MS ? 1 : 0),
          "vkd3d_thunk_abi_is_ms() disagrees with the active mode");
}

int main(int argc, char** argv) {
    bool struct_abort = false;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--struct-abort")) struct_abort = true;

    /* This rig is a guest ELF/SysV caller, so it must say so: on x86-64 the
     * default is MS-x64 because the deployment target is a PE game.  If this
     * call ever stops working the very first vtable call jumps with the wrong
     * registers. */
    if (vkd3d_thunk_abi_available() & (1u << VKD3D_ABI_SYSV)) {
        vkd3d_thunk_set_abi_sysv();
        if (vkd3d_thunk_abi() != VKD3D_ABI_SYSV) {
            std::printf("  FAIL could not select the SysV guest ABI\n");
            return 1;
        }
    }

    build_mock_vtables();

    if (struct_abort) {
        /* The negative control for VKD3D_THUNK_STRICT=1: one STRUCT_IFACE call,
         * which must abort the process when STRICT is set and return normally
         * when it is not. */
        std::printf("[one STRUCT_IFACE call]\n");
        MockObject list;
        mock_init(&list, VKD3D_IFACE_ID3D12GRAPHICSCOMMANDLIST, VT_LIST);
        void* p = vkd3d_proxy_wrap(host_of(list), VKD3D_IFACE_ID3D12GRAPHICSCOMMANDLIST);
        drive_struct_iface_slot(list, p);
        release(p);
        std::printf("survived (VKD3D_THUNK_STRICT was not set)\n");
        return 0;
    }

    test_interning();
    test_generic_dispatch();
    test_aggregate_return();
    test_byval_aggregate();
    test_float_shapes();
    test_marshalling();
    test_refused();
    test_struct_iface();
    test_flat_entries();
    test_interop_exports();
    test_threaded();
    test_pump();
    test_abi_tables();   /* last: it asserts the mode is frozen by now */

    std::printf("\n%d passed, %d failed, %llu boundary crossings\n",
                g_pass, g_fail, (unsigned long long) g_crossings.load());
    return g_fail ? 1 : 0;
}
