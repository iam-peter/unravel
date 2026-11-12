# Every test run also writes its results to build/test-results
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test-results)
set(test_output_dir ${CMAKE_BINARY_DIR}/test-results)

# The engine on the sample project: indexing, the analyses, layouts
qt_add_executable(tst_core tst_core.cpp)
target_link_libraries(tst_core PRIVATE unravel_core Qt6::Test)
target_compile_definitions(tst_core PRIVATE UNRAVEL_SAMPLE_DIR="${PROJECT_SOURCE_DIR}/samples/warehouse"
                                            UNRAVEL_PYTHON_SAMPLE_DIR="${PROJECT_SOURCE_DIR}/samples/pyshop")
set_target_properties(tst_core PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
add_test(NAME tst_core COMMAND tst_core -o ${test_output_dir}/tst_core.txt,txt -o -,txt)
