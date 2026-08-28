#include <drawforge/drawforge.hpp>

#include "../../src/lib/scene_internal.hpp"
#include "../../src/lib/transaction_internal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace df = drawforge;

namespace {

template <typename Id> [[nodiscard]] auto make_id(const char *value) -> Id {
  const auto id = Id::create(value);
  REQUIRE(id);
  return *id;
}

[[nodiscard]] auto make_extent() -> df::CanvasExtent {
  const auto extent = df::CanvasExtent::create(160, 96);
  REQUIRE(extent);
  return *extent;
}

[[nodiscard]] auto make_opacity(const double value) -> df::Opacity {
  const auto opacity = df::Opacity::create(value);
  REQUIRE(opacity);
  return *opacity;
}

[[nodiscard]] auto make_rectangle(const double x = 0, const double y = 0,
                                  const double width = 20,
                                  const double height = 10) -> df::Rectangle {
  const auto rectangle = df::Rectangle::create(x, y, width, height);
  REQUIRE(rectangle);
  return *rectangle;
}

[[nodiscard]] auto make_ellipse() -> df::Ellipse {
  const auto ellipse = df::Ellipse::create(20, 20, 5, 4);
  REQUIRE(ellipse);
  return *ellipse;
}

[[nodiscard]] auto make_path() -> df::Path {
  const auto first = df::Point::create(0, 0);
  const auto second = df::Point::create(10, 10);
  REQUIRE(first);
  REQUIRE(second);
  const auto path = df::Path::create(
      {df::MoveTo{*first}, df::LineTo{*second}, df::ClosePath{}});
  REQUIRE(path);
  return *path;
}

[[nodiscard]] auto filled(const df::Color color = {10, 20, 30, 255})
    -> df::Style {
  return df::Style{color, std::nullopt};
}

[[nodiscard]] auto make_document(const df::ResourceLimits limits = {})
    -> df::Document {
  const auto document = df::Document::create(
      make_id<df::DocumentId>("scene"), make_extent(), std::nullopt, limits);
  REQUIRE(document);
  return *document;
}

[[nodiscard]] auto
make_dispatcher(const df::ResourceLimits resource_limits = {},
                const df::TransactionStorageLimits storage_limits = {})
    -> df::TransactionDispatcher {
  auto dispatcher = df::TransactionDispatcher::create(
      make_document(resource_limits), storage_limits);
  REQUIRE(dispatcher);
  return std::move(*dispatcher);
}

[[nodiscard]] auto batch(const char *transaction_id,
                         const std::uint64_t revision,
                         std::vector<df::Operation> operations,
                         const char *document_id = "scene") -> df::Transaction {
  return df::Transaction{make_id<df::DocumentId>(document_id),
                         df::Revision{revision},
                         make_id<df::TransactionId>(transaction_id),
                         df::OperationBatch{std::move(operations)}};
}

[[nodiscard]] auto history(const char *transaction_id,
                           const std::uint64_t revision, const bool undo)
    -> df::Transaction {
  return df::Transaction{
      make_id<df::DocumentId>("scene"), df::Revision{revision},
      make_id<df::TransactionId>(transaction_id),
      undo ? df::TransactionBody{df::Undo{}} : df::TransactionBody{df::Redo{}}};
}

[[nodiscard]] auto summary(const df::TransactionDispatcher &dispatcher)
    -> df::DocumentSummary {
  const auto result = df::inspect(dispatcher.snapshot(), df::SummaryQuery{});
  REQUIRE(result);
  return *result;
}

[[nodiscard]] auto
contains_identity(const std::span<const df::IdentityRef> identities,
                  const df::IdentityRef &identity) -> bool {
  for (const auto &candidate : identities) {
    if (candidate == identity)
      return true;
  }
  return false;
}

struct CancellationCounter {
  std::uint64_t cancel_on{};
  mutable std::uint64_t polls{};
};

[[nodiscard]] auto cancellation_token(CancellationCounter &counter)
    -> df::CancellationToken {
  return df::CancellationToken{
      &counter, [](const void *context) noexcept {
        const auto *counter = static_cast<const CancellationCounter *>(context);
        ++counter->polls;
        return counter->polls == counter->cancel_on;
      }};
}

} // namespace

