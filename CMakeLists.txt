cmake_minimum_required(VERSION 3.16)
project(SHOViewer CXX)

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
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${IMGUIZMO_DIR}
)
if(SDL2_INCLUDE_DIRS)
    include_directories(${SDL2_INCLUDE_DIRS})
endif()

# ── Application sources ───────────────────────────────────────────────────────
set(APP_SOURCES
    src/main.cpp
    src/Common.cpp
    src/PS2Texture.cpp
    src/Loader.cpp
    src/UI.cpp
)

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

target_link_libraries(SHOViewer
    OpenGL::GL
    GLEW::GLEW
    glm::glm
    ${SHO_SDL2_TARGETS}
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
