# Overlay of microsoft/vcpkg ports/usd (usd 26.5, vcpkg 2026.06.24) for the duckdb-usd extension.
#
# The extension ships as a single file, so OpenUSD (and its TBB) must be linked into it statically. Differences
# from the upstream port:
#   * static libraries only (upstream forces ONLY_DYNAMIC_LIBRARY)
#   * only the libraries the extension uses: the core libraries and the schema libraries (no imaging, python, tools,
#     exec, validation, usdUtils, the renderer-specific usdRi and usdHydra, or file format plugins)
#   * 100: keep the raw whole-archive link flags out of the exported pxrTargets.cmake - the extension links the
#          libraries as whole archives itself
#   * 101: private ARCH_CONSTRUCTOR section names on macOS, so that another OpenUSD loaded into the same process
#          does not run this copy's static constructors (OpenUSD issue #1479)
#   * 102: only build the pxr/usd libraries the extension uses
#   * a private C++ namespace and plugin path environment variable

if(NOT VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    message(FATAL_ERROR "This overlay builds OpenUSD as static libraries only.")
endif()

# zero-pad version components to two digits (26.5 -> 26.05)
string(REPLACE "." ";" version_components ${VERSION})
foreach(component IN LISTS version_components)
    string(LENGTH ${component} component_length)
    if(component_length LESS 2)
        list(APPEND USD_VERSION "0${component}")
    else()
        list(APPEND USD_VERSION "${component}")
    endif()
endforeach()
string(JOIN "." USD_VERSION ${USD_VERSION})
string(REPLACE "." "_" USD_NAMESPACE_VERSION "${VERSION}")

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO PixarAnimationStudios/OpenUSD
    REF "v${USD_VERSION}"
    SHA512 d10222a457d71470a26ad6dc812685f257bf5c90a64a11d90e543ef7eaba803aa4e2593c358ebd430ba55856e987f7a6f50597b1ad6d2da737c239ad4f18ad6a
    HEAD_REF release
    PATCHES
        003-fix-dep.patch
        004-fix_cmake_package.patch
        007-fix_cmake_hgi_interop.patch
        008-fix_clang8_compiler_error.patch
        009-vcpkg_install_folder_conventions.patch
        010-cmake_export_plugin_as_modules.patch
        011-fix-tbb2023-task-api.patch
        100-static-plain-export-interface.patch
        101-darwin-private-ctor-sections.patch
        102-schema-libs-only.patch
)

file(REMOVE
    "${SOURCE_PATH}/cmake/modules/FindOpenColorIO.cmake"
    "${SOURCE_PATH}/pxr/imaging/hgiVulkan/vk_mem_alloc.cpp"
    "${SOURCE_PATH}/pxr/imaging/hgiVulkan/vk_mem_alloc.h"
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_SHARED_LIBS:BOOL=OFF
        -DPXR_BUILD_MONOLITHIC:BOOL=OFF

        -DPXR_BUILD_DOCUMENTATION:BOOL=OFF
        -DPXR_BUILD_PYTHON_DOCUMENTATION:BOOL=OFF
        -DPXR_BUILD_EXAMPLES:BOOL=OFF
        -DPXR_BUILD_TESTS:BOOL=OFF
        -DPXR_BUILD_TUTORIALS:BOOL=OFF
        -DPXR_BUILD_USD_TOOLS:BOOL=OFF
        -DPXR_BUILD_EXEC:BOOL=OFF
        -DPXR_BUILD_USD_VALIDATION:BOOL=OFF

        -DPXR_BUILD_IMAGING:BOOL=OFF
        -DPXR_BUILD_USD_IMAGING:BOOL=OFF
        -DPXR_BUILD_USDVIEW:BOOL=OFF
        -DPXR_ENABLE_GL_SUPPORT:BOOL=OFF
        -DPXR_ENABLE_METAL_SUPPORT:BOOL=OFF
        -DPXR_ENABLE_VULKAN_SUPPORT:BOOL=OFF
        -DPXR_ENABLE_MATERIALX_SUPPORT:BOOL=OFF
        -DPXR_BUILD_OPENIMAGEIO_PLUGIN:BOOL=OFF
        -DPXR_BUILD_OPENCOLORIO_PLUGIN:BOOL=OFF
        -DPXR_BUILD_ALEMBIC_PLUGIN:BOOL=OFF
        -DPXR_BUILD_DRACO_PLUGIN:BOOL=OFF
        -DPXR_BUILD_EMBREE_PLUGIN:BOOL=OFF
        -DPXR_BUILD_PRMAN_PLUGIN:BOOL=OFF
        -DPXR_ENABLE_OPENVDB_SUPPORT:BOOL=OFF
        -DPXR_ENABLE_PTEX_SUPPORT:BOOL=OFF
        -DPXR_ENABLE_OSL_SUPPORT:BOOL=OFF
        -DPXR_ENABLE_HDF5_SUPPORT:BOOL=OFF

        -DPXR_ENABLE_PYTHON_SUPPORT:BOOL=OFF
        -DPXR_USE_DEBUG_PYTHON:BOOL=OFF
        -DPXR_PREFER_SAFETY_OVER_SPEED:BOOL=ON
        -DPXR_FIND_TBB_IN_CONFIG:BOOL=ON

        # Isolation from any other OpenUSD in the same process: a private plugin path variable (instead of
        # PXR_PLUGINPATH_NAME) and a private C++ namespace
        -DPXR_OVERRIDE_PLUGINPATH_NAME=DUCKDB_USD_PLUGINPATH
        "-DPXR_SET_INTERNAL_NAMESPACE=pxrInternal_duckdbusd_v${USD_NAMESPACE_VERSION}"
    MAYBE_UNUSED_VARIABLES
        PXR_BUILD_OPENCOLORIO_PLUGIN
        PXR_BUILD_OPENIMAGEIO_PLUGIN
        PXR_BUILD_EMBREE_PLUGIN
        PXR_BUILD_PRMAN_PLUGIN
        PXR_ENABLE_PTEX_SUPPORT
        PXR_ENABLE_VULKAN_SUPPORT
        PXR_ENABLE_METAL_SUPPORT
        PXR_ENABLE_GL_SUPPORT
        PXR_ENABLE_HDF5_SUPPORT
        PXR_USE_DEBUG_PYTHON
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME "pxr")

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    # code generation templates - never needed at runtime, and their plugInfo.json must not be registered
    "${CURRENT_PACKAGES_DIR}/lib/usd/usd/resources/codegenTemplates"
    "${CURRENT_PACKAGES_DIR}/debug/lib/usd/usd/resources/codegenTemplates"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
