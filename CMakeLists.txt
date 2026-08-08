cmake_minimum_required(VERSION 3.16)
project("Climax Game Engine Toolkit" VERSION 0.6.0 LANGUAGES CXX)

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

# SDL2_image: for JPEG decoding in climax-play (Startup container backgrounds).
find_package(SDL2_image CONFIG QUIET)
if(TARGET SDL2_image::SDL2_image)
    set(SHO_SDL2_IMAGE SDL2_image::SDL2_image)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2_IMAGE QUIET SDL2_image)
        if(SDL2_IMAGE_FOUND)
            set(SHO_SDL2_IMAGE ${SDL2_IMAGE_LIBRARIES})
            if(SDL2_IMAGE_INCLUDE_DIRS)
                include_directories(${SDL2_IMAGE_INCLUDE_DIRS})
            endif()
        endif()
    endif()
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
    ${CMAKE_SOURCE_DIR}/src/UI/ImAnim
)
if(ENABLE_LIBRW)
    include_directories(${librw_SOURCE_DIR}/src)
endif()
if(SDL2_INCLUDE_DIRS)
    include_directories(${SDL2_INCLUDE_DIRS})
endif()

# ── Application sources ───────────────────────────────────────────────────────
# ── climax-core  (NO GL, NO SDL, NO ImGui) ────────────────────────────────────
# Pure engine data: archive, RW streams, CPU geometry, audio PCM decode.
# Any translation unit that links climax-core must compile clean without GL.
# Only files that actually compile without GL are in here. Loader.cpp,
# GeometryDecoder.cpp, SceneObject.cpp and Common.cpp still call GL directly
# and still live in the toolkit target; moving them in one at a time, with the
# build failing if they are not ready, is the whole point of this boundary.
add_library(climax-core STATIC
    src/Core/RWS/FileSystem/CArchive.cpp
    src/Core/UI/StringTable.cpp
    src/Core/UI/Font.cpp
    src/Core/UI/ScreenDef.cpp
    src/Platform/PS2/PS2Texture.cpp
    src/Platform/PS2/AudioParser.cpp
    src/Platform/Wii/WiiGeometry.cpp
)
target_include_directories(climax-core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
)
target_link_libraries(climax-core PUBLIC ZLIB::ZLIB glm::glm)

# ── climax-game  (NO GL, NO SDL, NO ImGui) ────────────────────────────────────
# Game logic: character controller, cameras, triggers, export.
add_library(climax-game STATIC
    src/Game/CameraLinks.cpp
    src/Game/CharacterController.cpp
    src/Game/ZoneLinks.cpp
    src/Game/FrontEnd.cpp
)
target_link_libraries(climax-game PUBLIC climax-core)

# ── ImGui / ImGuizmo / ImAnim sources ─────────────────────────────────────────
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
set(IMGUIZMO_SOURCES ${IMGUIZMO_DIR}/ImGuizmo.cpp)
set(IMANIM_SOURCES   ${CMAKE_SOURCE_DIR}/src/UI/ImAnim/im_anim.cpp)

# ── Toolkit exe  (SDL + GL + ImGui) ───────────────────────────────────────────
set(TOOLKIT_RENDER_SOURCES
    src/Rendering/RenderBackend.cpp
    src/Rendering/GLSLBackend.cpp
    src/Rendering/CPURasterizer.cpp
    src/Rendering/GPUMesh.cpp
    src/Rendering/GLTextureSink.cpp
    src/Rendering/PlayerModel.cpp
)
if(APPLE)
    list(APPEND TOOLKIT_RENDER_SOURCES src/Rendering/MetalBackend.mm)
endif()

add_executable(ClimaxGameEngineToolkit MACOSX_BUNDLE
    src/main.cpp
    src/UI/UI.cpp
    # Still GL-bound, so still here rather than in climax-core.
    src/Core/Common.cpp
    src/Loader/Loader.cpp
    src/Loader/GeometryDecoder.cpp
    src/Loader/ResourceLoader.cpp
    src/Loader/Export.cpp
    src/SG/SceneObject.cpp
    # RwsAudio plays through SDL and PS2Texture uploads to GL from inside the
    # decoder; WiiTexture builds on PS2Texture. Splitting decode from upload is
    # the next move, and until then they belong on this side of the line.
    src/Platform/PS2/RwsAudio.cpp
    src/Platform/Wii/WiiTexture.cpp
    ${TOOLKIT_RENDER_SOURCES}
    ${IMGUI_SOURCES}
    ${IMGUIZMO_SOURCES}
    ${IMANIM_SOURCES}
)
target_include_directories(ClimaxGameEngineToolkit PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${IMGUIZMO_DIR}
    ${CMAKE_SOURCE_DIR}/src/UI/ImAnim
)
if(SDL2_INCLUDE_DIRS)
    target_include_directories(ClimaxGameEngineToolkit PRIVATE ${SDL2_INCLUDE_DIRS})
