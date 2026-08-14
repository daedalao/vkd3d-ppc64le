/* GPU round trip through the emulated boundary.
 *
 * x86-64 guest ELF, run under FEX on the POWER box. Everything attach.c
 * proves, plus a real device: emulated caller -> guest proxies -> 0F 3F ->
 * native ppc64le vkd3d-proton -> RADV -> GPU -> and every result read back
 * and verified on the emulated side.
 *
 *   1. D3D12CreateDevice                (flat entry, riid out, proxy wrap)
 *   2. CreateFence + CPU Signal + the FENCE PUMP: SetEventOnCompletion with a
 *      fake guest-HANDLE cookie, then PUMP_WAIT returns exactly that cookie
 *      (the whole eventfd/reaper/doorbell machinery, under FEX)
 *   3. CreateCommandQueue/Allocator/List (riid outs, IN-iface unwrap of the
 *      allocator, NULL pInitialState)
 *   4. UPLOAD + READBACK committed buffers, Map/write/Unmap through the
 *      shared address space
 *   5. CopyBufferRegion + Close + ExecuteCommandLists (IN-iface array unwrap)
 *   6. queue->Signal(fence, 2) then SetEventOnCompletion(2, NULL) -- the
 *      blocking-wait contract: the guest thread parks inside native vkd3d
 *      until the GPU is done
 *   7. Map the readback buffer and verify all 4096 bytes
 *
 * MUST run with VKD3D_THUNK_ABI=sysv (this is an ELF caller). Slot numbers
 * are the frozen D3D12 vtable slots from ppc64le/thunk/interfaces.json; the
 * proxies' vtables are the generated SysV workers, so every call below takes
 * (Proxy*, uint64_t args...) and returns uint64_t.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vkd3d_unixlib.h"

/* ---- minimal D3D12 surface (values verified against ppc64le/idl/gen) ---- */

#define FL_12_0 0xc000u
#define HEAP_TYPE_UPLOAD 2u
#define HEAP_TYPE_READBACK 3u
#define RES_STATE_GENERIC_READ 0xac3u
#define RES_STATE_COPY_DEST 0x400u
#define RES_DIM_BUFFER 1u
#define LAYOUT_ROW_MAJOR 1u

typedef struct {
  int32_t Type;
  int32_t Priority;
  uint32_t Flags;
  uint32_t NodeMask;
} QueueDesc; /* D3D12_COMMAND_QUEUE_DESC, 16 bytes */

typedef struct {
  int32_t Type;
  int32_t CPUPageProperty;
  int32_t MemoryPoolPreference;
  uint32_t CreationNodeMask;
  uint32_t VisibleNodeMask;
} HeapProps; /* D3D12_HEAP_PROPERTIES, 20 bytes */

typedef struct {
  int32_t Dimension;
  uint64_t Alignment;
  uint64_t Width;
  uint32_t Height;
  uint16_t DepthOrArraySize;
  uint16_t MipLevels;
  int32_t Format;
  uint32_t SampleCount;
  uint32_t SampleQuality;
  int32_t Layout;
  uint32_t Flags;
} ResDesc; /* D3D12_RESOURCE_DESC, 56 bytes -- layout parity proven */

typedef struct {
  uint64_t Begin;
  uint64_t End;
} Range; /* D3D12_RANGE */

/* IIDs in memory layout (Data1 LE, Data2/3 LE, Data4 bytes) */
static const uint8_t IID_Device[16] = {0xf1, 0x19, 0x98, 0x18, 0xb6, 0x1d, 0x57, 0x4b,
                                       0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7};
static const uint8_t IID_Fence[16] = {0xcf, 0x3d, 0x75, 0x0a, 0xd8, 0xc4, 0x91, 0x4b,
                                      0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76};
static const uint8_t IID_Queue[16] = {0xa6, 0x70, 0xc8, 0x0e, 0x7e, 0x5d, 0x22, 0x4c,
                                      0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed};
static const uint8_t IID_Alloc[16] = {0xe4, 0xde, 0x02, 0x61, 0x59, 0xaf, 0x09, 0x4b,
                                      0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24};
static const uint8_t IID_List[16] = {0x0f, 0x0d, 0x16, 0x5b, 0x1b, 0xac, 0x85, 0x41,
                                     0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55};
static const uint8_t IID_Resource[16] = {0xbe, 0x42, 0x64, 0x69, 0x2e, 0xa7, 0x59, 0x40,
                                         0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad};

/* Every generated SysV worker is (Proxy*, uint64_t...) -> uint64_t. */
typedef uint64_t (*M0)(void*);
typedef uint64_t (*M1)(void*, uint64_t);
typedef uint64_t (*M2)(void*, uint64_t, uint64_t);
typedef uint64_t (*M3)(void*, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*M4)(void*, uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*M5)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*M6)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*M7)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

