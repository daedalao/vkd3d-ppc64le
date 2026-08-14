/* HAND-MAINTAINED -- not generated.
 *
 * Host-side runtime, native ppc64le (and x86-64, for the test rig): the dlopen
 * of vkd3d-proton, the eight flat entry points, the three float-class shapes,
 * and the fence/event pump that runtime/vkd3d_thunk_abi.h specifies.
 *
 * The generic vtable dispatcher lives in the generated file next to this one.
 * What is here is everything that cannot be generated: prototypes the
 * "pack into uint64_t[N]" convention cannot express, the fact that vkd3d is
 * attached at run time rather than linked, and the two slot overrides.
 *
 * NOTHING in this file includes a vkd3d or D3D12 header and the build links
 * against no vkd3d library: vkd3d-proton must stay swappable underneath the
 * emulator.  That is affordable only because the ABI is Microsoft's, not
 * vkd3d's -- vtable slot numbers come from the frozen D3D12 specification, so
 * the only thing a vkd3d upgrade can move is the address of these eight
 * symbols.
 *
 * Plain portable C++17: no x86 or POWER intrinsics, nothing conditional on the
 * host architecture.
 */
#include "vkd3d_thunk_ids.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace {

/* Every flat entry point takes integer-class arguments only, so one prototype
 * with the widest arity covers all eight (the widest is 4).  Calling a
 * two-argument function through a wider prototype is safe in both ABIs: SysV
 * x86-64 and ELFv2 are caller-cleanup, and the callee never reads the arguments
 * it does not declare. */
using EntryFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t);

const char* const kEntryName[VKD3D_ENTRY_COUNT_DLSYM] = {
    "D3D12CreateDevice",
    "D3D12GetDebugInterface",
    "D3D12GetInterface",
    "D3D12CreateRootSignatureDeserializer",
    "D3D12CreateVersionedRootSignatureDeserializer",
    "D3D12SerializeRootSignature",
    "D3D12SerializeVersionedRootSignature",
    "D3D12EnableExperimentalFeatures",
};

struct HostLib {
    void*   lib = nullptr;
    EntryFn fn[VKD3D_ENTRY_COUNT_DLSYM] = {};
    bool    ok  = false;
};

HostLib        g_lib;
std::once_flag g_lib_once;

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : fallback;
}

void load_lib() {
    /* The path is configurable so a Proton prefix can point at its own build
     * without this code knowing anything about the layout; the default is the
     * soname, resolved by the dynamic loader's normal search.
     *
     * RTLD_LOCAL: vkd3d's symbols must not land in the global namespace, where
     * they would be visible to -- and could collide with -- anything else in
     * the host process (dxvk's native half, above all).  RTLD_NOW so a missing
     * dependency fails here, with a message, rather than at the first call. */
    const char* path = env_or("VKD3D_THUNK_D3D12_LIB", "libvkd3d-proton-d3d12.so");

    g_lib.lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!g_lib.lib) {
        std::fprintf(stderr, "vkd3d_thunk: dlopen(%s) failed: %s\n", path, dlerror());
        return;
    }

    unsigned resolved = 0;
    for (unsigned i = 0; i < VKD3D_ENTRY_COUNT_DLSYM; i++) {
        void* sym = dlsym(g_lib.lib, kEntryName[i]);
        g_lib.fn[i] = reinterpret_cast<EntryFn>(sym);
        if (sym)
            resolved++;
        else
            std::fprintf(stderr, "vkd3d_thunk: %s unresolved\n", kEntryName[i]);
    }
    g_lib.ok = (resolved == VKD3D_ENTRY_COUNT_DLSYM);
    if (std::getenv("VKD3D_THUNK_TRACE"))
        std::fprintf(stderr, "vkd3d_thunk: host attached, %u/%u entry points\n",
                     resolved, unsigned(VKD3D_ENTRY_COUNT_DLSYM));
}

HostLib& libs() {
    std::call_once(g_lib_once, load_lib);
    return g_lib;
}

/* Call a real vtable slot on a host object.  Identical to the generated
 * dispatcher's tail; the pump overrides need it because they call the method
 * they are overriding. */
