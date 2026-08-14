/* HAND-MAINTAINED -- test rig, not shipped.  x86-64 ONLY.
 *
 * THE CALLER A GAME ACTUALLY IS.
 *
 * Every other test in this tree is a guest ELF caller, i.e. SysV.  The thing a
 * real game does -- call a COM method with the Microsoft x64 convention -- is
 * what this file does: it declares the method pointers __attribute__((ms_abi)),
 * which is the machine code a PE game's compiler emits, calls the proxy vtables
 * through them, and checks what the host actually received.  It needs no PE
 * binary and no Wine: the calling convention is a property of the CALL SITE.
 *
 * In the D3D11 project this test did not exist, and the consequence was that
 * all 2593 vtable slots were plain SysV for two milestones -- a game would have
 * died at its first AddRef with nothing on stderr.  This one exists from the
 * start, and so does its falsification:
 *
 * FALSIFICATION.  tests/build.sh builds this file a second time with
 * -DVKD3D_THUNK_ABI_NEGATIVE_CONTROL, which compiles the generated forwarders
 * as SysV -- i.e. reintroduces that defect exactly -- and REQUIRES the run to
 * fail.  A test that cannot fail proves nothing.
 *
 * D3D12 adds two shapes the D3D11 rig never had, and both are here:
 *   - AGGREGATE RETURN, called the way MSVC calls it: RET* (ms_abi)(this,
 *     RET* __ret, args...).
 *   - a BY-VALUE 8-byte descriptor handle, declared with its real struct type
 *     so the compiler -- not this test -- decides its register class.
 */
#if !defined(__x86_64__)
#error "msabi_caller.cpp is x86-64 only"
#endif

#include "vkd3d_proxy.h"
#include "vkd3d_thunk_ids.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

/* ------------------------------------------------------- the loopback ---- */

static std::atomic<uint64_t> g_crossings{0};

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
    return vkd3d_host_entry(entry, args);
}

extern "C" uint32_t vkd3d_thunk_host_probe(void) {
    return vkd3d_host_probe();
}

/* ------------------------------------------------------------ harness ---- */

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
           std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

/* ------------------------------------------------------------ mock host --- */

struct MockObject;
struct MockSub { void** vptr; MockObject* owner; uint32_t iface; };

struct MockCpuHandle { uint64_t ptr; };
struct MockGpuHandle { uint64_t ptr; };

