add_executable(${TEST_NAME}
  ${CMAKE_CURRENT_LIST_DIR}/../main.cpp
  ${CMAKE_CURRENT_LIST_DIR}/test.cpp
)

target_link_libraries(${TEST_NAME}
  PRIVATE Catch2::Catch2
  PRIVATE ${PROJECT_NAME}_cli_support
)

target_compile_definitions(${TEST_NAME}
  PRIVATE DRAWFORGE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
add_test(
  NAME 80cli-integration
  COMMAND ${Python3_EXECUTABLE} -B
          ${CMAKE_CURRENT_LIST_DIR}/integration.py
          $<TARGET_FILE:${PROJECT_NAME}>
)
set_tests_properties(80cli-integration PROPERTIES
  FIXTURES_REQUIRED runners
  TIMEOUT 45
)

add_test(
  NAME 80cli-evaluation-v2
  COMMAND ${CMAKE_COMMAND} -E env
          DRAWFORGE_BINARY=$<TARGET_FILE:${PROJECT_NAME}>
          ${Python3_EXECUTABLE} -B -m unittest discover
          -s ${PROJECT_SOURCE_DIR}/evaluation/tests
          -p test_*.py
          -v
)
set_tests_properties(80cli-evaluation-v2 PROPERTIES
  FIXTURES_REQUIRED runners
  TIMEOUT 60
)

add_test(
  NAME 80cli-evaluation-v2-corpus
  COMMAND ${Python3_EXECUTABLE} -B
          ${PROJECT_SOURCE_DIR}/evaluation/tools/evaluate_v2.py
          verify-corpus
          --drawforge $<TARGET_FILE:${PROJECT_NAME}>
)
set_tests_properties(80cli-evaluation-v2-corpus PROPERTIES
  FIXTURES_REQUIRED runners
  TIMEOUT 60
)