using GenericFn = uint64_t (*)(void*, uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t);

uint64_t call_slot(uint64_t host, uint32_t slot, const uint64_t* a) {
    void*  obj  = reinterpret_cast<void*>(host);
    void** vtbl = *reinterpret_cast<void***>(obj);
    GenericFn fn = reinterpret_cast<GenericFn>(vtbl[slot]);
    return fn(obj, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]);
}

/* ---------------------------------------------------------------- the pump
 *
 * runtime/vkd3d_thunk_abi.h, "fence/event pump", is the specification; this is
 * its host half.
 *
 * ONE GLOBAL PUMP.  The contract describes the queue as per-device and passes a
 * device cookie in args[0] of PUMP_WAIT; v1 keeps a single reaper thread, a
 * single completion queue and a single doorbell, and IGNORES that cookie.  That
 * is permitted by the header's own wording -- the PE shim creates one pump
 * thread per device, and several pump threads sharing one queue is harmless
 * because a cookie is just handed to whichever guest thread wakes up, and the
 * guest calls SetEvent(cookie) with it.  What it costs is that a second device
 * cannot be shut down independently of the first.  Making the queue per-device
 * is a change to this file alone; nothing in the boundary contract or in the
 * guest half has to move.
 */
struct PumpMsg {
    uint32_t op;        /* 0 = watch this fd, 1 = shut down */
    int      fd;
    uint64_t cookie;
};

struct Pump {
    int  ctrl_r   = -1;      /* reaper wakes on this ... */
    int  ctrl_w   = -1;      /* ... and registrations are written here */
    int  doorbell = -1;      /* EFD_SEMAPHORE: one post per completion */
    bool started  = false;
    pthread_t reaper{};

    std::mutex           m;
    std::deque<uint64_t> completions;
    std::vector<int>     pool;          /* recycled eventfds */

    std::atomic<bool>     shutting_down{false};
    std::atomic<uint32_t> registered{0};
    std::atomic<uint32_t> fired{0};
};

Pump           g_pump;
std::once_flag g_pump_once;

void* reaper_main(void*);

void pump_start() {
    int ctrl[2];
    if (pipe(ctrl) != 0) {
        std::fprintf(stderr, "vkd3d_thunk: pump: pipe() failed: %s\n",
                     std::strerror(errno));
        return;
    }
    /* The reaper drains the control pipe without blocking; registrations from
     * other threads are single writes of one message, which the kernel keeps
     * atomic well below PIPE_BUF. */
    fcntl(ctrl[0], F_SETFL, fcntl(ctrl[0], F_GETFL, 0) | O_NONBLOCK);
    g_pump.ctrl_r = ctrl[0];
    g_pump.ctrl_w = ctrl[1];

    /* EFD_SEMAPHORE so one read == one completion; a plain counting eventfd
     * would hand the whole backlog to the first waiter and leave the rest
     * unsignalled. */
    g_pump.doorbell = eventfd(0, EFD_SEMAPHORE);
    if (g_pump.doorbell < 0) {
        std::fprintf(stderr, "vkd3d_thunk: pump: eventfd() failed: %s\n",
                     std::strerror(errno));
        return;
    }

    if (pthread_create(&g_pump.reaper, nullptr, reaper_main, nullptr) != 0) {
        std::fprintf(stderr, "vkd3d_thunk: pump: pthread_create failed\n");
        return;
    }
    g_pump.started = true;
}

Pump& pump() {
    std::call_once(g_pump_once, pump_start);
    return g_pump;
}

/* Allocate an eventfd for one registration.
 *
 * NEVER fd 0: native vkd3d treats a native sync handle as `(int)(intptr_t)
 * os_handle` and takes 0 to mean "no handle" (include/private/
 * vkd3d_native_sync_handle.h), so an eventfd that landed on descriptor 0 would
 * be silently dropped.  If the kernel does hand us 0 -- which can only happen
 * when the process has closed stdin -- we dup it and keep descriptor 0 open and
 * unused forever, deliberately, so no later allocation can land there either. */