#define VT(obj) (*(void***)(obj))
#define CALL0(o, s) ((M0)VT(o)[s])(o)
#define CALL1(o, s, a) ((M1)VT(o)[s])(o, (uint64_t)(a))
#define CALL2(o, s, a, b) ((M2)VT(o)[s])(o, (uint64_t)(a), (uint64_t)(b))
#define CALL3(o, s, a, b, c) ((M3)VT(o)[s])(o, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c))
#define CALL4(o, s, a, b, c, d) ((M4)VT(o)[s])(o, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d))
#define CALL5(o, s, a, b, c, d, e) \
  ((M5)VT(o)[s])(o, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), (uint64_t)(e))
#define CALL6(o, s, a, b, c, d, e, f) \
  ((M6)VT(o)[s])(o, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), (uint64_t)(e), (uint64_t)(f))
#define CALL7(o, s, a, b, c, d, e, f, g) \
  ((M7)VT(o)[s])(o, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), (uint64_t)(e), (uint64_t)(f), (uint64_t)(g))

#define RELEASE(o) CALL0(o, 2)

static int failures;
#define CHECK(cond, ...)                 \
  do {                                   \
    if (cond) {                          \
      printf("ok   " __VA_ARGS__);       \
      printf("\n");                      \
    } else {                             \
      printf("FAIL " __VA_ARGS__);       \
      printf("\n");                      \
      failures++;                        \
    }                                    \
  } while (0)

#define BYTES 4096u
#define WORDS (BYTES / 4u)
#define PUMP_COOKIE 0x1234abcdu

