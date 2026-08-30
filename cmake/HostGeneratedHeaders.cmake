# A public header written by a host tool the build compiles first.
#
# Three headers from two tools, and nothing modelled or reported them:
#
#   arch/i386-all/include/mmakefile.src:24  aros/i386/libcall.h
#   arch/m68k-all/include/mmakefile.src:17  aros/m68k/asmcall.h, libcall.h
#
# Only i386 and m68k need one, because only their `aros/cpu.h` sets
# `__AROS_LIBCALL_H_FILE` (arch/i386-all/include/aros/cpu.h:148). An x86_64
# build never asks for it, which is why its absence stayed invisible until the
# 32-bit PC bootstrap was compiled and every one of its sources failed on
# `'aros/i386/libcall.h' file not found`.
#
# The tool runs on the host, so it is compiled with the host compiler, not the
# cross one. The header is staged into the same two roots the reference uses:
# $(AROS_INCLUDES) and $(GENINCDIR).

include_guard(GLOBAL)
include(CMakeParseArguments)

# aros_host_generated_header(TOOL <name> SOURCE <file> HEADER <relative>
#                            [ARGUMENTS <args...>])
function(aros_host_generated_header)
    set(oneValueArgs TOOL SOURCE HEADER)
    set(multiValueArgs ARGUMENTS)
    cmake_parse_arguments(HG "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    foreach(_required TOOL SOURCE HEADER)
        if(NOT HG_${_required})
            message(FATAL_ERROR
                "aros_host_generated_header: ${_required} is required")
        endif()
    endforeach()
    if(NOT EXISTS "${HG_SOURCE}")
        set_property(GLOBAL APPEND PROPERTY AROS_HOST_HEADER_GAPS
            "${HG_TOOL}: source ${HG_SOURCE} is missing")
        return()
    endif()

    set(_tool "${CMAKE_BINARY_DIR}/hosttools/${HG_TOOL}")
    if(NOT TARGET "aros-host-tool-${HG_TOOL}")
        # The host compiler, deliberately: this runs during the build, not on
        # the target. CMAKE_C_COMPILER here is the cross compiler.
        if(NOT AROS_HOST_C_COMPILER)
            find_program(AROS_HOST_C_COMPILER NAMES cc clang gcc)
        endif()
        if(NOT AROS_HOST_C_COMPILER)
            set_property(GLOBAL APPEND PROPERTY AROS_HOST_HEADER_GAPS
                "${HG_TOOL}: no host C compiler found")
            return()
        endif()
        add_custom_command(
            OUTPUT "${_tool}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${CMAKE_BINARY_DIR}/hosttools"
            COMMAND "${AROS_HOST_C_COMPILER}" -Wall -Werror
                -o "${_tool}" "${HG_SOURCE}"
            DEPENDS "${HG_SOURCE}"
            COMMENT "Building host tool ${HG_TOOL}"
            VERBATIM)
        add_custom_target("aros-host-tool-${HG_TOOL}" DEPENDS "${_tool}")
    endif()

    # Both include roots, as the reference stages them.
    set(_outputs "")
    foreach(_root "${AROS_SDK_INCLUDE_DIR}" "${AROS_GENINC_DIR}")
        list(APPEND _outputs "${_root}/${HG_HEADER}")
    endforeach()
    list(GET _outputs 0 _primary)
    get_filename_component(_primary_dir "${_primary}" DIRECTORY)
    set(_copies "")
    foreach(_output IN LISTS _outputs)
        if(_output STREQUAL _primary)
            continue()
        endif()
        get_filename_component(_dir "${_output}" DIRECTORY)
        list(APPEND _copies
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_primary}" "${_output}")
    endforeach()

    string(MAKE_C_IDENTIFIER "${HG_HEADER}" _stamp_id)
    add_custom_command(
        OUTPUT ${_outputs}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_primary_dir}"
        # The reference redirects the tool's standard output to the header.
        COMMAND "${CMAKE_COMMAND}" -E env
            "${_tool}" ${HG_ARGUMENTS} > "${_primary}"
        ${_copies}
        DEPENDS "${_tool}"
        COMMENT "Generating ${HG_HEADER} with ${HG_TOOL}"
        VERBATIM)
    add_custom_target("aros-host-header-${_stamp_id}" ALL DEPENDS ${_outputs})
    set_property(GLOBAL APPEND PROPERTY AROS_HOST_HEADER_TARGETS
        "aros-host-header-${_stamp_id}")
    # Everything that compiles may include it, so make the SDK header lane wait.
    if(TARGET "includes-generate")
        add_dependencies("includes-generate" "aros-host-header-${_stamp_id}")
    endif()
endfunction()

# aros_report_host_header_gaps()
function(aros_report_host_header_gaps)
    get_property(_gaps GLOBAL PROPERTY AROS_HOST_HEADER_GAPS)
    set(_report "${CMAKE_BINARY_DIR}/generated_targets.host-header-gaps.txt")
    if(NOT _gaps)
        file(REMOVE "${_report}")
        return()
    endif()
    list(REMOVE_DUPLICATES _gaps)
    list(SORT _gaps)
    string(REPLACE ";" "\n" _body "${_gaps}")
    file(WRITE "${_report}" "${_body}\n")
    list(LENGTH _gaps _count)
    message(STATUS
        "⚠️  ${_count} host-tool header(s) could not be generated -> ${_report}")
endfunction()
