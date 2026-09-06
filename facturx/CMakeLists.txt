cmake_minimum_required(VERSION 3.20)

set(PLUGIN_NAME "facturx" CACHE STRING "Plugin name")

project(${PLUGIN_NAME} LANGUAGES C CXX)

# Where the finished plugin lands. It defaults to the test project's Plugins
# folder so a plain build is immediately testable; release builds override it
# to keep the two macOS architecture slices apart before they are merged.
set(FACTURX_PLUGIN_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}-test/Plugins"
    CACHE PATH "Directory the built plugin is written to")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SDK_DIR "${ROOT_DIR}/4D-Plugin-SDK/4D Plugin API")
set(QPDF_DIR "${ROOT_DIR}/qpdf")
set(ZLIB_DIR "${ROOT_DIR}/zlib")
set(JPEG_DIR "${ROOT_DIR}/libjpeg-turbo")

foreach(_dep "${SDK_DIR}" "${QPDF_DIR}/libqpdf" "${ZLIB_DIR}/zlib.h" "${JPEG_DIR}/CMakeLists.txt")
    if(NOT EXISTS "${_dep}")
        message(FATAL_ERROR
            "Missing ${_dep}. Run: git submodule update --init --recursive")
    endif()
endforeach()

# Static CRT on Windows. Must be set before any add_subdirectory().
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

# --------------------------------------------------------------------
# zlib (zlib licence) -- compiled directly so the submodule stays clean
# --------------------------------------------------------------------
# The gz* translation units are omitted: qpdf only uses the raw
# deflate/inflate API, and gzlib.c needs a configure-generated zconf.h.
add_library(fx_zlib STATIC
    "${ZLIB_DIR}/adler32.c"
    "${ZLIB_DIR}/compress.c"
    "${ZLIB_DIR}/crc32.c"
    "${ZLIB_DIR}/deflate.c"
    "${ZLIB_DIR}/infback.c"
    "${ZLIB_DIR}/inffast.c"
    "${ZLIB_DIR}/inflate.c"
    "${ZLIB_DIR}/inftrees.c"
    "${ZLIB_DIR}/trees.c"
    "${ZLIB_DIR}/uncompr.c"
    "${ZLIB_DIR}/zutil.c")
target_include_directories(fx_zlib PUBLIC "${ZLIB_DIR}")
if(MSVC)
    target_compile_definitions(fx_zlib PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    target_compile_options(fx_zlib PRIVATE /wd4267 /wd4244)
endif()

# --------------------------------------------------------------------
# libjpeg-turbo (BSD/IJG) -- required by qpdf's DCT filter. qpdf's
# Pl_DCT.hh includes <jpeglib.h>, so it is needed even though this
# plugin never decodes JPEG data. libjpeg-turbo explicitly refuses
# add_subdirectory(), so it is built via ExternalProject as upstream
# instructs. SIMD is disabled: it is irrelevant here and it needs NASM.
# --------------------------------------------------------------------
include(ExternalProject)

set(JPEG_PREFIX "${CMAKE_BINARY_DIR}/jpeg")
set(JPEG_INSTALL "${JPEG_PREFIX}/install")
if(MSVC)
    set(JPEG_LIBRARY "${JPEG_INSTALL}/lib/jpeg-static.lib")
else()
    set(JPEG_LIBRARY "${JPEG_INSTALL}/lib/libjpeg.a")
endif()

ExternalProject_Add(jpeg_ext
    SOURCE_DIR "${JPEG_DIR}"
    PREFIX "${JPEG_PREFIX}"
    CMAKE_ARGS
        "-DCMAKE_INSTALL_PREFIX=${JPEG_INSTALL}"
        "-DCMAKE_INSTALL_LIBDIR=lib"
        "-DCMAKE_INSTALL_INCLUDEDIR=include"
        "-DCMAKE_BUILD_TYPE=$<IF:$<CONFIG:Debug>,Debug,Release>"
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}"
        "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
        "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        -DENABLE_SHARED=OFF
        -DENABLE_STATIC=ON
        -DWITH_TURBOJPEG=OFF
        -DWITH_TOOLS=OFF
        -DWITH_TESTS=OFF
        -DWITH_FUZZ=OFF
        -DWITH_SIMD=OFF
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config $<CONFIG>
    BUILD_BYPRODUCTS "${JPEG_LIBRARY}"
    USES_TERMINAL_BUILD OFF)

file(MAKE_DIRECTORY "${JPEG_INSTALL}/include")

# --------------------------------------------------------------------
# libqpdf (Apache-2.0) -- built here rather than via qpdf's own
# CMakeLists so the CLI, tests, examples and docs are not built and so
# the dependency resolution is fully deterministic on both platforms.
# --------------------------------------------------------------------
include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckCSourceCompiles)
include(CheckTypeSize)

