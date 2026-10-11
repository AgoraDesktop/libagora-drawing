#===----------------------------------------------------------------------===#
#
# Agora's build for Core Graphics (libagora-drawing).
#
# The whole library is C over Core Foundation: `CGColor` is a registered CF type
# (PROVENANCE.md), the value types are CF types, and nothing here imports
# Foundation. It needs `libagora-core` and the C libraries the drawing engine
# stands on: pixman (fills), little CMS (colour), FreeType (fonts), libpng and
# libjpeg (images).
#
#===----------------------------------------------------------------------===#

cmake_minimum_required(VERSION 3.24)

project(AgoraDrawing LANGUAGES C)

include(GNUInstallDirs)

set(CMAKE_POSITION_INDEPENDENT_CODE YES)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(CoreFoundation CONFIG REQUIRED)
find_package(lcms2 CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(Freetype REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIXMAN REQUIRED IMPORTED_TARGET pixman-1)

# ---------------------------------------------------------------------------
# Include shim: the headers are included as <CoreGraphics/...>
# ---------------------------------------------------------------------------
set(_agora_include_dir "${CMAKE_BINARY_DIR}/agora-include")
file(REMOVE_RECURSE "${_agora_include_dir}")
file(MAKE_DIRECTORY "${_agora_include_dir}")
file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}" "${_agora_include_dir}/CoreGraphics" SYMBOLIC)

# ---------------------------------------------------------------------------
# The library: every .c in the directory
# ---------------------------------------------------------------------------
file(GLOB DRAWING_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

# AGORA: EVERY LIBRARY IN THIS TREE IS SHARED. A consumer loads the whole .so, so an
# Objective-C category in it is registered whether or not anything references its symbols
# (a static archive drops such members and the categories silently stop answering) and a
# single copy of the runtime state is shared instead of one per binary. See README.md.
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

add_library(Drawing SHARED ${DRAWING_SOURCES})

set_target_properties(Drawing PROPERTIES OUTPUT_NAME agora-drawing)

# Core Foundation's public headers spell some doors with blocks, so every consumer of
# <CoreGraphics/...> needs -fblocks (libagora-core carries the same INTERFACE flag).
target_compile_options(Drawing
    INTERFACE
        -fblocks)

target_include_directories(Drawing
    PUBLIC
        "$<BUILD_INTERFACE:${_agora_include_dir}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# CORE FOUNDATION IS PUBLIC AND THE ENCODERS ARE NOT, AND THE DIFFERENCE IS MEASURED RATHER THAN STYLISTIC.
# Every public header here spells its doors with CF types AND a consumer calls Core Foundation itself --
# CFArrayCreate, CFDataCreate, CFStringRef comparisons -- so a client that links only this target fails to
# link with "undefined reference to symbol 'CFArrayCreate'" and "libagora-core.so: error adding symbols:
# DSO missing from command line". pixman, lcms2, libpng, libjpeg and FreeType are reached only from inside
# the shared library, so they stay PRIVATE and are resolved from its own NEEDED entries.
target_link_libraries(Drawing
    PUBLIC
        CoreFoundation::CoreFoundation
    PRIVATE
        lcms2::lcms2
        PNG::PNG
        JPEG::JPEG
        Freetype::Freetype
        PkgConfig::PIXMAN
        m)

# ---------------------------------------------------------------------------
# The suite: libagora-drawing's OWN surface. libagora-base's bridges suite tests
# the two libraries TOGETHER and knows nothing about CoreGraphics' doors, so
# before this target existed nothing in the tree held this library at all.
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

install(DIRECTORY . DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/CoreGraphics
        FILES_MATCHING PATTERN "*.h")
install(TARGETS Drawing
    EXPORT AgoraDrawingTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------
# AND IT IS EXPORTED, WHICH IT WAS NOT -- the reason nothing could consume it.
#
# The library installed a .so and a directory of headers and NOTHING A CONSUMER COULD find_package, so the
# only way to use it was to spell the include and link paths out by hand. Every other library in this tree
# that a consumer already finds has this block; this one did not, and a client that does not exist cannot
# complain about it.
#
# The idiom is libagora-core's, deliberately: same shape, same helpers, same version policy, so a consumer
# does not have to learn a second one.
# ---------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

install(EXPORT AgoraDrawingTargets
    FILE AgoraDrawingTargets.cmake
    NAMESPACE AgoraDrawing::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraDrawing)

configure_package_config_file(cmake/AgoraDrawingConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraDrawingConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraDrawing)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraDrawingConfigVersion.cmake
    VERSION 1.0.0
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraDrawingConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraDrawingConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraDrawing)
