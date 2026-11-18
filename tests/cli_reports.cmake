set(prefix "${OUTPUT_DIR}/cli-reports")
file(MAKE_DIRECTORY "${prefix}")
file(WRITE "${prefix}/architecture.json" [=[
{
  "version": 1,
  "layers": [
    { "id": "app", "name": "App", "paths": ["app/**"] },
    { "id": "storage", "name": "Storage", "paths": ["storage/**"] }
  ],
  "rules": [
    { "id": "app-not-storage", "from": "app", "to": "storage", "effect": "forbid", "scope": "semantic" }
  ],
  "exceptions": []
}
]=])
execute_process(
    COMMAND "${CLI}" "${SAMPLE}" --static-focus
        --focus-json "${prefix}/focus.json" --include-impact "${prefix}/includes.json"
        --architecture "${prefix}/architecture.json" --architecture-json "${prefix}/contracts.json"
        --fail-on-architecture --save "${prefix}/index.idx"
    RESULT_VARIABLE status ERROR_VARIABLE error OUTPUT_VARIABLE output TIMEOUT 30)
if(NOT status EQUAL 3)
    message(FATAL_ERROR "Expected architecture violation exit 3, got ${status}: ${error}\n${output}")
endif()
file(READ "${prefix}/focus.json" focus)
string(JSON count LENGTH "${focus}" functions)
if(NOT count EQUAL 20)
    message(FATAL_ERROR "Expected 20 unique focus entries, got ${count}")
endif()
file(READ "${prefix}/contracts.json" contracts)
string(JSON count LENGTH "${contracts}" violations)
if(count LESS 1)
    message(FATAL_ERROR "Expected forbidden app -> storage evidence")
endif()
file(READ "${prefix}/includes.json" includes)
string(JSON count LENGTH "${includes}" headers)
if(count LESS 1)
    message(FATAL_ERROR "Expected ranked header impact")
endif()
string(JSON partial GET "${includes}" partial)
if(NOT partial)
    message(FATAL_ERROR "Include estimates must retain partial-coverage warning")
endif()

execute_process(
    COMMAND "${CLI}" --load "${prefix}/index.idx" --static-focus --focus-json -
    RESULT_VARIABLE status ERROR_VARIABLE error OUTPUT_VARIABLE output TIMEOUT 30)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Static stdout export failed: ${error}")
endif()
string(JSON mode GET "${output}" mode)
if(NOT mode STREQUAL "static")
    message(FATAL_ERROR "Stdout must contain a single parseable static-priority report")
endif()

set(hunk_project "${prefix}/hunk-project")
file(REMOVE_RECURSE "${hunk_project}")
file(MAKE_DIRECTORY "${hunk_project}")
file(WRITE "${hunk_project}/fixture.cpp" "int reviewed() {\n    return 1;\n}\n")
execute_process(COMMAND git -C "${hunk_project}" init -q RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Could not initialize hunk-history fixture: ${error}")
endif()
execute_process(COMMAND git -C "${hunk_project}" add fixture.cpp RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Could not stage hunk-history fixture: ${error}")
endif()
execute_process(
    COMMAND git -C "${hunk_project}" -c user.name=Tester -c user.email=tester@example.test commit -qm initial
    RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Could not commit hunk-history fixture: ${error}")
endif()
execute_process(
    COMMAND "${CLI}" "${hunk_project}" --focus-hunks --history-days 3650 --focus-json -
    RESULT_VARIABLE status ERROR_VARIABLE error OUTPUT_VARIABLE output TIMEOUT 60)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Function-hunk CLI export failed: ${error}")
endif()
string(JSON mode GET "${output}" mode)
if(NOT mode STREQUAL "historical-function-hunk-estimate")
    message(FATAL_ERROR "Expected opt-in function-hunk attribution, got ${mode}")
endif()
string(JSON commits GET "${output}" functions 0 functionHunkCommits)
if(NOT commits GREATER 0)
    message(FATAL_ERROR "Root-commit hunks should map to the current function")
endif()

file(WRITE "${prefix}/invalid.json" "{\"version\":2}")
execute_process(
    COMMAND "${CLI}" --load "${prefix}/index.idx" --architecture "${prefix}/invalid.json"
    RESULT_VARIABLE status ERROR_VARIABLE error OUTPUT_VARIABLE output TIMEOUT 30)
if(NOT status EQUAL 2 OR error STREQUAL "")
    message(FATAL_ERROR "Malformed configuration must fail with exit 2 and a diagnostic")
endif()
