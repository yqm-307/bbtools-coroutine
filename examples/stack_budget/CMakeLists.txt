cmake_minimum_required(VERSION 3.16)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(stack_budget_example LANGUAGES CXX)
endif()

set(STACK_BUDGET_STAGE "ANALYZE" CACHE STRING "ANALYZE or CONSUME (separate build directories)")
set_property(CACHE STACK_BUDGET_STAGE PROPERTY STRINGS ANALYZE CONSUME)
set(STACK_BUDGET_ANALYSIS_DIR "" CACHE PATH "Existing ANALYZE build directory for CONSUME")
set(STACK_BUDGET_USABLE_BYTES "65536" CACHE STRING "Explicit current usable capacity U, not a default")
set(STACK_BUDGET_MARGIN_BYTES "4096" CACHE STRING "Explicit model margin")
set(STACK_BUDGET_PAGE_SIZE "4096" CACHE STRING "Explicit target page size (verify on deployment)")
set(BBT_COROUTINE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "bbtools-coroutine source headers")
get_filename_component(BBT_COROUTINE_SOURCE_DIR "${BBT_COROUTINE_SOURCE_DIR}" ABSOLUTE)
include("${BBT_COROUTINE_SOURCE_DIR}/scripts/stack_budget.cmake")
find_package(Boost 1.90 CONFIG REQUIRED COMPONENTS context)
find_package(Threads REQUIRED)

if (BBT_COROUTINE_STACK_DIAGNOSTICS)
    message(FATAL_ERROR "Example summary applies to diagnostic OFF only")
endif()
if (TARGET bbt_coroutine)
    set(_runtime bbt_coroutine)
    set(STACK_BUDGET_RUNTIME_FILE "$<TARGET_FILE:bbt_coroutine>")
else()
    set(BBT_COROUTINE_LIBRARY "" CACHE FILEPATH "Existing diagnostic-OFF libbbt_coroutine.so")
    if (NOT EXISTS "${BBT_COROUTINE_LIBRARY}")
        message(FATAL_ERROR "Set BBT_COROUTINE_LIBRARY to an existing diagnostic-OFF build")
    endif()
    execute_process(COMMAND nm -D "${BBT_COROUTINE_LIBRARY}"
        OUTPUT_VARIABLE _runtime_symbols RESULT_VARIABLE _nm_status)
    if (NOT _nm_status EQUAL 0 OR _runtime_symbols MATCHES "__cyg_profile_func_enter")
        message(FATAL_ERROR "Example assertion requires an unstripped diagnostic-OFF runtime")
    endif()
    add_library(stack_budget_runtime SHARED IMPORTED)
    set_target_properties(stack_budget_runtime PROPERTIES
        IMPORTED_LOCATION "${BBT_COROUTINE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${BBT_COROUTINE_SOURCE_DIR};${Boost_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "Boost::context;Threads::Threads;${CMAKE_DL_LIBS}")
    set(_runtime stack_budget_runtime)
    set(STACK_BUDGET_RUNTIME_FILE "${BBT_COROUTINE_LIBRARY}")
endif()

if (STACK_BUDGET_STAGE STREQUAL "ANALYZE")
    foreach(_pair "STDCXX;libstdc++.so.6" "GCC;libgcc_s.so.1" "LIBC;libc.so.6")
        list(GET _pair 0 _key)
        list(GET _pair 1 _file)
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" "-print-file-name=${_file}"
            OUTPUT_VARIABLE STACK_BUDGET_${_key} OUTPUT_STRIP_TRAILING_WHITESPACE)
        if (NOT EXISTS "${STACK_BUDGET_${_key}}")
            message(FATAL_ERROR "Cannot identify exact runtime dependency ${_file}")
        endif()
    endforeach()
    execute_process(COMMAND getconf GNU_LIBC_VERSION OUTPUT_VARIABLE STACK_BUDGET_GLIBC
        OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _glibc_status)
    if (NOT _glibc_status EQUAL 0)
        message(FATAL_ERROR "Cannot bind the example glibc version")
    endif()
    set(STACK_BUDGET_LOADER "/lib64/ld-linux-x86-64.so.2")
    if (NOT EXISTS "${STACK_BUDGET_LOADER}")
        message(FATAL_ERROR "UNSUPPORTED: missing validated x86_64 glibc loader")
    endif()
    add_library(stack_budget_entry OBJECT entry.cc)
    target_compile_features(stack_budget_entry PRIVATE cxx_std_17)
    set_target_properties(stack_budget_entry PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)
    target_compile_options(stack_budget_entry PRIVATE -O2 -Wall -Wextra)
    target_link_libraries(stack_budget_entry PRIVATE "${_runtime}")
    target_include_directories(stack_budget_entry PRIVATE "${BBT_COROUTINE_SOURCE_DIR}")
    bbt_stack_budget_collect(stack_budget_entry)
    add_library(stack_budget_registration OBJECT registration.cc)
    target_compile_features(stack_budget_registration PRIVATE cxx_std_17)
    set_target_properties(stack_budget_registration PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)
    target_compile_options(stack_budget_registration PRIVATE -O2 -Wall -Wextra)
    target_link_libraries(stack_budget_registration PRIVATE "${_runtime}")
    target_include_directories(stack_budget_registration PRIVATE "${BBT_COROUTINE_SOURCE_DIR}")
    bbt_stack_budget_collect(stack_budget_registration)
    add_executable(stack_budget_probe probe.cc
        $<TARGET_OBJECTS:stack_budget_entry> $<TARGET_OBJECTS:stack_budget_registration>)
    target_link_libraries(stack_budget_probe PRIVATE "${_runtime}")
    set(_map "${CMAKE_CURRENT_BINARY_DIR}/probe.map")
    target_link_options(stack_budget_probe PRIVATE "-Wl,-Map=${_map}")
    configure_file(assertion.json.in "${CMAKE_CURRENT_BINARY_DIR}/assertion-configured.json" @ONLY)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/assertion-template.json"
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/assertion-configured.json")
    set(_header "${CMAKE_CURRENT_BINARY_DIR}/stack_budget_generated.hpp")
    set(_assertion "${CMAKE_CURRENT_BINARY_DIR}/assertion-bound.json")
    set(_report "${CMAKE_CURRENT_BINARY_DIR}/stack_budget_report.json")
    set(_manifest "${CMAKE_CURRENT_BINARY_DIR}/manifest.json")
    file(GENERATE OUTPUT "${_manifest}" CONTENT
"{
  \"schema_version\": 1, \"target\": \"stack_budget_example\",
  \"objects\": [
    {\"id\": \"entry\", \"path\": \"$<TARGET_OBJECTS:stack_budget_entry>\"},
    {\"id\": \"registration\", \"path\": \"$<TARGET_OBJECTS:stack_budget_registration>\"}
  ],
  \"entries\": [{\"name\": \"ExampleEntry\", \"object\": \"entry\", \"symbol\": \"StackBudgetEntry\", \"usable_bytes\": ${STACK_BUDGET_USABLE_BYTES}}],
  \"page_size\": ${STACK_BUDGET_PAGE_SIZE}, \"margin_bytes\": ${STACK_BUDGET_MARGIN_BYTES},
  \"assertion\": \"${_assertion}\", \"generated_header\": \"${_header}\"
}
")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/entry-object.txt"
        CONTENT "$<TARGET_OBJECTS:stack_budget_entry>\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/registration-object.txt"
        CONTENT "$<TARGET_OBJECTS:stack_budget_registration>\n")
    bbt_stack_budget_analysis(stack_budget_analysis
        MANIFEST "${_manifest}" TEMPLATE "${CMAKE_CURRENT_BINARY_DIR}/assertion-template.json"
        ASSERTION "${_assertion}" PROBE stack_budget_probe MAP "${_map}"
        REPORT "${_report}" HEADER "${_header}" DEPENDS "${_runtime}")
