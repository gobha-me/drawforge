# The production protocol adapter is executable-private. Prefer an installed
# target, but verify the exact version in the translation unit because yyjson's
# installed CMake package does not ship a ConfigVersion file.
find_package(yyjson CONFIG QUIET)

if (NOT TARGET yyjson::yyjson AND NOT TARGET yyjson)
  if (NOT YYJSON_URI)
    set(YYJSON_URI https://github.com/ibireme/yyjson.git)
  endif ()
  if (NOT YYJSON_TAG)
    set(YYJSON_TAG 8b4a38dc994a110abaec8a400615567bd996105f)
  endif ()

  include(FetchContent)

  set(_drawforge_had_build_shared_libs FALSE)
  if (DEFINED BUILD_SHARED_LIBS)
    set(_drawforge_had_build_shared_libs TRUE)
    set(_drawforge_saved_build_shared_libs ${BUILD_SHARED_LIBS})
  endif ()

  set(BUILD_SHARED_LIBS OFF)
  set(YYJSON_BUILD_TESTS OFF)
  set(YYJSON_BUILD_FUZZER OFF)
  set(YYJSON_BUILD_MISC OFF)
  set(YYJSON_BUILD_DOC OFF)
  set(YYJSON_INSTALL OFF)
  set(YYJSON_DISABLE_UTILS ON)
  set(YYJSON_DISABLE_NON_STANDARD ON)

  FetchContent_Declare(
    yyjson
    GIT_REPOSITORY ${YYJSON_URI}
    GIT_TAG ${YYJSON_TAG}
    GIT_SHALLOW FALSE
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(yyjson)

  if (_drawforge_had_build_shared_libs)
    set(BUILD_SHARED_LIBS ${_drawforge_saved_build_shared_libs})
  else ()
    unset(BUILD_SHARED_LIBS)
  endif ()
  unset(_drawforge_had_build_shared_libs)
  unset(_drawforge_saved_build_shared_libs)
endif ()

if (TARGET yyjson AND NOT TARGET yyjson::yyjson)
  add_library(yyjson::yyjson ALIAS yyjson)
endif ()

if (NOT TARGET yyjson::yyjson)
  message(FATAL_ERROR "yyjson was found but did not provide yyjson::yyjson or yyjson")
endif ()
