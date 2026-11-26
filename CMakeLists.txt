cmake_minimum_required(VERSION 3.21)

project(BLKT VERSION 0.2.0 DESCRIPTION "Headless Bilibili live server" LANGUAGES CXX)
include(GNUInstallDirs)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network WebSockets Qml)

add_executable(blkt
    src/main.cpp
    src/auth/CredentialStore.cpp src/auth/CredentialStore.hpp
    src/core/Core.cpp src/core/Core.hpp src/core/Guard.cpp src/core/Guard.hpp
    src/platform/bilibili/BilibiliAdapter.cpp src/platform/bilibili/BilibiliAdapter.hpp
    src/plugin/IPlugin.hpp
    src/plugin/ScriptPluginManager.cpp src/plugin/ScriptPluginManager.hpp
)
target_link_libraries(blkt PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets Qt6::Qml)
set_target_properties(blkt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>" WIN32_EXECUTABLE FALSE)
if(MSVC)
    target_compile_options(blkt PRIVATE /W4 /permissive-)
else()
    target_compile_options(blkt PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

enable_testing()
add_executable(blkt_guard_smoke src/core/guard_smoke.cpp src/core/Guard.cpp)
target_link_libraries(blkt_guard_smoke PRIVATE Qt6::Core)
add_test(NAME security_guard_validation COMMAND blkt_guard_smoke)

install(TARGETS blkt RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY plugins/ DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins"
        FILES_MATCHING PATTERN "manifest.json" PATTERN "*.js")

if(WIN32 OR APPLE)
    set(deploy_script "${CMAKE_CURRENT_BINARY_DIR}/deploy-$<CONFIG>.cmake")
    if(APPLE)
        file(GENERATE OUTPUT "${deploy_script}" CONTENT "
include(\"${QT_DEPLOY_SUPPORT}\")
qt_deploy_runtime_dependencies(
    EXECUTABLE \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/$<TARGET_FILE_NAME:blkt>\"
    VERBOSE
)
")
    else()
        file(GENERATE OUTPUT "${deploy_script}" CONTENT "
include(\"${QT_DEPLOY_SUPPORT}\")
qt_deploy_runtime_dependencies(
    EXECUTABLE \"\${QT_DEPLOY_BIN_DIR}/$<TARGET_FILE_NAME:blkt>\"
    GENERATE_QT_CONF
    VERBOSE
)
")
    endif()
    install(SCRIPT "${deploy_script}")
else()
    message(STATUS "Linux package requires Qt 6 Core, Network, WebSockets and Qml runtime libraries on the target system.")
endif()
