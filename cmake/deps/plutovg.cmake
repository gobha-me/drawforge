# Phase 1 renderer selected by ADR-0001. Prefer the exact installed package so
# pixel behavior and reported renderer metadata cannot silently drift.
find_package(plutovg 1.3.3 EXACT CONFIG QUIET)

if (NOT plutovg_FOUND)
  if (NOT PlutoVG_URI)
    set(PlutoVG_URI https://github.com/sammycage/plutovg.git)
  endif ()
  if (NOT PlutoVG_TAG)
    set(PlutoVG_TAG bbd91f0d06a71491691b36330f29dffa4af87ccf)
  endif ()

  include(FetchContent)

  # PlutoVG v1.3.3 has unconditional install rules. Include them when this
  # top-level project is producing an installed DrawForge package, because the
  # exported static library needs plutovg::plutovg beside it. Exclude the
  # dependency subdirectory otherwise so an embedded DrawForge does not install
  # PlutoVG files into its consumer's prefix.
  set(_plutovg_fetch_options "")
  if (NOT ${PROJECT_NAME}_INSTALL)
    list(APPEND _plutovg_fetch_options EXCLUDE_FROM_ALL)
  endif ()

  set(_drawforge_had_build_shared_libs FALSE)
  if (DEFINED BUILD_SHARED_LIBS)
    set(_drawforge_had_build_shared_libs TRUE)
    set(_drawforge_saved_build_shared_libs ${BUILD_SHARED_LIBS})
  endif ()
  set(BUILD_SHARED_LIBS OFF)
  set(PLUTOVG_BUILD_EXAMPLES OFF)
  set(PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD ON)

  FetchContent_Declare(
    plutovg
    GIT_REPOSITORY ${PlutoVG_URI}
    GIT_TAG ${PlutoVG_TAG}
    GIT_SHALLOW FALSE
    ${_plutovg_fetch_options}
  )
  FetchContent_MakeAvailable(plutovg)

  if (_drawforge_had_build_shared_libs)
    set(BUILD_SHARED_LIBS ${_drawforge_saved_build_shared_libs})
  else ()
    unset(BUILD_SHARED_LIBS)
  endif ()
  unset(_drawforge_had_build_shared_libs)
  unset(_drawforge_saved_build_shared_libs)
  unset(_plutovg_fetch_options)
endif ()
