cmake_minimum_required(VERSION 3.21)
project(IRSDKExamples LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

# Two supported consumer modes:
#   1. find_package(IRSDK CONFIG): install prefix via CMAKE_PREFIX_PATH, or a packaged
#      archive via IRSDK_DIR=<pkg_root>/cmake.
#   2. Explicit package root: -DIR_SDK_ROOT=<pkg_root> (unchanged from earlier releases).
#
# IR_SDK_LIB_DIR always holds the link-time libraries (import libraries / .so files) and
# IR_SDK_BIN_DIR always holds the runtime images. They are the same directory on Linux
# packages, but <prefix>/lib and <prefix>/bin differ for an installed tree.
if(IR_SDK_ROOT)
    file(TO_CMAKE_PATH "${IR_SDK_ROOT}" IR_SDK_ROOT)
    set(IR_SDK_INCLUDE_DIR "${IR_SDK_ROOT}/include")
    set(IR_SDK_EXAMPLES_DIR "${IR_SDK_ROOT}/examples")
    set(IR_SDK_THIRD_PARTY_DIR "${IR_SDK_ROOT}/third_party/nlohmann_json/include")
    if(WIN32)
        set(IR_SDK_BIN_DIR "${IR_SDK_ROOT}/windows-x64/bin")
        set(IR_SDK_LIB_DIR "${IR_SDK_ROOT}/windows-x64/lib")
        set(IR_SDK_ANALYSIS_LIBRARY "${IR_SDK_LIB_DIR}/IR_analysis.lib")
        set(IR_SDK_STATION_LIBRARY "${IR_SDK_LIB_DIR}/IR_station.lib")
    else()
        set(IR_SDK_BIN_DIR "${IR_SDK_ROOT}/linux-x86_64/lib")
        set(IR_SDK_LIB_DIR "${IR_SDK_ROOT}/linux-x86_64/lib")
        set(IR_SDK_ANALYSIS_LIBRARY "${IR_SDK_LIB_DIR}/libIR_analysis.so")
        set(IR_SDK_STATION_LIBRARY "${IR_SDK_LIB_DIR}/libIR_station.so")
    endif()
else()
    find_package(IRSDK CONFIG QUIET)
    if(NOT IRSDK_FOUND)
        message(FATAL_ERROR
            "IR SDK not found. Pass -DIR_SDK_ROOT=<extracted package directory>, "
            "-DCMAKE_PREFIX_PATH=<installed prefix>, or -DIRSDK_DIR=<package>/cmake"
            ${IRSDK_NOT_FOUND_MESSAGE})
    endif()
    set(IR_SDK_INCLUDE_DIR "${IRSDK_INCLUDE_DIR}")
    set(IR_SDK_EXAMPLES_DIR "${IRSDK_ROOT_DIR}/examples")
    set(IR_SDK_THIRD_PARTY_DIR "${IRSDK_ROOT_DIR}/third_party/nlohmann_json/include")
    set(IR_SDK_BIN_DIR "${IRSDK_BIN_DIR}")
    set(IR_SDK_LIB_DIR "${IRSDK_LIB_DIR}")
    set(IR_SDK_ANALYSIS_LIBRARY IRSDK::IR_analysis)
    set(IR_SDK_STATION_LIBRARY IRSDK::IR_station)
    message(STATUS "Using IR SDK ${IRSDK_VERSION} from ${IRSDK_ROOT_DIR}")
endif()

add_library(ir_example_mean_plugin MODULE "${CMAKE_CURRENT_LIST_DIR}/plugins/mean_dn.cpp")
target_include_directories(ir_example_mean_plugin PRIVATE "${IR_SDK_INCLUDE_DIR}")
if(MSVC)
    target_compile_options(ir_example_mean_plugin PRIVATE /utf-8)
endif()

# Copy the runtime images next to a target. Windows images live in IR_SDK_BIN_DIR, Linux
# shared objects in IR_SDK_LIB_DIR; missing optional entries are skipped.
function(ir_copy_runtime target)
    set(runtime_libs)
    if(WIN32)
        set(runtime_dir "${IR_SDK_BIN_DIR}")
        list(APPEND runtime_libs "${runtime_dir}/IR_analysis.dll")
        if(ARGC GREATER 1)
            list(APPEND runtime_libs "${runtime_dir}/IR_station.dll")
        endif()
    else()
        set(runtime_dir "${IR_SDK_LIB_DIR}")
        list(APPEND runtime_libs "${runtime_dir}/libIR_analysis.so")
        if(ARGC GREATER 1)
            list(APPEND runtime_libs "${runtime_dir}/libIR_station.so")
        endif()
    endif()
    set(available)
    foreach(lib IN LISTS runtime_libs)
        if(EXISTS "${lib}")
            list(APPEND available "${lib}")
        endif()
    endforeach()
    if(available)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${available}
                "$<TARGET_FILE_DIR:${target}>")
    endif()
endfunction()

# Analysis-only client: links IR_analysis and needs no station runtime.
function(add_ir_analysis_client target source)
    add_executable(${target} "${source}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    target_include_directories(${target} PRIVATE "${IR_SDK_INCLUDE_DIR}")
    target_link_libraries(${target} PRIVATE "${IR_SDK_ANALYSIS_LIBRARY}")
    ir_copy_runtime(${target})
    if(NOT WIN32)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()
endfunction()

# Station client: links IR_station only; IR_station depends on IR_analysis privately.
function(add_ir_station_client target source)
    add_executable(${target} "${source}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    target_include_directories(${target} PRIVATE "${IR_SDK_INCLUDE_DIR}")
    target_link_libraries(${target} PRIVATE "${IR_SDK_STATION_LIBRARY}")
    ir_copy_runtime(${target} WITH_STATION)
    if(NOT WIN32)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()
endfunction()

# Mixed client (capture + analysis): links both libraries explicitly.
function(add_ir_combined_client target source)
    add_executable(${target} "${source}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    target_include_directories(${target} PRIVATE
        "${IR_SDK_INCLUDE_DIR}" "${IR_SDK_THIRD_PARTY_DIR}")
    target_link_libraries(${target} PRIVATE
        "${IR_SDK_STATION_LIBRARY}" "${IR_SDK_ANALYSIS_LIBRARY}" Threads::Threads)
    ir_copy_runtime(${target} WITH_STATION)
    if(NOT WIN32)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()
endfunction()

add_ir_analysis_client(ir_analysis_c_sample "${IR_SDK_EXAMPLES_DIR}/analysis_algorithms.c")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(ir_analysis_c_sample PRIVATE m)
endif()
add_ir_combined_client(ir_c_client "${IR_SDK_EXAMPLES_DIR}/c_client.c")
add_ir_combined_client(ir_cpp_client "${CMAKE_CURRENT_LIST_DIR}/cpp_client.cpp")
add_ir_station_client(ir_run_recipe "${CMAKE_CURRENT_LIST_DIR}/ir_recipe_cli.cpp")
# The JSON CLI parses station/recipe files with the bundled header-only nlohmann/json.
target_include_directories(ir_run_recipe PRIVATE "${IR_SDK_THIRD_PARTY_DIR}")
add_ir_combined_client(ir_capture_accept "${CMAKE_CURRENT_LIST_DIR}/ir_capture_accept.cpp")
