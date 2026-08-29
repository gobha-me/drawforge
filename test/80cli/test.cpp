#include "cli.hpp"
#include "sha256.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dfc = drawforge::cli;

namespace {

class ScratchDirectory {
public:
  explicit ScratchDirectory(const std::string_view name) {
    static std::uint64_t sequence{};
    m_path = std::filesystem::temp_directory_path() /
             ("drawforge-issue11-" + std::string{name} + "-" +
              std::to_string(++sequence));
    std::filesystem::remove_all(m_path);
    REQUIRE(std::filesystem::create_directory(m_path));
  }
  ~ScratchDirectory() { std::filesystem::remove_all(m_path); }
  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return m_path;
  }

private:
  std::filesystem::path m_path;
};

constexpr std::string_view create_frame =
    R"({"protocol":"drawforge.experimental/v1","request":{"background":null,"canvas":{"height":64,"width":160},"document_id":"status-badge","kind":"create_document"}})";
constexpr std::string_view apply_frame =
    R"({"protocol":"drawforge.experimental/v1","request":{"kind":"apply","mode":"commit","transaction":{"body":{"kind":"operations","operations":[{"index":0,"layer_id":"artwork","op":"create_layer","visible":true},{"geometry":{"height":64.0,"kind":"rectangle","radius_x":16.0,"radius_y":16.0,"width":160.0,"x":0.0,"y":0.0},"index":0,"object_id":"badge","op":"create_rectangle","opacity":1.0,"parent":{"id":"artwork","kind":"layer"},"style":{"fill":"#172033ff","stroke":null},"transform":{"a":1.0,"b":0.0,"c":0.0,"d":1.0,"e":0.0,"f":0.0},"visible":true}]},"document_id":"status-badge","expected_revision":0,"transaction_id":"create-status-badge-v1"}}})";
constexpr std::string_view summary_frame =
    R"({"protocol":"drawforge.experimental/v1","request":{"kind":"inspect","query":{"document_id":"status-badge","kind":"document_summary"}}})";
constexpr std::string_view render_frame =
    R"({"protocol":"drawforge.experimental/v1","request":{"artifact_id":"preview-1","document_id":"status-badge","expected_revision":1,"format":"png","kind":"render","time_us":0}})";

auto count_lines(const std::string &value) -> std::size_t {
  return static_cast<std::size_t>(std::count(value.begin(), value.end(), '\n'));
}

auto replace_once(std::string value, const std::string_view needle,
                  const std::string_view replacement) -> std::string {
  const auto position = value.find(needle);
  REQUIRE(position != std::string::npos);
  value.replace(position, needle.size(), replacement);
  return value;
}

struct CancellationState {
  std::uint64_t polls{};
  std::uint64_t cancel_at{};
};

auto cancel_after(const void *context) noexcept -> bool {
  auto &state = *const_cast<CancellationState *>(
      static_cast<const CancellationState *>(context));
  return ++state.polls >= state.cancel_at;
}

} // namespace