TEST_CASE("transaction limits and stable names are complete",
          "[transaction][public][failure]") {
  STATIC_REQUIRE(
      df::default_transaction_storage_limit_values.max_replay_records == 4'096);
  STATIC_REQUIRE(df::hard_transaction_storage_limit_values.max_replay_records ==
                 65'536);
  STATIC_REQUIRE(std::is_move_constructible_v<df::TransactionDispatcher>);
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<df::TransactionDispatcher>);

  auto invalid = df::hard_transaction_storage_limit_values;
  ++invalid.max_undo_steps;
  REQUIRE_FALSE(df::TransactionStorageLimits::create(invalid));

  constexpr std::array error_codes{
      df::TransactionErrorCode::wrong_document,
      df::TransactionErrorCode::transaction_id_conflict,
      df::TransactionErrorCode::stale_revision,
      df::TransactionErrorCode::empty_transaction,
      df::TransactionErrorCode::nothing_to_undo,
      df::TransactionErrorCode::nothing_to_redo,
      df::TransactionErrorCode::cancelled,
      df::TransactionErrorCode::resource_limit,
      df::TransactionErrorCode::revision_overflow,
      df::TransactionErrorCode::allocation_failure,
      df::TransactionErrorCode::missing_identity,
      df::TransactionErrorCode::duplicate_identity,
      df::TransactionErrorCode::invalid_parent,
      df::TransactionErrorCode::parent_cycle,
      df::TransactionErrorCode::invalid_geometry,
      df::TransactionErrorCode::unsupported_node_kind,
      df::TransactionErrorCode::unsupported_property,
      df::TransactionErrorCode::number_out_of_range,
      df::TransactionErrorCode::arithmetic_overflow,
  };
  for (const auto code : error_codes)
    REQUIRE(df::transaction_error_code_name(code) != "unknown");
  REQUIRE(df::transaction_warning_code_name(
              df::TransactionWarningCode::no_effect) == "no_effect");
  REQUIRE(df::retry_advice_name(df::RetryAdvice::refresh_then_retry) ==
          "refresh_then_retry");
  REQUIRE(df::transaction_field_name(df::TransactionField::object_id) ==
          "object_id");
}

TEST_CASE("transaction-level failures preserve the complete aggregate",
          "[transaction][failure][atomic]") {
  auto dispatcher = make_dispatcher();
  const auto before = summary(dispatcher);

  const auto wrong = dispatcher.apply(batch(
      "wrong", 0, {df::CreateLayer{make_id<df::LayerId>("layer"), 0, true}},
      "other"));
  REQUIRE_FALSE(wrong);
  REQUIRE(wrong.error().code == df::TransactionErrorCode::wrong_document);
  REQUIRE(summary(dispatcher) == before);

  const auto empty = dispatcher.apply(batch("empty", 0, {}));
  REQUIRE_FALSE(empty);
  REQUIRE(empty.error().code == df::TransactionErrorCode::empty_transaction);
  REQUIRE(empty.error().operation_index == std::nullopt);
  REQUIRE(summary(dispatcher) == before);

  const auto stale = dispatcher.apply(
      batch("stale", 1,
            {df::SetVisibility{make_id<df::ObjectId>("missing"), false}}));
  REQUIRE_FALSE(stale);
  REQUIRE(stale.error().code == df::TransactionErrorCode::stale_revision);
  REQUIRE(stale.error().retry_advice == df::RetryAdvice::refresh_then_retry);
  REQUIRE(summary(dispatcher) == before);
}

TEST_CASE("later operation failure rolls back earlier staged identities",
          "[transaction][failure][rollback]") {
  auto dispatcher = make_dispatcher();
  const auto transaction =
      batch("partial", 0,
            {df::CreateLayer{make_id<df::LayerId>("art"), 0, true},
             df::CreateRectangle{make_id<df::ObjectId>("box"),
                                 make_id<df::LayerId>("missing"),
                                 0,
                                 true,
                                 {},
                                 filled(),
                                 make_opacity(1),
                                 make_rectangle()}});
  const auto result = dispatcher.apply(transaction);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == df::TransactionErrorCode::missing_identity);
  REQUIRE(result.error().operation_index == 1);
  REQUIRE(result.error().field_path.segments().size() == 4);
  REQUIRE(summary(dispatcher).revision == df::Revision{});
  REQUIRE(summary(dispatcher).layer_count == 0);

  const auto forward = dispatcher.apply(
      batch("forward", 0,
            {df::CreateRectangle{make_id<df::ObjectId>("box"),
                                 make_id<df::ObjectId>("later"),
                                 0,
                                 true,
                                 {},
                                 filled(),
                                 make_opacity(1),
                                 make_rectangle()},
             df::CreateGroup{make_id<df::ObjectId>("later"),
                             make_id<df::LayerId>("art"),
                             0,
                             true,
                             {}}}));
  REQUIRE_FALSE(forward);
  REQUIRE(forward.error().code == df::TransactionErrorCode::missing_identity);
  REQUIRE(summary(dispatcher).revision == df::Revision{});
}

