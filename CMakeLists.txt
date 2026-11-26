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
find_package(Threads REQUIRED)

add_executable(blkt
    src/main.cpp
    src/auth/CredentialStore.cpp src/auth/CredentialStore.hpp
    src/core/Core.cpp src/core/Core.hpp src/core/Guard.cpp src/core/Guard.hpp
    src/platform/bilibili/BilibiliAdapter.cpp src/platform/bilibili/BilibiliAdapter.hpp
    src/plugin/IPlugin.hpp
    src/plugin/ScriptPluginManager.cpp src/plugin/ScriptPluginManager.hpp
)
target_link_libraries(blkt PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets Qt6::Qml Threads::Threads)
set_target_properties(blkt PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    WIN32_EXECUTABLE FALSE
)

if(APPLE)
    set_target_properties(blkt PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "BLKT"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.blkt.server"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
endif()

if(MSVC)
    target_compile_options(blkt PRIVATE /W4 /permissive-)
else()
    target_compile_options(blkt PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

enable_testing()
add_executable(blkt_guard_smoke src/core/guard_smoke.cpp src/core/Guard.cpp)
target_link_libraries(blkt_guard_smoke PRIVATE Qt6::Core)
add_test(NAME security_guard_validation COMMAND blkt_guard_smoke)

add_executable(blkt_credential_smoke
    src/auth/credential_smoke.cpp
    src/auth/CredentialStore.cpp
)
target_link_libraries(blkt_credential_smoke PRIVATE Qt6::Core)
add_test(NAME credential_cookie_scope_validation COMMAND blkt_credential_smoke)

add_executable(blkt_plugin_budget_smoke
    src/plugin/plugin_budget_smoke.cpp
    src/plugin/ScriptPluginManager.cpp
    src/core/Core.cpp
    src/core/Guard.cpp
)
target_link_libraries(blkt_plugin_budget_smoke PRIVATE
    Qt6::Core Qt6::Network Qt6::Qml Threads::Threads
)
add_test(NAME plugin_event_execution_budget COMMAND blkt_plugin_budget_smoke)

if(APPLE)
    install(TARGETS blkt BUNDLE DESTINATION .)
else()
    install(TARGETS blkt RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

if(WIN32)
    qt_generate_deploy_app_script(
        TARGET blkt
        OUTPUT_SCRIPT deploy_script
        NO_TRANSLATIONS
        DEPLOY_TOOL_OPTIONS
            --no-system-d3d-compiler
            --no-opengl-sw
            --no-quick-import
            --no-quick
            --no-opengl
            --skip-plugin-types
            generic,imageformats,networkinformation,platforms,qmltooling,styles
    )
    install(SCRIPT ${deploy_script})
    # Keep the executable and Qt runtime DLLs together in bin. Qt's plugin
    # loader still requires a type subdirectory for the TLS backend, but the
    # generated top-level "plugins" directory and unused plugin families
    # are removed from the portable package.
    install(CODE [[
        set(_blkt_bin "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")
        set(_blkt_plugins "${_blkt_bin}/plugins")
        if(NOT EXISTS "${_blkt_plugins}/tls")
            message(FATAL_ERROR "Required Qt TLS plugin was not deployed")
        endif()
        file(MAKE_DIRECTORY "${_blkt_bin}/tls")
        file(GLOB _blkt_tls_files "${_blkt_plugins}/tls/*")
        foreach(_blkt_tls_file IN LISTS _blkt_tls_files)
            get_filename_component(_blkt_tls_name "${_blkt_tls_file}" NAME)
            file(RENAME "${_blkt_tls_file}" "${_blkt_bin}/tls/${_blkt_tls_name}")
        endforeach()
        if(EXISTS "${_blkt_plugins}")
            file(REMOVE_RECURSE "${_blkt_plugins}")
        endif()
        file(WRITE "${_blkt_bin}/qt.conf" "[Paths]\nPlugins=.\n")
    ]])
elseif(APPLE)
    qt_generate_deploy_app_script(
        TARGET blkt
        OUTPUT_SCRIPT deploy_script
    )
    install(SCRIPT ${deploy_script})
else()
    message(STATUS "Linux package requires Qt 6 Core, Network, WebSockets and Qml runtime libraries on the target system.")
endif()
