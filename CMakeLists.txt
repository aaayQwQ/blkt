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
