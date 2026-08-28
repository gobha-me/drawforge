// A downstream project, in miniature. Its whole job is to prove that this
// DrawForge's library can be *consumed* — the same way a real project would use
// it, and identically across all three acquisition modes (see verify.sh).
//
// It prints the project name so the caller can check it, which is what makes
// this a link test rather than a compile test: project_info() is declared in
// the public header and defined in the library's translation unit, so a build
// that gets the include directory but not the archive fails at link. That
// distinction is the reason the public header exists at all — a header of pure
// constexpr would compile and "pass" while linking nothing.

#include <drawforge/drawforge.hpp>

#include <cstdio>

// The library advertises cxx_std_23 as a PUBLIC usage requirement, so a
// consumer inherits it even though it never set CMAKE_CXX_STANDARD and never
// used this project's toolchain files. Delete that line from
// src/lib/CMakeLists.txt and this is what goes red.
//
// Checked with a C++23 language feature-test macro rather than with
// __cplusplus, because that value is not a reliable "is this C++23" signal:
// CMake maps cxx_std_23 to -std=c++2b on GCC 13, where __cplusplus is 202100L
// rather than 202302L. The first version of this check asserted >= 202302L,
// passed on the Clang 20 job and on a GCC 14 workstation, and went red on CI's
// GCC 13 — which is the template's own documented floor. An undefined macro
// evaluates to 0 in #if, so this catches "the requirement never arrived" too.
#if __cpp_if_consteval < 202106L
#error "the library's cxx_std_23 usage requirement did not reach the consumer"
#endif

auto main() -> int {
  const auto info = drawforge::project_info();
  std::printf("%.*s\n", static_cast<int>(info.name.size()), info.name.data());

  const auto document = drawforge::DocumentId::create("consumer-scene");
  const auto extent = drawforge::CanvasExtent::create(64, 64);
  if (!document || !extent || extent->rgba8_bytes() != 16'384) {
    return 1;
  }

  // The installed package must carry the compiled immutable scene/query API,
  // not merely its foundation headers. This crosses both new translation
  // units and verifies that query results own their document identity.
  const auto scene = drawforge::Document::create(*document, *extent);
  if (!scene) {
    return 1;
  }
  const auto summary = drawforge::inspect(*scene, drawforge::SummaryQuery{});
  if (!summary || summary->document_id != *document ||
      summary->revision != drawforge::Revision{} || summary->layer_count != 0 ||
      summary->object_count != 0 || summary->track_count != 0) {
    return 1;
  }

  // The public mutation boundary must also survive all three acquisition
  // modes. A dispatcher owns history/replay state while snapshots remain
  // immutable values that can be inspected independently.
  auto dispatcher = drawforge::TransactionDispatcher::create(*scene);
  const auto layer = drawforge::LayerId::create("consumer-layer");
  const auto transaction_id =
      drawforge::TransactionId::create("consumer-create-v1");
  if (!dispatcher || !layer || !transaction_id) {
    return 1;
  }
  const auto applied = dispatcher->apply(drawforge::Transaction{
      *document, drawforge::Revision{}, *transaction_id,
      drawforge::OperationBatch{{drawforge::CreateLayer{*layer, 0, true}}}});
  if (!applied ||
      applied->disposition != drawforge::TransactionDisposition::committed ||
      applied->receipt.created().size() != 1) {
    return 1;
  }
  const auto revised =
      drawforge::inspect(dispatcher->snapshot(), drawforge::SummaryQuery{});
  if (!revised || revised->revision != drawforge::Revision{1} ||
      revised->layer_count != 1) {
    return 1;
  }

  // Rendering is compiled through a private PlutoVG adapter. Exercising both
  // stages proves that add_subdirectory, FetchContent, and installed packages
  // propagate the static link dependency without exposing its types.
  const auto render_config =
      drawforge::RenderConfig::create(0, extent->rgba8_bytes());
  if (!render_config) {
    return 1;
  }
  const auto rgba =
      drawforge::render_rgba(dispatcher->snapshot(), *render_config);
  if (!rgba || rgba->pixels().size() != extent->rgba8_bytes() ||
      rgba->renderer() != drawforge::renderer_info()) {
    return 1;
  }
  const auto png = drawforge::encode_png(*rgba);
  if (!png || png->bytes().size() < 8 || png->bytes()[0] != 0x89U ||
      png->bytes()[1] != 0x50U || png->bytes()[2] != 0x4eU ||
      png->bytes()[3] != 0x47U) {
    return 1;
  }

  // A second call, through the other half of the public API, so the check is
  // not one symbol wide.
  return drawforge::stage_name(info.stage) == "experimental" ? 0 : 1;
}