TEST_CASE("duplicate IDs indices cycles and node ceilings fail closed",
          "[transaction][failure][scene]") {
  {
    auto dispatcher = make_dispatcher();
    const auto duplicate = dispatcher.apply(
        batch("duplicate", 0,
              {df::CreateLayer{make_id<df::LayerId>("art"), 0, true},
               df::CreateLayer{make_id<df::LayerId>("art"), 1, true}}));
    REQUIRE_FALSE(duplicate);
    REQUIRE(duplicate.error().code ==
            df::TransactionErrorCode::duplicate_identity);
    REQUIRE(duplicate.error().operation_index == 1);
    REQUIRE(summary(dispatcher).layer_count == 0);

    const auto index = dispatcher.apply(batch(
        "index", 0, {df::CreateLayer{make_id<df::LayerId>("later"), 1, true}}));
    REQUIRE_FALSE(index);
    REQUIRE(index.error().code ==
            df::TransactionErrorCode::number_out_of_range);
    REQUIRE(summary(dispatcher).revision == df::Revision{});
  }

  {
    auto dispatcher = make_dispatcher();
    const auto layer = make_id<df::LayerId>("art");
    const auto parent = make_id<df::ObjectId>("parent");
    const auto child = make_id<df::ObjectId>("child");
    REQUIRE(
        dispatcher.apply(batch("groups", 0,
                               {df::CreateLayer{layer, 0, true},
                                df::CreateGroup{parent, layer, 0, true, {}},
                                df::CreateGroup{child, parent, 0, true, {}}})));
    const auto before = summary(dispatcher);
    const auto cycle = dispatcher.apply(
        batch("cycle", 1, {df::ReparentObject{parent, child, 0}}));
    REQUIRE_FALSE(cycle);
    REQUIRE(cycle.error().code == df::TransactionErrorCode::parent_cycle);
    REQUIRE(summary(dispatcher) == before);
  }

  {
    auto request = df::default_resource_limit_values;
    request.max_scene_nodes = 1;
    const auto limits = df::ResourceLimits::create(request);
    REQUIRE(limits);
    auto dispatcher = make_dispatcher(*limits);
    const auto layer = make_id<df::LayerId>("art");
    const auto exhausted = dispatcher.apply(batch(
        "nodes", 0,
        {df::CreateLayer{layer, 0, true},
         df::CreateGroup{make_id<df::ObjectId>("group"), layer, 0, true, {}}}));
    REQUIRE_FALSE(exhausted);
    REQUIRE(exhausted.error().code == df::TransactionErrorCode::resource_limit);
    REQUIRE(summary(dispatcher).layer_count == 0);
  }
}

