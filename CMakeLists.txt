cmake_minimum_required(VERSION 3.22)
project(tts VERSION 0.1.0)

option(BUILD_EXE "Build node executable file" ON)
option(BUILD_TESTING "Build unit tests" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT SKIP_CXX23_STDLIB_CHECK AND NOT _CXX23_STDLIB_VERIFIED)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_QUIET TRUE)
    string(APPEND CMAKE_REQUIRED_FLAGS " -std=c++23")
    string(TOUPPER "${PROJECT_NAME}" _PRJ_UPPER)
    check_cxx_source_compiles([[
        #include <expected>
        #include <format>
        #include <ranges>
        int main() {
            std::expected<int, int> result{42};
            const auto text = std::format("{}", result.value());
            const auto values = std::views::iota(0, 1);
            return static_cast<int>(text.size() + *values.begin());
        }
    ]] ${_PRJ_UPPER}_HAS_REQUIRED_CXX23_STDLIB)
    if (NOT ${_PRJ_UPPER}_HAS_REQUIRED_CXX23_STDLIB)
        message(FATAL_ERROR
                "${PROJECT_NAME} requires a C++23 standard library with <expected>, "
                "<format>, and ranges support. Use GCC 13+ or an equivalent compiler.")
    endif ()
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif ()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(std_msgs REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

if (NOT ROS2_WS_SOURCE_BUILD)
    find_package(common_cpp REQUIRED)
    find_package(audio_io_interface REQUIRED)
    find_package(tts_interface REQUIRED)
endif ()

# ---------------------------------------------------------------------------
# libtts：组合式节点 + 后端 + 缓存 + 音频工具。
# 其他功能包可链接它，把自己的 rclcpp::Node 传给 tts::TtsNode。
# ---------------------------------------------------------------------------
add_library(tts SHARED
        include/tts/paroli_backend.hpp
        include/tts/paths.hpp
        include/tts/piper_backend.hpp
        include/tts/process_runner.hpp
        include/tts/sentence_splitter.hpp
        include/tts/tts_backend.hpp
        include/tts/tts_cache.hpp
        include/tts/tts_node.hpp
        include/tts/wav_io.hpp
        src/paroli_backend.cpp
        src/paths.cpp
        src/piper_backend.cpp
        src/process_runner.cpp
        src/sentence_splitter.cpp
        src/tts_cache.cpp
        src/tts_node.cpp
        src/wav_io.cpp
)
target_compile_features(tts PUBLIC cxx_std_23)
set_target_properties(tts PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(tts PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(tts PUBLIC
        OpenSSL::Crypto
        CURL::libcurl
        Threads::Threads
)

# 接口包的 endpoint/typesupport 统一由各自的 cmake_include.cmake 处理：
# 普通 colcon 构建走 find_package，CLion 源码聚合构建直接用 in-tree target。
# 聚合构建用 keyword target_link_libraries，因此这里也统一用 keyword。
if (ROS2_WS_SOURCE_BUILD)
    if (NOT TARGET audio_io_interface)
        message(FATAL_ERROR "CLion workspace requires the in-tree audio_io_interface target.")
    endif ()
    if (NOT TARGET tts_interface)
        message(FATAL_ERROR "CLion workspace requires the in-tree tts_interface target.")
    endif ()
    target_include_directories(tts PUBLIC
            ${rclcpp_INCLUDE_DIRS}
            ${rclcpp_action_INCLUDE_DIRS}
            ${std_msgs_INCLUDE_DIRS})
    target_link_libraries(tts PUBLIC
            ${rclcpp_LIBRARIES}
            ${rclcpp_action_LIBRARIES}
            ${std_msgs_LIBRARIES})
    include("${audio_io_interface_SOURCE_DIR}/cmake_include.cmake")
    include("${tts_interface_SOURCE_DIR}/cmake_include.cmake")
else ()
    ament_target_dependencies(tts PUBLIC
            rclcpp
            rclcpp_action
            std_msgs
            audio_io_interface
            tts_interface)
endif ()

# ---------------------------------------------------------------------------
# tts_node：只做 ROS 组合与日志初始化。
# ---------------------------------------------------------------------------
if (BUILD_EXE)
    add_executable(tts_node src/main.cpp)
    target_compile_features(tts_node PRIVATE cxx_std_23)
    target_link_libraries(tts_node tts)
    if (NOT ROS2_WS_SOURCE_BUILD)
        ament_target_dependencies(tts_node rclcpp)
    endif ()

    # rcutils 输出处理器是进程全局对象，只能由 main.cpp 安装；
    # 不要让库目标链接 common_cpp_ros_logging。
    if (ROS2_WS_SOURCE_BUILD)
        if (NOT TARGET common_cpp_ros_logging)
            message(FATAL_ERROR
                    "CLion workspace configuration requires the in-tree common_cpp_ros_logging target. "
                    "Open the workspace root and ensure common_cpp is added before tts.")
        endif ()
        target_link_libraries(tts_node common_cpp_ros_logging)
    else ()
        target_link_libraries(tts_node common_cpp::common_cpp_ros_logging)
    endif ()
endif ()

install(TARGETS tts
        EXPORT ttsTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
        INCLUDES DESTINATION include
)
install(DIRECTORY include/${PROJECT_NAME}/
        DESTINATION include/${PROJECT_NAME}
)

if (BUILD_EXE)
    install(TARGETS tts_node
            DESTINATION lib/${PROJECT_NAME}
    )
    install(DIRECTORY config launch docs
            DESTINATION share/${PROJECT_NAME}
            PATTERN "__pycache__" EXCLUDE
            PATTERN "*.pyc" EXCLUDE
    )
    install(PROGRAMS tools/install_piper.sh
            DESTINATION share/${PROJECT_NAME}/tools
    )
    install(FILES README.md
            DESTINATION share/${PROJECT_NAME}
    )
endif ()

if (BUILD_TESTING)
    find_package(ament_cmake_gtest REQUIRED)

    ament_add_gtest(test_sentence_splitter test/test_sentence_splitter.cpp)
    target_link_libraries(test_sentence_splitter tts)

    ament_add_gtest(test_wav_io test/test_wav_io.cpp)
    target_link_libraries(test_wav_io tts)

    ament_add_gtest(test_tts_cache test/test_tts_cache.cpp)
    target_link_libraries(test_tts_cache tts)

    ament_add_gtest(test_backend_requests test/test_backend_requests.cpp)
    target_link_libraries(test_backend_requests tts)

    ament_add_gtest(test_process_runner test/test_process_runner.cpp)
    target_link_libraries(test_process_runner tts)
endif ()

ament_export_include_directories(include)
ament_export_libraries(tts)
ament_export_dependencies(rclcpp rclcpp_action std_msgs common_cpp audio_io_interface tts_interface)
ament_export_targets(ttsTargets HAS_LIBRARY_TARGET)
ament_package()
