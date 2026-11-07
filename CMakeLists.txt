cmake_minimum_required(VERSION 3.21)
project(resend VERSION 0.1.3 LANGUAGES CXX)

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

add_library(resend_core
    src/core/address.cpp src/core/json.cpp src/core/sim_time.cpp
    src/scenario/scenario.cpp src/scenario/edit.cpp src/scenario/scenario_json.cpp
    src/sim/simulation.cpp src/sim/medium.cpp src/sim/arp.cpp src/sim/ping_app.cpp
    src/sim/host.cpp src/sim/bridge.cpp
    src/explain/explain.cpp
    src/wire/wire.cpp)
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
    add_executable(resend-core-tests tests/core_tests.cpp)
    target_link_libraries(resend-core-tests PRIVATE resend_core)
    target_compile_options(resend-core-tests PRIVATE ${resend_warnings})
    add_test(NAME core-types COMMAND resend-core-tests)
    add_executable(resend-scenario-tests tests/scenario_tests.cpp)
    target_link_libraries(resend-scenario-tests PRIVATE resend_core)
    target_compile_options(resend-scenario-tests PRIVATE ${resend_warnings})
    target_compile_definitions(resend-scenario-tests PRIVATE RESEND_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples"
        RESEND_DOCS_DIR="${PROJECT_SOURCE_DIR}/docs")
    add_test(NAME scenario-files COMMAND resend-scenario-tests)
    add_executable(resend-sim-tests tests/sim_tests.cpp)
    target_link_libraries(resend-sim-tests PRIVATE resend_core)
    target_compile_options(resend-sim-tests PRIVATE ${resend_warnings})
    target_compile_definitions(resend-sim-tests PRIVATE RESEND_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")
    add_test(NAME simulation COMMAND resend-sim-tests)
    add_executable(resend-live-tests tests/live_tests.cpp)
    target_link_libraries(resend-live-tests PRIVATE resend_core)
    target_compile_options(resend-live-tests PRIVATE ${resend_warnings})
    add_test(NAME live-network COMMAND resend-live-tests)
    add_executable(resend-wire-tests tests/wire_tests.cpp)
    target_link_libraries(resend-wire-tests PRIVATE resend_core)
    target_compile_options(resend-wire-tests PRIVATE ${resend_warnings})
    add_test(NAME wire-format COMMAND resend-wire-tests)
    add_executable(resend-regression-tests tests/regression_tests.cpp)
    target_link_libraries(resend-regression-tests PRIVATE resend_core)
    target_compile_options(resend-regression-tests PRIVATE ${resend_warnings})
    target_compile_definitions(resend-regression-tests PRIVATE RESEND_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden")
    add_test(NAME regression COMMAND resend-regression-tests)
    add_executable(resend-app-tests tests/app_tests.cpp)
    target_link_libraries(resend-app-tests PRIVATE resend_app)
    target_compile_options(resend-app-tests PRIVATE ${resend_warnings})
    add_test(NAME app-logic COMMAND resend-app-tests)
    # The documented exit codes: 0 reachable, 1 unreachable, 2 invalid input.
    add_test(NAME cli-ping-reachable COMMAND resend-cli 192.168.1.20)
    add_test(NAME cli-ping-disconnected COMMAND resend-cli 192.168.1.20 --disconnect)
    add_test(NAME cli-ping-no-route COMMAND resend-cli 10.0.0.1)
    add_test(NAME cli-invalid-address COMMAND resend-cli 192.168.1.300)
    set(examples ${PROJECT_SOURCE_DIR}/examples)
    add_test(NAME cli-scenario COMMAND resend-cli --scenario ${examples}/two-switches.json --ping PC-C 192.168.1.10)
    add_test(NAME cli-concurrent-pings COMMAND resend-cli --scenario ${examples}/office-floor.json
        --ping Desk-1 10.0.0.22 --ping Desk-5 10.0.0.12 --trace)
    add_test(NAME cli-scenario-check COMMAND resend-cli --check --scenario ${examples}/office-floor.json)
    add_test(NAME cli-scenario-ping-fails COMMAND resend-cli --scenario ${examples}/unplugged.json --ping PC-A 192.168.1.20)
    add_test(NAME cli-unknown-host COMMAND resend-cli --ping Nobody 192.168.1.20)
    add_test(NAME cli-missing-file COMMAND resend-cli --scenario ${examples}/missing.json)
    add_test(NAME cli-ping-count COMMAND resend-cli --count 3 --interval 200ms --timeout 500ms)
    add_test(NAME cli-cable-down-in-flight COMMAND resend-cli --at 15ms --link-down 2)
    add_test(NAME cli-power-off-destination COMMAND resend-cli --count 2 --at 500ms --power-off PC-B)
    add_test(NAME cli-stale-address COMMAND resend-cli --count 2 --at 500ms --set-ip PC-B 192.168.1.21/24)
    add_test(NAME cli-switch-loop COMMAND resend-cli --scenario ${examples}/switch-loop.json --ping PC-A 192.168.1.20)
    add_test(NAME cli-switch-loop-broken COMMAND resend-cli --scenario ${examples}/switch-loop.json --link-down 5
        --ping PC-A 192.168.1.20)
    add_test(NAME cli-invalid-count COMMAND resend-cli --count 0)
    add_test(NAME cli-unknown-cable COMMAND resend-cli --link-down 9)
    set_tests_properties(cli-ping-disconnected cli-ping-no-route cli-invalid-address
        cli-scenario-ping-fails cli-unknown-host cli-missing-file cli-cable-down-in-flight
        cli-invalid-count cli-unknown-cable PROPERTIES WILL_FAIL TRUE)
    # A regular expression to match replaces the exit status.
    set_tests_properties(cli-ping-count PROPERTIES PASS_REGULAR_EXPRESSION "3 packets transmitted, 3 received")
    set_tests_properties(cli-power-off-destination PROPERTIES PASS_REGULAR_EXPRESSION "2 packets transmitted, 1 received")
    set_tests_properties(cli-stale-address PROPERTIES PASS_REGULAR_EXPRESSION "icmp_seq=1 ttl=64.*icmp_seq=2 lost: no Echo Reply")
    set_tests_properties(cli-switch-loop PROPERTIES PASS_REGULAR_EXPRESSION "Storm protection: the simulator stopped 3 frames")
    # Wireshark's expert system checks the captures; skipped without tshark.
    find_program(TSHARK tshark)
    add_test(NAME pcap-wireshark-expert COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:resend-cli> -DTSHARK=${TSHARK}
        -DEXAMPLES=${examples} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/captures -P ${PROJECT_SOURCE_DIR}/tests/pcap_expert.cmake)
    set_tests_properties(pcap-wireshark-expert PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED")
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
        target_compile_definitions(resend-gui-tests PRIVATE SLINT_FEATURE_EXPERIMENTAL
            RESEND_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")
        add_test(NAME gui-behavior COMMAND resend-gui-tests)
    endif()

    if(RESEND_BUILD_SPIKES)
        add_subdirectory(spikes/canvas)
    endif()
endif()

install(TARGETS resend-cli RUNTIME DESTINATION bin)
