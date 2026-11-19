if(NOT DEFINED UNRAVEL_SOURCE_ROOT)
    set(UNRAVEL_SOURCE_ROOT "${PROJECT_SOURCE_DIR}")
endif()

if(NOT DEFINED BUILD_TESTING)
    include(CTest)
endif()

# LLVM's CMake package runs C checks (Homebrew's looks for histedit.h)
enable_language(C)

find_package(LLVM CONFIG QUIET
    HINTS
        "${UNRAVEL_LLVM_ROOT}/lib/cmake/llvm"
        "${UNRAVEL_LLVM_ROOT}/lib/llvm-*/lib/cmake/llvm"
        "/usr/lib/llvm-23/lib/cmake/llvm"
        "/usr/lib/llvm-22/lib/cmake/llvm"
        "/usr/lib/llvm-21/lib/cmake/llvm"
        "/usr/lib/llvm-20/lib/cmake/llvm"
        "/usr/lib/llvm-19/lib/cmake/llvm"
        "/usr/lib/llvm-18/lib/cmake/llvm"
        "/usr/lib/llvm-17/lib/cmake/llvm")

if(NOT LLVM_FOUND)
    message(FATAL_ERROR
        "UNRAVEL_ENABLE_FLOW=ON requires LLVM development headers and LLVMConfig.cmake. "
        "Install the matching llvm-dev package or set UNRAVEL_LLVM_ROOT; core remains libclang-only when flow is OFF.")
endif()

set(_clang_cmake_hints
    "${UNRAVEL_LLVM_ROOT}/lib/cmake/clang"
    "${LLVM_INSTALL_PREFIX}/lib/cmake/clang"
    "${LLVM_LIBRARY_DIRS}/cmake/clang")
find_package(Clang CONFIG QUIET HINTS ${_clang_cmake_hints} NO_DEFAULT_PATH)

find_path(UNRAVEL_CLANG_CPP_INCLUDE_DIR clang/Analysis/CFG.h
    HINTS "${UNRAVEL_LLVM_ROOT}/include" "${LLVM_INSTALL_PREFIX}/include"
          ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS} ${LIBCLANG_INCLUDE_DIR}
          "/usr/lib/llvm-${LLVM_VERSION_MAJOR}/include" NO_DEFAULT_PATH)
find_file(UNRAVEL_CLANG_CPP_LIBRARY
    NAMES libclang-cpp.so libclang-cpp.so.${LLVM_VERSION_MAJOR}.1 libclang-cpp.dylib clang-cpp.lib
    HINTS "${UNRAVEL_LLVM_ROOT}/lib" "${LLVM_LIBRARY_DIRS}" "${LLVM_INSTALL_PREFIX}/lib"
          "/usr/lib/llvm-${LLVM_VERSION_MAJOR}/lib" NO_DEFAULT_PATH)
if(NOT UNRAVEL_CLANG_CPP_LIBRARY)
    file(GLOB _clang_cpp_candidates
        "${UNRAVEL_LLVM_ROOT}/lib/libclang-cpp.so.*"
        "${UNRAVEL_LLVM_ROOT}/lib/libclang-cpp.dylib"
        "${UNRAVEL_LLVM_ROOT}/lib/clang-cpp.lib"
        "${LLVM_LIBRARY_DIRS}/libclang-cpp.so.*"
        "${LLVM_LIBRARY_DIRS}/libclang-cpp.dylib"
        "${LLVM_LIBRARY_DIRS}/clang-cpp.lib"
        "${LLVM_INSTALL_PREFIX}/lib/libclang-cpp.so.*")
    if(_clang_cpp_candidates)
        list(SORT _clang_cpp_candidates COMPARE NATURAL)
        list(GET _clang_cpp_candidates -1 UNRAVEL_CLANG_CPP_LIBRARY)
    endif()
endif()
find_library(UNRAVEL_LLVM_LIBRARY
    NAMES LLVM LLVM-${LLVM_VERSION_MAJOR}
    HINTS "${LLVM_LIBRARY_DIRS}" "${LLVM_INSTALL_PREFIX}/lib" NO_DEFAULT_PATH)

set(_flow_clang_targets)
if(NOT UNRAVEL_CLANG_CPP_LIBRARY AND Clang_FOUND AND
   TARGET clangTooling AND TARGET clangFrontend)
    foreach(_component
            clangTooling clangToolingCore clangToolingInclusions
            clangToolingRefactor clangFrontend clangFrontendTool
            clangSerialization clangDriver clangParse clangSema clangAnalysis
            clangASTMatchers clangAST clangLex clangBasic clangEdit clangRewrite
            clangRewriteFrontend clangIndex)
        if(TARGET ${_component})
            list(APPEND _flow_clang_targets ${_component})
        endif()
    endforeach()
endif()

