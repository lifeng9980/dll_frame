if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "SOURCE_DIR and BINARY_DIR are required")
endif()

set(_gen_dir "${BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_gen_dir}")

if(DEFINED OVERRIDE_COMMIT AND NOT OVERRIDE_COMMIT STREQUAL "")
    set(IR_SDK_SOURCE_COMMIT "${OVERRIDE_COMMIT}")
else()
    find_package(Git QUIET)
    set(IR_SDK_SOURCE_COMMIT "unknown")
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse HEAD
            OUTPUT_VARIABLE _git_commit
            RESULT_VARIABLE _git_commit_status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_git_commit_status EQUAL 0 AND NOT _git_commit STREQUAL "")
            set(IR_SDK_SOURCE_COMMIT "${_git_commit}")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" status --porcelain
                OUTPUT_VARIABLE _git_status
                RESULT_VARIABLE _git_status_status
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(_git_status_status EQUAL 0 AND NOT _git_status STREQUAL "")
                string(APPEND IR_SDK_SOURCE_COMMIT "-dirty")
            endif()
        endif()
    endif()
endif()

string(REPLACE "\\" "\\\\" _escaped_commit "${IR_SDK_SOURCE_COMMIT}")
string(REPLACE "\"" "\\\"" _escaped_commit "${_escaped_commit}")

set(_header_tmp "${_gen_dir}/ir_build_info.h.tmp")
set(_header_out "${_gen_dir}/ir_build_info.h")
file(WRITE "${_header_tmp}" "#pragma once\n#define IR_SDK_SOURCE_COMMIT \"${_escaped_commit}\"\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_header_tmp}" "${_header_out}"
    RESULT_VARIABLE _copy_header_status
)
file(REMOVE "${_header_tmp}")
if(NOT _copy_header_status EQUAL 0)
    message(FATAL_ERROR "Failed to update ${_header_out}")
endif()

if(WIN32_RC)
    set(IR_SDK_SOURCE_COMMIT "${_escaped_commit}")
    foreach(_rc_pair IN ITEMS
        "station/version.rc.in|station_version.rc"
        "analysis/version.rc.in|analysis_version.rc"
    )
        string(REPLACE "|" ";" _pair_list "${_rc_pair}")
        list(GET _pair_list 0 _rc_in)
        list(GET _pair_list 1 _rc_name)
        set(_rc_tmp "${_gen_dir}/${_rc_name}.tmp")
        set(_rc_out "${_gen_dir}/${_rc_name}")
        configure_file("${SOURCE_DIR}/${_rc_in}" "${_rc_tmp}" @ONLY)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_rc_tmp}" "${_rc_out}"
            RESULT_VARIABLE _copy_rc_status
        )
        file(REMOVE "${_rc_tmp}")
        if(NOT _copy_rc_status EQUAL 0)
            message(FATAL_ERROR "Failed to update ${_rc_out}")
        endif()
    endforeach()
endif()
