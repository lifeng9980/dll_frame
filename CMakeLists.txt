cmake_minimum_required(VERSION 3.21)
project(OptMeasSDK VERSION 1.0.0 LANGUAGES C CXX)
include(CTest)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
if(MSVC)
    add_compile_options(/utf-8)
endif()
find_package(Qt5 5.15 REQUIRED COMPONENTS Core)
find_package(Threads REQUIRED)

add_library(optmeas_analysis SHARED analysis/frame_statistics.cpp)
target_compile_definitions(optmeas_analysis PRIVATE OPTMEAS_API_BUILD
    OPTMEAS_ANALYSIS_VERSION="${PROJECT_VERSION}")
add_library(optmeas_station SHARED
    station/acquisition/frame_source.cpp
    station/acquisition/raw_replay/source.cpp
    station/acquisition/raw_replay/RawConversionCore.cpp
    station/procedures/noise_run.cpp
    station/procedures/save_raw.cpp)
target_compile_definitions(optmeas_station PRIVATE OPTMEAS_DEVICE_BUILD)
target_link_libraries(optmeas_station PRIVATE optmeas_analysis Qt5::Core Threads::Threads)
foreach(target optmeas_station optmeas_analysis)
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include"
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/shared")
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN")
endforeach()

if(BUILD_TESTING)
    add_executable(c_client sdk_tests/c_client.c)
    target_link_libraries(c_client PRIVATE optmeas_station optmeas_analysis)
    if(NOT WIN32)
        target_link_libraries(c_client PRIVATE m)
    endif()
    add_test(NAME c_client COMMAND c_client "${PROJECT_VERSION}")
    add_executable(replay_roundtrip sdk_tests/replay_roundtrip.cpp)
    target_link_libraries(replay_roundtrip PRIVATE optmeas_station Qt5::Core)
    add_test(NAME replay_roundtrip COMMAND replay_roundtrip)
    if(WIN32)
        set_tests_properties(c_client replay_roundtrip PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt5::Core>")
    endif()
endif()

install(TARGETS optmeas_station optmeas_analysis RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(FILES SDK.md DESTINATION .)
if(WIN32)
    install(FILES "$<TARGET_FILE:Qt5::Core>" DESTINATION bin)
endif()