TEST_CASE("ordered operations build and revise the accepted scene algebra",
          "[transaction][operations][happy]") {
  auto dispatcher = make_dispatcher();
  const auto layer = make_id<df::LayerId>("art");
  const auto group = make_id<df::ObjectId>("group");
  const auto rectangle = make_id<df::ObjectId>("rectangle");
  const auto ellipse = make_id<df::ObjectId>("ellipse");
  const auto path = make_id<df::ObjectId>("path");
  const auto track_id = make_id<df::TrackId>("fade");
  const auto track = df::OpacityTrack::create(track_id, rectangle, 0, 1'000,
                                              make_opacity(0), make_opacity(1));
  REQUIRE(track);

  const auto create = dispatcher.apply(
      batch("create", 0,
            {df::CreateLayer{layer, 0, true},
             df::CreateGroup{group, layer, 0, true, {}},
             df::CreateRectangle{rectangle,
                                 group,
                                 0,
                                 true,
                                 {},
                                 filled(),
                                 make_opacity(1),
                                 make_rectangle()},
             df::CreateEllipse{ellipse,
                               group,
                               1,
                               true,
                               {},
                               filled({40, 50, 60, 255}),
                               make_opacity(1),
                               make_ellipse()},
             df::CreatePath{path,
                            layer,
                            1,
                            true,
                            {},
                            filled({70, 80, 90, 255}),
                            make_opacity(1),
                            make_path()},
             df::CreateOpacityTrack{*track},
             df::SetCanvasBackground{df::Color{1, 2, 3, 255}}}));
  REQUIRE(create);
  REQUIRE(create->disposition == df::TransactionDisposition::committed);
  REQUIRE(create->receipt.created().size() == 6);
  REQUIRE(create->receipt.changed().size() == 1);
  REQUIRE(create->receipt.dirty_bounds());
  REQUIRE(summary(dispatcher).revision == df::Revision{1});
  REQUIRE(summary(dispatcher).layer_count == 1);
  REQUIRE(summary(dispatcher).object_count == 4);
  REQUIRE(summary(dispatcher).track_count == 1);

  const auto transform = df::AffineTransform::create(1, 0, 0, 1, 12, 4);
  const auto replacement_track = df::OpacityTrack::create(
      track_id, ellipse, 5, 2'000, make_opacity(1), make_opacity(0));
  REQUIRE(transform);
  REQUIRE(replacement_track);
  const auto revise = dispatcher.apply(batch(
      "revise", 1,
      {df::SetVisibility{df::NodeRef{group}, false},
       df::SetTransform{group, *transform},
       df::SetGeometry{rectangle, df::Geometry{make_ellipse()}},
       df::SetStyle{rectangle, filled({100, 110, 120, 255})},
       df::SetOpacity{rectangle, make_opacity(0.5)},
       df::SetOpacityTrack{*replacement_track},
       df::ReparentObject{ellipse, layer, 1}, df::ReorderObject{path, 0}}));
  REQUIRE(revise);
  REQUIRE(revise->receipt.changed().size() == 5);
  REQUIRE(revise->receipt.warnings().empty());
  REQUIRE(summary(dispatcher).revision == df::Revision{2});

  const auto selection = df::SelectedObjectsQuery::create(
      {path, ellipse, rectangle},
      {df::SelectedField::parent_order, df::SelectedField::visibility,
       df::SelectedField::transform, df::SelectedField::geometry,
       df::SelectedField::style, df::SelectedField::opacity,
       df::SelectedField::opacity_track});
  REQUIRE(selection);
  const auto selected = df::inspect(dispatcher.snapshot(), *selection);
  REQUIRE(selected);
  REQUIRE(selected->objects[0].parent_order->sibling_index == 0);
  REQUIRE(std::get<df::LayerId>(selected->objects[1].parent_order->parent) ==
          layer);
  REQUIRE(std::holds_alternative<df::Ellipse>(*selected->objects[2].geometry));
  REQUIRE(selected->objects[1].opacity_track->id == track_id);

  const auto group_query = df::SelectedObjectsQuery::create(
      {group}, {df::SelectedField::visibility, df::SelectedField::transform});
  REQUIRE(group_query);
  const auto selected_group = df::inspect(dispatcher.snapshot(), *group_query);
  REQUIRE(selected_group);
  REQUIRE_FALSE(*selected_group->objects[0].visibility);
}

TEST_CASE("dry-run commit replay and conflicting reuse follow precedence",
          "[transaction][dry-run][replay]") {
  auto dispatcher = make_dispatcher();
  const auto transaction =
      batch("once", 0, {df::CreateLayer{make_id<df::LayerId>("art"), 0, true}});
  const auto before = summary(dispatcher);
  const auto dry_run = dispatcher.apply(transaction, df::ApplyMode::dry_run);
  REQUIRE(dry_run);
  REQUIRE(dry_run->disposition == df::TransactionDisposition::dry_run);
  REQUIRE(summary(dispatcher) == before);

  const auto committed = dispatcher.apply(transaction);
  REQUIRE(committed);
  REQUIRE(committed->receipt == dry_run->receipt);
  REQUIRE(summary(dispatcher).revision == df::Revision{1});

  const auto later = dispatcher.apply(
      batch("later", 1, {df::SetCanvasBackground{df::Color{9, 8, 7, 255}}}));
  REQUIRE(later);
  const auto after_later = summary(dispatcher);

  CancellationCounter counter{1};
  const auto replay = dispatcher.apply(transaction, df::ApplyMode::commit,
                                       cancellation_token(counter));
  REQUIRE(replay);
  REQUIRE(replay->disposition == df::TransactionDisposition::replayed);
  REQUIRE(replay->receipt == committed->receipt);
  REQUIRE(counter.polls == 0);
  REQUIRE(summary(dispatcher) == after_later);

  const auto conflict = dispatcher.apply(
      df::Transaction{make_id<df::DocumentId>("scene"), df::Revision{2},
                      make_id<df::TransactionId>("once"), df::Undo{}});
  REQUIRE_FALSE(conflict);
  REQUIRE(conflict.error().code ==
          df::TransactionErrorCode::transaction_id_conflict);
  REQUIRE(summary(dispatcher) == after_later);
}

TEST_CASE("same-value writes warn and still form one undoable revision",
          "[transaction][no-effect][undo]") {
  auto dispatcher = make_dispatcher();
  const auto no_effect = dispatcher.apply(
      batch("no-effect", 0, {df::SetCanvasBackground{std::nullopt}}));
  REQUIRE(no_effect);
  REQUIRE(no_effect->receipt.changed().empty());
  REQUIRE_FALSE(no_effect->receipt.dirty_bounds());
  REQUIRE(no_effect->receipt.warnings().size() == 1);
  REQUIRE(no_effect->receipt.warnings()[0].code ==
          df::TransactionWarningCode::no_effect);
  REQUIRE(summary(dispatcher).revision == df::Revision{1});

  const auto undone = dispatcher.apply(history("undo-no-effect", 1, true));
  REQUIRE(undone);
  REQUIRE(undone->receipt.changed().empty());
  REQUIRE(summary(dispatcher).revision == df::Revision{2});
}

TEST_CASE("operations that cancel out report the final committed delta",
          "[transaction][receipt][no-effect]") {
  auto dispatcher = make_dispatcher();
  const auto background = df::Color{10, 20, 30, 255};
  const auto result =
      dispatcher.apply(batch("cancel-out", 0,
                             {df::SetCanvasBackground{background},
                              df::SetCanvasBackground{std::nullopt}}));
  REQUIRE(result);
  REQUIRE(result->receipt.created().empty());
  REQUIRE(result->receipt.changed().empty());
  REQUIRE_FALSE(result->receipt.dirty_bounds());
  REQUIRE(result->receipt.warnings().empty());
  REQUIRE(summary(dispatcher).revision == df::Revision{1});
  REQUIRE(summary(dispatcher).background == std::nullopt);
}

TEST_CASE("undo and redo round-trip a mixed transaction atomically",
          "[transaction][undo][redo]") {
  auto dispatcher = make_dispatcher();
  const auto layer = make_id<df::LayerId>("art");
  const auto object = make_id<df::ObjectId>("box");
  const auto create = dispatcher.apply(
      batch("create-mixed", 0,
            {df::CreateLayer{layer, 0, true},
             df::CreateRectangle{object,
                                 layer,
                                 0,
                                 true,
                                 {},
                                 filled(),
                                 make_opacity(1),
                                 make_rectangle()},
             df::SetCanvasBackground{df::Color{2, 4, 6, 255}}}));
  REQUIRE(create);
  const auto accepted = summary(dispatcher);

  const auto dry_undo =
      dispatcher.apply(history("undo-mixed", 1, true), df::ApplyMode::dry_run);
  REQUIRE(dry_undo);
  REQUIRE(summary(dispatcher) == accepted);
  const auto undo = dispatcher.apply(history("undo-mixed", 1, true));
  REQUIRE(undo);
  REQUIRE(undo->receipt == dry_undo->receipt);
  REQUIRE(contains_identity(undo->receipt.changed(), df::IdentityRef{layer}));
  REQUIRE(contains_identity(undo->receipt.changed(), df::IdentityRef{object}));
  REQUIRE(summary(dispatcher).layer_count == 0);
  REQUIRE(summary(dispatcher).revision == df::Revision{2});

  const auto replay = dispatcher.apply(history("undo-mixed", 1, true));
  REQUIRE(replay);
  REQUIRE(replay->disposition == df::TransactionDisposition::replayed);
  REQUIRE(summary(dispatcher).revision == df::Revision{2});

  const auto redo = dispatcher.apply(history("redo-mixed", 2, false));
  REQUIRE(redo);
  REQUIRE(summary(dispatcher).layer_count == 1);
  REQUIRE(summary(dispatcher).object_count == 1);
  REQUIRE(summary(dispatcher).background == accepted.background);
  REQUIRE(summary(dispatcher).revision == df::Revision{3});
}

TEST_CASE("failed edits preserve redo while successful branch edits clear it",
          "[transaction][redo][failure]") {
  auto dispatcher = make_dispatcher();
  const auto layer = make_id<df::LayerId>("art");
  REQUIRE(
      dispatcher.apply(batch("create", 0, {df::CreateLayer{layer, 0, true}})));
  REQUIRE(dispatcher.apply(history("undo", 1, true)));

  const auto failed = dispatcher.apply(
      batch("failed", 2,
            {df::SetVisibility{make_id<df::ObjectId>("missing"), false}}));
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().code == df::TransactionErrorCode::missing_identity);
  REQUIRE(dispatcher.apply(history("redo", 2, false)));

  REQUIRE(dispatcher.apply(history("undo-again", 3, true)));
  REQUIRE(dispatcher.apply(
      batch("branch", 4, {df::SetCanvasBackground{df::Color{1, 1, 1, 255}}})));
  const auto no_redo = dispatcher.apply(history("no-redo", 5, false));
  REQUIRE_FALSE(no_redo);
  REQUIRE(no_redo.error().code == df::TransactionErrorCode::nothing_to_redo);
}