elseif (STACK_BUDGET_STAGE STREQUAL "CONSUME")
    if (NOT EXISTS "${STACK_BUDGET_ANALYSIS_DIR}/entry-object.txt")
        message(FATAL_ERROR "Run the explicit ANALYZE stage first")
    endif()
    file(READ "${STACK_BUDGET_ANALYSIS_DIR}/entry-object.txt" _entry_object)
    file(READ "${STACK_BUDGET_ANALYSIS_DIR}/registration-object.txt" _registration_object)
    string(STRIP "${_entry_object}" _entry_object)
    string(STRIP "${_registration_object}" _registration_object)
    if (NOT EXISTS "${_entry_object}" OR NOT EXISTS "${_registration_object}")
        message(FATAL_ERROR "Missing original analysis object; explicitly reanalyze")
    endif()
    # Neither entry nor std::function wrapper is recompiled in this stage.
    set_source_files_properties("${_entry_object}" "${_registration_object}"
        PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_library(stack_budget_glue OBJECT glue.cc)
    target_compile_features(stack_budget_glue PRIVATE cxx_std_17)
    target_compile_options(stack_budget_glue PRIVATE -Wall -Wextra)
    target_include_directories(stack_budget_glue PRIVATE "${STACK_BUDGET_ANALYSIS_DIR}")
    target_link_libraries(stack_budget_glue PRIVATE "${_runtime}")
    bbt_stack_budget_collect(stack_budget_glue)
    add_executable(stack_budget_consumer $<TARGET_OBJECTS:stack_budget_glue>
        "${_entry_object}" "${_registration_object}")
    target_link_libraries(stack_budget_consumer PRIVATE "${_runtime}")
    set(_map "${CMAKE_CURRENT_BINARY_DIR}/consumer.map")
    target_link_options(stack_budget_consumer PRIVATE "-Wl,-Map=${_map}")
    bbt_stack_budget_consumption(stack_budget_consume TARGET stack_budget_consumer
        MANIFEST "${STACK_BUDGET_ANALYSIS_DIR}/manifest.json"
        REPORT "${STACK_BUDGET_ANALYSIS_DIR}/stack_budget_report.json"
        HEADER "${STACK_BUDGET_ANALYSIS_DIR}/stack_budget_generated.hpp"
        MAP "${_map}" GLUE stack_budget_glue
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/consumption.json")
else()
    message(FATAL_ERROR "STACK_BUDGET_STAGE must be ANALYZE or CONSUME")
endif()
