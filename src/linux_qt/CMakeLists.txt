if(WIN32)
    # MinGW cross build: expose M_PI and friends, and keep windows.h from
    # defining min/max macros that break std::min/max and Qt. (Deliberately
    # NOT WIN32_LEAN_AND_MEAN: it leaks into the vendored GNS subdirectory,
    # which needs the full windows.h for the multimedia timer API.)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
endif()

set(HAMMER_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(HAMMER_ORIGINAL_SOURCE_DIR "${HAMMER_REPOSITORY_ROOT}/src/hammer")
set(HAMMER_PUBLIC_DIR "${HAMMER_REPOSITORY_ROOT}/src/public")

# Directly compiled slice of the licensed Hammer source. These files remain in
# their original locations rather than being copied into a rewrite.
add_library(hammer_original_portable STATIC
    "${HAMMER_ORIGINAL_SOURCE_DIR}/clock.cpp"
    "${HAMMER_ORIGINAL_SOURCE_DIR}/noise.cpp"
    "${HAMMER_ORIGINAL_SOURCE_DIR}/viewersettings.cpp"
    "${HAMMER_ORIGINAL_SOURCE_DIR}/blockarray.h"
)

target_include_directories(hammer_original_portable
    PUBLIC
        "${HAMMER_ORIGINAL_SOURCE_DIR}"
        "${HAMMER_PUBLIC_DIR}"
)
if(WIN32)
    # Shadows src/hammer/stdafx.h (MFC) with a Win32-API-only shim; must sit
    # ahead of the original source dir in the include order.
    target_include_directories(hammer_original_portable BEFORE
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/compat/win")
endif()

target_compile_definitions(hammer_original_portable
    PUBLIC
        PROTECTED_THINGS_DISABLE=1
        NO_MALLOC_OVERRIDE=1
        NO_THREAD_LOCAL=1
)
# The licensed source's platform.h has a native _WIN32 path (its home turf);
# the LINUX/POSIX defines only belong on unix builds, and force-feeding them
# to a Windows cross build sends it down alloca.h and friends.
if(NOT WIN32)
    target_compile_definitions(hammer_original_portable
        PUBLIC
            _LINUX=1
            LINUX=1
            POSIX=1
    )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hammer_original_portable PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    )
endif()


# Lossless VMF/KeyValues document model used by the Qt document adapter.
# It intentionally preserves repeated chunks and duplicate keys, both of which
# are required by real Hammer maps (especially entity I/O connections).
add_library(hammer_vmf STATIC
    vmf/VmfDocument.cpp
    vmf/VmfDocument.hpp
    vmf/VmfScene.cpp
    vmf/VmfScene.hpp
    vmf/VmfProjectedSurfaces.cpp
    vmf/VmfProjectedSurfaces.hpp
    vmf/VmfRope.cpp
    vmf/VmfRope.hpp
    vmf/VmfEditor.cpp
    vmf/VmfEditor.hpp
    vmf/VmfFaceEdit.cpp
    vmf/VmfFaceEdit.hpp
    vmf/VmfDisplacement.cpp
    vmf/VmfDisplacement.hpp
    vmf/VmfSolidCarve.cpp
    vmf/VmfSolidCarve.hpp
    vmf/VmfSolidClip.cpp
    vmf/VmfSolidClip.hpp
    vmf/VmfSolidMorph.cpp
    vmf/VmfSolidMorph.hpp
    vmf/VmfGroups.cpp
    vmf/VmfGroups.hpp
    vmf/VmfSync.cpp
    vmf/VmfSync.hpp
)
target_include_directories(hammer_vmf PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vmf")
# CMapDisp::ApplyNoise calls the original's PerlinNoise2D (hammer/noise.cpp);
# link it privately so the original's include paths do not leak into the port.
target_link_libraries(hammer_vmf PRIVATE hammer_original_portable)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hammer_vmf PRIVATE -Wall -Wextra -Wpedantic)
endif()


add_library(hammer_assets STATIC
    assets/GameFileSystem.cpp
    assets/GameFileSystem.hpp
    assets/DetailObjects.cpp
    assets/DetailObjects.hpp
    assets/MaterialSystem.cpp
    assets/MaterialSystem.hpp
    assets/StudioModelSystem.cpp
    assets/StudioModelSystem.hpp
)
target_include_directories(hammer_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_link_libraries(hammer_assets PUBLIC hammer_vmf)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hammer_assets PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(hammer_camera STATIC
    camera/Camera3D.cpp
    camera/Camera3D.hpp
)
target_include_directories(hammer_camera PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/camera")
target_link_libraries(hammer_camera PUBLIC hammer_vmf)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hammer_camera PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(hammer_fgd STATIC
    fgd/FgdDatabase.cpp
    fgd/FgdDatabase.hpp
)
target_include_directories(hammer_fgd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/fgd")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hammer_fgd PRIVATE -Wall -Wextra -Wpedantic)
endif()


add_library(hammer_ray_scene STATIC
    app/RayTracingScene.cpp
    app/RayTracingScene.hpp
    app/RadiosityBake.cpp
    app/RadiosityBake.hpp
)
target_include_directories(hammer_ray_scene PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/app"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    "${CMAKE_CURRENT_SOURCE_DIR}/camera"
    "${CMAKE_CURRENT_SOURCE_DIR}/vmf"
)
target_link_libraries(hammer_ray_scene PUBLIC hammer_vmf hammer_camera hammer_assets)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hammer_ray_scene PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(hammer-vmf-tool tools/vmf_tool.cpp)
target_link_libraries(hammer-vmf-tool PRIVATE hammer_vmf)
install(TARGETS hammer-vmf-tool RUNTIME DESTINATION bin)

# Valve GameNetworkingSockets carries the collaborative-editing transport:
# message-oriented, encrypted, reliable-ordered over UDP. Vendored (with its
# protobuf dependency) so neither the host nor the ubuntu:22.04 packaging
# container needs system packages. protobuf v21.x is the last line without the
# abseil dependency; OVERRIDE_FIND_PACKAGE satisfies GNS's find_package calls
# from the vendored build.
if(HAMMER_BUILD_QT)
    include(FetchContent)
    # Cross builds (the MinGW Windows package) cannot run a freshly built
    # protoc; they cross-compile only libprotobuf and generate code with a
    # native protoc of the SAME version, supplied by the build image.
    if(CMAKE_CROSSCOMPILING)
        set(protobuf_BUILD_PROTOC_BINARIES OFF CACHE BOOL "" FORCE)
        set(HAMMER_NATIVE_PROTOC "/opt/protoc/bin/protoc" CACHE FILEPATH
            "Native protoc matching the vendored protobuf version")
        # Cache variable: FindProtobuf's generate functions read it from other
        # directory scopes (GNS's subdirectory).
        set(Protobuf_PROTOC_EXECUTABLE "${HAMMER_NATIVE_PROTOC}" CACHE FILEPATH
            "protoc used for code generation" FORCE)
    endif()
    set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # ON: GNS's install(EXPORT) needs libprotobuf to be in an export set, or
    # CMake's generate step fails. The AppDir staging installs only
    # src/linux_qt's component, so none of this actually lands in the bundle.
    set(protobuf_INSTALL ON CACHE BOOL "" FORCE)
    set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
    # EXCLUDE_FROM_ALL (both deps): keeps the vendored install() rules out of
    # the AppDir staging install, which otherwise fails on protobuf artifacts
    # a hammer-qt6-only build never produces.
    FetchContent_Declare(Protobuf
        GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
        GIT_TAG v21.12
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
        OVERRIDE_FIND_PACKAGE)
    set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    # Direct host:port connections only; ICE/WebRTC NAT traversal is not built.
    set(ENABLE_ICE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(GameNetworkingSockets
        GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
        GIT_TAG e707b3a6b638f4de31ee3e13a64284bd9abfa98e
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(Protobuf)
    # GNS calls protobuf_generate_cpp, which lives in CMake's FindProtobuf
    # module, not in the vendored package's config files. The module uses the
    # protobuf::protoc target the vendored build defines.
    include(FindProtobuf)
    if(CMAKE_CROSSCOMPILING AND NOT TARGET protobuf::protoc)
        # The vendored build makes no protoc when cross-compiling, and CMake's
        # protobuf_generate insists on this target (the variable alone is not
        # honored): point it at the build image's native protoc.
        add_executable(protobuf::protoc IMPORTED GLOBAL)
        set_target_properties(protobuf::protoc PROPERTIES
            IMPORTED_LOCATION "${HAMMER_NATIVE_PROTOC}")
    endif()
    FetchContent_MakeAvailable(GameNetworkingSockets)

    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGL Network)
    if(UNIX AND NOT APPLE AND HAMMER_ENABLE_WAYLAND_CAPTURE AND
       Qt6_VERSION VERSION_LESS 6.9)
        find_package(Qt6 6.5 QUIET COMPONENTS GuiPrivate)
    endif()
    # ScrollMenu's anchored-scrolling fixup needs QMenuPrivate. Optional: when
    # the private headers are missing, menubar dropdowns fall back to stock
    # QMenu placement.
    find_package(Qt6 6.5 QUIET COMPONENTS WidgetsPrivate)
    qt_standard_project_setup()

    qt_add_executable(hammer-qt6
        app/main.cpp
        app/FaceEditSheet.cpp
        app/FaceEditSheet.hpp
        app/MainWindow.cpp
        app/MainWindow.hpp
        app/HammerIconFactory.cpp
        app/HammerIconFactory.hpp
        app/ToolbarIcons.cpp
        app/ToolbarIcons.hpp
        app/HammerTheme.cpp
        app/HammerTheme.hpp
        app/ScrollMenu.cpp
        app/ScrollMenu.hpp
        app/MapDocumentWidget.cpp
        app/ObjectPropertiesDialog.cpp
        app/ObjectPropertiesDialog.hpp
        app/MapDocumentWidget.hpp
        app/RecentFiles.cpp
        app/RecentFiles.hpp
        app/MapViewWidget.cpp
        app/MapViewWidget.hpp
        app/FreemanWindow.cpp
        app/FreemanWindow.hpp
        app/WaylandPointerLock.cpp
        app/WaylandPointerLock.hpp
        app/Hardware3DViewport.cpp
        app/Hardware3DViewport.hpp
        app/CollabSession.cpp
        app/CollabSession.hpp
        app/CollabAssets.cpp
        app/CollabAssets.hpp
        app/CollabServer.cpp
        app/CollabServer.hpp
        hammer_resources.qrc
    )

    set(HAMMER_HAVE_VULKAN_RAY_TRACING OFF)
    if(HAMMER_ENABLE_VULKAN_RAY_TRACING)
        find_package(Vulkan QUIET)
        find_program(GLSLANG_VALIDATOR_EXECUTABLE NAMES glslangValidator)
        if(Vulkan_FOUND AND GLSLANG_VALIDATOR_EXECUTABLE)
            # The live preview shader plus the six VRAD radiosity bake passes.
            # The bake passes all #include shaders/rad_common.glsl, so they need
            # an include path and a dependency on it - without the DEPENDS an
            # edit to the shared header would not rebuild anything.
            set(HAMMER_COMPUTE_SHADERS
                raytraced_preview.comp
                rad_patch_direct.comp
                rad_transfers.comp
                rad_bounce.comp
                rad_luxel_direct.comp
                rad_resolve.comp
                rad_prop_vertex.comp
                bloom_bright.comp
                bloom_blur.comp
                composite.comp)
            set(HAMMER_SHADER_INCLUDES
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/rad_common.glsl")
            set(HAMMER_COMPUTE_SHADER_SPVS)
            foreach(shader IN LISTS HAMMER_COMPUTE_SHADERS)
                set(shaderSource "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}")
                set(shaderSpv
                    "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders/${shader}.spv")
                add_custom_command(
                    OUTPUT "${shaderSpv}"
                    COMMAND "${CMAKE_COMMAND}" -E make_directory
                            "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders"
                    COMMAND "${GLSLANG_VALIDATOR_EXECUTABLE}"
                            -V --target-env vulkan1.2 -S comp
                            -I"${CMAKE_CURRENT_SOURCE_DIR}/shaders"
                            -o "${shaderSpv}"
                            "${shaderSource}"
                    DEPENDS "${shaderSource}" ${HAMMER_SHADER_INCLUDES}
                    COMMENT "Compiling Vulkan compute shader ${shader}"
                    VERBATIM)
                set_source_files_properties("${shaderSpv}" PROPERTIES
                    GENERATED TRUE QT_RESOURCE_ALIAS "${shader}.spv")
                list(APPEND HAMMER_COMPUTE_SHADER_SPVS "${shaderSpv}")
            endforeach()
            add_custom_target(hammer_raytraced_shader
                DEPENDS ${HAMMER_COMPUTE_SHADER_SPVS})
            qt_add_resources(hammer-qt6 "hammer_raytraced_shaders"
                PREFIX "/shaders"
                FILES ${HAMMER_COMPUTE_SHADER_SPVS})
            add_dependencies(hammer-qt6 hammer_raytraced_shader)
            target_sources(hammer-qt6 PRIVATE
                app/VulkanRayTracedViewport.cpp
                app/VulkanRayTracedViewport.hpp)
            target_link_libraries(hammer-qt6 PRIVATE Vulkan::Vulkan)
            target_compile_definitions(hammer-qt6 PRIVATE
                HAMMER_HAVE_VULKAN_RAY_TRACING=1)
            set(HAMMER_HAVE_VULKAN_RAY_TRACING ON)
            message(STATUS "Vulkan KHR ray-traced preview enabled")
        else()
            message(WARNING
                "Vulkan ray-traced preview disabled: install Vulkan-Headers/Vulkan-Loader "
                "development files and glslangValidator")
        endif()
    endif()

    target_link_libraries(hammer-qt6 PRIVATE hammer_original_portable hammer_vmf hammer_camera hammer_fgd hammer_assets hammer_ray_scene Qt6::Widgets Qt6::OpenGL Qt6::Network GameNetworkingSockets_s)
    if(TARGET Qt6::WidgetsPrivate)
        target_link_libraries(hammer-qt6 PRIVATE Qt6::WidgetsPrivate)
        target_compile_definitions(hammer-qt6 PRIVATE HAMMER_HAVE_QMENU_PRIVATE=1)
        message(STATUS "Scrollable menubar dropdowns enabled (QtWidgets private headers found)")
    else()
        message(STATUS "QtWidgets private headers not found: menubar dropdowns use stock QMenu placement")
    endif()
    set_target_properties(hammer-qt6 PROPERTIES OUTPUT_NAME "hammerminusminus")
    if(WIN32)
        # GUI subsystem: no console window behind the editor.
        set_target_properties(hammer-qt6 PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()

    if(UNIX AND NOT APPLE AND HAMMER_ENABLE_WAYLAND_CAPTURE)
        # QWindow::winId() became the public wl_surface handle on Qt 6.9. Older
        # Qt versions can still use the private native window interface when the
        # matching private development package is installed, but it is optional.
        set(HAMMER_CAN_ACCESS_WAYLAND_SURFACE OFF)
        if(Qt6_VERSION VERSION_GREATER_EQUAL 6.9)
            set(HAMMER_CAN_ACCESS_WAYLAND_SURFACE ON)
            target_compile_definitions(hammer-qt6 PRIVATE HAMMER_WAYLAND_SURFACE_FROM_WINID=1)
        elseif(TARGET Qt6::GuiPrivate)
            set(HAMMER_CAN_ACCESS_WAYLAND_SURFACE ON)
            target_link_libraries(hammer-qt6 PRIVATE Qt6::GuiPrivate)
            target_compile_definitions(hammer-qt6 PRIVATE HAMMER_WAYLAND_SURFACE_FROM_QT_PRIVATE=1)
        endif()

        if(HAMMER_CAN_ACCESS_WAYLAND_SURFACE)
            find_package(PkgConfig QUIET)
            if(PkgConfig_FOUND)
                pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
                pkg_get_variable(WAYLAND_PROTOCOLS_DATA_DIR wayland-protocols pkgdatadir)
            endif()
            find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)

            set(POINTER_CONSTRAINTS_XML "${WAYLAND_PROTOCOLS_DATA_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml")
            set(RELATIVE_POINTER_XML "${WAYLAND_PROTOCOLS_DATA_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml")
            if(TARGET PkgConfig::WAYLAND_CLIENT AND WAYLAND_SCANNER_EXECUTABLE AND
               EXISTS "${POINTER_CONSTRAINTS_XML}" AND EXISTS "${RELATIVE_POINTER_XML}")
                set(HAMMER_WAYLAND_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-wayland")
                file(MAKE_DIRECTORY "${HAMMER_WAYLAND_GENERATED_DIR}")
                set(POINTER_CONSTRAINTS_HEADER "${HAMMER_WAYLAND_GENERATED_DIR}/pointer-constraints-unstable-v1-client-protocol.h")
                set(POINTER_CONSTRAINTS_CODE "${HAMMER_WAYLAND_GENERATED_DIR}/pointer-constraints-unstable-v1-protocol.c")
                set(RELATIVE_POINTER_HEADER "${HAMMER_WAYLAND_GENERATED_DIR}/relative-pointer-unstable-v1-client-protocol.h")
                set(RELATIVE_POINTER_CODE "${HAMMER_WAYLAND_GENERATED_DIR}/relative-pointer-unstable-v1-protocol.c")

                add_custom_command(
                    OUTPUT "${POINTER_CONSTRAINTS_HEADER}" "${POINTER_CONSTRAINTS_CODE}"
                    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${POINTER_CONSTRAINTS_XML}" "${POINTER_CONSTRAINTS_HEADER}"
                    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${POINTER_CONSTRAINTS_XML}" "${POINTER_CONSTRAINTS_CODE}"
                    DEPENDS "${POINTER_CONSTRAINTS_XML}"
                    VERBATIM)
                add_custom_command(
                    OUTPUT "${RELATIVE_POINTER_HEADER}" "${RELATIVE_POINTER_CODE}"
                    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${RELATIVE_POINTER_XML}" "${RELATIVE_POINTER_HEADER}"
                    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${RELATIVE_POINTER_XML}" "${RELATIVE_POINTER_CODE}"
                    DEPENDS "${RELATIVE_POINTER_XML}"
                    VERBATIM)

                target_sources(hammer-qt6 PRIVATE
                    "${POINTER_CONSTRAINTS_HEADER}" "${POINTER_CONSTRAINTS_CODE}"
                    "${RELATIVE_POINTER_HEADER}" "${RELATIVE_POINTER_CODE}")
                target_include_directories(hammer-qt6 PRIVATE "${HAMMER_WAYLAND_GENERATED_DIR}")
                target_link_libraries(hammer-qt6 PRIVATE PkgConfig::WAYLAND_CLIENT)
                target_compile_definitions(hammer-qt6 PRIVATE HAMMER_HAVE_WAYLAND_CAPTURE=1)
                message(STATUS "Native Wayland Z-camera pointer capture enabled")
            else()
                message(WARNING "Native Wayland Z-camera capture disabled: install wayland-devel and wayland-protocols-devel (or configure -DHAMMER_ENABLE_WAYLAND_CAPTURE=OFF)")
            endif()
        else()
            message(WARNING "Native Wayland Z-camera capture needs Qt 6.9+ or the matching Qt GuiPrivate development target; using the soft Qt fallback")
        endif()
    endif()
    target_compile_definitions(hammer-qt6 PRIVATE
        HAMMER_PORT_VERSION="${PROJECT_VERSION}"
        "HAMMER_APP_DISPLAY_NAME=\"Hammer--\""
        "HAMMER_APP_ID=\"hammerminusminus\""
        "HAMMER_SETTINGS_ORGANIZATION=\"HammerPlusPlus\""
        HAMMER_ORIGINAL_SOURCE_ROOT="${HAMMER_REPOSITORY_ROOT}/src"
        HAMMER_TOOL_ICON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/tool_icons"
        HAMMER_APP_LOGO_PATH="${CMAKE_CURRENT_SOURCE_DIR}/resources/app_logo.png"
        HAMMER_WINDOW_ICON_PATH="${CMAKE_CURRENT_SOURCE_DIR}/resources/window_icon.png"
    )
    install(TARGETS hammer-qt6 RUNTIME DESTINATION bin)
    install(DIRECTORY resources/tool_icons DESTINATION share/hammerminusminus)
    install(FILES resources/app_logo.png resources/window_icon.png DESTINATION share/hammerminusminus)
    install(FILES resources/app_logo.png
            DESTINATION share/icons/hicolor/256x256/apps
            RENAME hammerminusminus.png)
    install(FILES resources/hammerminusminus.desktop DESTINATION share/applications)
endif()

if(HAMMER_BUILD_TESTS)
    enable_testing()
    add_executable(hammer-port-tests tests/portable_core_tests.cpp)
    target_link_libraries(hammer-port-tests PRIVATE hammer_original_portable)
    add_test(NAME hammer-port-tests COMMAND hammer-port-tests)

    add_executable(hammer-vmf-tests tests/vmf_document_tests.cpp)
    target_link_libraries(hammer-vmf-tests PRIVATE hammer_vmf hammer_camera hammer_fgd)
    add_test(NAME hammer-vmf-tests COMMAND hammer-vmf-tests)

    add_executable(hammer-sync-tests tests/vmf_sync_tests.cpp)
    target_link_libraries(hammer-sync-tests PRIVATE hammer_vmf)
    add_test(NAME hammer-sync-tests COMMAND hammer-sync-tests)

    if(HAMMER_BUILD_QT)
        qt_add_executable(hammer-collab-server-tests
            tests/collab_server_tests.cpp
            app/CollabSession.cpp
            app/CollabSession.hpp
            app/CollabAssets.cpp
            app/CollabAssets.hpp)
        target_include_directories(hammer-collab-server-tests PRIVATE app)
        target_link_libraries(hammer-collab-server-tests PRIVATE
            hammer_vmf hammer_assets Qt6::Core Qt6::Network GameNetworkingSockets_s)
        add_test(NAME hammer-collab-server-tests
                 COMMAND hammer-collab-server-tests $<TARGET_FILE:hammer-qt6>)

        qt_add_executable(hammer-collab-tests
            tests/collab_session_tests.cpp
            app/CollabSession.cpp
            app/CollabSession.hpp
            app/CollabAssets.cpp
            app/CollabAssets.hpp)
        target_include_directories(hammer-collab-tests PRIVATE app)
        target_link_libraries(hammer-collab-tests PRIVATE hammer_vmf hammer_assets Qt6::Core Qt6::Network GameNetworkingSockets_s)
        add_test(NAME hammer-collab-tests COMMAND hammer-collab-tests)
    endif()

    add_executable(hammer-groups-tests tests/vmf_groups_tests.cpp)
    target_link_libraries(hammer-groups-tests PRIVATE hammer_vmf)
    add_test(NAME hammer-groups-tests COMMAND hammer-groups-tests)

    add_executable(hammer-ray-scene-tests tests/ray_tracing_scene_tests.cpp)
    target_link_libraries(hammer-ray-scene-tests PRIVATE hammer_ray_scene)
    add_test(NAME hammer-ray-scene-tests COMMAND hammer-ray-scene-tests)

    add_executable(hammer-assets-tests tests/asset_system_tests.cpp)
    target_link_libraries(hammer-assets-tests PRIVATE hammer_assets)
    add_test(NAME hammer-assets-tests COMMAND hammer-assets-tests)
    add_test(NAME hammer-studio-animation-tests COMMAND hammer-assets-tests)
    set_tests_properties(hammer-studio-animation-tests PROPERTIES
        ENVIRONMENT "HAMMER_ANIMATION_ONLY=1")
    add_test(NAME hammer-material-proxy-tests COMMAND hammer-assets-tests)
    set_tests_properties(hammer-material-proxy-tests PROPERTIES
        ENVIRONMENT "HAMMER_MATERIAL_PROXY_ONLY=1")

    if(HAMMER_BUILD_QT)
        # Drives MapViewWidget's 2D selection tool with synthetic mouse events
        # on the offscreen platform: click-select, click-to-cycle handles, and
        # drag-to-move must behave identically for brushes and point entities.
        qt_add_executable(hammer-view-tests
            tests/view_interaction_tests.cpp
            app/MapViewWidget.cpp
            app/MapViewWidget.hpp
            app/Hardware3DViewport.cpp
            app/Hardware3DViewport.hpp
            app/WaylandPointerLock.cpp
            app/WaylandPointerLock.hpp
        )
        target_link_libraries(hammer-view-tests PRIVATE
            hammer_original_portable hammer_vmf hammer_camera hammer_fgd
            hammer_assets hammer_ray_scene Qt6::Widgets Qt6::OpenGL)
        # Map > Check for Problems: runs every check and every fix over a map
        # built to trip all of them.
        qt_add_executable(hammer-map-check-tests
            tests/map_check_tests.cpp
            app/MapDocumentWidget.cpp
            app/ObjectPropertiesDialog.cpp
            app/ObjectPropertiesDialog.hpp
            app/MapDocumentWidget.hpp
            app/FaceEditSheet.cpp
            app/FaceEditSheet.hpp
            app/MapViewWidget.cpp
            app/MapViewWidget.hpp
            app/Hardware3DViewport.cpp
            app/Hardware3DViewport.hpp
            app/WaylandPointerLock.cpp
            app/WaylandPointerLock.hpp
        )
        target_link_libraries(hammer-map-check-tests PRIVATE
            hammer_original_portable hammer_vmf hammer_camera hammer_fgd
            hammer_assets hammer_ray_scene Qt6::Widgets Qt6::OpenGL)
        add_test(NAME hammer-map-check-tests COMMAND hammer-map-check-tests -platform offscreen)

        # Document-level Grouping / VisGrouping behaviour. Mirrors the map-check
        # target's source list because it drives the same MapDocumentWidget.
        qt_add_executable(hammer-grouping-tests
            tests/grouping_integration_tests.cpp
            app/MapDocumentWidget.cpp
            app/ObjectPropertiesDialog.cpp
            app/ObjectPropertiesDialog.hpp
            app/MapDocumentWidget.hpp
            app/FaceEditSheet.cpp
            app/FaceEditSheet.hpp
            app/MapViewWidget.cpp
            app/MapViewWidget.hpp
            app/Hardware3DViewport.cpp
            app/Hardware3DViewport.hpp
            app/WaylandPointerLock.cpp
            app/WaylandPointerLock.hpp
        )
        target_link_libraries(hammer-grouping-tests PRIVATE
            hammer_original_portable hammer_vmf hammer_camera hammer_fgd
            hammer_assets hammer_ray_scene Qt6::Widgets Qt6::OpenGL)
        add_test(NAME hammer-grouping-tests COMMAND hammer-grouping-tests -platform offscreen)

        # VisGroup dialog regression test and screenshot diagnostic. Registered:
        # it guards the reentrancy crash where rebuilding the Filter Control
        # tree from inside itemChanged freed the item Qt was still using.
        # Given a map path it also writes PNGs of each dialog for eyeballing.
        qt_add_executable(hammer-visgroup-dialog-harness
            tests/visgroup_dialog_harness.cpp
        app/FaceEditSheet.cpp
        app/FaceEditSheet.hpp
        app/MainWindow.cpp
        app/MainWindow.hpp
        app/HammerIconFactory.cpp
        app/HammerIconFactory.hpp
        app/ToolbarIcons.cpp
        app/ToolbarIcons.hpp
        app/HammerTheme.cpp
        app/HammerTheme.hpp
        app/ScrollMenu.cpp
        app/ScrollMenu.hpp
        app/MapDocumentWidget.cpp
        app/ObjectPropertiesDialog.cpp
        app/ObjectPropertiesDialog.hpp
        app/MapDocumentWidget.hpp
        app/RecentFiles.cpp
        app/RecentFiles.hpp
        app/MapViewWidget.cpp
        app/MapViewWidget.hpp
        app/FreemanWindow.cpp
        app/FreemanWindow.hpp
        app/WaylandPointerLock.cpp
        app/WaylandPointerLock.hpp
        app/Hardware3DViewport.cpp
        app/Hardware3DViewport.hpp
        app/CollabSession.cpp
        app/CollabSession.hpp
        app/CollabAssets.cpp
        app/CollabAssets.hpp
        hammer_resources.qrc
        )
        target_link_libraries(hammer-visgroup-dialog-harness PRIVATE
            hammer_original_portable hammer_vmf hammer_camera hammer_fgd
            hammer_assets hammer_ray_scene Qt6::Widgets Qt6::OpenGL Qt6::Network GameNetworkingSockets_s)
        if(HAMMER_HAVE_VULKAN_RAY_TRACING)
            target_sources(hammer-visgroup-dialog-harness PRIVATE
                app/VulkanRayTracedViewport.cpp
                app/VulkanRayTracedViewport.hpp)
            qt_add_resources(hammer-visgroup-dialog-harness "harness_raytraced_shaders"
                PREFIX "/shaders"
                FILES ${HAMMER_COMPUTE_SHADER_SPVS})
            add_dependencies(hammer-visgroup-dialog-harness hammer_raytraced_shader)
            target_link_libraries(hammer-visgroup-dialog-harness PRIVATE Vulkan::Vulkan)
            target_compile_definitions(hammer-visgroup-dialog-harness PRIVATE
                HAMMER_HAVE_VULKAN_RAY_TRACING=1)
        endif()
        # Mirrors hammer-qt6's definitions; MainWindow.cpp needs them to compile.
        target_compile_definitions(hammer-visgroup-dialog-harness PRIVATE
            HAMMER_PORT_VERSION="${PROJECT_VERSION}"
            "HAMMER_APP_DISPLAY_NAME=\"Hammer--\""
            "HAMMER_APP_ID=\"hammerminusminus\""
            "HAMMER_SETTINGS_ORGANIZATION=\"HammerPlusPlus\""
            HAMMER_ORIGINAL_SOURCE_ROOT="${HAMMER_REPOSITORY_ROOT}/src"
            HAMMER_TOOL_ICON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/tool_icons"
            HAMMER_APP_LOGO_PATH="${CMAKE_CURRENT_SOURCE_DIR}/resources/app_logo.png"
            HAMMER_WINDOW_ICON_PATH="${CMAKE_CURRENT_SOURCE_DIR}/resources/window_icon.png"
        )
        # MALLOC_PERTURB_ fills freed memory, so a use-after-free faults here
        # instead of quietly reading recycled bytes and passing.
        add_test(NAME hammer-visgroup-dialog-tests
                 COMMAND hammer-visgroup-dialog-harness -platform offscreen)
        set_tests_properties(hammer-visgroup-dialog-tests PROPERTIES
            ENVIRONMENT "MALLOC_PERTURB_=165;MALLOC_CHECK_=3")


        # Presence-overlay regression test: injects collaborator poses into a
        # MapDocumentWidget rendered offscreen and asserts the views'"'"' pixels
        # change. Given an output dir it also writes before/after PNGs.
        qt_add_executable(hammer-collab-overlay-harness
            tests/collab_overlay_harness.cpp
        app/FaceEditSheet.cpp
        app/FaceEditSheet.hpp
        app/MainWindow.cpp
        app/MainWindow.hpp
        app/HammerIconFactory.cpp
        app/HammerIconFactory.hpp
        app/ToolbarIcons.cpp
        app/ToolbarIcons.hpp
        app/HammerTheme.cpp
        app/HammerTheme.hpp
        app/ScrollMenu.cpp
        app/ScrollMenu.hpp
        app/MapDocumentWidget.cpp
        app/ObjectPropertiesDialog.cpp
        app/ObjectPropertiesDialog.hpp
        app/MapDocumentWidget.hpp
        app/RecentFiles.cpp
        app/RecentFiles.hpp
        app/MapViewWidget.cpp
        app/MapViewWidget.hpp
        app/FreemanWindow.cpp
        app/FreemanWindow.hpp
        app/WaylandPointerLock.cpp
        app/WaylandPointerLock.hpp
        app/Hardware3DViewport.cpp
        app/Hardware3DViewport.hpp
        app/CollabSession.cpp
        app/CollabSession.hpp
        app/CollabAssets.cpp
        app/CollabAssets.hpp
        hammer_resources.qrc
        )
        target_link_libraries(hammer-collab-overlay-harness PRIVATE
            hammer_original_portable hammer_vmf hammer_camera hammer_fgd
            hammer_assets hammer_ray_scene Qt6::Widgets Qt6::OpenGL Qt6::Network GameNetworkingSockets_s)
        if(HAMMER_HAVE_VULKAN_RAY_TRACING)
            target_sources(hammer-collab-overlay-harness PRIVATE
                app/VulkanRayTracedViewport.cpp
                app/VulkanRayTracedViewport.hpp)
            qt_add_resources(hammer-collab-overlay-harness "collab_overlay_raytraced_shaders"
                PREFIX "/shaders"
                FILES ${HAMMER_COMPUTE_SHADER_SPVS})
            add_dependencies(hammer-collab-overlay-harness hammer_raytraced_shader)
            target_link_libraries(hammer-collab-overlay-harness PRIVATE Vulkan::Vulkan)
            target_compile_definitions(hammer-collab-overlay-harness PRIVATE
                HAMMER_HAVE_VULKAN_RAY_TRACING=1)
        endif()
        # Mirrors hammer-qt6's definitions; MainWindow.cpp needs them to compile.
        target_compile_definitions(hammer-collab-overlay-harness PRIVATE
            HAMMER_PORT_VERSION="${PROJECT_VERSION}"
            "HAMMER_APP_DISPLAY_NAME=\"Hammer--\""
            "HAMMER_APP_ID=\"hammerminusminus\""
            "HAMMER_SETTINGS_ORGANIZATION=\"HammerPlusPlus\""
            HAMMER_ORIGINAL_SOURCE_ROOT="${HAMMER_REPOSITORY_ROOT}/src"
            HAMMER_TOOL_ICON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/tool_icons"
            HAMMER_APP_LOGO_PATH="${CMAKE_CURRENT_SOURCE_DIR}/resources/app_logo.png"
            HAMMER_WINDOW_ICON_PATH="${CMAKE_CURRENT_SOURCE_DIR}/resources/window_icon.png"
        )
        # MALLOC_PERTURB_ fills freed memory, so a use-after-free faults here
        # instead of quietly reading recycled bytes and passing.
        add_test(NAME hammer-collab-overlay-tests
                 COMMAND hammer-collab-overlay-harness -platform offscreen)
        set_tests_properties(hammer-collab-overlay-tests PROPERTIES
            ENVIRONMENT "MALLOC_PERTURB_=165;MALLOC_CHECK_=3")



        # Manual water-shader diagnostic; not a registered test.
        qt_add_executable(hammer-water-preview-harness
            tests/water_preview_harness.cpp
            app/MapViewWidget.cpp
            app/MapViewWidget.hpp
            app/Hardware3DViewport.cpp
            app/Hardware3DViewport.hpp
            app/WaylandPointerLock.cpp
            app/WaylandPointerLock.hpp
        )
        target_link_libraries(hammer-water-preview-harness PRIVATE
            hammer_original_portable hammer_vmf hammer_camera hammer_fgd
            hammer_assets hammer_ray_scene Qt6::Widgets Qt6::OpenGL)

        # Manual diagnostic: detail props scattered over a %detailtype ground
        # material, in both viewports. Not a ctest - it needs a game mount.
        if(HAMMER_HAVE_VULKAN_RAY_TRACING)
            qt_add_executable(hammer-detail-prop-harness
                tests/detail_prop_harness.cpp
                app/MapViewWidget.cpp
                app/MapViewWidget.hpp
                app/Hardware3DViewport.cpp
                app/Hardware3DViewport.hpp
                app/VulkanRayTracedViewport.cpp
                app/VulkanRayTracedViewport.hpp
                app/WaylandPointerLock.cpp
                app/WaylandPointerLock.hpp
            )
            target_compile_definitions(hammer-detail-prop-harness PRIVATE
                HAMMER_HAVE_VULKAN_RAY_TRACING=1)
            target_link_libraries(hammer-detail-prop-harness PRIVATE
                hammer_original_portable hammer_vmf hammer_camera hammer_fgd
                hammer_assets hammer_ray_scene Vulkan::Vulkan Qt6::Widgets Qt6::OpenGL)
            add_dependencies(hammer-detail-prop-harness hammer_raytraced_shader)
            qt_add_resources(hammer-detail-prop-harness "detail_harness_shaders"
                PREFIX "/shaders"
                FILES ${HAMMER_COMPUTE_SHADER_SPVS})
        endif()

        # Manual diagnostic: a move_rope/keyframe_rope chain in both the OpenGL
        # and the ray-traced viewport. Not a ctest - it needs a game mount.
        if(HAMMER_HAVE_VULKAN_RAY_TRACING)
            qt_add_executable(hammer-rope-preview-harness
                tests/rope_preview_harness.cpp
                app/MapViewWidget.cpp
                app/MapViewWidget.hpp
                app/Hardware3DViewport.cpp
                app/Hardware3DViewport.hpp
                app/VulkanRayTracedViewport.cpp
                app/VulkanRayTracedViewport.hpp
                app/WaylandPointerLock.cpp
                app/WaylandPointerLock.hpp
            )
            target_compile_definitions(hammer-rope-preview-harness PRIVATE
                HAMMER_HAVE_VULKAN_RAY_TRACING=1)
            target_link_libraries(hammer-rope-preview-harness PRIVATE
                hammer_original_portable hammer_vmf hammer_camera hammer_fgd
                hammer_assets hammer_ray_scene Vulkan::Vulkan Qt6::Widgets Qt6::OpenGL)
            add_dependencies(hammer-rope-preview-harness hammer_raytraced_shader)
            qt_add_resources(hammer-rope-preview-harness "rope_harness_shaders"
                PREFIX "/shaders"
                FILES ${HAMMER_COMPUTE_SHADER_SPVS})
        endif()

        # Manual diagnostic: renders a scene through the real ray-traced
        # preview offscreen. Not a ctest - it needs a game mount - but it is
        # the only way to exercise the radiosity solve end to end.
        if(HAMMER_HAVE_VULKAN_RAY_TRACING)
            qt_add_executable(hammer-rt-light-color-harness
                tests/rt_light_color_harness.cpp
                app/MapViewWidget.cpp
                app/MapViewWidget.hpp
                app/Hardware3DViewport.cpp
                app/Hardware3DViewport.hpp
                app/VulkanRayTracedViewport.cpp
                app/VulkanRayTracedViewport.hpp
                app/WaylandPointerLock.cpp
                app/WaylandPointerLock.hpp
            )
            target_compile_definitions(hammer-rt-light-color-harness PRIVATE
                HAMMER_HAVE_VULKAN_RAY_TRACING=1)
            target_link_libraries(hammer-rt-light-color-harness PRIVATE
                hammer_original_portable hammer_vmf hammer_camera hammer_fgd
                hammer_assets hammer_ray_scene Vulkan::Vulkan Qt6::Widgets Qt6::OpenGL)
            add_dependencies(hammer-rt-light-color-harness hammer_raytraced_shader)
            qt_add_resources(hammer-rt-light-color-harness "harness_shaders"
                PREFIX "/shaders"
                FILES ${HAMMER_COMPUTE_SHADER_SPVS})
        endif()

        qt_add_executable(hammer-recent-files-tests
            tests/recent_files_tests.cpp
            app/RecentFiles.cpp
            app/RecentFiles.hpp
        )
        target_include_directories(hammer-recent-files-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/app")
        target_link_libraries(hammer-recent-files-tests PRIVATE Qt6::Core)
        add_test(NAME hammer-recent-files-tests COMMAND hammer-recent-files-tests)

        add_test(NAME hammer-view-tests COMMAND hammer-view-tests)
        set_tests_properties(hammer-view-tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