TEST_CASE("cancellation at every boundary preserves all committed state",
          "[transaction][cancellation][atomic]") {
  const auto transaction =
      batch("cancel", 0,
            {df::CreateLayer{make_id<df::LayerId>("first"), 0, true},
             df::CreateLayer{make_id<df::LayerId>("second"), 1, true}});
  for (std::uint64_t poll = 1; poll <= 4; ++poll) {
    DYNAMIC_SECTION("poll " << poll) {
      auto dispatcher = make_dispatcher();
      const auto before = summary(dispatcher);
      CancellationCounter counter{poll};
      const auto result = dispatcher.apply(transaction, df::ApplyMode::commit,
                                           cancellation_token(counter));
      REQUIRE_FALSE(result);
      REQUIRE(result.error().code == df::TransactionErrorCode::cancelled);
      REQUIRE(summary(dispatcher) == before);
    }
  }

  auto dispatcher = make_dispatcher();
  CancellationCounter counter{5};
  const auto committed = dispatcher.apply(transaction, df::ApplyMode::commit,
                                          cancellation_token(counter));
  REQUIRE(committed);
  REQUIRE(summary(dispatcher).layer_count == 2);
}

TEST_CASE("replay and history retention fail closed",
          "[transaction][resource][atomic]") {
  auto replay_request = df::default_transaction_storage_limit_values;
  replay_request.max_replay_records = 1;
  const auto replay_limits =
      df::TransactionStorageLimits::create(replay_request);
  REQUIRE(replay_limits);
  auto replay_dispatcher = make_dispatcher({}, *replay_limits);
  const auto first_transaction = batch(
      "first", 0, {df::CreateLayer{make_id<df::LayerId>("first"), 0, true}});
  const auto first = replay_dispatcher.apply(first_transaction);
  REQUIRE(first);
  const auto full = summary(replay_dispatcher);
  const auto rejected = replay_dispatcher.apply(batch(
      "second", 1, {df::CreateLayer{make_id<df::LayerId>("second"), 1, true}}));
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code == df::TransactionErrorCode::resource_limit);
  REQUIRE(summary(replay_dispatcher) == full);
  const auto replay = replay_dispatcher.apply(first_transaction);
  REQUIRE(replay);
  REQUIRE(replay->disposition == df::TransactionDisposition::replayed);

  auto history_request = df::default_transaction_storage_limit_values;
  history_request.max_undo_steps = 0;
  const auto history_limits =
      df::TransactionStorageLimits::create(history_request);
  REQUIRE(history_limits);
  auto history_dispatcher = make_dispatcher({}, *history_limits);
  const auto before = summary(history_dispatcher);
  const auto no_history = history_dispatcher.apply(
      batch("no-history", 0,
            {df::CreateLayer{make_id<df::LayerId>("layer"), 0, true}}));
  REQUIRE_FALSE(no_history);
  REQUIRE(no_history.error().code == df::TransactionErrorCode::resource_limit);
  REQUIRE(summary(history_dispatcher) == before);
}

