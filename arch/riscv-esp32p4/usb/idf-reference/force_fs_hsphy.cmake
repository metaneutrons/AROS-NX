# Build-only comparison against the pinned TinyUSB DWC2 source. The managed
# dependency remains untouched; only the component target uses this generated
# copy when the opt-in comparison is enabled.
function(hs_reference_force_fs_hsphy target source output)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: target '${target}' does not exist")
    endif()

    get_target_property(_target_source_dir "${target}" SOURCE_DIR)
    if(NOT _target_source_dir)
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: cannot determine SOURCE_DIR for '${target}'")
    endif()
    file(REAL_PATH "${_target_source_dir}" _target_source_dir)

    if(IS_ABSOLUTE "${source}")
        set(_source_candidate "${source}")
    else()
        set(_source_candidate "${_target_source_dir}/${source}")
    endif()
    cmake_path(NORMAL_PATH _source_candidate OUTPUT_VARIABLE _source_candidate)
    if(NOT EXISTS "${_source_candidate}" OR IS_DIRECTORY "${_source_candidate}")
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: source '${_source_candidate}' does not exist")
    endif()
    file(REAL_PATH "${_source_candidate}" _source)
    get_filename_component(_source_dir "${_source}" DIRECTORY)
    file(REAL_PATH "${CMAKE_BINARY_DIR}" _binary_dir_real)
    cmake_path(IS_PREFIX _source_dir "${_binary_dir_real}" NORMALIZE
        _binary_dir_in_dependency_dir)
    if(_binary_dir_in_dependency_dir)
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: binary directory resolves inside the original dependency directory '${_source_dir}'")
    endif()

    if(NOT IS_ABSOLUTE "${output}")
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: output must be an absolute path: '${output}'")
    endif()
    cmake_path(NORMAL_PATH output OUTPUT_VARIABLE _output)
    cmake_path(NORMAL_PATH CMAKE_BINARY_DIR OUTPUT_VARIABLE _binary_dir)
    if(_output STREQUAL _binary_dir)
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: output must name a file under the binary directory")
    endif()
    cmake_path(IS_PREFIX _binary_dir "${_output}" NORMALIZE _output_in_binary_dir)
    if(NOT _output_in_binary_dir)
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: output '${_output}' must be inside '${_binary_dir}'")
    endif()

    cmake_path(IS_PREFIX _source_dir "${_output}" NORMALIZE _output_in_dependency_dir)
    if(_output_in_dependency_dir)
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: output must not be inside the original dependency directory '${_source_dir}'")
    endif()
    if(_output STREQUAL _source)
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: output must be distinct from the original source")
    endif()
    if(IS_SYMLINK "${_output}")
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: output must not be a symbolic link: '${_output}'")
    endif()
    if(EXISTS "${_output}")
        file(REAL_PATH "${_output}" _existing_output)
        file(RELATIVE_PATH _output_relative "${_binary_dir}" "${_output}")
        set(_expected_output "${_binary_dir_real}/${_output_relative}")
        cmake_path(NORMAL_PATH _expected_output OUTPUT_VARIABLE _expected_output)
        if(NOT _existing_output STREQUAL _expected_output)
            message(FATAL_ERROR
                "hs_reference_force_fs_hsphy: existing output resolves outside its path: '${_output}'")
        endif()
    endif()

    get_target_property(_target_sources "${target}" SOURCES)
    if(NOT _target_sources OR _target_sources MATCHES "-NOTFOUND$")
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: target '${target}' has no SOURCES")
    endif()

    set(_updated_sources)
    set(_source_match_count 0)
    foreach(_target_source IN LISTS _target_sources)
        # Generator expressions cannot identify this concrete dependency file.
        if(_target_source MATCHES "\\$<")
            list(APPEND _updated_sources "${_target_source}")
            continue()
        endif()
        if(IS_ABSOLUTE "${_target_source}")
            set(_candidate_source "${_target_source}")
        else()
            set(_candidate_source "${_target_source_dir}/${_target_source}")
        endif()
        cmake_path(NORMAL_PATH _candidate_source OUTPUT_VARIABLE _candidate_source)
        if(EXISTS "${_candidate_source}")
            file(REAL_PATH "${_candidate_source}" _candidate_source)
        endif()
        if(_candidate_source STREQUAL _source)
            math(EXPR _source_match_count "${_source_match_count} + 1")
            list(APPEND _updated_sources "${_output}")
        else()
            list(APPEND _updated_sources "${_target_source}")
        endif()
    endforeach()
    if(NOT _source_match_count EQUAL 1)
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: expected exactly one '${_source}' source in target '${target}', found ${_source_match_count}")
    endif()

    set(_expected_sha256 "b02378ee5b90f4eb00fee6289ab7531432e7e41b91e618c0319890b03b9fb25f")
    file(SHA256 "${_source}" _actual_sha256)
    if(NOT _actual_sha256 STREQUAL _expected_sha256)
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: SHA-256 mismatch for '${_source}': expected ${_expected_sha256}, got ${_actual_sha256}")
    endif()

    file(READ "${_source}" _original_contents)
    set(_original_statement "dcfg |= DCFG_DSPD_HS << DCFG_DSPD_Pos;")
    set(_replacement_statement "dcfg |= DCFG_DSPD_FS_HSPHY << DCFG_DSPD_Pos;")
    string(FIND "${_original_contents}" "${_original_statement}" _statement_position)
    if(_statement_position EQUAL -1)
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: expected DCFG statement is missing from '${_source}'")
    endif()
    string(LENGTH "${_original_statement}" _statement_length)
    math(EXPR _after_statement "${_statement_position} + ${_statement_length}")
    string(SUBSTRING "${_original_contents}" ${_after_statement} -1 _remaining_contents)
    string(FIND "${_remaining_contents}" "${_original_statement}" _second_statement_position)
    if(NOT _second_statement_position EQUAL -1)
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: expected one DCFG statement in '${_source}', found multiple")
    endif()

    if(ARGC GREATER 3)
        if(NOT ARGV3 STREQUAL "RX_TRACE")
            message(FATAL_ERROR "Unknown DCD diagnostic mode")
        endif()
        set(_patched_contents "#include \"rx_trace.h\"\n${_original_contents}")
        # Exact hash above pins each insertion. Reuse values already read by
        # the DCD: no extra MMIO/FIFO read or acknowledgement is introduced.
        foreach(_site IN ITEMS
            "const uint32_t gintsts = dwc2->gintsts & gintmask;"
            "const dwc2_grxstsp_t grxstsp = {.value = dwc2->grxstsp};"
            "static void handle_epout_slave(uint8_t rhport, uint8_t epnum, dwc2_doepint_t doepint_bm) {\n  if (doepint_bm.setup_phase_done) {")
            if(_site MATCHES "gintsts =")
                set(_hook "hs_rx_record(&hs_rx_state, rhport, HS_RX_IRQ, gintsts);")
            elseif(_site MATCHES "grxstsp =")
                set(_hook "hs_rx_record(&hs_rx_state, rhport, HS_RX_POP, grxstsp.value);")
            else()
                set(_hook "hs_rx_record(&hs_rx_state, rhport, HS_RX_EP_SETUP, epnum);")
            endif()
            string(FIND "${_patched_contents}" "${_site}" _site_position)
            if(_site_position EQUAL -1)
                message(FATAL_ERROR "RX diagnostic site is missing")
            endif()
            string(LENGTH "${_site}" _site_length)
            math(EXPR _site_end "${_site_position} + ${_site_length}")
            string(SUBSTRING "${_patched_contents}" ${_site_end} -1 _site_tail)
            string(FIND "${_site_tail}" "${_site}" _site_duplicate)
            if(NOT _site_duplicate EQUAL -1)
                message(FATAL_ERROR "RX diagnostic site is ambiguous")
            endif()
            string(REPLACE "${_site}" "${_site}\n  ${_hook}" _patched_contents "${_patched_contents}")
        endforeach()
        if(HS_REFERENCE_BVALID)
            # Diagnostic-only backport of upstream bus-powered operation.
            # Keep this independent of the upstream FS USB_WRAP guard change.
            set(_session_old "dwc2->gotgctl &= ~(GOTGCTL_BVALOEN | GOTGCTL_BVALOVAL | GOTGCTL_VBVALOVAL);")
            set(_session_new "dwc2->gotgctl = (dwc2->gotgctl & ~GOTGCTL_AVALOEN) | GOTGCTL_BVALOEN | GOTGCTL_BVALOVAL;")
            string(FIND "${_patched_contents}" "${_session_old}" _session_pos)
            if(_session_pos EQUAL -1)
                message(FATAL_ERROR "Pinned B-session initialization is missing")
            endif()
            string(REPLACE "${_session_old}" "${_session_new}" _patched_contents "${_patched_contents}")
        endif()
    else()
        string(REPLACE "${_original_statement}" "${_replacement_statement}"
            _patched_contents "${_original_contents}")
    endif()
    get_filename_component(_output_dir "${_output}" DIRECTORY)
    # Reject symlink components before creating directories. A lexical path
    # under CMAKE_BINARY_DIR can otherwise escape through a parent symlink.
    file(RELATIVE_PATH _output_dir_relative "${_binary_dir}" "${_output_dir}")
    set(_output_path_component "${_binary_dir}")
    string(REPLACE "/" ";" _output_dir_components "${_output_dir_relative}")
    foreach(_output_dir_component IN LISTS _output_dir_components)
        if(_output_dir_component STREQUAL "" OR _output_dir_component STREQUAL ".")
            continue()
        endif()
        set(_output_path_component "${_output_path_component}/${_output_dir_component}")
        if(IS_SYMLINK "${_output_path_component}")
            message(FATAL_ERROR
                "hs_reference_force_fs_hsphy: output parent contains a symbolic-link component: '${_output_path_component}'")
        endif()
    endforeach()
    file(MAKE_DIRECTORY "${_output_dir}")
    file(REAL_PATH "${_output_dir}" _output_dir_real)
    cmake_path(IS_PREFIX _binary_dir_real "${_output_dir_real}" NORMALIZE
        _output_dir_in_binary_dir)
    if(NOT _output_dir_in_binary_dir)
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: resolved output directory '${_output_dir_real}' must be inside '${_binary_dir_real}'")
    endif()
    cmake_path(IS_PREFIX _source_dir "${_output_dir_real}" NORMALIZE
        _output_dir_in_dependency_dir)
    if(_output_dir_in_dependency_dir)
        message(FATAL_ERROR
            "hs_reference_force_fs_hsphy: resolved output directory must not be inside the original dependency directory '${_source_dir}'")
    endif()
    # Never truncate the destination inode: an existing hard link could share
    # the original dependency's inode even when REAL_PATH differs. Replace the
    # directory entry from a new file under the verified output directory.
    string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef _temporary_suffix)
    set(_temporary_output "${_output_dir}/.dcd-${_temporary_suffix}.tmp")
    if(EXISTS "${_temporary_output}" OR IS_SYMLINK "${_temporary_output}")
        message(FATAL_ERROR "hs_reference_force_fs_hsphy: temporary output collision")
    endif()
    file(WRITE "${_temporary_output}" "${_patched_contents}")
    file(RENAME "${_temporary_output}" "${_output}" RESULT _rename_result)
    if(NOT _rename_result STREQUAL "0")
        message(FATAL_ERROR "hs_reference_force_fs_hsphy: output replacement failed: ${_rename_result}")
    endif()

    set_property(TARGET "${target}" PROPERTY SOURCES "${_updated_sources}")
    target_include_directories("${target}" PRIVATE "${_source_dir}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source}")
endfunction()