int pump_alloc_fd(Pump& p) {
    {
        std::lock_guard<std::mutex> lk(p.m);
        if (!p.pool.empty()) {
            int fd = p.pool.back();
            p.pool.pop_back();
            return fd;
        }
    }
    /* EFD_NONBLOCK: the reaper reads these only after poll() says readable, and
     * recycling drains them without ever being able to block. */
    int fd = eventfd(0, EFD_NONBLOCK);
    if (fd < 0) {
        std::fprintf(stderr, "vkd3d_thunk: pump: eventfd() failed: %s\n",
                     std::strerror(errno));
        return -1;
    }
    if (fd == 0) {
        int nfd = dup(fd);      /* descriptor 0 stays open on purpose */
        if (nfd < 0) {
            std::fprintf(stderr, "vkd3d_thunk: pump: cannot move an eventfd off "
                         "descriptor 0\n");
            return -1;
        }
        fd = nfd;
    }
    return fd;
}

void pump_recycle_fd(Pump& p, int fd) {
    if (fd < 0)
        return;
    uint64_t drain;
    while (read(fd, &drain, sizeof(drain)) == sizeof(drain))
        ; /* leave it unsignalled for the next user */
    std::lock_guard<std::mutex> lk(p.m);
    p.pool.push_back(fd);
}

void pump_post(Pump& p, uint64_t cookie) {
    {
        std::lock_guard<std::mutex> lk(p.m);
        p.completions.push_back(cookie);
    }
    uint64_t one = 1;
    ssize_t r = write(p.doorbell, &one, sizeof(one));
    (void) r;
}

void* reaper_main(void*) {
    Pump& p = g_pump;
    std::vector<struct pollfd> fds;
    std::vector<uint64_t>      cookies;   /* parallel to fds[1..] */
    bool done = false;

    fds.push_back(pollfd{p.ctrl_r, POLLIN, 0});

    while (true) {
        int n = poll(fds.data(), nfds_t(fds.size()), -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            std::fprintf(stderr, "vkd3d_thunk: pump: poll() failed: %s\n",
                         std::strerror(errno));
            break;
        }

        if (fds[0].revents & POLLIN) {
            PumpMsg msg;
            while (read(p.ctrl_r, &msg, sizeof(msg)) == ssize_t(sizeof(msg))) {
                if (msg.op == 1) {
                    done = true;
                } else {
                    fds.push_back(pollfd{msg.fd, POLLIN, 0});
                    cookies.push_back(msg.cookie);
                }
            }
        }

        for (size_t i = 1; i < fds.size();) {
            if (!(fds[i].revents & (POLLIN | POLLERR | POLLHUP))) {
                ++i;
                continue;
            }
            uint64_t v = 0;
            ssize_t  r = read(fds[i].fd, &v, sizeof(v));
            (void) r;
            p.fired.fetch_add(1, std::memory_order_relaxed);
            pump_post(p, cookies[i - 1]);
            pump_recycle_fd(p, fds[i].fd);
            fds.erase(fds.begin() + i);
            cookies.erase(cookies.begin() + (i - 1));
        }

        /* Registrations still pending at shutdown are abandoned rather than
         * waited for: the guest is going away and their events will never be
         * looked at again. */
        if (done)
            break;
    }
    return nullptr;
}

/* PUMP_WAIT.  args[0] is the device cookie -- accepted and ignored, see the
 * one-global-pump note above.  Blocks host-side on the doorbell, which is a
 * FEX-legal place to block because the guest called us. */
uint32_t pump_wait(uint64_t* a) {
    Pump& p = pump();
    if (!p.started)
        return 0;
    a[1] = 0;

    while (true) {
        uint64_t one = 0;
        ssize_t  r   = read(p.doorbell, &one, sizeof(one));
        if (r < 0 && errno == EINTR)
            continue;
        if (r != ssize_t(sizeof(one)))
            return 0;

        {
            std::lock_guard<std::mutex> lk(p.m);
            if (!p.completions.empty()) {
                a[1] = p.completions.front();
                p.completions.pop_front();
                return 1;
            }
        }
        if (p.shutting_down.load(std::memory_order_acquire))
            return 0;
    }
}

