#include <catch2/catch_test_macros.hpp>

#include <drawforge/drawforge.hpp>
#include <version.hpp>

TEST_CASE("project metadata is provided by the compiled library",
          "[foundation][metadata]") {
  const auto info = drawforge::project_info();

  REQUIRE(info.name == PROGRAM_NAME);
  REQUIRE(info.version.major == VERSION_MAJOR);
  REQUIRE(info.version.minor == VERSION_MINOR);
  REQUIRE(info.version.patch == VERSION_PATCH);
  REQUIRE(info.version.tweak == VERSION_TWEAK);
  REQUIRE(info.version.dirty == VERSION_DIRTY);
  REQUIRE(drawforge::stage_name(info.stage) == "experimental");
}
