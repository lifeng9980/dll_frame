cmake_minimum_required(VERSION 3.21)
project(IRSDK VERSION 1.8.0 LANGUAGES C CXX)
include(CTest)
include(CMakePackageConfigHelpers)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
endif()
set(IR_SDK_SOURCE_COMMIT "" CACHE STRING "Optional override for recorded source commit")
set(IR_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(IR_BUILD_INFO_HEADER "${IR_GENERATED_DIR}/ir_build_info.h")
set(IR_BUILD_INFO_SENTINEL "${IR_GENERATED_DIR}/ir_build_info.stamp")
set(IR_BUILD_INFO_ARGS
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DBINARY_DIR=${PROJECT_BINARY_DIR}"
    "-DPROJECT_VERSION=${PROJECT_VERSION}"
    "-DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}"
    "-DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}"
    "-DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}"
    "-DOVERRIDE_COMMIT=${IR_SDK_SOURCE_COMMIT}"
)
if(WIN32)
    list(APPEND IR_BUILD_INFO_ARGS "-DWIN32_RC=ON")
    set(IR_STATION_RC "${IR_GENERATED_DIR}/station_version.rc")
    set(IR_ANALYSIS_RC "${IR_GENERATED_DIR}/analysis_version.rc")
    set(IR_BUILD_INFO_BYPRODUCTS "${IR_BUILD_INFO_HEADER}" "${IR_STATION_RC}" "${IR_ANALYSIS_RC}")
else()
    list(APPEND IR_BUILD_INFO_ARGS "-DWIN32_RC=OFF")
    set(IR_BUILD_INFO_BYPRODUCTS "${IR_BUILD_INFO_HEADER}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" ${IR_BUILD_INFO_ARGS} -P "${PROJECT_SOURCE_DIR}/cmake/generate_build_info.cmake"
    RESULT_VARIABLE IR_BUILD_INFO_INIT_STATUS
)
if(NOT IR_BUILD_INFO_INIT_STATUS EQUAL 0)
    message(FATAL_ERROR "Failed to initialize generated build metadata")
endif()
add_custom_command(
    OUTPUT "${IR_BUILD_INFO_SENTINEL}"
    BYPRODUCTS ${IR_BUILD_INFO_BYPRODUCTS}
    COMMAND "${CMAKE_COMMAND}" ${IR_BUILD_INFO_ARGS} -P "${PROJECT_SOURCE_DIR}/cmake/generate_build_info.cmake"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Checking build version metadata"
    VERBATIM
)
set_source_files_properties("${IR_BUILD_INFO_SENTINEL}" PROPERTIES SYMBOLIC TRUE)
add_custom_target(ir_build_info DEPENDS "${IR_BUILD_INFO_SENTINEL}")
find_package(Threads REQUIRED)
find_path(IR_ASIO_INCLUDE_DIR asio.hpp
    HINTS "${PROJECT_SOURCE_DIR}/third_party/asio/include" REQUIRED)
file(STRINGS "${IR_ASIO_INCLUDE_DIR}/asio/version.hpp" IR_ASIO_VERSION_LINE
    REGEX "^#define ASIO_VERSION 103600([ ]|$)")
if(NOT IR_ASIO_VERSION_LINE)
    message(FATAL_ERROR "IR SDK requires standalone Asio 1.36.0 (ASIO_VERSION 103600)")
endif()

add_library(IR_analysis SHARED
    analysis/basic_stats.cpp
    analysis/noise3d_dn.cpp
    analysis/gb_gjb.cpp
    analysis/composable_ops.cpp
    analysis/spectral_ops.cpp)
add_dependencies(IR_analysis ir_build_info)
target_compile_definitions(IR_analysis PRIVATE IR_ANALYSIS_BUILD
    IR_ANALYSIS_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_sources(IR_analysis PRIVATE "${IR_ANALYSIS_RC}")
endif()
add_library(IR_station SHARED)
add_dependencies(IR_station ir_build_info)
# 仪器控制：型号协议使用同一份 Asio TCP / 串口实现。
target_sources(IR_station PRIVATE
    shared/utf8_path.cpp
    station/version.cpp
    station/instruments/device_api.cpp
    station/instruments/discovery.cpp
    station/instruments/doc058.cpp
    station/instruments/it6332a.cpp
    station/instruments/power_drivers.cpp
    station/instruments/gws_sten.cpp
    station/instruments/visa_link.cpp
    station/instruments/dmm6500.cpp
    station/instruments/detector_serial.cpp
    station/instruments/cooler_modbus.cpp
    station/instruments/asio_link.cpp)

# 采集：公共入口按来源创建 FrameSource，实时后端随目标平台编译。
target_sources(IR_station PRIVATE
    station/acquisition/frame_source.cpp
    station/acquisition/frame_queue.cpp
    station/acquisition/raw_replay_source.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(IR_station PRIVATE station/acquisition/v4l2_source.cpp)
endif()
if(WIN32)
    target_sources(IR_station PRIVATE
        station/acquisition/directshow_source.cpp
        "${IR_STATION_RC}")
    target_link_libraries(IR_station PRIVATE strmiids ole32 oleaut32)
endif()
# 流程编排与算子插件：协调仪器、采集和独立的 IR_analysis 指标库。
target_sources(IR_station PRIVATE
    station/plugins/plugin_runtime.cpp
    station/procedures/measurement.cpp
    station/procedures/recipe_json.cpp
    station/procedures/metrics_basic.cpp
    station/procedures/metrics_standard.cpp
    station/procedures/recipe_metrics.cpp
    station/procedures/recipe_parse.cpp
    station/procedures/recipe_instruments.cpp
    station/procedures/recipe_runner.cpp
    station/procedures/save_raw.cpp)
target_compile_definitions(IR_station PRIVATE
    IR_STATION_BUILD
    ASIO_STANDALONE
    IR_STATION_VERSION="${PROJECT_VERSION}")
target_include_directories(IR_station PRIVATE "${IR_GENERATED_DIR}")
target_include_directories(IR_station SYSTEM PRIVATE "${IR_ASIO_INCLUDE_DIR}")
target_include_directories(IR_station SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
target_link_libraries(IR_station PRIVATE IR_analysis Threads::Threads)
if(NOT WIN32)
    target_link_libraries(IR_station PRIVATE ${CMAKE_DL_LIBS})
endif()
if(WIN32)
    target_compile_definitions(IR_station PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(IR_station PRIVATE ws2_32 mswsock advapi32)
endif()
foreach(target IR_station IR_analysis)
    # 公共头目录使用 build/install 接口分离，third_party 仅作私有编译期依赖。
    target_include_directories(${target}
        PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
               "$<INSTALL_INTERFACE:include>"
        PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/shared")
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN"
        C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-gnu-unique>)
    endif()
endforeach()

add_executable(ir_run_recipe examples/ir_recipe_cli.cpp)
target_include_directories(ir_run_recipe SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
target_link_libraries(ir_run_recipe PRIVATE IR_station Threads::Threads)
set_target_properties(ir_run_recipe PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")

add_executable(ir_capture_accept examples/ir_capture_accept.cpp)
target_include_directories(ir_capture_accept SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
target_link_libraries(ir_capture_accept PRIVATE IR_station IR_analysis Threads::Threads)
set_target_properties(ir_capture_accept PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")

function(ir_add_metric_plugin target_name)
    add_library(${target_name} MODULE ${ARGN})
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /utf-8)
    endif()
    target_include_directories(${target_name} PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    set_target_properties(${target_name} PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib/plugins"
        LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/plugins"
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/plugins")
endfunction()

if(BUILD_TESTING)
    set(IR_TEST_TARGETS
        c_client gb_gjb_test noise3d_test
        analysis_algorithms_c composable_ops_test drift_nodisk_test
        frame_queue_test replay_roundtrip recipe_test
        flicker_test discovery_test plugin_abi_test)
    set(IR_TEST_NAMES
        c_client gb_gjb noise3d
        analysis_algorithms_c composable_ops drift_nodisk frame_queue
        replay_roundtrip recipe_flow flicker discovery
        plugin_abi)
    add_executable(c_client sdk_tests/c_client.c)
    target_link_libraries(c_client PRIVATE IR_station IR_analysis)
    if(NOT WIN32)
        target_link_libraries(c_client PRIVATE m)
    endif()
    add_test(NAME c_client COMMAND c_client "${PROJECT_VERSION}")
    add_executable(gb_gjb_test sdk_tests/gb_gjb_test.cpp)
    target_link_libraries(gb_gjb_test PRIVATE IR_analysis)
    add_test(NAME gb_gjb COMMAND gb_gjb_test)
    add_executable(noise3d_test sdk_tests/noise3d_test.cpp)
    target_link_libraries(noise3d_test PRIVATE IR_analysis)
    add_test(NAME noise3d COMMAND noise3d_test)
    add_executable(analysis_algorithms_c sdk_tests/analysis_algorithms.c)
    target_link_libraries(analysis_algorithms_c PRIVATE IR_analysis)
    if(NOT WIN32)
        target_link_libraries(analysis_algorithms_c PRIVATE m)
    endif()
    add_test(NAME analysis_algorithms_c COMMAND analysis_algorithms_c)
    add_executable(composable_ops_test sdk_tests/composable_ops_test.cpp)
    target_link_libraries(composable_ops_test PRIVATE IR_analysis)
    add_test(NAME composable_ops COMMAND composable_ops_test)
    add_executable(drift_nodisk_test sdk_tests/drift_nodisk_test.cpp)
    target_link_libraries(drift_nodisk_test PRIVATE IR_station IR_analysis)
    add_test(NAME drift_nodisk COMMAND drift_nodisk_test)
    add_executable(frame_queue_test
        sdk_tests/frame_queue_test.cpp
        station/acquisition/frame_queue.cpp
        station/acquisition/frame_source.cpp
        station/acquisition/raw_replay_source.cpp)
    target_include_directories(frame_queue_test PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/station/acquisition"
        "${PROJECT_SOURCE_DIR}/station"
        "${PROJECT_SOURCE_DIR}/shared")
    # Exercise capture startup with the platform live factory replaced by a test source.
    target_compile_definitions(frame_queue_test PRIVATE IR_STATION_BUILD)
    target_link_libraries(frame_queue_test PRIVATE Threads::Threads)
    add_test(NAME frame_queue COMMAND frame_queue_test)
    add_test(NAME capture_exit_while_running COMMAND frame_queue_test --exit-while-running)
    add_executable(replay_roundtrip sdk_tests/replay_roundtrip.cpp)
    target_link_libraries(replay_roundtrip PRIVATE IR_station)
    add_test(NAME replay_roundtrip COMMAND replay_roundtrip)
    add_executable(recipe_test sdk_tests/recipe_test.cpp)
    target_include_directories(recipe_test SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
    target_link_libraries(recipe_test PRIVATE IR_station IR_analysis Threads::Threads)
    add_test(NAME recipe_flow COMMAND recipe_test "${PROJECT_SOURCE_DIR}/examples")
    add_executable(flicker_test sdk_tests/flicker_test.cpp)
    target_link_libraries(flicker_test PRIVATE IR_analysis)
    add_test(NAME flicker COMMAND flicker_test)
    add_executable(discovery_test sdk_tests/discovery_test.cpp)
    target_include_directories(discovery_test SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
    target_link_libraries(discovery_test PRIVATE IR_station)
    add_test(NAME discovery COMMAND discovery_test)
    ir_add_metric_plugin(ir_test_plugin_good sdk_tests/plugins/test_plugin_good.cpp)
    ir_add_metric_plugin(ir_test_plugin_bad_abi sdk_tests/plugins/test_plugin_bad_abi.cpp)
    ir_add_metric_plugin(ir_test_plugin_duplicate sdk_tests/plugins/test_plugin_duplicate.cpp)
    ir_add_metric_plugin(ir_test_power_driver sdk_tests/plugins/mock_power_driver.cpp)
    if(WIN32)
        target_link_libraries(ir_test_power_driver PRIVATE winhttp)

        # VISA 链路测试：mock 运行时 DLL + 以 IR_VISA_TEST_OVERRIDE 单独编译的 visa_link.cpp。
        # IR_station.dll 不定义该宏，生产路径始终只从 System32 加载 visa64.dll。
        add_library(ir_test_mock_visa SHARED sdk_tests/mock_visa_runtime.cpp)
        set_target_properties(ir_test_mock_visa PROPERTIES
            C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES
            RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests/bin"
            LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests/bin")
        add_executable(visa_link_test
            sdk_tests/visa_link_test.cpp
            station/instruments/visa_link.cpp
            station/instruments/asio_link.cpp
            shared/utf8_path.cpp)
        target_compile_definitions(visa_link_test PRIVATE
            IR_VISA_TEST_OVERRIDE=1 ASIO_STANDALONE _WIN32_WINNT=0x0A00)
        target_include_directories(visa_link_test PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/station/instruments"
            "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/shared")
        target_include_directories(visa_link_test SYSTEM PRIVATE "${IR_ASIO_INCLUDE_DIR}")
        target_link_libraries(visa_link_test PRIVATE Threads::Threads ws2_32 mswsock)
        set_target_properties(visa_link_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests/bin")
        add_dependencies(visa_link_test ir_test_mock_visa)
        if(MSVC)
            target_compile_options(visa_link_test PRIVATE /UNDEBUG)
        else()
            target_compile_options(visa_link_test PRIVATE -UNDEBUG)
        endif()
        add_test(NAME visa_link COMMAND visa_link_test "$<TARGET_FILE:ir_test_mock_visa>")
    endif()
    add_executable(plugin_abi_test
        sdk_tests/plugin_abi_test.cpp
        sdk_tests/plugins/test_plugin_good.cpp)
    target_include_directories(plugin_abi_test SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
    target_link_libraries(plugin_abi_test PRIVATE IR_station Threads::Threads)
    add_dependencies(plugin_abi_test
        ir_test_plugin_good ir_test_plugin_bad_abi ir_test_plugin_duplicate)
    add_test(NAME plugin_abi COMMAND plugin_abi_test
        "$<TARGET_FILE_DIR:ir_test_plugin_good>" "$<TARGET_FILE:IR_analysis>")
    add_test(NAME ir_capture_accept_list COMMAND "$<TARGET_FILE:ir_capture_accept>" --list)
    set_target_properties(${IR_TEST_TARGETS} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests/bin")
    # The C and C++ tests use assert for both exercising the API and validating results.
    foreach(target IN LISTS IR_TEST_TARGETS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${target} PRIVATE -UNDEBUG)
        endif()
    endforeach()
    if(WIN32)
        set_tests_properties(${IR_TEST_NAMES} PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:IR_station>")
        option(IR_ENABLE_COM0COM_HIL "Run live com0com regression using the local serial map" OFF)
    endif()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME ir_run_recipe_offline_check COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/sdk_tests/raw_replay_cli_test.py"
            --recipe-cli "$<TARGET_FILE:ir_run_recipe>" --fixture-parent "${PROJECT_BINARY_DIR}")
        add_test(NAME ir_capture_accept_soak COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/sdk_tests/raw_replay_cli_test.py"
            --capture-accept "$<TARGET_FILE:ir_capture_accept>" --fixture-parent "${PROJECT_BINARY_DIR}")
        set_tests_properties(ir_run_recipe_offline_check ir_capture_accept_soak PROPERTIES TIMEOUT 60)

        add_test(NAME python_binding_abi COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/sdk_tests/python_binding_abi_test.py"
            --c-client "$<TARGET_FILE:c_client>"
            --library-dir "$<TARGET_FILE_DIR:IR_station>"
            --expected-version "${PROJECT_VERSION}"
            --bindings-dir "${PROJECT_SOURCE_DIR}/bindings/python"
            --examples-dir "${PROJECT_SOURCE_DIR}/examples")
        set_tests_properties(python_binding_abi PROPERTIES TIMEOUT 120
            ENVIRONMENT "IR_TEST_C_COMPILER=${CMAKE_C_COMPILER}")

        add_test(NAME ir_drift_example_self_test COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/sdk_tests/raw_replay_cli_test.py"
            --drift-example "${PROJECT_SOURCE_DIR}/examples/python/ir_drift_nodisk.py"
            --library-dir "$<TARGET_FILE_DIR:IR_station>" --fixture-parent "${PROJECT_BINARY_DIR}")
        set_tests_properties(ir_drift_example_self_test PROPERTIES TIMEOUT 120)

        add_test(NAME blackbody_tcp COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/sdk_tests/blackbody_mock_test.py"
            tcp "$<TARGET_FILE:IR_station>" --fixture-parent "${PROJECT_BINARY_DIR}")
        set_tests_properties(blackbody_tcp PROPERTIES TIMEOUT 60)

        add_test(NAME instruments_mock COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/sdk_tests/instruments_mock_test.py"
            "$<TARGET_FILE:IR_station>" --fixture-parent "${PROJECT_BINARY_DIR}")
        set_tests_properties(instruments_mock PROPERTIES TIMEOUT 60)

        add_test(NAME power_tcp COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/sdk_tests/power_mock_test.py"
            --tcp "$<TARGET_FILE:IR_station>" --fixture-parent "${PROJECT_BINARY_DIR}"
            --power-driver "$<TARGET_FILE:ir_test_power_driver>")
        # The external-power regressions spawn child interpreters and copy the mock driver, which
        # can be slowed by on-access virus scanning; allow generous headroom over the ~11-16 s run.
        set_tests_properties(power_tcp PROPERTIES TIMEOUT 120)

        add_test(NAME virtual_station_hil COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/sdk_tests/virtual_station_hil_test.py"
            "$<TARGET_FILE:IR_station>" "$<TARGET_FILE:ir_run_recipe>"
            --fixture-parent "${PROJECT_BINARY_DIR}"
            --power-driver "$<TARGET_FILE:ir_test_power_driver>")
        set_tests_properties(virtual_station_hil PROPERTIES TIMEOUT 60)
        if(WIN32 AND IR_ENABLE_COM0COM_HIL)
            add_test(NAME com0com_hil COMMAND "${Python3_EXECUTABLE}" -B
                "${PROJECT_SOURCE_DIR}/tools/verify_com0com_hil.py"
                --station-lib "$<TARGET_FILE:IR_station>" --cli "$<TARGET_FILE:ir_run_recipe>"
                --work-parent "${PROJECT_BINARY_DIR}")
            set_tests_properties(com0com_hil PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME blackbody_serial COMMAND "${Python3_EXECUTABLE}" -B
                "${PROJECT_SOURCE_DIR}/sdk_tests/blackbody_mock_test.py"
                serial "$<TARGET_FILE:IR_station>" --fixture-parent "${PROJECT_BINARY_DIR}")
            set_tests_properties(blackbody_serial PROPERTIES TIMEOUT 120)
            add_test(NAME power_serial COMMAND "${Python3_EXECUTABLE}" -B
                "${PROJECT_SOURCE_DIR}/sdk_tests/power_mock_test.py" "$<TARGET_FILE:IR_station>"
                --fixture-parent "${PROJECT_BINARY_DIR}")
            set_tests_properties(power_serial PROPERTIES TIMEOUT 120)
        endif()
    endif()
endif()

if(WIN32 AND BUILD_TESTING)
    set(_ir_ctest_tmp "${PROJECT_BINARY_DIR}/ctest-tmp")
    file(MAKE_DIRECTORY "${_ir_ctest_tmp}")
    get_property(_ir_ctest_names DIRECTORY PROPERTY TESTS)
    # Append instead of overwrite so a test's own ENVIRONMENT (IR_TEST_C_COMPILER on
    # python_binding_abi) survives the shared TEMP/TMP redirection.
    set_property(TEST ${_ir_ctest_names} APPEND PROPERTY
        ENVIRONMENT "TEMP=${_ir_ctest_tmp}" "TMP=${_ir_ctest_tmp}")
endif()

install(TARGETS IR_station IR_analysis EXPORT IRSDKTargets RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(TARGETS ir_run_recipe ir_capture_accept RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
# 标准 CMake 包：<prefix>/lib/cmake/IRSDK 下的 targets 与 config 文件。
install(EXPORT IRSDKTargets FILE IRSDKTargets.cmake
    NAMESPACE IRSDK:: DESTINATION lib/cmake/IRSDK)
configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/IRSDKConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/IRSDKConfig.cmake"
    INSTALL_DESTINATION lib/cmake/IRSDK
    NO_SET_AND_CHECK_MACRO)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/IRSDKConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)
# The CMake helper scripts write host line endings; normalize to LF so the Windows and
# Linux install trees are byte-identical (tools/package_sdk.py verifies the hashes).
# file(CONFIGURE ... NEWLINE_STYLE LF) is the only CMake primitive that keeps LF on Windows
# (file(WRITE)/configure_file/file(GENERATE) all rewrite LF to CRLF there), but it expands
# @VAR@ tokens, so reword the helper banner first and fail fast on any remaining token.
foreach(_ir_pkg_generated IRSDKConfig.cmake IRSDKConfigVersion.cmake)
    file(READ "${CMAKE_CURRENT_BINARY_DIR}/${_ir_pkg_generated}" _ir_pkg_text)
    string(REPLACE "\r\n" "\n" _ir_pkg_text "${_ir_pkg_text}")
    string(REPLACE "\r" "" _ir_pkg_text "${_ir_pkg_text}")
    string(REPLACE "@PACKAGE_INIT@" "PACKAGE_INIT" _ir_pkg_text "${_ir_pkg_text}")
    string(FIND "${_ir_pkg_text}" "@" _ir_pkg_at)
    if(NOT _ir_pkg_at EQUAL -1)
        message(FATAL_ERROR
            "${_ir_pkg_generated} contains an '@' token that the LF rewrite would expand")
    endif()
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_ir_pkg_generated}"
        CONTENT "${_ir_pkg_text}" NEWLINE_STYLE LF @ONLY)
endforeach()
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/IRSDKConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/IRSDKConfigVersion.cmake"
    DESTINATION lib/cmake/IRSDK)
# 官方精简 Python 绑定（纯 ctypes，零第三方依赖）；仅装发布文件，不带本地 build/dist。
install(FILES bindings/python/pyproject.toml DESTINATION bindings/python)
install(FILES bindings/python/irsdk/__init__.py
    bindings/python/irsdk/abi.py
    bindings/python/irsdk/loader.py
    DESTINATION bindings/python/irsdk)
install(FILES SDK.md DESTINATION .)
install(FILES docs/客户接入说明.md DESTINATION docs)
install(FILES sdk_tests/c_client.c sdk_tests/analysis_algorithms.c DESTINATION examples)
install(DIRECTORY simulators/ DESTINATION simulators
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
    PATTERN "*.pyo" EXCLUDE)
install(DIRECTORY examples/ DESTINATION examples
    PATTERN "output" EXCLUDE
    PATTERN "tc002a_640" EXCLUDE
    PATTERN "s07_1280" EXCLUDE
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
    PATTERN "*.pyo" EXCLUDE
    REGEX "examples/csharp/(bin|obj)(/.*)?$" EXCLUDE)
install(DIRECTORY third_party/nlohmann_json/ DESTINATION third_party/nlohmann_json)
