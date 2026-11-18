# Every test run also writes its results to build/test-results
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test-results)
set(test_output_dir ${CMAKE_BINARY_DIR}/test-results)

# The engine on the sample project: indexing, the analyses, layouts
qt_add_executable(tst_core tst_core.cpp)
target_link_libraries(tst_core PRIVATE unravel_core Qt6::Test)
target_compile_definitions(tst_core PRIVATE UNRAVEL_SAMPLE_DIR="${PROJECT_SOURCE_DIR}/samples/warehouse"
                                            UNRAVEL_PYTHON_SAMPLE_DIR="${PROJECT_SOURCE_DIR}/samples/pyshop")
set_target_properties(tst_core PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
unravel_copy_libclang(tst_core)
add_test(NAME tst_core COMMAND tst_core -o ${test_output_dir}/tst_core.txt,txt -o -,txt)
add_test(NAME tst_cli_reports COMMAND ${CMAKE_COMMAND}
    -DCLI=$<TARGET_FILE:unravel-cli> -DSAMPLE=${PROJECT_SOURCE_DIR}/samples/warehouse
    -DOUTPUT_DIR=${test_output_dir} -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_reports.cmake)

foreach(feature architecture includeimpact)
    qt_add_executable(tst_${feature} tst_${feature}.cpp)
    target_link_libraries(tst_${feature} PRIVATE unravel_core Qt6::Test)
    set_target_properties(tst_${feature} PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
    unravel_copy_libclang(tst_${feature})
    add_test(NAME tst_${feature} COMMAND tst_${feature} -o ${test_output_dir}/tst_${feature}.txt,txt -o -,txt)
endforeach()

if(TARGET unravel)
    qt_add_executable(tst_hotspots tst_hotspots.cpp
        ../src/app/session.h ../src/app/session.cpp
        ../src/app/hotspots.h ../src/app/hotspots.cpp
        ../src/app/reviewfocus.h ../src/app/reviewfocus.cpp
        ../src/app/contracts.h ../src/app/contracts.cpp
        ../src/app/includeimpactmodel.h ../src/app/includeimpactmodel.cpp
        ../src/app/controlflow.h ../src/app/controlflow.cpp
        ../src/app/changes.h ../src/app/changes.cpp
        ../src/app/models.h ../src/app/models.cpp
        ../src/app/highlight.h ../src/app/highlight.cpp
        ../src/app/views.h ../src/app/views.cpp
        ../src/app/graphscene.h ../src/app/graphscene.cpp
        ../src/app/sgutil.h ../src/app/sgutil.cpp)
    target_include_directories(tst_hotspots PRIVATE ../src/app)
    target_link_libraries(tst_hotspots PRIVATE unravel_core Qt6::Test Qt6::Quick Qt6::Concurrent)
    set_target_properties(tst_hotspots PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
    unravel_copy_libclang(tst_hotspots)
    add_test(NAME tst_hotspots COMMAND tst_hotspots -o ${test_output_dir}/tst_hotspots.txt,txt -o -,txt)
    set_tests_properties(tst_hotspots PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