uint32_t pump_shutdown() {
    Pump& p = pump();
    if (!p.started)
        return 0;
    p.shutting_down.store(true, std::memory_order_release);

    PumpMsg msg{1, -1, 0};
    ssize_t w = write(p.ctrl_w, &msg, sizeof(msg));
    (void) w;

    /* Wake every possible waiter.  With EFD_SEMAPHORE one post wakes exactly
     * one read, and the guest may legitimately have several pump threads
     * parked here (one per device, all sharing this queue in v1). */
    uint64_t many = 64;
    ssize_t r = write(p.doorbell, &many, sizeof(many));
    (void) r;
    return 1;
}

/* The override itself.  hEvent == NULL is the D3D12 blocking-wait contract and
 * is forwarded unchanged: native vkd3d blocks the calling thread on a condvar
 * until the value completes, which is correct across a synchronous crossing.
 * Otherwise the guest HANDLE is a Wine/ntsync object that means nothing to
 * native vkd3d, so it is replaced with a pooled eventfd and the guest's HANDLE
 * bits ride along as the completion cookie. */
uint64_t pump_set_event(uint32_t slot, uint64_t host, uint64_t* a,
                        unsigned event_index) {
    Pump& p = pump();
    uint64_t cookie = a[event_index];

    if (!cookie || !p.started)
        return call_slot(host, slot, a);

    int fd = pump_alloc_fd(p);
    if (fd < 0)
        return uint64_t(uint32_t(VKD3D_E_OUTOFMEMORY));

    uint64_t saved = a[event_index];
    a[event_index] = uint64_t(uint32_t(fd));   /* vkd3d reads this as an fd */
    uint64_t hr = call_slot(host, slot, a);
    a[event_index] = saved;                    /* leave the guest's block alone */

    if (int32_t(uint32_t(hr)) < 0) {
        pump_recycle_fd(p, fd);
        return hr;
    }

    /* Registering AFTER the call is what makes the already-complete case work:
     * vkd3d signals the eventfd immediately, and poll() reports a level that is
     * already high the moment the reaper adds it. */
    PumpMsg msg{0, fd, cookie};
    if (write(p.ctrl_w, &msg, sizeof(msg)) != ssize_t(sizeof(msg))) {
        std::fprintf(stderr, "vkd3d_thunk: pump: cannot register a completion\n");
        pump_recycle_fd(p, fd);
        return hr;
    }
    p.registered.fetch_add(1, std::memory_order_relaxed);
    return hr;
}

} /* namespace */

/* ------------------------------------------------------------- exports ---- */

extern "C" uint32_t vkd3d_host_probe(void) {
    HostLib& l = libs();
    unsigned n = 0;
    for (unsigned i = 0; i < VKD3D_ENTRY_COUNT_DLSYM; i++)
        if (l.fn[i])
            n++;
    return n;
}

extern "C" uint32_t vkd3d_host_entry(uint32_t entry, uint64_t* a) {
    if (!a) {
        std::fprintf(stderr, "vkd3d_thunk: entry %u with no argument block\n", entry);
        return uint32_t(VKD3D_E_INVALIDARG);
    }

    /* The pump ops are implemented here, not by vkd3d. */
    if (entry == VKD3D_ENTRY_PUMP_WAIT)
        return pump_wait(a);
    if (entry == VKD3D_ENTRY_PUMP_SHUTDOWN)
        return pump_shutdown();

    if (entry >= VKD3D_ENTRY_COUNT_DLSYM) {
        std::fprintf(stderr, "vkd3d_thunk: bad entry %u\n", entry);
        return uint32_t(VKD3D_E_INVALIDARG);
    }
    EntryFn fn = libs().fn[entry];
    if (!fn)
        return uint32_t(VKD3D_E_FAIL);

    /* Out-parameters are guest addresses.  The host writes host interface
     * pointers straight into them; the guest turns those into proxies.  Legal
     * only because FEX shares the address space -- the one property this whole
     * design rests on. */
    return uint32_t(fn(a[0], a[1], a[2], a[3], a[4], a[5]));
}

