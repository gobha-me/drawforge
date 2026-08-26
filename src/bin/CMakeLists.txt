add_executable(${PROJECT_NAME}
  main.cpp
)

# PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the latter is the top-level build's
# root, which belongs to someone else as soon as this project is consumed.
target_include_directories(${PROJECT_NAME}
  PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME}
  PRIVATE ${PROJECT_NAME}::lib
)