TEST_CASE("history evicts oldest edits without evicting replay records",
          "[transaction][history][retention]") {
  auto request = df::default_transaction_storage_limit_values;
  request.max_undo_steps = 2;
  const auto limits = df::TransactionStorageLimits::create(request);
  REQUIRE(limits);
  auto dispatcher = make_dispatcher({}, *limits);
  const auto one =
      batch("one", 0, {df::CreateLayer{make_id<df::LayerId>("one"), 0, true}});
  REQUIRE(dispatcher.apply(one));
  REQUIRE(dispatcher.apply(batch(
      "two", 1, {df::CreateLayer{make_id<df::LayerId>("two"), 1, true}})));
  REQUIRE(dispatcher.apply(batch(
      "three", 2, {df::CreateLayer{make_id<df::LayerId>("three"), 2, true}})));
  REQUIRE(dispatcher.apply(history("undo-three", 3, true)));
  REQUIRE(dispatcher.apply(history("undo-two", 4, true)));
  const auto exhausted = dispatcher.apply(history("undo-one", 5, true));
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code == df::TransactionErrorCode::nothing_to_undo);
  const auto state = summary(dispatcher);
  const auto replay = dispatcher.apply(one);
  REQUIRE(replay);
  REQUIRE(replay->disposition == df::TransactionDisposition::replayed);
  REQUIRE(summary(dispatcher) == state);
}