/* Reached from the generated dispatcher when a slot carries VKD3D_SLOT_PUMP.
 * The (iface, slot) pairs are generated from the census, so a new device
 * version inheriting SetEventOnMultipleFenceCompletion is covered without
 * anything here changing. */
extern "C" uint64_t vkd3d_host_pump_dispatch(uint32_t kind, uint32_t iface,
                                             uint32_t slot, uint64_t host,
                                             uint64_t* a) {
    (void) iface;
    switch (kind) {
    case VKD3D_PUMP_FENCE_EVENT:
        /* ID3D12Fence::SetEventOnCompletion(UINT64 value, HANDLE event) */
        return pump_set_event(slot, host, a, 1);
    case VKD3D_PUMP_MULTI_FENCE_EVENT:
        /* ID3D12Device1::SetEventOnMultipleFenceCompletion(fences, values,
         * count, flags, HANDLE event) -- the fence array was already unwrapped
         * guest-side (ARRAY_SPECS), so only the event needs replacing. */
        return pump_set_event(slot, host, a, 4);
    default:
        std::fprintf(stderr, "vkd3d_thunk: bad pump kind %u\n", kind);
        return call_slot(host, slot, a);
    }
}

/* Test/diagnostic hooks: how many registrations the pump took and how many
 * eventfds it has seen fire. */
extern "C" uint32_t vkd3d_host_pump_registered(void) {
    return g_pump.registered.load(std::memory_order_relaxed);
}

extern "C" uint32_t vkd3d_host_pump_fired(void) {
    return g_pump.fired.load(std::memory_order_relaxed);
}

/* ------------------------------------------- the three float-class shapes */

/* Switching on the shape rather than on (iface, slot): ClearDepthStencilView is
 * the same slot in ID3D12GraphicsCommandList and in all ten versions of it, so
 * a switch on (iface, slot) would need 23 cases and would be the exact thing
 * that rots.  The guest picks the shape from a generated table.
 *
 * VkdCpuDescriptorHandle is this project's own POD of the same 8-byte shape as
 * D3D12_CPU_DESCRIPTOR_HANDLE (generated into vkd3d_thunk_ids.h); passing it by
 * value is what makes the native callee read `depth` out of the FP register the
 * ABI actually put it in. */
extern "C" uint64_t vkd3d_host_dispatch_float(uint32_t iface, uint32_t slot,
                                              uint32_t shape, uint64_t host,
                                              uint64_t* a, const float* fin,
                                              float* fout) {
    (void) fout;
    if (iface >= VKD3D_IFACE_COUNT || slot >= kVkdSlotCount[iface] || !host || !a) {
        std::fprintf(stderr, "vkd3d_thunk: bad float dispatch iface=%u slot=%u "
                     "host=%p\n", iface, slot, (void*) host);
        return 0;
    }
    void*  obj  = reinterpret_cast<void*>(host);
    void** vtbl = *reinterpret_cast<void***>(obj);
    void*  fp   = vtbl[slot];

    switch (shape) {
    case VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW: {
        using Fn = void (*)(void*, VkdCpuDescriptorHandle, uint32_t, float,
                            uint8_t, uint32_t, const void*);
        if (!fin)
            return 0;
        reinterpret_cast<Fn>(fp)(obj, vkd3d_agg_from<VkdCpuDescriptorHandle>(a[0]),
                                 uint32_t(a[1]), fin[0], uint8_t(a[2]),
                                 uint32_t(a[3]),
                                 reinterpret_cast<const void*>(a[4]));
        return 0;
    }
    case VKD3D_FSHAPE_OM_SET_DEPTH_BOUNDS: {
        using Fn = void (*)(void*, float, float);
        if (!fin)
            return 0;
        reinterpret_cast<Fn>(fp)(obj, fin[0], fin[1]);
        return 0;
    }
    case VKD3D_FSHAPE_RS_SET_DEPTH_BIAS: {
        using Fn = void (*)(void*, float, float, float);
        if (!fin)
            return 0;
        reinterpret_cast<Fn>(fp)(obj, fin[0], fin[1], fin[2]);
        return 0;
    }
    default:
        std::fprintf(stderr, "vkd3d_thunk: bad float shape %u\n", shape);
        return 0;
    }
}