set(_flow_clang_component_libraries)
if(NOT UNRAVEL_CLANG_CPP_LIBRARY AND NOT _flow_clang_targets)
    set(_flow_required_clang_components
        Tooling Frontend Serialization Driver Parse Sema Analysis AST Lex Basic)
    set(_flow_clang_components
        Tooling ToolingCore ToolingInclusions ToolingRefactor Frontend
        FrontendTool Serialization Driver Parse Sema Analysis ASTMatchers AST
        Lex Basic Edit Rewrite RewriteFrontend Index)
    foreach(_component IN LISTS _flow_clang_components)
        set(_component_var "UNRAVEL_CLANG_${_component}_LIBRARY")
        find_library(${_component_var} NAMES "clang${_component}"
            HINTS "${UNRAVEL_LLVM_ROOT}/lib" "${LLVM_LIBRARY_DIRS}"
                  "${LLVM_INSTALL_PREFIX}/lib"
                  "/usr/lib/llvm-${LLVM_VERSION_MAJOR}/lib" NO_DEFAULT_PATH)
        if(${_component_var})
            list(APPEND _flow_clang_component_libraries "${${_component_var}}")
        endif()
    endforeach()
    foreach(_component IN LISTS _flow_required_clang_components)
        if(NOT UNRAVEL_CLANG_${_component}_LIBRARY)
            set(_flow_clang_component_libraries)
            break()
        endif()
    endforeach()
endif()

set(_flow_llvm_component_libraries)
if(NOT UNRAVEL_LLVM_LIBRARY AND NOT _flow_clang_targets AND
   _flow_clang_component_libraries)
    if(COMMAND llvm_map_components_to_libnames)
        llvm_map_components_to_libnames(_flow_llvm_component_libraries all)
    endif()
endif()

if(NOT UNRAVEL_CLANG_CPP_INCLUDE_DIR OR
   (NOT UNRAVEL_CLANG_CPP_LIBRARY AND NOT _flow_clang_targets AND
    NOT _flow_clang_component_libraries) OR
   (NOT UNRAVEL_LLVM_LIBRARY AND NOT _flow_clang_targets AND
    NOT _flow_llvm_component_libraries))
    message(FATAL_ERROR
        "UNRAVEL_ENABLE_FLOW=ON requires matching LLVM and Clang development files "
        "(Analysis/Tooling headers and either libclang-cpp or exported Clang component targets). "
        "Set UNRAVEL_LLVM_ROOT to that installation.")
endif()
if(Clang_VERSION_MAJOR AND NOT Clang_VERSION_MAJOR STREQUAL LLVM_VERSION_MAJOR)
    message(FATAL_ERROR
        "UNRAVEL_ENABLE_FLOW=ON found Clang ${Clang_VERSION_MAJOR} with LLVM "
        "${LLVM_VERSION_MAJOR}; the Clang and LLVM development files must match.")
endif()