typedef int32_t (*pfn_create_device)(void* adapter, uint32_t fl, const void* riid, void** out);
typedef uint32_t (*pfn_call_entry)(uint32_t entry, uint64_t* args);

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "./build/libvkd3d_d3d12-guest.so";
  void* h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  if (!h) {
    printf("FAIL dlopen(%s): %s\n", path, dlerror());
    return 1;
  }

  pfn_create_device create_device = (pfn_create_device)dlsym(h, "D3D12CreateDevice");
  pfn_call_entry call_entry = (pfn_call_entry)dlsym(h, "vkd3d_thunk_call_entry");
  if (!create_device || !call_entry) {
    printf("FAIL dlsym entry points\n");
    return 1;
  }

  /* 1 -- the device */
  void* device = 0;
  int32_t hr = create_device(0, FL_12_0, IID_Device, &device);
  CHECK(hr == 0 && device, "D3D12CreateDevice -> 0x%08x, device proxy %p", (unsigned)hr, device);
  if (failures)
    return 1;

  /* 2 -- fence + pump.  Register for value 1 with a fake guest-HANDLE cookie,
   * CPU-signal to 1, then PUMP_WAIT must hand back exactly that cookie. */
  void* fence = 0;
  hr = (int32_t)CALL4(device, 36, 0 /*initial*/, 0 /*flags*/, IID_Fence, &fence);
  CHECK(hr == 0 && fence, "CreateFence -> 0x%08x, fence proxy %p", (unsigned)hr, fence);

  hr = (int32_t)CALL2(fence, 9, 1 /*value*/, PUMP_COOKIE);
  CHECK(hr == 0, "SetEventOnCompletion(1, cookie) -> 0x%08x", (unsigned)hr);

  hr = (int32_t)CALL1(fence, 10, 1); /* ID3D12Fence::Signal(1), CPU side */
  CHECK(hr == 0, "fence->Signal(1) -> 0x%08x", (unsigned)hr);

  {
    uint64_t args[8];
    memset(args, 0, sizeof(args));
    uint32_t rc = call_entry(32 /*VKD3D_ENTRY_PUMP_WAIT*/, args);
    CHECK(rc == 1 && args[1] == PUMP_COOKIE, "PUMP_WAIT -> rc=%u cookie=0x%llx (want 0x%x)", rc,
          (unsigned long long)args[1], PUMP_COOKIE);
  }

  uint64_t completed = CALL0(fence, 8);
  CHECK(completed == 1, "GetCompletedValue -> %llu (want 1)", (unsigned long long)completed);

  /* 3 -- queue, allocator, list */
  QueueDesc qd;
  memset(&qd, 0, sizeof(qd)); /* DIRECT, NORMAL, NONE, node 0 */
  void* queue = 0;
  hr = (int32_t)CALL3(device, 8, &qd, IID_Queue, &queue);
  CHECK(hr == 0 && queue, "CreateCommandQueue -> 0x%08x, proxy %p", (unsigned)hr, queue);

  void* alloc = 0;
  hr = (int32_t)CALL3(device, 9, 0 /*DIRECT*/, IID_Alloc, &alloc);
  CHECK(hr == 0 && alloc, "CreateCommandAllocator -> 0x%08x, proxy %p", (unsigned)hr, alloc);

  void* list = 0;
  hr = (int32_t)CALL6(device, 12, 0 /*node*/, 0 /*DIRECT*/, alloc, 0 /*pInitialState NULL*/, IID_List, &list);
  CHECK(hr == 0 && list, "CreateCommandList(alloc proxy unwrapped) -> 0x%08x, proxy %p", (unsigned)hr, list);

  if (failures)
    return 1;

  /* 4 -- buffers */
  HeapProps up_heap, rb_heap;
  memset(&up_heap, 0, sizeof(up_heap));
  memset(&rb_heap, 0, sizeof(rb_heap));
  up_heap.Type = HEAP_TYPE_UPLOAD;
  rb_heap.Type = HEAP_TYPE_READBACK;

  ResDesc rd;
  memset(&rd, 0, sizeof(rd));
  rd.Dimension = RES_DIM_BUFFER;
  rd.Width = BYTES;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleCount = 1;
  rd.Layout = LAYOUT_ROW_MAJOR;

  void* upload = 0;
  hr = (int32_t)CALL7(device, 27, &up_heap, 0 /*heap flags*/, &rd, RES_STATE_GENERIC_READ, 0 /*clear*/, IID_Resource,
                      &upload);
  CHECK(hr == 0 && upload, "CreateCommittedResource(UPLOAD) -> 0x%08x, proxy %p", (unsigned)hr, upload);

  void* readback = 0;
  hr = (int32_t)CALL7(device, 27, &rb_heap, 0, &rd, RES_STATE_COPY_DEST, 0, IID_Resource, &readback);
  CHECK(hr == 0 && readback, "CreateCommittedResource(READBACK) -> 0x%08x, proxy %p", (unsigned)hr, readback);

  if (failures)
    return 1;

  /* Map/write through the shared address space. The pointer written by native
   * vkd3d is a host VA; this emulated guest writes straight through it. */
  {
    void* p = 0;
    hr = (int32_t)CALL3(upload, 8, 0 /*sub*/, 0 /*range=NULL: whole*/, &p);
    CHECK(hr == 0 && p, "upload->Map -> 0x%08x, ptr %p", (unsigned)hr, p);
    if (!p)
      return 1;
    uint32_t* w = (uint32_t*)p;
    for (uint32_t i = 0; i < WORDS; ++i)
      w[i] = i * 2654435761u + 0x9e3779b9u;
    CALL2(upload, 9, 0, 0); /* Unmap(0, NULL) */
    printf("ok   pattern written through host pointer\n");
  }

  /* 5 -- record + submit */
  CALL5(list, 15, readback, 0, upload, 0, BYTES); /* CopyBufferRegion: both resources are IN-iface proxies */
  hr = (int32_t)CALL0(list, 9); /* Close */
  CHECK(hr == 0, "list->Close -> 0x%08x", (unsigned)hr);

  {
    void* lists[1] = {list};
    CALL2(queue, 10, 1, lists); /* ExecuteCommandLists: IN-iface array unwrap */
    printf("ok   ExecuteCommandLists submitted\n");
  }

  /* 6 -- GPU-side signal + the blocking-wait contract */
  hr = (int32_t)CALL2(queue, 14, fence, 2); /* queue->Signal(fence proxy, 2) */
  CHECK(hr == 0, "queue->Signal(fence, 2) -> 0x%08x", (unsigned)hr);

  hr = (int32_t)CALL2(fence, 9, 2, 0 /*NULL event: block until done*/);
  CHECK(hr == 0, "SetEventOnCompletion(2, NULL) blocking wait -> 0x%08x", (unsigned)hr);

  completed = CALL0(fence, 8);
  CHECK(completed == 2, "GetCompletedValue -> %llu (want 2)", (unsigned long long)completed);

  /* 7 -- read back and verify every byte */
  {
    void* p = 0;
    Range wrote = {0, 0}; /* we will not write the readback buffer */
    hr = (int32_t)CALL3(readback, 8, 0, 0, &p);
    CHECK(hr == 0 && p, "readback->Map -> 0x%08x, ptr %p", (unsigned)hr, p);
    if (p) {
      const uint32_t* r = (const uint32_t*)p;
      uint32_t bad = 0;
      for (uint32_t i = 0; i < WORDS; ++i)
        if (r[i] != i * 2654435761u + 0x9e3779b9u)
          bad++;
      CHECK(bad == 0, "readback verified: %u/%u words exact", WORDS - bad, WORDS);
      CALL2(readback, 9, 0, &wrote); /* Unmap(0, &empty) */
    }
  }

  /* teardown, reverse order */
  RELEASE(list);
  RELEASE(alloc);
  RELEASE(upload);
  RELEASE(readback);
  RELEASE(queue);
  RELEASE(fence);
  uint32_t devrefs = (uint32_t)RELEASE(device);
  printf("ok   released everything (device refcount after release: %u)\n", devrefs);

  printf("%s\n", failures ? "FAILURES" : "ALL OK -- emulated x86-64 drove native ppc64le D3D12 to the GPU and back");
  return failures != 0;
}
