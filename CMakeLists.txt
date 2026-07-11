cmake_minimum_required(VERSION 3.24)

project(WimForge VERSION 0.1.0 LANGUAGES CXX)

option(WIMFORGE_DOCUMENTATION_CAPTURE
    "Build a demo+screenshot-only GUI with an asInvoker manifest"
    OFF)

set(WIMFORGE_BUILD_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Version embedded in WimForge binaries and release metadata")
string(REPLACE "." "," WIMFORGE_VERSION_COMMA "${WIMFORGE_BUILD_VERSION}")
set(WIMFORGE_MANIFEST_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/wimforge.manifest")
if(WIMFORGE_DOCUMENTATION_CAPTURE)
    set(WIMFORGE_MANIFEST_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/wimforge.capture.manifest")
endif()
configure_file(assets/wimforge.rc.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/wimforge.rc @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

option(WIMFORGE_CLI_ONLY
    "Build only the portable Qt Core command-line application"
    OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
if(NOT WIMFORGE_CLI_ONLY)
    find_package(Qt6 6.8 REQUIRED COMPONENTS
        Gui Qml Quick QuickControls2 QuickDialogs2 Network)
endif()

qt_standard_project_setup(REQUIRES 6.8)

set(WIMFORGE_CORE_SOURCES
    src/cli/CliRunner.cpp
    src/cli/CliRunner.h
    src/core/ActionHistory.cpp
    src/core/ActionHistory.h
    src/core/EmbeddedTerminalSession.cpp
    src/core/EmbeddedTerminalSession.h
    src/core/GitHistory.cpp
    src/core/GitHistory.h
    src/core/NotificationStore.cpp
    src/core/NotificationStore.h
    src/core/OpenCodeSetup.cpp
    src/core/OpenCodeSetup.h
    src/core/ProjectConfig.cpp
    src/core/ProjectConfig.h
    src/core/ProjectBundle.cpp
    src/core/ProjectBundle.h
    src/core/GpoCatalog.cpp
    src/core/GpoCatalog.h
    src/core/GpoPolicyCompiler.cpp
    src/core/GpoPolicyCompiler.h
    src/core/ImageSourceInspector.cpp
    src/core/ImageSourceInspector.h
    src/core/JobEngine.cpp
    src/core/JobEngine.h
    src/core/PackageStudio.cpp
    src/core/PackageStudio.h
    src/core/PayloadCatalog.cpp
    src/core/PayloadCatalog.h
    src/core/ProcessLaunch.cpp
    src/core/ProcessLaunch.h
    src/core/ServicingPlan.cpp
    src/core/ServicingPlan.h
    src/core/StructuredLogger.cpp
    src/core/StructuredLogger.h
    src/core/SearchIndex.cpp
    src/core/SearchIndex.h
    src/core/UnattendBuilder.cpp
    src/core/UnattendBuilder.h
    src/core/UpdateCatalog.cpp
    src/core/UpdateCatalog.h
    src/core/VmLab.cpp
    src/core/VmLab.h
    src/core/VmLabCliService.cpp
    src/core/VmLabCliService.h
    src/core/VmLabManager.cpp
    src/core/VmLabManager.h
    src/core/VmLabProviders.cpp
    src/core/VmLabProviders.h
    src/core/VmLabScope.cpp
    src/core/VmLabScope.h
    src/core/VmValidationStore.cpp
    src/core/VmValidationStore.h
    src/core/VmLabVmx.cpp
    src/core/VmLabVmx.h
    src/core/WorkspaceTabs.cpp
    src/core/WorkspaceTabs.h
    src/core/WinForgeBridge.cpp
    src/core/WinForgeBridge.h
)

qt_add_executable(WimForgeCli
    src/cli_main.cpp
    ${WIMFORGE_CORE_SOURCES}
)

target_include_directories(WimForgeCli PRIVATE src)
target_compile_definitions(WimForgeCli PRIVATE
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    WIMFORGE_VERSION="${WIMFORGE_BUILD_VERSION}"
)
target_link_libraries(WimForgeCli PRIVATE Qt6::Core)
if(WIN32)
    target_link_libraries(WimForgeCli PRIVATE shell32)
endif()

if(MSVC)
    target_compile_options(WimForgeCli PRIVATE /W4 /permissive- /utf-8 /FS)
else()
    target_compile_options(WimForgeCli PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS WimForgeCli RUNTIME DESTINATION .)

if(NOT WIMFORGE_CLI_ONLY)
    qt_add_executable(WimForge WIN32
        src/main.cpp
        src/startup/Elevation.cpp
        src/startup/Elevation.h
        src/AppController.cpp
        src/AppController.h
        ${WIMFORGE_CORE_SOURCES}
        ${CMAKE_CURRENT_BINARY_DIR}/generated/wimforge.rc
    )

    qt_add_executable(WimForgeIconTool EXCLUDE_FROM_ALL tools/icon_converter.cpp)
    target_link_libraries(WimForgeIconTool PRIVATE Qt6::Gui)

    set_source_files_properties(qml/components/DesignTokens.qml PROPERTIES
        QT_QML_SINGLETON_TYPE TRUE)

    qt_add_qml_module(WimForge
        URI WimForge
        VERSION 1.0
        QML_FILES
            qml/Main.qml
            qml/components/DesignTokens.qml
            qml/components/WfCard.qml
            qml/components/WfButton.qml
            qml/components/WfStatusChip.qml
            qml/components/WfPageHeader.qml
            qml/components/WfField.qml
            qml/components/WfIconButton.qml
            qml/components/WfIcon.qml
            qml/components/UpdateCatalogSheet.qml
            qml/components/ProjectStartPage.qml
            qml/components/MetricCard.qml
            qml/components/NotificationCenter.qml
            qml/components/ContextHistoryPanel.qml
            qml/components/Snackbar.qml
            qml/components/SearchPalette.qml
            qml/pages/CustomizePage.qml
            qml/pages/DashboardPage.qml
            qml/pages/HistoryPage.qml
            qml/pages/GpoStudioPage.qml
            qml/pages/PackageStudioPage.qml
            qml/pages/PlanPage.qml
            qml/pages/SettingsPage.qml
            qml/pages/SourcePage.qml
            qml/pages/TerminalPage.qml
            qml/pages/UnattendedStudioPage.qml
            qml/pages/VmLabPage.qml
            qml/pages/WinForgeBridgePage.qml
        RESOURCES
            assets/app-icon.svg
            assets/icons/overview.svg
            assets/icons/source.svg
            assets/icons/customize.svg
            assets/icons/policy.svg
            assets/icons/unattended.svg
            assets/icons/package.svg
            assets/icons/bridge.svg
            assets/icons/vm.svg
            assets/icons/run.svg
            assets/icons/history.svg
            assets/icons/settings.svg
            assets/icons/terminal.svg
    )

    target_include_directories(WimForge PRIVATE src)
    target_compile_definitions(WimForge PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
        WIMFORGE_VERSION="${WIMFORGE_BUILD_VERSION}"
    )
    if(WIMFORGE_DOCUMENTATION_CAPTURE)
        target_compile_definitions(WimForge PRIVATE
            WIMFORGE_DOCUMENTATION_CAPTURE=1)
    endif()
    target_link_libraries(WimForge PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickDialogs2
        Qt6::Network
    )
    if(WIN32)
        target_link_libraries(WimForge PRIVATE advapi32 shell32)
    endif()

    if(MSVC)
        target_compile_options(WimForge PRIVATE /W4 /permissive- /utf-8 /FS)
        # The application resource already embeds the audited DPI/elevation
        # manifest. Prevent link.exe from adding a second resource with ID 1.
        target_link_options(WimForge PRIVATE /MANIFEST:NO)
    else()
        target_compile_options(WimForge PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    set_target_properties(WimForge PROPERTIES
        OUTPUT_NAME "WimForge"
        WIN32_EXECUTABLE TRUE
    )

    install(TARGETS WimForge
        BUNDLE DESTINATION .
        RUNTIME DESTINATION .
    )

    qt_generate_deploy_qml_app_script(
        TARGET WimForge
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${deploy_script})
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