TEST_CASE("identical transaction runs produce value-identical evidence",
          "[transaction][deterministic]") {
  auto first = make_dispatcher();
  auto second = make_dispatcher();
  const auto layer = make_id<df::LayerId>("art");
  const auto object = make_id<df::ObjectId>("box");
  const auto transaction =
      batch("deterministic", 0,
            {df::CreateLayer{layer, 0, true},
             df::CreateRectangle{object,
                                 layer,
                                 0,
                                 true,
                                 {},
                                 filled(),
                                 make_opacity(0.75),
                                 make_rectangle(4, 6, 12, 8)}});
  const auto first_result = first.apply(transaction);
  const auto second_result = second.apply(transaction);
  REQUIRE(first_result);
  REQUIRE(second_result);
  REQUIRE(*first_result == *second_result);
  REQUIRE(summary(first) == summary(second));

  const auto query = df::SelectedObjectsQuery::create(
      {object}, {df::SelectedField::parent_order, df::SelectedField::geometry,
                 df::SelectedField::style, df::SelectedField::opacity});
  REQUIRE(query);
  REQUIRE(df::inspect(first.snapshot(), *query) ==
          df::inspect(second.snapshot(), *query));
}

TEST_CASE("injected candidate allocation failures are atomic",
          "[transaction][allocation][atomic]") {
  const auto transaction =
      batch("candidate", 0,
            {df::CreateLayer{make_id<df::LayerId>("layer"), 0, true}});
  for (const auto fault :
       {df::detail::TransactionFaultPoint::stage_allocation,
        df::detail::TransactionFaultPoint::receipt_allocation}) {
    auto dispatcher = make_dispatcher();
    const auto before = summary(dispatcher);
    df::detail::set_transaction_fault(fault);
    const auto result = dispatcher.apply(transaction);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            df::TransactionErrorCode::allocation_failure);
    REQUIRE(result.error().retry_advice == df::RetryAdvice::same_request);
    REQUIRE(summary(dispatcher) == before);
    const auto retried = dispatcher.apply(transaction);
    REQUIRE(retried);
    REQUIRE(retried->disposition == df::TransactionDisposition::committed);
    REQUIRE(summary(dispatcher).revision == df::Revision{1});
  }
}

