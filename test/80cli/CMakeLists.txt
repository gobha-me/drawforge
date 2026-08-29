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
