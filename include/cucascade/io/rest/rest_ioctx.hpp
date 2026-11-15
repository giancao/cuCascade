/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

/**
 * @file rest_ioctx.hpp
 * @brief The @c ioctx served by kvikIO's `uring_http` engine.
 */

#include <cucascade/io/config.hpp>
#include <cucascade/io/engine_ioctx.hpp>
#include <cucascade/io/object_store_config.hpp>
#include <cucascade/io/rest/config.hpp>
#include <cucascade/io/rest/forward.hpp>
#include <cucascade/io/rest/rest_reactor.hpp>

#include <kvikio/io/nic_topology.hpp>
#include <kvikio/io/rest/uring_http_api.hpp>
#include <kvikio/io/rest/uring_http_engine.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cucascade::io::rest {

/**
 * @brief What the host knows about the interface the REST engine sends through.
 *
 * Discovers @p interface (empty: the default route's) from sysfs and describes
 * it for kvikIO, which does no discovery of its own.  No value when nothing
 * could be discovered; the engine then uses its default pool size and pins
 * nothing.
 */
[[nodiscard]] std::optional<kvikio::io::nic_topology> discover_egress_nic(
  std::string_view interface) noexcept;

/**
 * @brief An @c engine_ioctx over the `uring_http` engine: object stores
 *        (s3://) over HTTP/1.1 on io_uring.
 *
 * The data path and the opens go through the engine interface. LIST, batched
 * footer resolution and the transport counters are the engine's own
 * (@c kvikio::io::rest::uring_http_api) and are forwarded here under the names
 * they always had.
 *
 * Build one with @ref make_rest_ioctx, which gets the engine by name from
 * kvikIO's engine registry.
 */
class rest_ioctx : public engine_ioctx {
 public:
  /// Serve @p engine, which must be the `uring_http` engine.
  /// @throws std::invalid_argument when it is another engine.
  explicit rest_ioctx(std::shared_ptr<kvikio::io::engine> engine);

  /// Build a pool of @p n_reactors reactors (0 = sized from the egress NIC),
  /// all sharing @p ctx, whose staging source the caller keeps alive.  Creates
  /// the engine directly, which needs the engine plugin at link time
  /// (`kvikio::engine_uring_http`); tests and benchmarks do that.
  rest_ioctx(std::size_t n_reactors, std::shared_ptr<rest_reactor::reactor_context> ctx)
    : rest_ioctx(make_engine(n_reactors, std::move(ctx)))
  {
  }

  /// Counters of the whole pool.  A snapshot; safe while the pool runs.
  [[nodiscard]] transport_stats stats() const noexcept { return _api->stats(); }

  /// Stream a bucket's ListObjectsV2 pages under @p prefix to @p sink.
  void list_objects_paged(std::string_view bucket,
                          std::string_view prefix,
                          std::size_t page_size,
                          std::function<bool(s3::list_objects_v2_page const&)> const& sink,
                          std::optional<std::size_t> max_scanned = std::nullopt)
  {
    _api->list_objects_paged(bucket, prefix, page_size, sink, max_scanned);
  }

  /// The entries of a bucket under @p prefix.
  [[nodiscard]] std::vector<s3::list_entry> list_objects(
    std::string_view bucket,
    std::string_view prefix,
    std::size_t page_size               = 1000,
    std::optional<std::size_t> max_keys = std::nullopt)
  {
    return _api->list_objects(bucket, prefix, page_size, max_keys);
  }

  /// The configured cap on the matches of one listing.
  [[nodiscard]] std::size_t list_max_matches() const { return _api->list_max_matches(); }

  /// Resolve the size and footer bytes of a batch of objects.
  void resolve_footer_objects(std::span<std::string const> paths,
                              std::function<void(footer_resolve_result)> const& on_result,
                              std::stop_token stop = {})
  {
    _api->resolve_footer_objects(paths, on_result, std::move(stop));
  }

  [[nodiscard]] std::size_t footer_stash_reserved_bytes() const noexcept
  {
    return _api->footer_stash_reserved_bytes();
  }

  [[nodiscard]] std::size_t footer_stash_reserved_peak_bytes() const noexcept
  {
    return _api->footer_stash_reserved_peak_bytes();
  }

 private:
  [[nodiscard]] static std::shared_ptr<kvikio::io::engine> make_engine(
    std::size_t n_reactors, std::shared_ptr<rest_reactor::reactor_context> ctx)
  {
    // The pool is planned for the interface the config names, as before.
    auto nic = ctx != nullptr ? discover_egress_nic(ctx->cfg().interface) : std::nullopt;
    return std::make_shared<kvikio::io::rest::uring_http_engine>(
      n_reactors, std::move(ctx), std::move(nic));
  }

  kvikio::io::rest::uring_http_api* _api;
};

/**
 * @brief Select kvikIO's `uring_http` engine for s3:// paths and serve it.
 *
 * The engine is selected in kvikIO's process-wide registry, which loads its
 * plugin, signs for @p object_store, stages device reads through @p staging
 * (may be null: staged device reads then fail) and is already started.
 *
 * @throws kvikio::io::engine_selection_error when the engine cannot be
 *         selected: its plugin is missing, io_uring is unusable, or the
 *         configuration is rejected.
 */
[[nodiscard]] std::shared_ptr<rest_ioctx> make_rest_ioctx(
  std::size_t n_reactors,
  config cfg,
  object_store_config const& object_store,
  std::shared_ptr<kvikio::io::staging_source> staging);

/**
 * @brief The CPUs a REST pool built now from the calling thread would pin its reactors to.
 *
 * Runs the planning the engine runs when it starts (the egress NIC from
 * @c cfg.interface, its queue-IRQ steering, @c cfg.pin_to_irq_cpus, @p n_reactors with 0 =
 * sized from the NIC) without starting anything.  Sorted and unique; empty when the pool would
 * run unpinned.  An embedder that sizes its own thread pools before the REST pool exists uses
 * it to keep them off these CPUs.  Call it from a thread with the affinity the pool will be
 * built from.  Does not load the engine plugin.
 */
[[nodiscard]] std::vector<int> planned_reactor_cpus(std::size_t n_reactors, config const& cfg);

}  // namespace cucascade::io::rest