TEST_CASE("private SHA-256 uses standard vectors", "[cli][sha256]") {
  const std::array<std::uint8_t, 0> empty{};
  REQUIRE(dfc::sha256_hex(empty) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  constexpr std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
  REQUIRE(dfc::sha256_hex(abc) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("JSONL pipeline commits and writes one deterministic artifact",
          "[cli][pipeline][artifact]") {
  ScratchDirectory artifacts{"pipeline"};
  std::istringstream input{
      std::string{create_frame} + "\r\n" + std::string{apply_frame} + "\n" +
      std::string{summary_frame} + "\n" + std::string{render_frame}};
  std::ostringstream output;
  std::ostringstream diagnostics;

  REQUIRE(dfc::run_jsonl(input, output, diagnostics, artifacts.path()) ==
          dfc::exit_success);
  REQUIRE(diagnostics.str().empty());
  REQUIRE(count_lines(output.str()) == 4U);
  REQUIRE(output.str().find(R"("disposition":"committed")") !=
          std::string::npos);
  REQUIRE(output.str().find(R"("revision":1)") != std::string::npos);

  const auto artifact = artifacts.path() / "preview-1.png";
  REQUIRE(std::filesystem::is_regular_file(artifact));
  std::ifstream stream{artifact, std::ios::binary};
  const std::string bytes{std::istreambuf_iterator<char>{stream},
                          std::istreambuf_iterator<char>{}};
  REQUIRE(bytes.starts_with(std::string_view{"\x89PNG\r\n\x1a\n", 8U}));
  const auto digest = dfc::sha256_hex(std::span{
      reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
  REQUIRE(output.str().find(digest) != std::string::npos);
}

TEST_CASE("encoding failures continue without mutating the session",
          "[cli][encoding][rollback]") {
  ScratchDirectory artifacts{"encoding"};
  const auto duplicate =
      R"({"protocol":"drawforge.experimental/v1","protocol":"drawforge.experimental/v1","request":{"kind":"inspect"}})";
  std::istringstream input{std::string{create_frame} + "\n" + duplicate +
                           "\n\n" + std::string{summary_frame} + "\n"};
  std::ostringstream output;
  std::ostringstream diagnostics;

  REQUIRE(dfc::run_jsonl(input, output, diagnostics, artifacts.path()) ==
          dfc::exit_encoding);
  REQUIRE(count_lines(output.str()) == 4U);
  REQUIRE(output.str().find(R"("code":"duplicate_key")") != std::string::npos);
  REQUIRE(output.str().find(R"("code":"invalid_json")") != std::string::npos);
  REQUIRE(output.str().find(R"("revision":0)") != std::string::npos);
}

TEST_CASE("wire rejection matrix is structured and independently framed",
          "[cli][encoding][failure-matrix]") {
  ScratchDirectory artifacts{"wire-matrix"};
  std::vector<std::pair<std::string, std::string_view>> cases;
  cases.emplace_back(std::string{"\xc3\x28", 2U}, "invalid_utf8");
  cases.emplace_back("\xef\xbb\xbf{}", "invalid_utf8");
  cases.emplace_back(
      R"({"protocol":"drawforge.other/v1","request":{"kind":"inspect"}})",
      "unsupported_version");
  cases.emplace_back(
      R"({"extra":true,"protocol":"drawforge.experimental/v1","request":{"kind":"inspect"}})",
      "unknown_field");
  cases.emplace_back(R"({"protocol":"drawforge.experimental/v1"})",
                     "missing_field");
  cases.emplace_back(std::string(33U, '[') + "0" + std::string(33U, ']'),
                     "nesting_limit");
  cases.emplace_back(
      std::string{
          R"({"protocol":"drawforge.experimental/v1","request":{"background":null,"canvas":{"height":1,"width":)"} +
          std::string(65U, '1') +
          R"(},"document_id":"scene","kind":"create_document"}})",
      "resource_limit");
  std::string embedded_nul{"{}"};
  embedded_nul.push_back('\0');
  cases.emplace_back(std::move(embedded_nul), "invalid_utf8");

  std::string payload;
  for (const auto &[frame, expected] : cases) {
    payload += frame;
    payload += '\n';
  }
  payload += create_frame;
  payload += '\n';
  std::istringstream input{payload};
  std::ostringstream output;
  std::ostringstream diagnostics;

  REQUIRE(dfc::run_jsonl(input, output, diagnostics, artifacts.path()) ==
          dfc::exit_encoding);
  REQUIRE(count_lines(output.str()) == cases.size() + 1U);
  for (const auto &[frame, expected] : cases) {
    static_cast<void>(frame);
    REQUIRE(output.str().find("\"code\":\"" + std::string{expected} + "\"") !=
            std::string::npos);
  }
  REQUIRE(output.str().find(R"("kind":"document_created")") !=
          std::string::npos);
}

TEST_CASE("dry runs, replays, stale revisions, and rollback preserve state",
          "[cli][transaction][failure-matrix]") {
  SECTION("dry run does not advance revision") {
    ScratchDirectory artifacts{"dry-run"};
    const auto dry_run = replace_once(
        std::string{apply_frame}, R"("mode":"commit")", R"("mode":"dry_run")");
    std::istringstream input{std::string{create_frame} + "\n" + dry_run + "\n" +
                             std::string{summary_frame} + "\n"};
    std::ostringstream output;
    std::ostringstream diagnostics;
    REQUIRE(dfc::run_jsonl(input, output, diagnostics, artifacts.path()) ==
            dfc::exit_success);
    REQUIRE(output.str().find(R"("disposition":"dry_run")") !=
            std::string::npos);
    REQUIRE(output.str().find(R"("layer_count":0)") != std::string::npos);
    REQUIRE(output.str().find(R"("revision":0)") != std::string::npos);
  }

  SECTION("successful transaction identifiers replay") {
    ScratchDirectory artifacts{"replay"};
    std::istringstream input{std::string{create_frame} + "\n" +
                             std::string{apply_frame} + "\n" +
                             std::string{apply_frame} + "\n"};
    std::ostringstream output;
    std::ostringstream diagnostics;
    REQUIRE(dfc::run_jsonl(input, output, diagnostics, artifacts.path()) ==
            dfc::exit_success);
    REQUIRE(output.str().find(R"("disposition":"committed")") !=
            std::string::npos);
    REQUIRE(output.str().find(R"("disposition":"replayed")") !=
            std::string::npos);
  }

  SECTION("stale request fails and later inspection continues") {
    ScratchDirectory artifacts{"stale"};
    const auto stale = replace_once(std::string{apply_frame},
                                    "create-status-badge-v1", "stale-v2");
    std::istringstream input{std::string{create_frame} + "\n" +
                             std::string{apply_frame} + "\n" + stale + "\n" +
                             std::string{summary_frame} + "\n"};
    std::ostringstream output;
    std::ostringstream diagnostics;
    REQUIRE(dfc::run_jsonl(input, output, diagnostics, artifacts.path()) ==
            dfc::exit_domain);
    REQUIRE(output.str().find(R"("code":"stale_revision")") !=
            std::string::npos);
    REQUIRE(output.str().find(R"("revision":1)") != std::string::npos);
  }

  SECTION("partial transaction failure rolls back") {
    ScratchDirectory artifacts{"rollback"};
    const auto invalid = replace_once(std::string{apply_frame},
                                      R"("id":"artwork")", R"("id":"missing")");
    std::istringstream input{std::string{create_frame} + "\n" + invalid + "\n" +
                             std::string{summary_frame} + "\n"};
    std::ostringstream output;
    std::ostringstream diagnostics;
    REQUIRE(dfc::run_jsonl(input, output, diagnostics, artifacts.path()) ==
            dfc::exit_domain);
    REQUIRE(output.str().find(R"("operation_index":1)") != std::string::npos);
    REQUIRE(output.str().find(R"("layer_count":0)") != std::string::npos);
    REQUIRE(output.str().find(R"("revision":0)") != std::string::npos);
  }
}

TEST_CASE("artifact identities never overwrite existing output",
          "[cli][artifact][no-clobber]") {
  ScratchDirectory artifacts{"collision"};
  std::istringstream input{
      std::string{create_frame} + "\n" + std::string{apply_frame} + "\n" +
      std::string{render_frame} + "\n" + std::string{render_frame} + "\n"};
  std::ostringstream output;
  std::ostringstream diagnostics;

  REQUIRE(dfc::run_jsonl(input, output, diagnostics, artifacts.path()) ==
          dfc::exit_adapter);
  REQUIRE(count_lines(output.str()) == 4U);
  REQUIRE(output.str().find(R"("code":"artifact_exists")") !=
          std::string::npos);
  REQUIRE(output.str().find(R"("source":"adapter")") != std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(artifacts.path() / "preview-1.png"));
}

TEST_CASE("injected cancellation returns one response and stops",
          "[cli][cancellation]") {
  ScratchDirectory artifacts{"cancel"};
  std::istringstream input{std::string{create_frame} + "\n" +
                           std::string{apply_frame} + "\n" +
                           std::string{summary_frame} + "\n"};
  std::ostringstream output;
  std::ostringstream diagnostics;
  CancellationState state{.cancel_at = 1U};

  REQUIRE(dfc::run_jsonl(input, output, diagnostics, artifacts.path(),
                         drawforge::CancellationToken{&state, cancel_after}) ==
          dfc::exit_cancelled);
  REQUIRE(count_lines(output.str()) == 2U);
  REQUIRE(output.str().find(R"("code":"cancelled")") != std::string::npos);
  REQUIRE(output.str().find(R"("revision":1)") == std::string::npos);
}

TEST_CASE("invalid artifact directory is an invocation failure",
          "[cli][invocation]") {
  ScratchDirectory parent{"missing-dir"};
  std::istringstream input{std::string{create_frame}};
  std::ostringstream output;
  std::ostringstream diagnostics;
  REQUIRE(dfc::run_jsonl(input, output, diagnostics,
                         parent.path() / "missing") == dfc::exit_invocation);
  REQUIRE(output.str().empty());
  REQUIRE(diagnostics.str().find("existing directory") != std::string::npos);
}