TEST_CASE("operation and revision resource ceilings are enforced first",
          "[transaction][resource][revision]") {
  auto resource_request = df::default_resource_limit_values;
  resource_request.max_transaction_operations = 1;
  const auto resource_limits = df::ResourceLimits::create(resource_request);
  REQUIRE(resource_limits);
  auto dispatcher = make_dispatcher(*resource_limits);
  const auto oversized = dispatcher.apply(
      batch("oversized", 0,
            {df::CreateLayer{make_id<df::LayerId>("one"), 0, true},
             df::CreateLayer{make_id<df::LayerId>("two"), 1, true}}));
  REQUIRE_FALSE(oversized);
  REQUIRE(oversized.error().code == df::TransactionErrorCode::resource_limit);
  REQUIRE(summary(dispatcher).revision == df::Revision{});

  auto identifier_request = df::default_resource_limit_values;
  identifier_request.max_identifier_bytes = 5;
  const auto identifier_limits = df::ResourceLimits::create(identifier_request);
  REQUIRE(identifier_limits);
  auto identifier_dispatcher = make_dispatcher(*identifier_limits);
  const auto oversized_transaction_id = identifier_dispatcher.apply(
      batch("too-long", 0, {df::SetCanvasBackground{df::Color{1, 2, 3, 255}}}));
  REQUIRE_FALSE(oversized_transaction_id);
  REQUIRE(oversized_transaction_id.error().code ==
          df::TransactionErrorCode::resource_limit);
  REQUIRE(oversized_transaction_id.error().field_path ==
          df::FieldPath{df::TransactionField::transaction_id});
  REQUIRE(summary(identifier_dispatcher).revision == df::Revision{});

  const auto built = df::detail::DocumentBuilder::build(
      make_id<df::DocumentId>("scene"),
      df::Revision{std::numeric_limits<std::uint64_t>::max()}, make_extent(),
      std::nullopt, {}, {}, {}, {});
  REQUIRE(built);
  auto overflow = df::TransactionDispatcher::create(*built);
  REQUIRE(overflow);
  const auto empty_at_limit = overflow->apply(
      batch("empty-at-limit", std::numeric_limits<std::uint64_t>::max(), {}));
  REQUIRE_FALSE(empty_at_limit);
  REQUIRE(empty_at_limit.error().code ==
          df::TransactionErrorCode::empty_transaction);
  const auto exhausted = overflow->apply(
      batch("overflow", std::numeric_limits<std::uint64_t>::max(),
            {df::CreateLayer{make_id<df::LayerId>("layer"), 0, true}}));
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code ==
          df::TransactionErrorCode::revision_overflow);
  REQUIRE(exhausted.error().retry_advice == df::RetryAdvice::not_retryable);
}