struct MockObject {
    MockSub  primary;
    MockSub  unknown;
    std::atomic<int> refs{1};
    uint64_t a[VKD3D_THUNK_ARGS] = {};
    float    f[4] = {};
    MockCpuHandle cpu{0};
    MockGpuHandle gpu{0};
    uint64_t agg_ret_slot = 0;
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

/* The host half is native, i.e. SysV here.  These are deliberately NOT ms_abi:
 * the whole point is that the convention is converted on the guest side. */
static int32_t mock_qi(void* self, const void* riid, void** ppv) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    *ppv = nullptr;
    if (!std::memcmp(riid, kIidIUnknown, 16)) {
        o->refs.fetch_add(1);
        *ppv = &o->unknown;
        return VKD3D_S_OK;
    }
    return VKD3D_E_NOINTERFACE;
}
static uint32_t mock_addref(void* self) {
    return static_cast<MockSub*>(self)->owner->refs.fetch_add(1) + 1;
}
static uint32_t mock_release(void* self) {
    return uint32_t(static_cast<MockSub*>(self)->owner->refs.fetch_sub(1) - 1);
}
static uint64_t mock_generic(void* self, uint64_t a0, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                             uint64_t a7, uint64_t a8, uint64_t a9) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->a[0]=a0; o->a[1]=a1; o->a[2]=a2; o->a[3]=a3; o->a[4]=a4;
    o->a[5]=a5; o->a[6]=a6; o->a[7]=a7; o->a[8]=a8; o->a[9]=a9;
    return 0xfeedfacecafebeefull;
}
static void mock_clear_dsv(void* self, MockCpuHandle dsv, uint32_t flags,
                           float depth, uint8_t stencil, uint32_t rect_count,
                           const void* rects) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->cpu  = dsv;
    o->a[1] = flags;
    o->a[2] = stencil;
    o->a[3] = rect_count;
    o->a[4] = reinterpret_cast<uint64_t>(rects);
    o->f[0] = depth;
}
static void mock_depth_bounds(void* self, float lo, float hi) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->f[0] = lo;
    o->f[1] = hi;
}
static void mock_set_root_table(void* self, uint64_t index, MockGpuHandle h) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->a[0] = index;
    o->gpu  = h;
}
static void* mock_get_cpu_handle(void* self, MockCpuHandle* ret) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->agg_ret_slot = reinterpret_cast<uint64_t>(ret);
    ret->ptr = 0x00c0ffee12345678ull;
    return ret;
}
static MockObject* g_out_object = nullptr;
static uint64_t mock_riid_out(void* self, uint64_t desc, uint64_t riid,
                              uint64_t pp) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->a[0] = desc; o->a[1] = riid; o->a[2] = pp;
    if (pp && g_out_object) {
        g_out_object->refs.fetch_add(1);
        *reinterpret_cast<uint64_t*>(pp) =
            reinterpret_cast<uint64_t>(&g_out_object->primary);
    }
    return uint64_t(uint32_t(VKD3D_S_OK));
}
/* The widest slot in the surface: ten parameters, the last two riid-driven. */
static uint64_t mock_create_res3(void* self, uint64_t a0, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5,
                                 uint64_t a6, uint64_t a7, uint64_t a8,
                                 uint64_t a9) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->a[0]=a0; o->a[1]=a1; o->a[2]=a2; o->a[3]=a3; o->a[4]=a4;
    o->a[5]=a5; o->a[6]=a6; o->a[7]=a7; o->a[8]=a8; o->a[9]=a9;
    if (a9 && g_out_object) {
        g_out_object->refs.fetch_add(1);
        *reinterpret_cast<uint64_t*>(a9) =
            reinterpret_cast<uint64_t>(&g_out_object->primary);
    }
    return uint64_t(uint32_t(VKD3D_S_OK));
}
static uint64_t mock_execute(void* self, uint64_t count, uint64_t pp) {
    MockObject* o = static_cast<MockSub*>(self)->owner;
    o->a[0] = count;
    o->a[1] = pp;
    if (pp && count)
        o->a[2] = reinterpret_cast<const uint64_t*>(pp)[0];
    return 0;
}

#define MOCK_VT_SLOTS 128
enum { VT_DEVICE = 0, VT_DEVICE10, VT_HEAP, VT_LIST, VT_QUEUE, VT_UNKNOWN,
       VT_ROLE_COUNT };
static void* g_vt[VT_ROLE_COUNT][MOCK_VT_SLOTS];

static void build_vtables() {
    for (int r = 0; r < VT_ROLE_COUNT; r++) {
        for (int i = 0; i < MOCK_VT_SLOTS; i++)
            g_vt[r][i] = (void*) mock_generic;
        g_vt[r][0] = (void*) mock_qi;
        g_vt[r][1] = (void*) mock_addref;
        g_vt[r][2] = (void*) mock_release;
    }
    g_vt[VT_DEVICE][VKD3D_SLOT_ID3D12DEVICE_CREATECOMMANDQUEUE] = (void*) mock_riid_out;
    g_vt[VT_DEVICE10][VKD3D_SLOT_ID3D12DEVICE10_CREATECOMMITTEDRESOURCE3] =
        (void*) mock_create_res3;
    g_vt[VT_HEAP][VKD3D_SLOT_ID3D12DESCRIPTORHEAP_GETCPUDESCRIPTORHANDLEFORHEAPSTART] =
        (void*) mock_get_cpu_handle;
    g_vt[VT_LIST][kVkdFloatSlot[VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW]] =
        (void*) mock_clear_dsv;
    g_vt[VT_LIST][kVkdFloatSlot[VKD3D_FSHAPE_OM_SET_DEPTH_BOUNDS]] =
        (void*) mock_depth_bounds;
    g_vt[VT_LIST][VKD3D_SLOT_ID3D12GRAPHICSCOMMANDLIST_SETGRAPHICSROOTDESCRIPTORTABLE] =
        (void*) mock_set_root_table;
    g_vt[VT_QUEUE][VKD3D_SLOT_ID3D12COMMANDQUEUE_EXECUTECOMMANDLISTS] =
        (void*) mock_execute;
}

