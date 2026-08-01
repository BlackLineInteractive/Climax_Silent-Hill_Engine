cmake_minimum_required(VERSION 3.16)
project(SHOViewer VERSION 0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Fetch third-party UI libraries from GitHub ────────────────────────────────
include(FetchContent)

# Both dependencies are pinned: an unpinned `master` silently changes the API and
# the on-disk layout under us (ImGuizmo moved its sources into src/ upstream).
# SOURCE_SUBDIR points at a directory that does not exist so that
# FetchContent_MakeAvailable only downloads and never calls add_subdirectory —
# neither library ships a CMakeLists we want to build here.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-configure
)

FetchContent_Declare(imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        5ab7676402ace03cdf930b2d972f59c7d03c6fa8
    SOURCE_SUBDIR  do-not-configure
)

FetchContent_MakeAvailable(imgui imguizmo)

# ── librw — RenderWare 3.7.0.2 (aap/librw) ──────────────────────────────────
option(ENABLE_LIBRW "Fetch and link librw (RenderWare 3.7.0.2 reimplementation)" OFF)

if(ENABLE_LIBRW)
    message(STATUS "ENABLE_LIBRW=ON: Fetching and compiling librw (RenderWare 3.7.0.2)")
    set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/vendor" CACHE PATH "" FORCE)
    set(LIBRW_PLATFORM "NULL" CACHE STRING "librw render platform" FORCE)
    FetchContent_Declare(librw
        GIT_REPOSITORY https://github.com/aap/librw.git
        GIT_TAG        4c8cab66bb768717169505396d8e9ee5ac885b7b   # 2026-07-14
        GIT_SHALLOW    FALSE
    )
    FetchContent_MakeAvailable(librw)
else()
    message(STATUS "ENABLE_LIBRW=OFF: Skipping librw fetch and build")
endif()

# ImGuizmo keeps its sources in src/ on recent revisions and in the repo root on
# older ones — support both so a re-pin does not break the build silently.
if(EXISTS "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
    set(IMGUIZMO_DIR "${imguizmo_SOURCE_DIR}/src")
elseif(EXISTS "${imguizmo_SOURCE_DIR}/ImGuizmo.cpp")
    set(IMGUIZMO_DIR "${imguizmo_SOURCE_DIR}")
else()
    message(FATAL_ERROR "ImGuizmo.cpp not found under ${imguizmo_SOURCE_DIR}")
endif()

# ── System dependencies ───────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)
find_package(GLEW   REQUIRED)   # vcpkg / brew / libglew-dev all export GLEW::GLEW
find_package(ZLIB   REQUIRED)   # SH.ARC payloads are raw zlib streams

# SDL2: prefer the CMake config package (vcpkg, recent Homebrew, SDL built from
# source). Fall back to pkg-config, which is what most Linux distros ship.
find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
    set(SHO_SDL2_TARGETS SDL2::SDL2)
    if(TARGET SDL2::SDL2main)
        # Windows only: SDL2main provides the WinMain shim that forwards to main().
        list(PREPEND SHO_SDL2_TARGETS SDL2::SDL2main)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    set(SHO_SDL2_TARGETS ${SDL2_LIBRARIES})
endif()

# glm is header-only; fetch it when the system does not provide it so Windows
# builds do not need a separate install step.
find_package(glm CONFIG QUIET)
if(NOT TARGET glm::glm)
    FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        1.0.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(glm)
endif()

# ── Include paths ─────────────────────────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${IMGUIZMO_DIR}
)
if(ENABLE_LIBRW)
    include_directories(${librw_SOURCE_DIR}/src)
endif()
if(SDL2_INCLUDE_DIRS)
    include_directories(${SDL2_INCLUDE_DIRS})
endif()

# ── Application sources ───────────────────────────────────────────────────────
set(APP_SOURCES
    src/main.cpp
    src/Core/Common.cpp
    src/Core/Arc.cpp
    src/Core/AudioParser.cpp
    src/Rendering/PS2Texture.cpp
    src/Rendering/RenderBackend.cpp
    src/Rendering/GLSLBackend.cpp
    src/Rendering/CPURasterizer.cpp
    src/Loader/Loader.cpp
    src/Loader/Export.cpp
    src/UI/UI.cpp
)

if(APPLE)
    list(APPEND APP_SOURCES src/Rendering/MetalBackend.mm)
endif()

# ── ImGui sources (fetched) ───────────────────────────────────────────────────
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

# ── ImGuizmo sources (fetched) ────────────────────────────────────────────────
set(IMGUIZMO_SOURCES
    ${IMGUIZMO_DIR}/ImGuizmo.cpp
)

# ── Target ────────────────────────────────────────────────────────────────────
add_executable(SHOViewer
    ${APP_SOURCES}
    ${IMGUI_SOURCES}
    ${IMGUIZMO_SOURCES}
)

if(MSVC)
    target_compile_options(SHOViewer PRIVATE /W3 /utf-8 /permissive-)
    target_compile_definitions(SHOViewer PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    # Console subsystem: the viewer logs shader/parser diagnostics to stderr.
    set_target_properties(SHOViewer PROPERTIES WIN32_EXECUTABLE FALSE)
else()
    target_compile_options(SHOViewer PRIVATE -Wall)
endif()

if(SDL2_LIBRARY_DIRS)
    target_link_directories(SHOViewer PRIVATE ${SDL2_LIBRARY_DIRS})
endif()
if(SDL2_CFLAGS_OTHER)
    target_compile_options(SHOViewer PRIVATE ${SDL2_CFLAGS_OTHER})
endif()

set(EXTRA_LIBS)
if(APPLE)
    find_library(FOUNDATION_LIB Foundation REQUIRED)
    find_library(METAL_LIB Metal REQUIRED)
    find_library(METALKIT_LIB MetalKit REQUIRED)
    find_library(QUARTZCORE_LIB QuartzCore REQUIRED)
    list(APPEND EXTRA_LIBS ${FOUNDATION_LIB} ${METAL_LIB} ${METALKIT_LIB} ${QUARTZCORE_LIB})
endif()

if(ENABLE_LIBRW)
    list(APPEND EXTRA_LIBS librw)
    target_compile_definitions(SHOViewer PRIVATE ENABLE_LIBRW=1)
endif()

target_link_libraries(SHOViewer
    OpenGL::GL
    GLEW::GLEW
    ZLIB::ZLIB
    glm::glm
    ${SHO_SDL2_TARGETS}
    ${EXTRA_LIBS}
)

# Expose the exact RW version the SHO PS2 binary uses as a compile definition
# so that any translation unit can reference it with SHO_RW_VERSION.
target_compile_definitions(SHOViewer PRIVATE
    SHO_RW_VERSION=0x1c020065   # RenderWare 3.7.0.2 PS2
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS SHOViewer RUNTIME DESTINATION bin)

# Windows: copy the SDL2/GLEW DLLs next to the executable so it runs from the
# build tree and so CI can archive a self-contained folder.
if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
    add_custom_command(TARGET SHOViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:SHOViewer> $<TARGET_FILE_DIR:SHOViewer>
        COMMAND_EXPAND_LISTS)
endif()
