cmake_minimum_required(VERSION 3.24)

project(WimForge VERSION 0.1.0 LANGUAGES CXX)

set(WIMFORGE_BUILD_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Version embedded in WimForge binaries and release metadata")
string(REPLACE "." "," WIMFORGE_VERSION_COMMA "${WIMFORGE_BUILD_VERSION}")
configure_file(assets/wimforge.rc.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/wimforge.rc @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.8)

set(WIMFORGE_CORE_SOURCES
    src/cli/CliRunner.cpp
    src/cli/CliRunner.h
    src/core/ActionHistory.cpp
    src/core/ActionHistory.h
    src/core/GitHistory.cpp
    src/core/GitHistory.h
    src/core/NotificationStore.cpp
    src/core/NotificationStore.h
    src/core/ProjectConfig.cpp
    src/core/ProjectConfig.h
    src/core/ProjectBundle.cpp
    src/core/ProjectBundle.h
    src/core/GpoCatalog.cpp
    src/core/GpoCatalog.h
    src/core/GpoPolicyCompiler.cpp
    src/core/GpoPolicyCompiler.h
    src/core/JobEngine.cpp
    src/core/JobEngine.h
    src/core/PackageStudio.cpp
    src/core/PackageStudio.h
    src/core/ServicingPlan.cpp
    src/core/ServicingPlan.h
    src/core/UnattendBuilder.cpp
    src/core/UnattendBuilder.h
    src/core/WinForgeBridge.cpp
    src/core/WinForgeBridge.h
)

qt_add_executable(WimForge WIN32
    src/main.cpp
    src/AppController.cpp
    src/AppController.h
    ${WIMFORGE_CORE_SOURCES}
    ${CMAKE_CURRENT_BINARY_DIR}/generated/wimforge.rc
)

qt_add_executable(WimForgeCli
    src/cli_main.cpp
    ${WIMFORGE_CORE_SOURCES}
)

qt_add_executable(WimForgeIconTool EXCLUDE_FROM_ALL tools/icon_converter.cpp)
target_link_libraries(WimForgeIconTool PRIVATE Qt6::Gui)

qt_add_qml_module(WimForge
    URI WimForge
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/components/MetricCard.qml
        qml/components/NotificationCenter.qml
        qml/components/ContextHistoryPanel.qml
        qml/components/Snackbar.qml
        qml/pages/CustomizePage.qml
        qml/pages/DashboardPage.qml
        qml/pages/HistoryPage.qml
        qml/pages/GpoStudioPage.qml
        qml/pages/PackageStudioPage.qml
        qml/pages/PlanPage.qml
        qml/pages/SettingsPage.qml
        qml/pages/SourcePage.qml
        qml/pages/UnattendedStudioPage.qml
        qml/pages/WinForgeBridgePage.qml
    RESOURCES
        assets/app-icon.svg
)

target_include_directories(WimForge PRIVATE src)
target_include_directories(WimForgeCli PRIVATE src)
target_compile_definitions(WimForge PRIVATE
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    WIMFORGE_VERSION="${WIMFORGE_BUILD_VERSION}"
)
target_compile_definitions(WimForgeCli PRIVATE
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    WIMFORGE_VERSION="${WIMFORGE_BUILD_VERSION}"
)

target_link_libraries(WimForge PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)
target_link_libraries(WimForgeCli PRIVATE Qt6::Core)

if(MSVC)
    target_compile_options(WimForge PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(WimForgeCli PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(WimForge PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(WimForgeCli PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(WimForge PROPERTIES
    OUTPUT_NAME "WimForge"
    WIN32_EXECUTABLE TRUE
)

install(TARGETS WimForge
    BUNDLE DESTINATION .
    RUNTIME DESTINATION .
)
install(TARGETS WimForgeCli RUNTIME DESTINATION .)

qt_generate_deploy_qml_app_script(
    TARGET WimForge
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
