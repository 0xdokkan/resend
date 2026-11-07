cmake_minimum_required(VERSION 3.21)
project(resend VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RESEND_BUILD_GUI "Build the Slint sandbox application" ON)
option(RESEND_FETCH_SLINT "Download and build pinned Slint sources if no SDK is installed" OFF)
option(RESEND_BUILD_SPIKES "Build the UI experiments in spikes/" OFF)
set(RESEND_SLINT_COMPILATION_UNITS 4 CACHE STRING
    "Number of C++ files generated from ui/app.slint, compiled in parallel")

if(MSVC)
    # /utf-8: sources and string literals are UTF-8 whatever the system code page.
    set(resend_warnings /W4 /utf-8)
else()
    set(resend_warnings -Wall -Wextra -Wpedantic)
endif()

add_library(resend_core src/core/address.cpp src/core/sim_time.cpp src/network.cpp)
target_include_directories(resend_core PUBLIC src)
target_compile_options(resend_core PRIVATE ${resend_warnings})

# App logic without Slint: playback and what the canvas shows.
add_library(resend_app src/app/playback.cpp src/app/scene.cpp)
target_link_libraries(resend_app PUBLIC resend_core)
target_compile_options(resend_app PRIVATE ${resend_warnings})

add_executable(resend-cli src/cli.cpp)
target_link_libraries(resend-cli PRIVATE resend_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(resend-tests tests/network_tests.cpp)
    target_link_libraries(resend-tests PRIVATE resend_core)
    target_compile_options(resend-tests PRIVATE ${resend_warnings})
    add_test(NAME network-behavior COMMAND resend-tests)
    add_executable(resend-core-tests tests/core_tests.cpp)
    target_link_libraries(resend-core-tests PRIVATE resend_core)
    target_compile_options(resend-core-tests PRIVATE ${resend_warnings})
    add_test(NAME core-types COMMAND resend-core-tests)
    add_executable(resend-app-tests tests/app_tests.cpp)
    target_link_libraries(resend-app-tests PRIVATE resend_app)
    target_compile_options(resend-app-tests PRIVATE ${resend_warnings})
    add_test(NAME app-logic COMMAND resend-app-tests)
    # The documented exit codes: 0 reachable, 1 unreachable, 2 invalid input.
    add_test(NAME cli-ping-reachable COMMAND resend-cli 192.168.1.20)
    add_test(NAME cli-ping-disconnected COMMAND resend-cli 192.168.1.20 --disconnect)
    add_test(NAME cli-ping-no-route COMMAND resend-cli 10.0.0.1)
    add_test(NAME cli-invalid-address COMMAND resend-cli 192.168.1.300)
    set_tests_properties(cli-ping-disconnected cli-ping-no-route cli-invalid-address PROPERTIES WILL_FAIL TRUE)
endif()

if(RESEND_BUILD_GUI)
    find_package(Slint 1.18 CONFIG QUIET)
    if(NOT Slint_FOUND AND RESEND_FETCH_SLINT)
        include(FetchContent)
        set(SLINT_FEATURE_BACKEND_QT OFF CACHE BOOL "" FORCE)
        set(SLINT_FEATURE_BACKEND_WINIT ON CACHE BOOL "" FORCE)
        set(SLINT_FEATURE_RENDERER_FEMTOVG ON CACHE BOOL "" FORCE)
        set(SLINT_FEATURE_RENDERER_SOFTWARE ON CACHE BOOL "" FORCE)
        set(SLINT_FEATURE_RENDERER_SKIA OFF CACHE BOOL "" FORCE)
        set(SLINT_FEATURE_TESTING ${BUILD_TESTING} CACHE BOOL "" FORCE)
        set(SLINT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SLINT_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(Slint
            GIT_REPOSITORY https://github.com/slint-ui/slint.git
            GIT_TAG 372cf0ee5577c3dfec309a45e7b778ba4e81b734)
        FetchContent_MakeAvailable(Slint)
    endif()
    if(NOT TARGET Slint::Slint)
        message(FATAL_ERROR
            "Slint C++ SDK 1.18 is required. Run scripts/fetch-slint.sh and pass its output as -DSlint_DIR=..., "
            "set -DCMAKE_PREFIX_PATH=/path/to/Slint-cpp-1.18.1-Linux-x86_64, "
            "or use -DRESEND_FETCH_SLINT=ON to build Slint with Rust. "
            "Use -DRESEND_BUILD_GUI=OFF for the independent core, CLI and tests.")
    endif()
    if(BUILD_TESTING)
        # The GUI tests find elements by accessible label, which needs element
        # debug info in the generated code; slint_target_sources() emits it
        # when this feature is listed on the Slint target.
        set_property(TARGET Slint::Slint APPEND PROPERTY SLINT_ENABLED_FEATURES SYSTEM_TESTING)
    endif()

    # Window, generated UI code and controller, shared by the app and its tests.
    add_library(resend_ui STATIC src/app_controller.cpp)
    target_link_libraries(resend_ui PUBLIC resend_app Slint::Slint)
    target_compile_options(resend_ui PRIVATE ${resend_warnings})
    slint_target_sources(resend_ui ui/app.slint COMPILATION_UNITS ${RESEND_SLINT_COMPILATION_UNITS})
    target_compile_definitions(resend_ui PRIVATE RESEND_VERSION="${PROJECT_VERSION}")

    add_executable(resend src/main.cpp)
    target_link_libraries(resend PRIVATE resend_ui)
    install(TARGETS resend RUNTIME DESTINATION bin)

    if(BUILD_TESTING)
        add_executable(resend-gui-tests tests/gui_tests.cpp)
        target_link_libraries(resend-gui-tests PRIVATE resend_ui)
        # slint::testing is still marked experimental in Slint 1.18.
        target_compile_definitions(resend-gui-tests PRIVATE SLINT_FEATURE_EXPERIMENTAL)
        add_test(NAME gui-behavior COMMAND resend-gui-tests)
    endif()

    if(RESEND_BUILD_SPIKES)
        add_subdirectory(spikes/canvas)
    endif()
endif()

install(TARGETS resend-cli RUNTIME DESTINATION bin)
