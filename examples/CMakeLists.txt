cmake_minimum_required(VERSION 3.21)
project(IRSDKExamples LANGUAGES C CXX)

if(NOT IR_SDK_ROOT)
    message(FATAL_ERROR "Set IR_SDK_ROOT to the extracted IR SDK package directory")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(IR_SDK_INCLUDE_DIR "${IR_SDK_ROOT}/include")

if(WIN32)
    set(IR_SDK_BIN_DIR "${IR_SDK_ROOT}/windows-x64/bin")
    set(IR_SDK_LIB_DIR "${IR_SDK_ROOT}/windows-x64/lib")
    set(IR_STATION_LIBRARY "${IR_SDK_LIB_DIR}/IR_station.lib")
    set(IR_ANALYSIS_LIBRARY "${IR_SDK_LIB_DIR}/IR_analysis.lib")
else()
    set(IR_SDK_BIN_DIR "${IR_SDK_ROOT}/linux-x86_64/lib")
    set(IR_SDK_LIB_DIR "${IR_SDK_ROOT}/linux-x86_64/lib")
    set(IR_STATION_LIBRARY "${IR_SDK_LIB_DIR}/libIR_station.so")
    set(IR_ANALYSIS_LIBRARY "${IR_SDK_LIB_DIR}/libIR_analysis.so")
endif()

function(add_ir_client target source)
    add_executable(${target} "${source}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    target_include_directories(${target} PRIVATE "${IR_SDK_INCLUDE_DIR}")
    target_link_libraries(${target} PRIVATE "${IR_STATION_LIBRARY}" "${IR_ANALYSIS_LIBRARY}")
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${IR_SDK_BIN_DIR}/IR_station.dll" "${IR_SDK_BIN_DIR}/IR_analysis.dll"
                "$<TARGET_FILE_DIR:${target}>")
    else()
        set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${IR_SDK_BIN_DIR}/libIR_station.so" "${IR_SDK_BIN_DIR}/libIR_analysis.so"
                "$<TARGET_FILE_DIR:${target}>")
    endif()
endfunction()

add_ir_client(ir_c_client "${IR_SDK_ROOT}/examples/c_client.c")
add_ir_client(ir_cpp_client "${CMAKE_CURRENT_LIST_DIR}/cpp_client.cpp")

add_executable(ir_run_recipe "${CMAKE_CURRENT_LIST_DIR}/ir_run_recipe.cpp")
if(MSVC)
    target_compile_options(ir_run_recipe PRIVATE /utf-8)
endif()
target_include_directories(ir_run_recipe PRIVATE
    "${IR_SDK_INCLUDE_DIR}"
    "${IR_SDK_ROOT}/third_party/nlohmann_json/include")
target_link_libraries(ir_run_recipe PRIVATE "${IR_STATION_LIBRARY}")
if(WIN32)
    add_custom_command(TARGET ir_run_recipe POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${IR_SDK_BIN_DIR}/IR_station.dll" "${IR_SDK_BIN_DIR}/IR_analysis.dll"
            "$<TARGET_FILE_DIR:ir_run_recipe>")
else()
    set_target_properties(ir_run_recipe PROPERTIES BUILD_RPATH "$ORIGIN")
    add_custom_command(TARGET ir_run_recipe POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${IR_SDK_BIN_DIR}/libIR_station.so" "${IR_SDK_BIN_DIR}/libIR_analysis.so"
            "$<TARGET_FILE_DIR:ir_run_recipe>")
endif()