static void mock_init(MockObject* o, uint32_t iface, int role) {
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

/* ---- the MS-x64 call site.  This is the part that has to be exercised. --- */

#define MS __attribute__((ms_abi))
typedef MS int32_t  (*MsQI)(void*, const void*, void**);
typedef MS uint32_t (*MsRef)(void*);
typedef MS uint64_t (*MsFn1)(void*, uint64_t);
typedef MS uint64_t (*MsFn2)(void*, uint64_t, uint64_t);
typedef MS uint64_t (*MsFn3)(void*, uint64_t, uint64_t, uint64_t);
typedef MS uint64_t (*MsFn10)(void*, uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t);
typedef MS void     (*MsClearDsv)(void*, MockCpuHandle, uint32_t, float, uint8_t,
                                  uint32_t, const void*);
typedef MS void     (*MsDepthBounds)(void*, float, float);
typedef MS void     (*MsRootTable)(void*, uint64_t, MockGpuHandle);
typedef MS MockCpuHandle* (*MsAggCpu)(void*, MockCpuHandle*);

int main() {
    /* Line-buffered: the negative control dies mid-run, and where it died is
     * the evidence that this test can see the defect. */
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    if (!(vkd3d_thunk_abi_available() & (1u << VKD3D_ABI_MS))) {
        std::printf("  FAIL this build has no MS-x64 vtables\n");
        return 1;
    }
    vkd3d_thunk_set_abi(VKD3D_ABI_MS);
    CHECK(vkd3d_thunk_abi_is_ms() == 1, "ABI mode is not MS-x64");

    build_vtables();

    MockObject dev, heap, list, queue, out, cmdlist;
    mock_init(&dev, VKD3D_IFACE_ID3D12DEVICE, VT_DEVICE);
    mock_init(&heap, VKD3D_IFACE_ID3D12DESCRIPTORHEAP, VT_HEAP);
    /* A DERIVED command list: OMSetDepthBounds and RSSetDepthBias only
     * exist from version 1 and 9 respectively, and the float slots are
     * inherited, so this also exercises the shape table's claim that every
     * version agrees on the slot number. */
    mock_init(&list, VKD3D_IFACE_ID3D12GRAPHICSCOMMANDLIST9, VT_LIST);
    mock_init(&queue, VKD3D_IFACE_ID3D12COMMANDQUEUE, VT_QUEUE);
    mock_init(&out, VKD3D_IFACE_ID3D12COMMANDQUEUE, VT_QUEUE);
    mock_init(&cmdlist, VKD3D_IFACE_ID3D12COMMANDLIST, VT_QUEUE);
    g_out_object = &out;

    /* wrap() CONSUMES a host reference, the way every COM producer hands you
     * one; model that, so the balance at the end means something. */
    for (MockObject* o : {&dev, &heap, &list, &queue, &cmdlist})
        mock_addref(&o->primary);
    void* pdev  = vkd3d_proxy_wrap(host_of(dev), VKD3D_IFACE_ID3D12DEVICE);
    void* pheap = vkd3d_proxy_wrap(host_of(heap), VKD3D_IFACE_ID3D12DESCRIPTORHEAP);
    void* plist = vkd3d_proxy_wrap(host_of(list), VKD3D_IFACE_ID3D12GRAPHICSCOMMANDLIST9);
    void* pq    = vkd3d_proxy_wrap(host_of(queue), VKD3D_IFACE_ID3D12COMMANDQUEUE);
    void* pcl   = vkd3d_proxy_wrap(host_of(cmdlist), VKD3D_IFACE_ID3D12COMMANDLIST);
    CHECK(pdev && pheap && plist && pq && pcl, "wrap failed");
    CHECK(as_proxy(pdev)->vtbl ==
              (const void*) vkd3d_thunk_vtable_for(VKD3D_IFACE_ID3D12DEVICE,
                                                   VKD3D_ABI_MS),
          "the proxy did not get the MS-x64 vtable");

    /* ---- 0. The seam itself, checked WITHOUT dereferencing anything.
     *
     * A slot override that records the `this` and the argument it was handed
     * and touches neither.  Under a correct build the ms_abi forwarder moves
     * RCX->RDI and RDX->RSI and the override sees the proxy; under the negative
     * control it sees whatever happened to be in RDI, and says so instead of
     * dying before any assertion can be printed.  Everything after this point
     * dereferences `this` and a wrong-convention build crashes there -- which
     * is exactly what a game would do at its first AddRef. */
    std::printf("[the ms_abi seam: this and the first argument]\n");
    {
        static void*    seen_self = nullptr;
        static uint64_t seen_arg  = 0;
        struct Ov {
            static uint64_t fn(void* self, uint64_t a) {
                seen_self = self;
                seen_arg  = a;
                return 0xabcdef01ull;
            }
        };
        const void** target = const_cast<const void**>(
            vkd3d_thunk_vtable(VKD3D_IFACE_ID3D12DEVICE));
        const void* const* ms =
            vkd3d_thunk_vtable_for(VKD3D_IFACE_ID3D12DEVICE, VKD3D_ABI_MS);
        uint32_t s = VKD3D_SLOT_ID3D12DEVICE_GETDESCRIPTORHANDLEINCREMENTSIZE;
        const void* saved = target[s];
        target[s] = (const void*) &Ov::fn;
        MsFn1 through_ms = reinterpret_cast<MsFn1>(
            reinterpret_cast<const uint64_t*>(ms)[s]);
        uint64_t got = through_ms(pdev, 0x777);
        target[s] = saved;
        CHECK(got == 0xabcdef01ull, "the forwarder did not reach the target "
              "(got 0x%llx)", (unsigned long long) got);
        CHECK(seen_self == pdev, "`this` arrived as %p, want the proxy %p -- the "
              "forwarder is not converting the calling convention",
              seen_self, pdev);
        CHECK(seen_arg == 0x777, "the first argument arrived as 0x%llx, want "
              "0x777", (unsigned long long) seen_arg);
    }

    /* ---- 1. AddRef / Release.  The first thing any game does, and the first
     * thing that breaks when a vtable is the wrong convention: `this` is in RCX
     * for an MS caller and RDI for SysV. */
    std::printf("[AddRef/Release through an MS-x64 call site]\n");
    MsRef addref  = reinterpret_cast<MsRef>(vslot(pdev, 1));
    MsRef release = reinterpret_cast<MsRef>(vslot(pdev, 2));
    uint32_t r1 = addref(pdev);
    CHECK(r1 == 2, "AddRef returned %u, want 2", r1);
    uint32_t r2 = addref(pdev);
    CHECK(r2 == 3, "AddRef returned %u, want 3", r2);
    CHECK(as_proxy(pdev)->refs.load() == 3, "guest refcount is %u, want 3",
          as_proxy(pdev)->refs.load());
    CHECK(release(pdev) == 2, "Release did not return 2");
    CHECK(release(pdev) == 1, "Release did not return 1");
    CHECK(dev.refs.load() == 2, "host refcount disturbed by guest AddRef/Release: %d",
          dev.refs.load());
    CHECK(g_crossings.load() == 0, "AddRef/Release crossed the boundary");

    /* ---- 2. QueryInterface: three arguments, RCX/RDX/R8. */
    std::printf("[QueryInterface through an MS-x64 call site]\n");
    MsQI qi = reinterpret_cast<MsQI>(vslot(pdev, 0));
    void* unk = nullptr;
    int32_t hr = qi(pdev, kIidIUnknown, &unk);
    CHECK(hr == VKD3D_S_OK, "QI(IUnknown) -> 0x%08x", unsigned(hr));
    CHECK(unk != nullptr && unk != pdev, "QI(IUnknown) gave %p", unk);
    CHECK(unk && as_proxy(unk)->host == reinterpret_cast<uint64_t>(&dev.unknown),
          "QI landed on the wrong host sub-object");
    if (unk) reinterpret_cast<MsRef>(vslot(unk, 2))(unk);

    /* ---- 3. Ten arguments: four in registers, six on the stack, plus 32
     * bytes of shadow space the SysV side does not have.  This is the widest
     * slot in the D3D12 surface. */
    std::printf("[ten integer arguments, register and stack]\n");
    MockObject dev10;
    mock_init(&dev10, VKD3D_IFACE_ID3D12DEVICE10, VT_DEVICE10);
    mock_addref(&dev10.primary);
    void* pdev10 = vkd3d_proxy_wrap(host_of(dev10), VKD3D_IFACE_ID3D12DEVICE10);
    MsFn10 create3 = reinterpret_cast<MsFn10>(
        vslot(pdev10, VKD3D_SLOT_ID3D12DEVICE10_CREATECOMMITTEDRESOURCE3));
    uint8_t iid_q[16];
    iid_of(VKD3D_IFACE_ID3D12COMMANDQUEUE, iid_q);
    void* made = nullptr;
    uint64_t rv = create3(pdev10, 0x1111111111111111ull, 0x2222222222222222ull,
                          0x3333333333333333ull, 0x4444444444444444ull,
                          0x5555555555555555ull, 0 /* IN iface: null */,
                          0x7777777777777777ull, 0x8888888888888888ull,
                          reinterpret_cast<uint64_t>(iid_q),
                          reinterpret_cast<uint64_t>(&made));
    CHECK(int32_t(uint32_t(rv)) == VKD3D_S_OK,
          "the arity-10 slot returned 0x%08x", unsigned(rv));
    bool ordered = true;
    for (int i = 0; i < 8; i++) {
        if (i == 5)
            continue;
        uint64_t want = 0x1111111111111111ull * uint64_t(i + 1);
        if (dev10.a[i] != want) {
            ordered = false;
            std::printf("  arg %d arrived as 0x%llx, want 0x%llx\n", i,
                        (unsigned long long) dev10.a[i], (unsigned long long) want);
        }
    }
    CHECK(ordered, "argument order/values mangled at arity 10");
    CHECK(dev10.a[5] == 0, "a null IN interface pointer did not stay null");
    CHECK(made && as_proxy(made)->host == host_of(out),
          "the riid out of the arity-10 slot was not wrapped");
    if (made) reinterpret_cast<MsRef>(vslot(made, 2))(made);

    /* ---- 4. Float placement, which is where a partial fix is still silently
     * wrong.  ClearDepthStencilView(this, D3D12_CPU_DESCRIPTOR_HANDLE, UINT,
     * FLOAT depth, UINT8, UINT, const D3D12_RECT*): the by-value handle takes
     * an INTEGER position, so `depth` is the FOURTH argument and MS-x64 puts it
     * in XMM3 while SysV puts the first float in XMM0. */
    std::printf("[float argument placement: MS-x64 XMM3 vs SysV XMM0]\n");
    MsClearDsv clear = reinterpret_cast<MsClearDsv>(
        vslot(plist, kVkdFloatSlot[VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW]));
    MockCpuHandle dsv{0x1234000000005678ull};
    clear(plist, dsv, 3u, 0.375f, 0xa5, 2u, reinterpret_cast<const void*>(0x99));
    CHECK(list.f[0] == 0.375f, "Depth arrived as %.9g, want 0.375", list.f[0]);
    CHECK(list.cpu.ptr == 0x1234000000005678ull,
          "the by-value handle in a float slot arrived as 0x%llx",
          (unsigned long long) list.cpu.ptr);
    CHECK(list.a[1] == 3u && list.a[2] == 0xa5 && list.a[3] == 2u && list.a[4] == 0x99,
          "the integer arguments around the float are wrong");

    /* Two floats: MS-x64 XMM1/XMM2, SysV XMM0/XMM1. */
    MsDepthBounds bounds = reinterpret_cast<MsDepthBounds>(
        vslot(plist, kVkdFloatSlot[VKD3D_FSHAPE_OM_SET_DEPTH_BOUNDS]));
    bounds(plist, 0.125f, 0.875f);
    CHECK(list.f[0] == 0.125f && list.f[1] == 0.875f,
          "depth bounds arrived as %.9g / %.9g", list.f[0], list.f[1]);

    /* ---- 5. A by-value 8-byte aggregate through an MS-x64 call site. */
    std::printf("[by-value descriptor handle through an MS-x64 call site]\n");
    MsRootTable table = reinterpret_cast<MsRootTable>(
        vslot(plist, VKD3D_SLOT_ID3D12GRAPHICSCOMMANDLIST_SETGRAPHICSROOTDESCRIPTORTABLE));
    MockGpuHandle gpu{0xfeedfacedeadbeefull};
    table(plist, 3, gpu);
    CHECK(list.a[0] == 3, "the root parameter index arrived as %llu",
          (unsigned long long) list.a[0]);
    CHECK(list.gpu.ptr == 0xfeedfacedeadbeefull,
          "the by-value GPU handle arrived as 0x%llx",
          (unsigned long long) list.gpu.ptr);

    /* ---- 6. AGGREGATE RETURN through an MS-x64 call site.  New relative to
     * the D3D11 rig: MSVC returns every class/struct from a member function
     * through a hidden pointer in the position right after `this`, and the widl
     * C vtable native vkd3d implements declares exactly that.  So the guest
     * stub, the forwarder and the native slot all agree on
     * (this, __ret, ...) -> __ret, with no special ABI anywhere. */
    std::printf("[aggregate return through an MS-x64 call site]\n");
    MsAggCpu get_cpu = reinterpret_cast<MsAggCpu>(
        vslot(pheap, VKD3D_SLOT_ID3D12DESCRIPTORHEAP_GETCPUDESCRIPTORHANDLEFORHEAPSTART));
    MockCpuHandle handle{0};
    MockCpuHandle* ret = get_cpu(pheap, &handle);
    CHECK(handle.ptr == 0x00c0ffee12345678ull,
          "the mock's write through __ret did not reach the caller: 0x%llx",
          (unsigned long long) handle.ptr);
    CHECK(ret == &handle, "the returned pointer is not __ret (%p vs %p)",
          (void*) ret, (void*) &handle);
    CHECK(heap.agg_ret_slot == reinterpret_cast<uint64_t>(&handle),
          "the host received a different __ret pointer than the caller passed");

    /* ---- 7. Interface marshalling reached through an MS call site: an IN
     * array and a riid-driven OUT, both inside stubs the PE caller invoked with
     * the wrong-by-default convention. */
    std::printf("[interface marshalling through an MS-x64 call site]\n");
    MsFn3 create_q = reinterpret_cast<MsFn3>(
        vslot(pdev, VKD3D_SLOT_ID3D12DEVICE_CREATECOMMANDQUEUE));
    void* newq = nullptr;
    hr = int32_t(create_q(pdev, 0x1234, reinterpret_cast<uint64_t>(iid_q),
                          reinterpret_cast<uint64_t>(&newq)));
    CHECK(hr == VKD3D_S_OK, "CreateCommandQueue -> 0x%08x", unsigned(hr));
    CHECK(newq && as_proxy(newq)->host == host_of(out),
          "the riid out wrapped the wrong host pointer");
    CHECK(newq && as_proxy(newq)->vtbl ==
              (const void*) vkd3d_thunk_vtable_for(VKD3D_IFACE_ID3D12COMMANDQUEUE,
                                                   VKD3D_ABI_MS),
          "a proxy produced by marshalling got the wrong ABI's vtable");
    if (newq) reinterpret_cast<MsRef>(vslot(newq, 2))(newq);

    void* lists[1] = {pcl};
    MsFn2 execute = reinterpret_cast<MsFn2>(
        vslot(pq, VKD3D_SLOT_ID3D12COMMANDQUEUE_EXECUTECOMMANDLISTS));
    execute(pq, 1, reinterpret_cast<uint64_t>(lists));
    CHECK(queue.a[0] == 1, "the command list count arrived as %llu",
          (unsigned long long) queue.a[0]);
    CHECK(queue.a[1] != reinterpret_cast<uint64_t>(lists),
          "the IN array passed the guest's proxy array straight through");
    CHECK(queue.a[2] == host_of(cmdlist),
          "the IN array element was not unwrapped to its host pointer");

    /* ---- 8. A slot override installed in the PE-facing target table is
     * reached through the MS vtable and NOT through the SysV one.  That is the
     * structural property the next work package (hand-written fixups for the
     * struct-with-interface slots) depends on. */
    std::printf("[a PE-side vtable override does not reach SysV callers]\n");
    {
        const void** target = const_cast<const void**>(
            vkd3d_thunk_vtable(VKD3D_IFACE_ID3D12DEVICE));
        const void* const* sysv =
            vkd3d_thunk_vtable_for(VKD3D_IFACE_ID3D12DEVICE, VKD3D_ABI_SYSV);
        const void* const* ms =
            vkd3d_thunk_vtable_for(VKD3D_IFACE_ID3D12DEVICE, VKD3D_ABI_MS);
        CHECK(target != sysv && target != ms, "the three arrays are not distinct");
        uint32_t s = VKD3D_SLOT_ID3D12DEVICE_GETDESCRIPTORHANDLEINCREMENTSIZE;
        const void* saved = target[s];
        /* A plain SysV override: the ms_abi forwarder has already converted the
         * convention by the time it is called, so an override does not have to
         * be ms_abi itself. */
        struct Ov { static uint64_t fn(Proxy*, uint64_t) { return 0xabcdef01ull; } };
        target[s] = (const void*) &Ov::fn;
        MsFn1 via_ms_fn = reinterpret_cast<MsFn1>(
            reinterpret_cast<const uint64_t*>(ms)[s]);
        uint64_t via_ms = via_ms_fn(pdev, 0);
        CHECK(via_ms == 0xabcdef01ull,
              "the PE-facing override was not reached through the MS vtable "
              "(got 0x%llx)", (unsigned long long) via_ms);
        CHECK(reinterpret_cast<const uint64_t*>(sysv)[s] !=
                  reinterpret_cast<uint64_t>(&Ov::fn),
              "patching the PE-facing table also changed the SysV vtable");
        target[s] = saved;
    }

    /* Tear down. */
    for (void* p : {pcl, pq, plist, pheap, pdev10, pdev})
        reinterpret_cast<MsRef>(vslot(p, 2))(p);
    CHECK(vkd3d_proxy_live_count() == 0, "%u proxies leaked",
          vkd3d_proxy_live_count());
    CHECK(dev.refs.load() == 1 && heap.refs.load() == 1 && list.refs.load() == 1 &&
          queue.refs.load() == 1 && cmdlist.refs.load() == 1,
          "host refcounts ended at %d/%d/%d/%d/%d, want 1/1/1/1/1",
          dev.refs.load(), heap.refs.load(), list.refs.load(), queue.refs.load(),
          cmdlist.refs.load());

    std::printf("\n%d passed, %d failed, %llu boundary crossings\n",
                g_pass, g_fail, (unsigned long long) g_crossings.load());
    return g_fail ? 1 : 0;
}
