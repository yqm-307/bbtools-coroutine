# Optional, target-local GCC metadata collection. Including this file alone has no effect.
include(CMakeParseArguments)
set(_BBT_STACK_BUDGET_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/stack_budget.py")

function(bbt_stack_budget_collect target)
    if (NOT TARGET "${target}")
        message(FATAL_ERROR "stack budget: unknown target ${target}")
    endif()
    get_target_property(_type "${target}" TYPE)
    get_target_property(_sources "${target}" SOURCES)
    list(LENGTH _sources _count)
    if (NOT _type STREQUAL "OBJECT_LIBRARY" OR NOT _count EQUAL 1)
        message(FATAL_ERROR "stack budget: use one-source OBJECT libraries for explicit object identity")
    endif()
    if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
            OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
            OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
            OR NOT CMAKE_CXX_COMPILER_VERSION MATCHES "^15\\.2\\.")
        message(FATAL_ERROR "stack budget: UNSUPPORTED (requires Linux x86_64 GCC 15.2 non-LTO)")
    endif()
    get_target_property(_ipo "${target}" INTERPROCEDURAL_OPTIMIZATION)
    if (_ipo)
        message(FATAL_ERROR "stack budget: UNSUPPORTED LTO; do not silently disable it")
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    target_compile_options("${target}" PRIVATE -fstack-usage -fcallgraph-info=su)
    set(_launcher "${Python3_EXECUTABLE};${_BBT_STACK_BUDGET_SCRIPT};record")
    if (CMAKE_CXX_COMPILER_LAUNCHER)
        list(LENGTH CMAKE_CXX_COMPILER_LAUNCHER _launcher_count)
        if (NOT _launcher_count EQUAL 1)
            message(FATAL_ERROR "stack budget: only one explicit compiler launcher executable supported")
        endif()
        list(APPEND _launcher --launcher "${CMAKE_CXX_COMPILER_LAUNCHER}")
    endif()
    list(APPEND _launcher --)
    set_property(TARGET "${target}" PROPERTY CXX_COMPILER_LAUNCHER "${_launcher}")
endfunction()

function(bbt_stack_budget_analysis name)
    cmake_parse_arguments(B "" "MANIFEST;TEMPLATE;ASSERTION;PROBE;MAP;REPORT;HEADER" "DEPENDS" ${ARGN})
    foreach(_required MANIFEST TEMPLATE ASSERTION PROBE MAP REPORT HEADER)
        if (NOT B_${_required})
            message(FATAL_ERROR "stack budget analysis: missing ${_required}")
        endif()
    endforeach()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(
        OUTPUT "${B_REPORT}" "${B_HEADER}"
        BYPRODUCTS "${B_ASSERTION}"
        COMMAND "${Python3_EXECUTABLE}" "${_BBT_STACK_BUDGET_SCRIPT}" bind-assertion
            --template "${B_TEMPLATE}" --manifest "${B_MANIFEST}" --output "${B_ASSERTION}"
        COMMAND "${Python3_EXECUTABLE}" "${_BBT_STACK_BUDGET_SCRIPT}" analyze
            --manifest "${B_MANIFEST}" --elf "$<TARGET_FILE:${B_PROBE}>"
            --map "${B_MAP}" --link-directory "${CMAKE_BINARY_DIR}"
            --output "${B_REPORT}" --header "${B_HEADER}"
        DEPENDS "${B_PROBE}" "${B_MANIFEST}" "${B_TEMPLATE}" "${_BBT_STACK_BUDGET_SCRIPT}" ${B_DEPENDS}
        VERBATIM)
    add_custom_target("${name}" DEPENDS "${B_REPORT}" "${B_HEADER}")
endfunction()

function(bbt_stack_budget_consumption name)
    cmake_parse_arguments(B "" "TARGET;MANIFEST;REPORT;HEADER;MAP;OUTPUT;GLUE" "" ${ARGN})
    foreach(_required TARGET MANIFEST REPORT HEADER MAP OUTPUT GLUE)
        if (NOT B_${_required})
            message(FATAL_ERROR "stack budget consumption: missing ${_required}")
        endif()
    endforeach()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_check "${Python3_EXECUTABLE}" "${_BBT_STACK_BUDGET_SCRIPT}" consume
        --report "${B_REPORT}" --manifest "${B_MANIFEST}" --header "${B_HEADER}"
        --elf "$<TARGET_FILE:${B_TARGET}>" --map "${B_MAP}"
        --link-directory "${CMAKE_BINARY_DIR}"
        --glue-object "$<TARGET_OBJECTS:${B_GLUE}>" --output "${B_OUTPUT}")
    add_custom_command(TARGET "${B_TARGET}" POST_BUILD COMMAND ${_check} VERBATIM)
    # Always revalidate live dependencies, even if the executable needs no relink.
    # No dependency on the analysis target: consumption never regenerates F.
    add_custom_target("${name}" COMMAND ${_check} DEPENDS "${B_TARGET}" VERBATIM)
endfunction()
