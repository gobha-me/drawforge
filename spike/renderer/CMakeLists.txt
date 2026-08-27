cmake_minimum_required(VERSION 3.28)

project(drawforge_renderer_spike LANGUAGES C CXX)

set(DRAWFORGE_SPIKE_BACKEND "" CACHE STRING "Renderer backend: blend2d, plutovg, or cairo")
set_property(CACHE DRAWFORGE_SPIKE_BACKEND PROPERTY STRINGS blend2d plutovg cairo)

if(NOT DRAWFORGE_SPIKE_BACKEND MATCHES "^(blend2d|plutovg|cairo)$")
  message(FATAL_ERROR "Set DRAWFORGE_SPIKE_BACKEND to blend2d, plutovg, or cairo")
endif()

include(FetchContent)

add_executable(drawforge-renderer-spike
  src/main.cpp
  "src/backend_${DRAWFORGE_SPIKE_BACKEND}.cpp"
)
target_compile_features(drawforge-renderer-spike PRIVATE cxx_std_23)
target_include_directories(drawforge-renderer-spike PRIVATE src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(drawforge-renderer-spike PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(DRAWFORGE_SPIKE_BACKEND STREQUAL "blend2d")
  set(BLEND2D_STATIC ON CACHE BOOL "" FORCE)
  set(BLEND2D_TEST OFF CACHE BOOL "" FORCE)
  set(BLEND2D_DEMOS OFF CACHE BOOL "" FORCE)
  set(BLEND2D_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(blend2d
    URL https://blend2d.com/download/blend2d-0.21.2.tar.gz
    URL_HASH SHA256=234ba57a64153c96885a6271f15fdcf6c594ac8cb03adaad19abb8d3b2b533fa
  )
  FetchContent_MakeAvailable(blend2d)
  target_link_libraries(drawforge-renderer-spike PRIVATE blend2d::blend2d)
  target_compile_definitions(drawforge-renderer-spike PRIVATE
    DRAWFORGE_SPIKE_BACKEND_VERSION="0.21.2"
  )
elseif(DRAWFORGE_SPIKE_BACKEND STREQUAL "plutovg")
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(PLUTOVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(PLUTOVG_ENABLE_IMAGE_WRITE ON CACHE BOOL "" FORCE)
  set(PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD ON CACHE BOOL "" FORCE)
  FetchContent_Declare(plutovg
    GIT_REPOSITORY https://github.com/sammycage/plutovg.git
    GIT_TAG bbd91f0d06a71491691b36330f29dffa4af87ccf
    GIT_SHALLOW FALSE
  )
  FetchContent_MakeAvailable(plutovg)
  target_link_libraries(drawforge-renderer-spike PRIVATE plutovg::plutovg)
  target_compile_definitions(drawforge-renderer-spike PRIVATE
    DRAWFORGE_SPIKE_BACKEND_VERSION="1.3.3"
  )
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo=1.18.4)
  target_link_libraries(drawforge-renderer-spike PRIVATE PkgConfig::CAIRO)
  target_compile_definitions(drawforge-renderer-spike PRIVATE
    DRAWFORGE_SPIKE_BACKEND_VERSION="1.18.4"
  )
endif()
