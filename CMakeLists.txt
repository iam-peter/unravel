cmake_minimum_required(VERSION 3.21)

project(Unravel VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(UNRAVEL_ENABLE_FLOW "Build the optional experimental Clang CFG helper" OFF)

find_package(Qt6 6.9 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Svg Concurrent)
qt_standard_project_setup(REQUIRES 6.8)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# An installed unravel (cmake --install, e.g. into ~/.local) finds the Qt
# and the libclang it was built against
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
if(MSVC)
    # The sources are UTF-8 (arrows and dashes in strings); the indexer is big
    add_compile_options(/utf-8 /bigobj /permissive- /Zc:__cplusplus)
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
# Lay QML modules out by URI so qmllint and qmlls find them (as igitt does)
set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)

# --- libclang ---------------------------------------------------------------
# clang does the parsing. Its stable C interface (libclang) is all unravel
# needs; UNRAVEL_LLVM_ROOT points at an LLVM installation when it isn't one
# of the usual ones.
set(UNRAVEL_LLVM_ROOT "" CACHE PATH "LLVM installation with libclang (include/clang-c and lib/libclang)")
set(_llvm_hints ${UNRAVEL_LLVM_ROOT})
foreach(v 23 22 21 20 19 18 17)
    list(APPEND _llvm_hints /usr/lib/llvm-${v} /usr/local/opt/llvm@${v} /opt/homebrew/opt/llvm@${v})
endforeach()
list(APPEND _llvm_hints /usr/local/opt/llvm /opt/homebrew/opt/llvm "C:/Program Files/LLVM" /usr/local /usr)
foreach(h ${_llvm_hints})
    if(EXISTS "${h}/include/clang-c/Index.h")
        find_library(LIBCLANG_LIBRARY NAMES clang libclang PATHS "${h}/lib" NO_DEFAULT_PATH)
        if(LIBCLANG_LIBRARY)
            set(LIBCLANG_INCLUDE_DIR "${h}/include" CACHE PATH "libclang headers")
            break()
        endif()
    endif()
endforeach()
if(NOT LIBCLANG_LIBRARY OR NOT LIBCLANG_INCLUDE_DIR)
    message(FATAL_ERROR "libclang not found: install libclang-dev (or LLVM) or set UNRAVEL_LLVM_ROOT")
endif()
message(STATUS "libclang: ${LIBCLANG_LIBRARY} (${LIBCLANG_INCLUDE_DIR})")
add_library(libclang UNKNOWN IMPORTED)
set_target_properties(libclang PROPERTIES
    IMPORTED_LOCATION ${LIBCLANG_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${LIBCLANG_INCLUDE_DIR})
# clang's own headers (stddef.h, stdarg.h, ...) in this LLVM installation:
# the indexer's last resort when none are next to the program, which is
# how builds and tests run from the build folder. libclang finds them by
# itself on some systems only (not with Homebrew's LLVM).
get_filename_component(_llvm_root "${LIBCLANG_INCLUDE_DIR}" DIRECTORY)
file(GLOB _clang_resource_headers "${_llvm_root}/lib/clang/*/include/stddef.h")
set(UNRAVEL_CLANG_BUILD_RESOURCE_DIR "")
if(_clang_resource_headers)
    list(SORT _clang_resource_headers COMPARE NATURAL ORDER DESCENDING)
    list(GET _clang_resource_headers 0 _clang_stddef)
    get_filename_component(_clang_include "${_clang_stddef}" DIRECTORY)
    get_filename_component(UNRAVEL_CLANG_BUILD_RESOURCE_DIR "${_clang_include}" DIRECTORY)
endif()
message(STATUS "clang resource directory: ${UNRAVEL_CLANG_BUILD_RESOURCE_DIR}")
# On Windows the import library is in lib, the DLL in bin: copied next to
# the programs, so they and the tests start from the build folder
if(WIN32)
    get_filename_component(_llvm_prefix "${LIBCLANG_INCLUDE_DIR}" DIRECTORY)
    find_file(LIBCLANG_DLL NAMES libclang.dll PATHS "${_llvm_prefix}/bin" NO_DEFAULT_PATH)
endif()
# with clang's own headers in lib/clang, where the indexer looks for them
function(unravel_copy_libclang target)
    if(LIBCLANG_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBCLANG_DLL}" "$<TARGET_FILE_DIR:${target}>")
        if(EXISTS "${_llvm_prefix}/lib/clang")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${_llvm_prefix}/lib/clang" "$<TARGET_FILE_DIR:${target}>/lib/clang")
        endif()
    endif()
endfunction()

# --- The engine: no GUI ------------------------------------------------------
# The Python indexer runs with the system's python3; it is compiled in as text
set(python_script ${CMAKE_CURRENT_SOURCE_DIR}/src/core/python/unravel_python.py)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${python_script})
# As bytes: string literals that long don't compile everywhere
file(READ ${python_script} python_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," python_bytes "${python_hex}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/unravel_python_script.h.new
     "#pragma once\n// Made by CMake from src/core/python/unravel_python.py\n"
     "static const unsigned char kPythonScript[] = {${python_bytes} 0};\n")
# Only touched when it changed, so nothing is rebuilt for nothing
configure_file(${CMAKE_CURRENT_BINARY_DIR}/generated/unravel_python_script.h.new
               ${CMAKE_CURRENT_BINARY_DIR}/generated/unravel_python_script.h COPYONLY)

