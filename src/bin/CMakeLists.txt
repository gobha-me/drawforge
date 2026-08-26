add_executable(${PROJECT_NAME}
  main.cpp
)

# PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the latter is the top-level build's
# root, which belongs to someone else as soon as this project is consumed.
target_include_directories(${PROJECT_NAME}
  PUBLIC ${PROJECT_SOURCE_DIR}/include
)

if (TARGET ${PROJECT_NAME}::lib)
  target_link_libraries(${PROJECT_NAME}
    PRIVATE ${PROJECT_NAME}::lib
  )
  target_compile_definitions(${PROJECT_NAME} PRIVATE DRAWFORGE_HAS_LIB=1)
endif ()
