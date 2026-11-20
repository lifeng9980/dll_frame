cmake_minimum_required(VERSION 3.21)
project(IRSDK VERSION 1.0.0 LANGUAGES C CXX)
include(CTest)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
if(MSVC)
    add_compile_options(/utf-8)
endif()
find_package(Qt5 5.15 REQUIRED COMPONENTS Core)
find_package(Threads REQUIRED)
find_path(IR_ASIO_INCLUDE_DIR asio.hpp REQUIRED)

add_library(IR_analysis SHARED analysis/frame_statistics.cpp)
target_compile_definitions(IR_analysis PRIVATE IR_ANALYSIS_BUILD
    IR_ANALYSIS_VERSION="${PROJECT_VERSION}")
add_library(IR_station SHARED)
# 仪器控制：型号协议使用同一份 Asio TCP / 串口实现。
target_sources(IR_station PRIVATE
    station/instruments/device_api.cpp
    station/instruments/blackbody/doc058.cpp
    station/instruments/transport/asio_link.cpp)

# 采集：公共入口按来源创建 FrameSource，实时后端随目标平台编译。
target_sources(IR_station PRIVATE
    station/acquisition/frame_source.cpp
    station/acquisition/raw_replay/source.cpp
    station/acquisition/raw_replay/RawConversionCore.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(IR_station PRIVATE station/acquisition/v4l2/source.cpp)
endif()
if(WIN32)
    target_sources(IR_station PRIVATE station/acquisition/directshow/source.cpp)
    target_link_libraries(IR_station PRIVATE strmiids ole32 oleaut32)
endif()
# 流程编排：协调仪器、采集和独立的 IR_analysis 指标库。
target_sources(IR_station PRIVATE
    station/procedures/noise_run.cpp
    station/procedures/save_raw.cpp)
target_compile_definitions(IR_station PRIVATE IR_STATION_BUILD ASIO_STANDALONE)
target_include_directories(IR_station SYSTEM PRIVATE "${IR_ASIO_INCLUDE_DIR}")
target_link_libraries(IR_station PRIVATE IR_analysis Qt5::Core Threads::Threads)
if(WIN32)
    target_compile_definitions(IR_station PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(IR_station PRIVATE ws2_32 mswsock)
endif()
foreach(target IR_station IR_analysis)
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include"
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/shared")
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN"
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
endforeach()

if(BUILD_TESTING)
    add_executable(c_client sdk_tests/c_client.c)
    target_link_libraries(c_client PRIVATE IR_station IR_analysis)
    add_executable(analysis_conditions sdk_tests/analysis_conditions.c)
    target_link_libraries(analysis_conditions PRIVATE IR_analysis)
    if(NOT WIN32)
        target_link_libraries(analysis_conditions PRIVATE m)
    endif()
    if(NOT WIN32)
        target_link_libraries(c_client PRIVATE m)
    endif()
    add_test(NAME c_client COMMAND c_client "${PROJECT_VERSION}")
    add_test(NAME analysis_conditions COMMAND analysis_conditions)
    add_executable(replay_roundtrip sdk_tests/replay_roundtrip.cpp)
    target_link_libraries(replay_roundtrip PRIVATE IR_station Qt5::Core)
    add_test(NAME replay_roundtrip COMMAND replay_roundtrip)
    add_executable(noise_recipe sdk_tests/noise_recipe.c)
    target_link_libraries(noise_recipe PRIVATE IR_station)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME blackbody_tcp COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/sdk_tests/blackbody_mock_test.py"
            tcp "$<TARGET_FILE:IR_station>" --client "$<TARGET_FILE:noise_recipe>" --qt-bin "$<TARGET_FILE_DIR:Qt5::Core>" --fixture-parent "${PROJECT_BINARY_DIR}")
        set_tests_properties(blackbody_tcp PROPERTIES TIMEOUT 60)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME blackbody_serial COMMAND "${Python3_EXECUTABLE}" -B
                "${PROJECT_SOURCE_DIR}/sdk_tests/blackbody_mock_test.py"
                serial "$<TARGET_FILE:IR_station>" --client "$<TARGET_FILE:noise_recipe>" --fixture-parent "${PROJECT_BINARY_DIR}")
            set_tests_properties(blackbody_serial PROPERTIES TIMEOUT 60)
        endif()
    endif()
    if(WIN32)
        set_tests_properties(c_client replay_roundtrip PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt5::Core>")
    endif()
endif()

install(TARGETS IR_station IR_analysis RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(FILES SDK.md DESTINATION .)
install(FILES sdk_tests/c_client.c sdk_tests/noise_recipe.c sdk_tests/analysis_conditions.c DESTINATION examples)
if(WIN32)
    install(FILES "$<TARGET_FILE:Qt5::Core>" DESTINATION bin)
endif()
