cmake_minimum_required(VERSION 3.21)
project(resend VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RESEND_BUILD_GUI "Build the Slint sandbox application" ON)
option(RESEND_FETCH_SLINT "Download and build pinned Slint sources if no SDK is installed" OFF)
option(RESEND_BUILD_SPIKES "Build the UI experiments in spikes/" OFF)
set(RESEND_SLINT_COMPILATION_UNITS 4 CACHE STRING
    "Number of C++ files generated from ui/app.slint, compiled in parallel")

if(MSVC)
    set(resend_warnings /W4)
    # Sources and string literals are UTF-8 whatever the system code page, and
    # the conforming preprocessor passes raw string literals through macros.
    add_compile_options(/utf-8 /Zc:preprocessor)
    # getenv() is deprecated by the CRT for the pointer it returns; every
    # value read is copied at once.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(resend_warnings -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
    # windows.h without its min and max macros and its rarely used parts.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_library(resend_core
    src/core/address.cpp src/core/json.cpp src/core/sim_time.cpp src/core/text.cpp
    src/scenario/scenario.cpp src/scenario/edit.cpp src/scenario/scenario_json.cpp
    src/sim/simulation.cpp src/sim/medium.cpp src/sim/arp.cpp src/sim/ping_app.cpp
    src/sim/host.cpp src/sim/bridge.cpp src/sim/router.cpp src/sim/routing.cpp
    src/explain/explain.cpp src/explain/lab_explain.cpp
    src/wire/wire.cpp
    src/lab/lab.cpp src/lab/check.cpp
    src/term/terminal.cpp)
target_include_directories(resend_core PUBLIC src)
target_compile_options(resend_core PRIVATE ${resend_warnings})

# The examples, built into the app in the order of its menu.
set(resend_examples sample-lan.json two-switches.json office-floor.json unplugged.json mismatched-masks.json switch-loop.json
    two-lans-router.json wrong-gateway.json missing-return-route.json routing-loop.json longest-prefix.json)
list(TRANSFORM resend_examples PREPEND ${PROJECT_SOURCE_DIR}/examples/ OUTPUT_VARIABLE resend_example_files)
string(REPLACE ";" "," resend_example_list "${resend_examples}")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/examples_data.cpp
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/examples -DEXAMPLES=${resend_example_list}
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/examples_data.cpp -P ${PROJECT_SOURCE_DIR}/cmake/embed_examples.cmake
    DEPENDS ${resend_example_files} ${PROJECT_SOURCE_DIR}/cmake/embed_examples.cmake
    COMMENT "Embedding the example scenarios")

# The labs, built into the app in the order of its library: by topic, the
# first steps first.
set(resend_labs first-network.json one-subnet-two-sides.json missing-gateway.json ipv4-basics.json vlsm.json
    static-routes.json missing-return-route.json routing-loop.json watch-arp-and-ping.json explain-traceroute.json)
list(TRANSFORM resend_labs PREPEND ${PROJECT_SOURCE_DIR}/labs/ OUTPUT_VARIABLE resend_lab_files)
string(REPLACE ";" "," resend_lab_list "${resend_labs}")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/labs_data.cpp
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/labs -DLABS=${resend_lab_list}
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/labs_data.cpp -P ${PROJECT_SOURCE_DIR}/cmake/embed_labs.cmake
    DEPENDS ${resend_lab_files} ${PROJECT_SOURCE_DIR}/cmake/embed_labs.cmake
    COMMENT "Embedding the labs")

# The look: themes and sprite sheets, built into the app.
set(resend_sheets devices.txt packets.txt icons.txt)
list(TRANSFORM resend_sheets PREPEND ${PROJECT_SOURCE_DIR}/assets/sprites/ OUTPUT_VARIABLE resend_sheet_files)
string(REPLACE ";" "," resend_sheet_list "${resend_sheets}")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/assets -DSHEETS=${resend_sheet_list}
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp -P ${PROJECT_SOURCE_DIR}/cmake/embed_assets.cmake
    DEPENDS ${resend_sheet_files} ${PROJECT_SOURCE_DIR}/assets/palettes.json ${PROJECT_SOURCE_DIR}/assets/sprites/legend.txt
        ${PROJECT_SOURCE_DIR}/cmake/embed_assets.cmake
    COMMENT "Embedding the themes and sprites")

