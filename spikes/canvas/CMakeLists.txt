# UI spike: canvas interaction and rendering cost in Slint (see docs/decisions/0002-ui-toolkit.md).
add_library(canvas_spike_core STATIC canvas_controller.cpp)
target_include_directories(canvas_spike_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(canvas_spike_core PRIVATE ${resend_warnings})

add_library(canvas_spike_ui STATIC spike_app.cpp)
target_link_libraries(canvas_spike_ui PUBLIC canvas_spike_core Slint::Slint)
target_compile_options(canvas_spike_ui PRIVATE ${resend_warnings})
slint_target_sources(canvas_spike_ui spike.slint)

add_executable(canvas-spike main.cpp)
target_link_libraries(canvas-spike PRIVATE canvas_spike_ui)

if(BUILD_TESTING)
    add_executable(canvas-spike-tests spike_tests.cpp)
    target_link_libraries(canvas-spike-tests PRIVATE canvas_spike_ui)
    target_include_directories(canvas-spike-tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_compile_definitions(canvas-spike-tests PRIVATE SLINT_FEATURE_EXPERIMENTAL)
    add_test(NAME canvas-spike COMMAND canvas-spike-tests)
endif()