add_library(unravel_core STATIC
    src/core/model.h src/core/model.cpp
    src/core/project.h src/core/project.cpp
    src/core/indexer.h src/core/indexer.cpp
    src/core/analysis.h src/core/analysis.cpp
    src/core/layout.h src/core/layout.cpp
    src/core/report.h src/core/report.cpp
    src/core/history.h src/core/history.cpp
    src/core/focus.h src/core/focus.cpp
    src/core/architecture.h src/core/architecture.cpp
    src/core/includeimpact.h src/core/includeimpact.cpp
    src/core/python.h src/core/python.cpp
)
target_include_directories(unravel_core PUBLIC src/core)
target_include_directories(unravel_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(unravel_core PUBLIC Qt6::Core libclang)
target_compile_definitions(unravel_core PRIVATE UNRAVEL_CLANG_BUILD_RESOURCE_DIR="${UNRAVEL_CLANG_BUILD_RESOURCE_DIR}")
set_target_properties(unravel_core PROPERTIES AUTOMOC OFF)

# The command line: index a project, print what was found, write the report
qt_add_executable(unravel-cli src/cli.cpp)
target_link_libraries(unravel-cli PRIVATE unravel_core)
set_target_properties(unravel-cli PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
unravel_copy_libclang(unravel-cli)

if(NOT UNRAVEL_CORE_ONLY)
    # The controls' style and the themes, taken unchanged from igitt as Cache
    # does (IgittStyle, see style/)
    add_subdirectory(style)

    # --- The application: Qt Quick views over the engine ---------------------
    qt_add_executable(unravel WIN32 MACOSX_BUNDLE src/app/main.cpp)
    target_compile_definitions(unravel PRIVATE PROJECT_VERSION_STRING="${PROJECT_VERSION}")
    set_target_properties(unravel PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME Unravel
        MACOSX_BUNDLE_GUI_IDENTIFIER io.github.iam-peter.unravel
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_ICON_FILE unravel.icns)
    if(WIN32)
        target_sources(unravel PRIVATE logo/unravel.rc)
    elseif(APPLE)
        target_sources(unravel PRIVATE logo/unravel.icns)
        set_source_files_properties(logo/unravel.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    endif()
    unravel_copy_libclang(unravel)

    file(GLOB icon_files RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS icons/*.svg)
    set_source_files_properties(qml/Theme.qml qml/Icons.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

    qt_add_qml_module(unravel
        URI Unravel
        VERSION 1.0
        QML_FILES
            qml/Main.qml
            qml/Theme.qml
            qml/Icons.qml
            qml/Icon.qml
            qml/IconButton.qml
            qml/SplitterHandle.qml
            qml/StartPage.qml
            qml/SearchBox.qml
            qml/Sidebar.qml
            qml/InfoPanel.qml
            qml/CodeView.qml
            qml/GraphCanvas.qml
            qml/NodeCard.qml
            qml/ReferenceList.qml
            qml/KindToggles.qml
            qml/Legend.qml
            qml/ViewBar.qml
            qml/GraphView.qml
            qml/FlowView.qml
            qml/FlowPage.qml
            qml/RelationsPanel.qml
            qml/EventsPanel.qml
            qml/ControlFlowPanel.qml
            qml/ArchitectureView.qml
            qml/MatrixPage.qml
            qml/BundlesPage.qml
            qml/TreemapPage.qml
            qml/MetricsView.qml
            qml/HistoryBar.qml
            qml/ReviewPage.qml
            qml/FocusView.qml
            qml/ContractsPanel.qml
            qml/IncludeImpactPanel.qml
            qml/FindingsView.qml
            qml/ChangesView.qml
            qml/FoldMenu.qml
            qml/SettingsDialog.qml
            qml/ErrorsDialog.qml
            qml/ShortcutsDialog.qml
        SOURCES
            src/app/session.h src/app/session.cpp
            src/app/changes.h src/app/changes.cpp
            src/app/hotspots.h src/app/hotspots.cpp
            src/app/reviewfocus.h src/app/reviewfocus.cpp
            src/app/contracts.h src/app/contracts.cpp
            src/app/includeimpactmodel.h src/app/includeimpactmodel.cpp
            src/app/controlflow.h src/app/controlflow.cpp
            src/app/models.h src/app/models.cpp
            src/app/graphscene.h src/app/graphscene.cpp
            src/app/views.h src/app/views.cpp
            src/app/sgutil.h src/app/sgutil.cpp
            src/app/highlight.h src/app/highlight.cpp
            src/app/gridbackground.h src/app/gridbackground.cpp
            src/app/Fonts.h src/app/Fonts.cpp
        RESOURCES
            ${icon_files}
            logo/unravel.svg
            images/hatch.svg
            fonts/Inter-Bold.ttf
            fonts/Inter-Italic.ttf
            fonts/Inter-Medium.ttf
            fonts/Inter-Regular.ttf
            fonts/Inter-SemiBold.ttf
            fonts/JetBrainsMono-Bold.ttf
            fonts/JetBrainsMono-Italic.ttf
            fonts/JetBrainsMono-Regular.ttf
    )
    target_include_directories(unravel PRIVATE src/app)
    target_link_libraries(unravel PRIVATE
        unravel_core
        igitt_style
        igitt_styleplugin
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Concurrent
        # Not called directly: linking it makes the deploy tools bring the
        # SVG image plugin the icons need
        Qt6::Svg)

    include(GNUInstallDirs)
    install(TARGETS unravel unravel-cli BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(UNIX AND NOT APPLE)
        install(FILES logo/unravel.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(FILES logo/unravel.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
        # Running menus and finders (GTK) read an icon folder once and look
        # again only when the theme's folder changes, as the icon theme
        # specification asks installers to make it do
        install(CODE "file(TOUCH_NOCREATE \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor\")")
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
if(UNRAVEL_ENABLE_FLOW)
    add_subdirectory(src/flowtool)
    if(TARGET unravel)
        add_dependencies(unravel unravel-flow)
    endif()
    include(GNUInstallDirs)
    install(TARGETS unravel-flow RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
