# Bundled LVGL 8.4.0 core and benchmark (see PORTING.md).
file(GLOB_RECURSE SOURCES "src/*.c")
if(CONFIG_LV_USE_DEMO_BENCHMARK)
    file(GLOB_RECURSE BENCHMARK_SOURCES "demos/benchmark/*.c")
    list(APPEND SOURCES ${BENCHMARK_SOURCES})
endif()
foreach(option LV_BUILD_EXAMPLES LV_USE_DEMO_WIDGETS LV_USE_DEMO_STRESS
               LV_USE_DEMO_KEYPAD_AND_ENCODER LV_USE_DEMO_MUSIC)
    if(CONFIG_${option})
        message(FATAL_ERROR "${option}: this local LVGL component contains only core and benchmark; see PORTING.md")
    endif()
endforeach()
idf_component_register(SRCS ${SOURCES} INCLUDE_DIRS "." "src" "demos"
                       REQUIRES esp_timer)
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_CONF_INCLUDE_SIMPLE)
if(CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC "LV_ATTRIBUTE_FAST_MEM=IRAM_ATTR")
endif()