endif()

if(MSVC)
    target_compile_options(ClimaxGameEngineToolkit PRIVATE /W3 /utf-8 /permissive-)
    target_compile_definitions(ClimaxGameEngineToolkit PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    # Console subsystem: the viewer logs shader/parser diagnostics to stderr.
    set_target_properties(ClimaxGameEngineToolkit PROPERTIES WIN32_EXECUTABLE FALSE)
else()
    target_compile_options(ClimaxGameEngineToolkit PRIVATE -Wall)
endif()

if(SDL2_LIBRARY_DIRS)
    target_link_directories(ClimaxGameEngineToolkit PRIVATE ${SDL2_LIBRARY_DIRS})
endif()
if(SDL2_CFLAGS_OTHER)
    target_compile_options(ClimaxGameEngineToolkit PRIVATE ${SDL2_CFLAGS_OTHER})
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
    target_compile_definitions(ClimaxGameEngineToolkit PRIVATE ENABLE_LIBRW=1)
endif()

target_link_libraries(ClimaxGameEngineToolkit
    climax-game     # includes climax-core transitively
    OpenGL::GL
    GLEW::GLEW
    ${SHO_SDL2_TARGETS}
    ${EXTRA_LIBS}
)

# ── climax-play  (port exe, NO ImGui) ─────────────────────────────────────────
# Enforcement: if anyone adds #include "imgui.h" to game/core code,
# this target will fail to link — making the violation a build error.
option(BUILD_CLIMAX_PLAY "Build the game target, without the toolkit" ON)
if(BUILD_CLIMAX_PLAY)
    # Only its own main, plus the two libraries. The toolkit's renderer is not
    # here: it is bound to ViewerState and to ImGui, and dragging it in would
    # defeat the point of this target.
    add_executable(climax-play src/play_main.cpp)
    target_link_libraries(climax-play
        climax-game
        OpenGL::GL GLEW::GLEW
        ${SHO_SDL2_TARGETS}
        ${SHO_SDL2_IMAGE}
        ${EXTRA_LIBS}
    )
    if(SDL2_INCLUDE_DIRS)
        target_include_directories(climax-play PRIVATE ${SDL2_INCLUDE_DIRS})
    endif()
    # Explicit sentinel: any accidental imgui include will cause an
    # undefined-symbol error at link time.
    target_compile_definitions(climax-play PRIVATE CLIMAX_NO_IMGUI=1)
endif()

# Expose the exact RW version the SHO PS2 binary uses as a compile definition
# so that any translation unit can reference it with SHO_RW_VERSION.
target_compile_definitions(ClimaxGameEngineToolkit PRIVATE
    SHO_RW_VERSION=0x1c020065   # RenderWare 3.7.0.2 PS2
)

install(TARGETS ClimaxGameEngineToolkit
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

# On macOS, fixup the bundle by copying required dylibs inside the app package.
if(APPLE)
    install(CODE "
        include(BundleUtilities)
        set(BU_CHMOD_BUNDLE_ITEMS ON)
        get_filename_component(APP_ABS \"\${CMAKE_INSTALL_PREFIX}/ClimaxGameEngineToolkit.app\" ABSOLUTE)
        fixup_bundle(\"\${APP_ABS}\" \"\" \"\")
        execute_process(COMMAND codesign --force --deep --sign - \"\${APP_ABS}\")
    ")
endif()

# Windows: copy the SDL2/GLEW DLLs next to the executable so it runs from the
# build tree and so CI can archive a self-contained folder.
if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
    add_custom_command(TARGET ClimaxGameEngineToolkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:ClimaxGameEngineToolkit> $<TARGET_FILE_DIR:ClimaxGameEngineToolkit>
        COMMAND_EXPAND_LISTS)
endif()