# App logic without Slint: the workspace, the canvas, playback, files and the look.
add_library(resend_app src/app/playback.cpp src/app/scene.cpp src/app/effects.cpp src/app/history.cpp src/app/workspace.cpp
    src/app/canvas.cpp src/app/files.cpp src/app/timeline.cpp ${CMAKE_CURRENT_BINARY_DIR}/examples_data.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/labs_data.cpp
    src/look/look.cpp src/look/image_files.cpp ${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp)
target_link_libraries(resend_app PUBLIC resend_core)
target_compile_options(resend_app PRIVATE ${resend_warnings})
target_sources(resend_app PRIVATE src/app/cues.cpp)

# Sound (G5, docs/decisions/0005-audio-output.md): the synthesizer and its
# sounds are always built; miniaudio, one header pinned by its hash, takes
# them to the sound card. Without it, or offline at the first configure,
# resend is silent and otherwise the same.
option(RESEND_AUDIO "Play sounds through miniaudio" ON)
set(RESEND_MINIAUDIO_VERSION 0.11.25)
set(RESEND_MINIAUDIO_DIR ${PROJECT_SOURCE_DIR}/.deps/miniaudio-${RESEND_MINIAUDIO_VERSION} CACHE PATH
    "Where miniaudio.h and its LICENSE are, or are downloaded to")
set(resend_miniaudio_hashes
    miniaudio.h ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
    LICENSE 457f1b500e0adf6bc059edddfa78a2f62012e7c3bb43476c20e0bd23b25ba0eb)
set(resend_has_miniaudio OFF)
if(RESEND_AUDIO)
    set(resend_has_miniaudio ON)
    list(LENGTH resend_miniaudio_hashes resend_count)
    math(EXPR resend_last "${resend_count} - 1")
    foreach(resend_index RANGE 0 ${resend_last} 2)
        math(EXPR resend_next "${resend_index} + 1")
        list(GET resend_miniaudio_hashes ${resend_index} resend_file)
        list(GET resend_miniaudio_hashes ${resend_next} resend_hash)
        set(resend_path ${RESEND_MINIAUDIO_DIR}/${resend_file})
        if(NOT EXISTS ${resend_path})
            message(STATUS "Downloading miniaudio ${RESEND_MINIAUDIO_VERSION}: ${resend_file}")
            file(DOWNLOAD https://raw.githubusercontent.com/mackron/miniaudio/${RESEND_MINIAUDIO_VERSION}/${resend_file}
                ${resend_path}.part STATUS resend_status TLS_VERIFY ON)
            list(GET resend_status 0 resend_code)
            if(resend_code EQUAL 0)
                file(RENAME ${resend_path}.part ${resend_path})
            else()
                file(REMOVE ${resend_path}.part)
                message(WARNING "miniaudio could not be downloaded (${resend_status}): resend will be silent")
                set(resend_has_miniaudio OFF)
                break()
            endif()
        endif()
        file(SHA256 ${resend_path} resend_actual)
        if(NOT resend_actual STREQUAL resend_hash)
            message(FATAL_ERROR "${resend_path} is not miniaudio ${RESEND_MINIAUDIO_VERSION}: "
                "its SHA-256 is ${resend_actual}, not ${resend_hash}. Delete it to download it again.")
        endif()
    endforeach()
endif()

add_library(resend_audio src/audio/synth.cpp src/audio/sounds.cpp src/audio/output.cpp)
target_include_directories(resend_audio PUBLIC src)
target_compile_options(resend_audio PRIVATE ${resend_warnings})
find_package(Threads REQUIRED)
target_link_libraries(resend_audio PRIVATE Threads::Threads)
if(resend_has_miniaudio)
    # miniaudio's implementation, built with its own warnings left to it.
    target_sources(resend_audio PRIVATE src/audio/miniaudio.cpp)
    set_source_files_properties(src/audio/miniaudio.cpp PROPERTIES COMPILE_OPTIONS $<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>)
    target_include_directories(resend_audio PRIVATE ${RESEND_MINIAUDIO_DIR})
    target_compile_definitions(resend_audio PRIVATE RESEND_HAS_MINIAUDIO=1)
    if(NOT WIN32)
        # It opens ALSA, PulseAudio or JACK at run time: no link to any of them.
        target_link_libraries(resend_audio PRIVATE ${CMAKE_DL_LIBS} m)
    endif()
    install(FILES ${RESEND_MINIAUDIO_DIR}/LICENSE DESTINATION share/doc/resend/miniaudio)
endif()

# The app icon's files, painted from the sprites at build time: PNGs for
# the icon theme and an ICO for Windows (src/look/make_icons.cpp).
add_executable(resend-icons src/look/make_icons.cpp)
target_link_libraries(resend-icons PRIVATE resend_app)
target_compile_options(resend-icons PRIVATE ${resend_warnings})
set(resend_icon_dir ${CMAKE_CURRENT_BINARY_DIR}/icons)
add_custom_command(
    OUTPUT ${resend_icon_dir}/resend.ico
    COMMAND resend-icons ${resend_icon_dir}
    DEPENDS resend-icons
    COMMENT "Painting the app icon")
add_custom_target(resend-icon-files ALL DEPENDS ${resend_icon_dir}/resend.ico)

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
    add_executable(resend-routing-tests tests/routing_tests.cpp)
    target_link_libraries(resend-routing-tests PRIVATE resend_core)
    target_compile_options(resend-routing-tests PRIVATE ${resend_warnings})
    add_test(NAME routing COMMAND resend-routing-tests)
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
    add_executable(resend-effects-tests tests/effects_tests.cpp)
    target_link_libraries(resend-effects-tests PRIVATE resend_app)
    target_compile_options(resend-effects-tests PRIVATE ${resend_warnings})
    target_compile_definitions(resend-effects-tests PRIVATE RESEND_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")
    add_test(NAME effects COMMAND resend-effects-tests)
    add_executable(resend-sound-tests tests/sound_tests.cpp)
    target_link_libraries(resend-sound-tests PRIVATE resend_app resend_audio)
    target_compile_options(resend-sound-tests PRIVATE ${resend_warnings})
    target_compile_definitions(resend-sound-tests PRIVATE RESEND_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples"
        RESEND_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/cues")
    add_test(NAME sound COMMAND resend-sound-tests)
    add_executable(resend-workspace-tests tests/workspace_tests.cpp)
    target_link_libraries(resend-workspace-tests PRIVATE resend_app)
    target_compile_options(resend-workspace-tests PRIVATE ${resend_warnings})
    add_test(NAME workspace COMMAND resend-workspace-tests)
    add_executable(resend-canvas-tests tests/canvas_tests.cpp)
    target_link_libraries(resend-canvas-tests PRIVATE resend_app)
    target_compile_options(resend-canvas-tests PRIVATE ${resend_warnings})
    add_test(NAME canvas COMMAND resend-canvas-tests)
    add_executable(resend-files-tests tests/files_tests.cpp)
    target_link_libraries(resend-files-tests PRIVATE resend_app)
    target_compile_options(resend-files-tests PRIVATE ${resend_warnings})
    target_compile_definitions(resend-files-tests PRIVATE RESEND_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")
    add_test(NAME files COMMAND resend-files-tests)
    add_executable(resend-look-tests tests/look_tests.cpp)
    target_link_libraries(resend-look-tests PRIVATE resend_app)
    target_compile_options(resend-look-tests PRIVATE ${resend_warnings})
    target_compile_definitions(resend-look-tests PRIVATE RESEND_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME look COMMAND resend-look-tests)
    add_executable(resend-terminal-tests tests/terminal_tests.cpp)
    target_link_libraries(resend-terminal-tests PRIVATE resend_app)
    target_compile_options(resend-terminal-tests PRIVATE ${resend_warnings})
    target_compile_definitions(resend-terminal-tests PRIVATE RESEND_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples"
        RESEND_LABS_DIR="${PROJECT_SOURCE_DIR}/labs")
    add_test(NAME terminal COMMAND resend-terminal-tests)
    add_executable(resend-lab-tests tests/lab_tests.cpp)
    target_link_libraries(resend-lab-tests PRIVATE resend_app)
    target_compile_options(resend-lab-tests PRIVATE ${resend_warnings})
    target_compile_definitions(resend-lab-tests PRIVATE RESEND_LABS_DIR="${PROJECT_SOURCE_DIR}/labs"
        RESEND_DOCS_DIR="${PROJECT_SOURCE_DIR}/docs")
    add_test(NAME labs COMMAND resend-lab-tests)
    # Every lab proves itself: unsolved as given, solved by its solution.
    foreach(lab IN LISTS resend_labs)
        string(REPLACE ".json" "" lab_id "${lab}")
        add_test(NAME labs-verify-${lab_id} COMMAND resend-cli --lab ${PROJECT_SOURCE_DIR}/labs/${lab} --verify)
    endforeach()
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
    add_test(NAME cli-router COMMAND resend-cli --scenario ${examples}/two-lans-router.json --ping PC-A 10.2.0.20)
    add_test(NAME cli-traceroute COMMAND resend-cli --scenario ${examples}/two-lans-router.json --traceroute PC-A 10.2.0.20)
    add_test(NAME cli-ttl-exceeded COMMAND resend-cli --scenario ${examples}/two-lans-router.json --ttl 1 --ping PC-A 10.2.0.20)
    add_test(NAME cli-net-unreachable COMMAND resend-cli --scenario ${examples}/two-lans-router.json --ping PC-A 203.0.113.1)
    add_test(NAME cli-host-unreachable COMMAND resend-cli --scenario ${examples}/two-lans-router.json --timeout 5s --ping PC-A 10.2.0.99)
    add_test(NAME cli-routing-loop COMMAND resend-cli --scenario ${examples}/routing-loop.json --ping PC-A 10.9.9.9)
    add_test(NAME cli-wrong-gateway COMMAND resend-cli --scenario ${examples}/wrong-gateway.json --ping PC-C 10.2.0.20)
    add_test(NAME cli-set-gateway COMMAND resend-cli --scenario ${examples}/wrong-gateway.json --set-gateway PC-C 10.1.0.1 --ping PC-C 10.2.0.20)
    add_test(NAME cli-invalid-max-hops COMMAND resend-cli --max-hops 0 --traceroute PC-A 192.168.1.20)
    set_tests_properties(cli-ping-disconnected cli-ping-no-route cli-invalid-address
        cli-scenario-ping-fails cli-unknown-host cli-missing-file cli-cable-down-in-flight
        cli-invalid-count cli-unknown-cable cli-wrong-gateway cli-invalid-max-hops PROPERTIES WILL_FAIL TRUE)
    # A regular expression to match replaces the exit status.
    set_tests_properties(cli-ping-count PROPERTIES PASS_REGULAR_EXPRESSION "3 packets transmitted, 3 received")
    set_tests_properties(cli-power-off-destination PROPERTIES PASS_REGULAR_EXPRESSION "2 packets transmitted, 1 received")
    set_tests_properties(cli-stale-address PROPERTIES PASS_REGULAR_EXPRESSION "icmp_seq=1 ttl=64.*icmp_seq=2 lost: no Echo Reply")
    set_tests_properties(cli-switch-loop PROPERTIES PASS_REGULAR_EXPRESSION "Storm protection: the simulator stopped 3 frames")
    set_tests_properties(cli-router PROPERTIES PASS_REGULAR_EXPRESSION "icmp_seq=1 ttl=63")
    set_tests_properties(cli-traceroute PROPERTIES PASS_REGULAR_EXPRESSION " 1  10\\.1\\.0\\.1  80\\.000 ms\n 2  10\\.2\\.0\\.20  120\\.000 ms\n")
    set_tests_properties(cli-ttl-exceeded PROPERTIES PASS_REGULAR_EXPRESSION "From 10\\.1\\.0\\.1 icmp_seq=1 Time to live exceeded")
    set_tests_properties(cli-net-unreachable PROPERTIES PASS_REGULAR_EXPRESSION "From 10\\.1\\.0\\.1 icmp_seq=1 Destination Net Unreachable")
    set_tests_properties(cli-host-unreachable PROPERTIES PASS_REGULAR_EXPRESSION "From 10\\.1\\.0\\.1 icmp_seq=1 Destination Host Unreachable")
    set_tests_properties(cli-routing-loop PROPERTIES PASS_REGULAR_EXPRESSION "From 10\\.12\\.0\\.2 icmp_seq=1 Time to live exceeded")
    # Wireshark's expert system checks the captures, and its dissection the
    # inspector's; skipped without tshark.
    find_program(TSHARK tshark)
    add_executable(resend-dissect-tests tests/dissect_tshark.cpp)
    target_link_libraries(resend-dissect-tests PRIVATE resend_core)
    target_compile_options(resend-dissect-tests PRIVATE ${resend_warnings})
    target_compile_definitions(resend-dissect-tests PRIVATE RESEND_EXAMPLES_DIR="${examples}"
        RESEND_TSHARK="$<IF:$<BOOL:${TSHARK}>,${TSHARK},>")
    add_test(NAME dissection-tshark COMMAND resend-dissect-tests)
    set_tests_properties(dissection-tshark PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED")
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
    add_library(resend_ui STATIC src/app_controller.cpp src/app_inspector.cpp src/app_documents.cpp src/app_look.cpp src/app_commands.cpp src/app_lab.cpp src/app_sound.cpp src/app_terminal.cpp)
    target_link_libraries(resend_ui PUBLIC resend_app resend_audio Slint::Slint)
    target_compile_options(resend_ui PRIVATE ${resend_warnings})
    # Fonts and images go into the program, so it looks the same on a
    # machine with none of the fonts installed.
    set_target_properties(resend_ui PROPERTIES SLINT_EMBED_RESOURCES embed-files)
    slint_target_sources(resend_ui ui/app.slint COMPILATION_UNITS ${RESEND_SLINT_COMPILATION_UNITS})
    target_compile_definitions(resend_ui PRIVATE RESEND_VERSION="${PROJECT_VERSION}")

    add_executable(resend src/main.cpp)
    target_link_libraries(resend PRIVATE resend_ui)
    target_compile_definitions(resend PRIVATE RESEND_FONT_DIR="${PROJECT_SOURCE_DIR}/assets/fonts")
    if(WIN32)
        # A windowed program, whose entry point stays main(), with its icon
        # and version information.
        set(resend_icon_file ${resend_icon_dir}/resend.ico)
        configure_file(packaging/windows/resend.rc.in ${CMAKE_CURRENT_BINARY_DIR}/resend.rc @ONLY)
        target_sources(resend PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/resend.rc)
        set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/resend.rc PROPERTIES OBJECT_DEPENDS ${resend_icon_file})
        add_dependencies(resend resend-icon-files)
        set_target_properties(resend PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(resend PRIVATE /ENTRY:mainCRTStartup)
        endif()
        # Slint's DLL beside the programs, so that they run from the build.
        add_custom_command(TARGET resend POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:resend> $<TARGET_FILE_DIR:resend>
            COMMAND_EXPAND_LISTS)
    else()
        # Installed, the program finds the Slint library in ../lib.
        set_target_properties(resend PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
    endif()

    install(TARGETS resend RUNTIME DESTINATION bin)
    # The Slint library goes with the program: into lib/ on Linux, beside the
    # program on Windows. An SDK provides it as an imported target.
    if(TARGET slint_cpp-shared)
        install(IMPORTED_RUNTIME_ARTIFACTS slint_cpp-shared LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
        # cmake --install --strip (and cpack) strip only what was built here.
        if(UNIX AND CMAKE_STRIP)
            install(CODE "if(CMAKE_INSTALL_DO_STRIP)
                execute_process(COMMAND \"${CMAKE_STRIP}\" --strip-unneeded \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/lib/libslint_cpp.so\")
            endif()")
        endif()
    endif()
    # Slint's licence, and those of the code compiled into its library.
    get_filename_component(resend_slint_licenses "${Slint_DIR}/../../../licenses" ABSOLUTE)
    if(Slint_DIR AND EXISTS "${resend_slint_licenses}/LICENSE.md")
        install(DIRECTORY ${resend_slint_licenses}/ DESTINATION share/doc/resend/slint)
    elseif(DEFINED slint_SOURCE_DIR)
        install(FILES ${slint_SOURCE_DIR}/LICENSE.md DESTINATION share/doc/resend/slint)
        install(DIRECTORY ${slint_SOURCE_DIR}/LICENSES/ DESTINATION share/doc/resend/slint/LICENSES)
    endif()
    # The fonts are built into the program; installed as well, with their
    # licences, so that fontconfig always knows one font (src/main.cpp).
    install(DIRECTORY assets/fonts/ DESTINATION share/resend/fonts)
    if(UNIX AND NOT APPLE)
        # How the desktop shows resend: its launcher, its icon at every size
        # and its AppStream metadata.
        install(FILES packaging/linux/io.github._0xdokkan.resend.desktop DESTINATION share/applications)
        install(FILES packaging/linux/io.github._0xdokkan.resend.metainfo.xml DESTINATION share/metainfo)
        install(DIRECTORY ${resend_icon_dir}/hicolor DESTINATION share/icons)
    endif()

    if(BUILD_TESTING)
        add_executable(resend-gui-tests tests/gui_tests.cpp)
        target_link_libraries(resend-gui-tests PRIVATE resend_ui)
        # slint::testing is still marked experimental in Slint 1.18.
        target_compile_definitions(resend-gui-tests PRIVATE SLINT_FEATURE_EXPERIMENTAL
            RESEND_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples" RESEND_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
        add_test(NAME gui-behavior COMMAND resend-gui-tests)
        if(WIN32)
            add_dependencies(resend-gui-tests resend)  # which copies Slint's DLL
        endif()
    endif()

    if(RESEND_BUILD_SPIKES)
        add_subdirectory(spikes/canvas)
    endif()
endif()

install(TARGETS resend-cli RUNTIME DESTINATION bin)
install(FILES LICENSE README.md THIRD-PARTY-NOTICES.md DESTINATION share/doc/resend)
if(WIN32)
    # The Visual C++ runtime the programs and Slint's DLL need, which a
    # clean Windows does not have.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    include(InstallRequiredSystemLibraries)
endif()

# Packages: cpack in the build directory writes a tar.gz on Linux, a zip and
# an installer on Windows. The AppImage is made by scripts/make-appimage.sh.
set(CPACK_PACKAGE_NAME resend)
set(CPACK_PACKAGE_VENDOR resend)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A desktop networking lab: build networks and see why a packet arrives or stops")
set(CPACK_PACKAGE_HOMEPAGE_URL https://github.com/0xdokkan/resend)
set(CPACK_PACKAGE_INSTALL_DIRECTORY resend)
set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/LICENSE)
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_SOURCE_GENERATOR "")
if(WIN32)
    set(CPACK_GENERATOR ZIP NSIS)
    set(CPACK_PACKAGE_FILE_NAME resend-${PROJECT_VERSION}-windows-x64)
    set(CPACK_PACKAGE_EXECUTABLES resend resend)
    set(CPACK_CREATE_DESKTOP_LINKS resend)
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_DISPLAY_NAME resend)
    set(CPACK_NSIS_PACKAGE_NAME resend)
    set(CPACK_NSIS_MUI_ICON ${resend_icon_dir}/resend.ico)
    set(CPACK_NSIS_MUI_UNIICON ${resend_icon_dir}/resend.ico)
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\resend.exe")
    set(CPACK_NSIS_URL_INFO_ABOUT https://github.com/0xdokkan/resend)
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)
    set(CPACK_NSIS_BRANDING_TEXT "resend ${PROJECT_VERSION}")
else()
    set(CPACK_GENERATOR TGZ)
    set(CPACK_PACKAGE_FILE_NAME resend-${PROJECT_VERSION}-linux-x86_64)
endif()
include(CPack)
