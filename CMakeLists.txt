cmake_minimum_required(VERSION 3.21)

project(BLKT
    VERSION 0.1.0
    DESCRIPTION "Portable Bilibili live assistant"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

find_package(Qt6 6.4 REQUIRED COMPONENTS
    Core
    Network
    WebSockets
    Widgets
)

add_executable(blkt
    src/app/MainWindow.cpp
    src/app/MainWindow.hpp
    src/main.cpp
    src/auth/CredentialStore.cpp
    src/auth/CredentialStore.hpp
    src/core/Core.cpp
    src/core/Core.hpp
    src/core/Guard.cpp
    src/core/Guard.hpp
    src/platform/bilibili/BilibiliAdapter.cpp
    src/platform/bilibili/BilibiliAdapter.hpp
    src/plugin/IPlugin.hpp
    src/plugin/PluginManager.cpp
    src/plugin/PluginManager.hpp
)

if(WIN32)
    set_target_properties(blkt PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

set_target_properties(blkt PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

target_link_libraries(blkt PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Widgets
)

if(MSVC)
    target_compile_options(blkt PRIVATE /W4 /permissive-)
else()
    target_compile_options(blkt PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

# Create the portable plugin directory in the build output, not at runtime.
add_custom_command(TARGET blkt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:blkt>/plugins"
)

# This worker is a separate executable, never linked into the BLKT host.
add_executable(blkt_echo_plugin
    src/plugin/example/echo_worker.cpp
)
set_target_properties(blkt_echo_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/plugins"
)
target_link_libraries(blkt_echo_plugin PRIVATE Qt6::Core)

if(WIN32)
    set_target_properties(blkt_echo_plugin PROPERTIES
        OUTPUT_NAME "echo"
        SUFFIX ".exe"
    )
else()
    set_target_properties(blkt_echo_plugin PROPERTIES
        OUTPUT_NAME "echo"
        SUFFIX ".blkt-plugin"
    )
endif()



# A process-level smoke test exercises the actual worker stdin/stdout handshake.
enable_testing()
add_executable(blkt_worker_smoke
    src/plugin/example/worker_smoke.cpp
)
set_target_properties(blkt_worker_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests"
)
target_link_libraries(blkt_worker_smoke PRIVATE Qt6::Core)
add_dependencies(blkt_worker_smoke blkt_echo_plugin)
add_test(
    NAME plugin_worker_handshake
    COMMAND blkt_worker_smoke "$<TARGET_FILE:blkt_echo_plugin>"
)
