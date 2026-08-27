# ── Library target ──────────────────────────────────────────────────────────
# DrawForge is a compiled static library. Disable it entirely with:
#   cmake -B build -D<PROJECT>_BUILD_LIB=OFF
# (consumers that only want the bin, or none of the lib, can opt out.)
#
# OFF removes the target outright, so nothing can link it — including the
# auto-discovered tests, which pick it up automatically when it exists (see
# test/CMakeLists.txt). Pair OFF with -D<PROJECT>_TESTS=OFF, or keep only tests
# that do not call into this library.

option(${PROJECT_NAME}_BUILD_LIB "Build the compiled library target" ON)

if (${PROJECT_NAME}_BUILD_LIB)
  add_library(${PROJECT_NAME}_lib STATIC
    foundation.cpp
    lib.cpp
  )

  # Compiled libs get the project's include dir PUBLIC so consumers (bin,
  # tests, downstream) inherit it transitively.
  #
  # The two generator expressions are not decoration. PROJECT_SOURCE_DIR is this
  # project's root; CMAKE_SOURCE_DIR would be the *consumer's* root the moment
  # someone add_subdirectory()s us, silently pointing the include path at the
  # wrong tree. And a bare absolute source path in a PUBLIC include directory
  # makes install(TARGETS ... EXPORT ...) fail outright — an installed package
  # must describe where its headers landed, not where they were built from.
  target_include_directories(${PROJECT_NAME}_lib
    PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )

  # The C++ standard is a property of this library's public header, not of the
  # build that happens to be compiling it. cmake/toolchain/default.cmake sets
  # CMAKE_CXX_STANDARD for *our* builds, but a consumer uses their own toolchain
  # — without this, they compile DrawForge headers at whatever their default is.
  target_compile_features(${PROJECT_NAME}_lib PUBLIC cxx_std_23)

  add_library(${PROJECT_NAME}::lib ALIAS ${PROJECT_NAME}_lib)
endif ()