check_include_file(inttypes.h HAVE_INTTYPES_H)
check_include_file(stdint.h HAVE_STDINT_H)
check_symbol_exists(localtime_r time.h HAVE_LOCALTIME_R)
check_symbol_exists(random stdlib.h HAVE_RANDOM)
check_symbol_exists(fseeko stdio.h HAVE_FSEEKO)
check_symbol_exists(fseeko64 stdio.h HAVE_FSEEKO64)
check_symbol_exists(open_memstream stdio.h HAVE_OPEN_MEMSTREAM)
check_symbol_exists(malloc_info malloc.h HAVE_MALLOC_INFO)
check_c_source_compiles(
    "#include <time.h>
     int main() { struct tm t; t.tm_gmtoff = 0; return 0; }"
    HAVE_TM_GMTOFF)
check_c_source_compiles(
    "#include <time.h>
     extern long timezone;
     int main() { return (int)timezone; }"
    HAVE_EXTERN_LONG_TIMEZONE)
check_type_size("size_t" SIZEOF_SIZE_T LANGUAGE C)

set(USE_CRYPTO_NATIVE 1)
set(DEFAULT_CRYPTO "native")

configure_file(
    "${QPDF_DIR}/libqpdf/qpdf/qpdf-config.h.in"
    "${CMAKE_BINARY_DIR}/qpdf-generated/qpdf/qpdf-config.h"
    NEWLINE_STYLE UNIX)

file(GLOB QPDF_SOURCES "${QPDF_DIR}/libqpdf/*.cc")
list(APPEND QPDF_SOURCES
    "${QPDF_DIR}/libqpdf/sha2.c"
    "${QPDF_DIR}/libqpdf/sha2big.c")
# Only the native crypto provider is compiled in: it avoids an OpenSSL or
# GnuTLS runtime dependency, which matters for a statically linked plugin.
list(REMOVE_ITEM QPDF_SOURCES
    "${QPDF_DIR}/libqpdf/QPDFCrypto_openssl.cc"
    "${QPDF_DIR}/libqpdf/QPDFCrypto_gnutls.cc")

add_library(fx_qpdf STATIC ${QPDF_SOURCES})
add_dependencies(fx_qpdf jpeg_ext)
target_include_directories(fx_qpdf
    PUBLIC
        "${QPDF_DIR}/include"
    PRIVATE
        "${QPDF_DIR}/libqpdf"
        "${CMAKE_BINARY_DIR}/qpdf-generated")
target_link_libraries(fx_qpdf PUBLIC fx_zlib "${JPEG_LIBRARY}")
target_include_directories(fx_qpdf SYSTEM PRIVATE "${JPEG_INSTALL}/include")
if(MSVC)
    target_compile_definitions(fx_qpdf PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(fx_qpdf PRIVATE /bigobj /utf-8 /EHsc /wd4267 /wd4244 /wd4996)
    target_link_libraries(fx_qpdf PUBLIC ws2_32 advapi32 crypt32 bcrypt)
endif()

# --------------------------------------------------------------------
# plugin
# --------------------------------------------------------------------
set(PLUGIN_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}-4dplugin.cpp"
    "${SDK_DIR}/4DPluginAPI.c")

set(PLUGIN_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}-4dplugin.h")

set(PLUGIN_RESOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/constants.xlf")

set_source_files_properties("${SDK_DIR}/4DPluginAPI.c" PROPERTIES LANGUAGE CXX)

if(APPLE)
    add_library(${PLUGIN_NAME} MODULE
        ${PLUGIN_SOURCES} ${PLUGIN_HEADERS} ${PLUGIN_RESOURCES})

    set_target_properties(${PLUGIN_NAME} PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.4d.${PLUGIN_NAME}"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0")

    set_source_files_properties(${PLUGIN_RESOURCES} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)

    set_source_files_properties("${SDK_DIR}/4DPluginAPI.c" PROPERTIES
        COMPILE_FLAGS "-Wno-int-conversion -Wno-multichar")

    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(${PLUGIN_NAME} PRIVATE
        ${COREGRAPHICS_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})

    set_target_properties(${PLUGIN_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${FACTURX_PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${FACTURX_PLUGIN_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${FACTURX_PLUGIN_OUTPUT_DIR}")

elseif(WIN32)
    add_library(${PLUGIN_NAME} SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

    set_target_properties(${PLUGIN_NAME} PROPERTIES
        SUFFIX ".4DX"
        LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.def\"")

    target_compile_definitions(${PLUGIN_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    target_compile_options(${PLUGIN_NAME} PRIVATE
        /bigobj /utf-8 /EHsc /wd4267 /wd4244 /wd4305 /wd4996)

    set(PLUGIN_OUT_DIR "${FACTURX_PLUGIN_OUTPUT_DIR}/${PLUGIN_NAME}/Contents/Windows64")
    set(RESOURCE_OUT_DIR "${FACTURX_PLUGIN_OUTPUT_DIR}/${PLUGIN_NAME}/Contents/Resources")

    set_target_properties(${PLUGIN_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PLUGIN_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PLUGIN_OUT_DIR}")

    add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RESOURCE_OUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PLUGIN_RESOURCES} "${RESOURCE_OUT_DIR}"
        COMMENT "Copying manifest.json and constants.xlf into Contents/Resources")
else()
    message(FATAL_ERROR "Only macOS and Windows are supported targets.")
endif()

target_include_directories(${PLUGIN_NAME} PRIVATE "${SDK_DIR}")
target_link_libraries(${PLUGIN_NAME} PRIVATE fx_qpdf)