add_library(unravel_flow_lib STATIC flow.cpp flow.h facts.cpp facts.h)
target_include_directories(unravel_flow_lib
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${UNRAVEL_CLANG_CPP_INCLUDE_DIR}" ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_link_libraries(unravel_flow_lib PUBLIC unravel_core)
if(UNRAVEL_CLANG_CPP_LIBRARY)
    target_link_libraries(unravel_flow_lib PRIVATE
        "${UNRAVEL_CLANG_CPP_LIBRARY}" "${UNRAVEL_LLVM_LIBRARY}")
else()
    if(_flow_clang_targets)
        target_link_libraries(unravel_flow_lib PRIVATE ${_flow_clang_targets})
    else()
        target_link_libraries(unravel_flow_lib PRIVATE ${_flow_clang_component_libraries})
    endif()
    if(UNRAVEL_LLVM_LIBRARY)
        target_link_libraries(unravel_flow_lib PRIVATE "${UNRAVEL_LLVM_LIBRARY}")
    elseif(_flow_llvm_component_libraries)
        target_link_libraries(unravel_flow_lib PRIVATE ${_flow_llvm_component_libraries})
    endif()
endif()

add_executable(unravel-flow main.cpp)
target_link_libraries(unravel-flow PRIVATE unravel_flow_lib)
if(COMMAND unravel_copy_libclang)
    unravel_copy_libclang(unravel-flow)
endif()

if(WIN32)
    get_filename_component(_llvm_prefix "${LLVM_INSTALL_PREFIX}" ABSOLUTE)
    if(NOT UNRAVEL_LLVM_ROOT STREQUAL "")
        set(_flow_llvm_prefix "${UNRAVEL_LLVM_ROOT}")
    else()
        set(_flow_llvm_prefix "${_llvm_prefix}")
    endif()
    if(UNRAVEL_CLANG_CPP_LIBRARY MATCHES "\\.lib$")
        find_file(UNRAVEL_CLANG_CPP_DLL NAMES libclang-cpp.dll clang-cpp.dll
            HINTS "${_flow_llvm_prefix}/bin" "${LLVM_INSTALL_PREFIX}/bin")
        if(UNRAVEL_CLANG_CPP_DLL)
            add_custom_command(TARGET unravel-flow POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${UNRAVEL_CLANG_CPP_DLL}" "$<TARGET_FILE_DIR:unravel-flow>")
        endif()
    endif()
    find_file(UNRAVEL_LLVM_DLL NAMES "LLVM-${LLVM_VERSION_MAJOR}.dll" LLVM.dll
        HINTS "${_flow_llvm_prefix}/bin" "${LLVM_INSTALL_PREFIX}/bin")
    if(UNRAVEL_LLVM_DLL)
        add_custom_command(TARGET unravel-flow POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${UNRAVEL_LLVM_DLL}" "$<TARGET_FILE_DIR:unravel-flow>")
    endif()
endif()

include(GNUInstallDirs)
set_property(TARGET unravel-flow PROPERTY INSTALL_RPATH_USE_LINK_PATH FALSE)
if(APPLE AND TARGET unravel)
    set(_flow_runtime_install_dir "$<TARGET_BUNDLE_DIR_NAME:unravel>/Contents/MacOS")
    set(_flow_library_install_dir "$<TARGET_BUNDLE_DIR_NAME:unravel>/Contents/Frameworks")
    set_property(TARGET unravel-flow PROPERTY INSTALL_RPATH "@loader_path/../Frameworks")
elseif(WIN32)
    set(_flow_runtime_install_dir "${CMAKE_INSTALL_BINDIR}")
    set(_flow_library_install_dir "${CMAKE_INSTALL_BINDIR}")
else()
    set(_flow_runtime_install_dir "${CMAKE_INSTALL_BINDIR}")
    set(_flow_library_install_dir "${CMAKE_INSTALL_LIBDIR}")
    set_property(TARGET unravel-flow PROPERTY INSTALL_RPATH
        "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
if(UNIX AND NOT APPLE)
    install(TARGETS unravel-flow
        RUNTIME_DEPENDENCIES
        PRE_EXCLUDE_REGEXES "^/lib/.*" "^/lib64/.*" "^/usr/lib/.*"
        POST_EXCLUDE_REGEXES ".*libclang.*" ".*libLLVM.*"
        DIRECTORIES
            "$<TARGET_FILE_DIR:Qt6::Core>"
            "${LLVM_LIBRARY_DIRS}"
            "${LIBCLANG_INCLUDE_DIR}/../lib"
        RUNTIME DESTINATION "${_flow_runtime_install_dir}"
        LIBRARY DESTINATION "${_flow_library_install_dir}")
else()
    install(TARGETS unravel-flow RUNTIME DESTINATION "${_flow_runtime_install_dir}")
endif()

if(WIN32)
    foreach(_runtime_dll IN ITEMS
            "${LIBCLANG_DLL}" "${UNRAVEL_CLANG_CPP_DLL}" "${UNRAVEL_LLVM_DLL}")
        if(_runtime_dll AND EXISTS "${_runtime_dll}")
            install(FILES "${_runtime_dll}" DESTINATION "${_flow_library_install_dir}")
        endif()
    endforeach()
elseif(APPLE OR UNIX)
    foreach(_runtime_library IN ITEMS
            "${LIBCLANG_LIBRARY}" "${UNRAVEL_CLANG_CPP_LIBRARY}" "${UNRAVEL_LLVM_LIBRARY}")
        if(_runtime_library AND EXISTS "${_runtime_library}" AND
           _runtime_library MATCHES "\\.(dylib|so(\\.[0-9]+)*)$")
            if(UNIX AND NOT APPLE)
                install(CODE
                    "file(INSTALL DESTINATION \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_flow_library_install_dir}\" TYPE SHARED_LIBRARY FILES \"${_runtime_library}\" FOLLOW_SYMLINK_CHAIN)")
            else()
                install(FILES "${_runtime_library}" DESTINATION "${_flow_library_install_dir}")
            endif()
        endif()
    endforeach()
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.9 REQUIRED COMPONENTS Test)
    qt_add_executable(tst_controlflow "${UNRAVEL_SOURCE_ROOT}/tests/tst_controlflow.cpp")
    target_link_libraries(tst_controlflow PRIVATE unravel_flow_lib Qt6::Test)
    target_compile_definitions(tst_controlflow PRIVATE
        UNRAVEL_FLOW_FIXTURE="${UNRAVEL_SOURCE_ROOT}/tests/fixtures/flow.cpp")
    set_target_properties(tst_controlflow PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
    add_test(NAME tst_controlflow COMMAND tst_controlflow)
endif()
