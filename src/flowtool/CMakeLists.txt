if(NOT DEFINED UNRAVEL_SOURCE_ROOT)
    set(UNRAVEL_SOURCE_ROOT "${PROJECT_SOURCE_DIR}")
endif()

if(NOT DEFINED BUILD_TESTING)
    include(CTest)
endif()

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

find_path(UNRAVEL_CLANG_CPP_INCLUDE_DIR clang/Analysis/CFG.h
    HINTS ${LIBCLANG_INCLUDE_DIR} ${LLVM_INCLUDE_DIRS}
          "${UNRAVEL_LLVM_ROOT}/include" "${LLVM_INSTALL_PREFIX}/include"
          "/usr/lib/llvm-${LLVM_VERSION_MAJOR}/include")
find_file(UNRAVEL_CLANG_CPP_LIBRARY
    NAMES libclang-cpp.so libclang-cpp.so.${LLVM_VERSION_MAJOR}.1 libclang-cpp.dylib clang-cpp.lib
    HINTS "${UNRAVEL_LLVM_ROOT}/lib" "${LLVM_LIBRARY_DIRS}" "${LLVM_INSTALL_PREFIX}/lib"
          "/usr/lib/llvm-${LLVM_VERSION_MAJOR}/lib")
if(NOT UNRAVEL_CLANG_CPP_LIBRARY)
    file(GLOB _clang_cpp_candidates
        "${UNRAVEL_LLVM_ROOT}/lib/libclang-cpp.so.*"
        "${LLVM_LIBRARY_DIRS}/libclang-cpp.so.*"
        "${LLVM_INSTALL_PREFIX}/lib/libclang-cpp.so.*")
    if(_clang_cpp_candidates)
        list(SORT _clang_cpp_candidates COMPARE NATURAL)
        list(GET _clang_cpp_candidates -1 UNRAVEL_CLANG_CPP_LIBRARY)
    endif()
endif()
find_library(UNRAVEL_LLVM_LIBRARY
    NAMES LLVM-${LLVM_VERSION_MAJOR} LLVM
    HINTS "${LLVM_LIBRARY_DIRS}" "${LLVM_INSTALL_PREFIX}/lib")

if(NOT UNRAVEL_CLANG_CPP_INCLUDE_DIR OR NOT UNRAVEL_CLANG_CPP_LIBRARY OR NOT UNRAVEL_LLVM_LIBRARY)
    message(FATAL_ERROR
        "UNRAVEL_ENABLE_FLOW=ON requires Clang Analysis/Tooling headers, libclang-cpp, and libLLVM "
        "from one LLVM installation. Set UNRAVEL_LLVM_ROOT to that installation.")
endif()

add_library(unravel_flow_lib STATIC flow.cpp flow.h)
target_include_directories(unravel_flow_lib
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${UNRAVEL_CLANG_CPP_INCLUDE_DIR}" ${LLVM_INCLUDE_DIRS})
target_link_libraries(unravel_flow_lib PUBLIC unravel_core PRIVATE
    "${UNRAVEL_CLANG_CPP_LIBRARY}" "${UNRAVEL_LLVM_LIBRARY}")

add_executable(unravel-flow main.cpp)
target_link_libraries(unravel-flow PRIVATE unravel_flow_lib)

if(BUILD_TESTING)
    find_package(Qt6 6.9 REQUIRED COMPONENTS Test)
    qt_add_executable(tst_controlflow "${UNRAVEL_SOURCE_ROOT}/tests/tst_controlflow.cpp")
    target_link_libraries(tst_controlflow PRIVATE unravel_flow_lib Qt6::Test)
    target_compile_definitions(tst_controlflow PRIVATE
        UNRAVEL_FLOW_FIXTURE="${UNRAVEL_SOURCE_ROOT}/tests/fixtures/flow.cpp")
    set_target_properties(tst_controlflow PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
    add_test(NAME tst_controlflow COMMAND tst_controlflow)
endif()
