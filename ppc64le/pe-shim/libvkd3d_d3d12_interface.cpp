/* thunkgen interface definition for the vkd3d-proton D3D12 thunk.
 *
 * Four functions, all scalar-argument, all custom_host_impl. See
 * include/vkd3d_thunk_api.h for why the surface is this shape.
 *
 * -- the `version = 0` member, and the one deployment step it implies --------
 *
 * thunkgen always emits a loader that dlopen()s "<libname>.so[.version]"
 * (Generator/gen.cpp: library_filename = libfilename + ".so" + version_suffix)
 * and makes EXPORTS() hand FEX a null table when it fails. <libname> is this
 * thunk's own name, so the file it looks for is
 *
 *     libvkd3d_d3d12.so.0
 *
 * dxvk-ppc64le got that name for free -- its thunk is called libdxvk_d3d11 and
 * DXVK's native SONAME really is libdxvk_d3d11.so.0. vkd3d-proton's native
 * libraries are libvkd3d-proton-d3d12.so (the eight-export loader shim) and
 * libvkd3d-proton-d3d12core.so (the implementation), with no version suffix at
 * all: libs/d3d12/meson.build passes no `version`/`soversion` to
 * shared_library(). The names cannot be made to coincide -- the thunk name is
 * also the LOAD_LIB token and a C identifier prefix, so it cannot contain the
 * hyphens.
 *
 * So the deployment provides the name as a SYMLINK, and that is the entire
 * install step for this gate:
 *
 *     ln -sf <native>/libvkd3d-proton-d3d12.so \
 *            $FEX_THUNKHOSTLIBS/libvkd3d_d3d12.so.0
 *
 * build.sh creates it in build/ when it can find a native build, run-attach.sh
 * puts build/ on LD_LIBRARY_PATH, and README.md records the failure signature
 * when it is missing ("Failed to initialize thunk library" at load, before any
 * D3D12 call). Keeping the gate is deliberate: it is what turns "vkd3d-proton
 * is not installed" into a load-time diagnosis instead of a null call at the
 * first CreateDevice, and it is the thunkgen-layer expression of "vkd3d-proton
 * is always dynamically attached" -- nothing here links against it.
 *
 * None of the four functions exists in vkd3d-proton, so the generated dlsym
 * for each yields a null fexldr_ptr_* that is never called; custom_host_impl
 * is what routes every one of them to the implementations in
 * libvkd3d_d3d12_Host.cpp. The real resolution of native vkd3d-proton is the
 * host runtime's own dlopen in ppc64le/thunk/runtime.
 */
#include <common/GeneratorInterface.h>

#include "vkd3d_thunk_api.h"

template<auto>
struct fex_gen_config {
  unsigned version = 0;
};

template<>
struct fex_gen_config<vkd3d_host_dispatch> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<vkd3d_host_dispatch_float> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<vkd3d_host_entry> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<vkd3d_host_probe> : fexgen::custom_host_impl {};
