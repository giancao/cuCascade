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
 * @file uring_ioctx.hpp
 * @brief The @c ioctx served by kvikIO's `uring` engine.
 */

#include <cucascade/io/engine_ioctx.hpp>
#include <cucascade/io/uring/uring_reactor.hpp>

#include <kvikio/io/uring/uring_engine.hpp>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace cucascade::io::uring {

/// The gauges of the `uring` engine: reach it with
/// `engine_api<uring_diagnostics>(ctx)`.
using kvikio::io::uring::uring_diagnostics;

/**
 * @brief An @c engine_ioctx over the `uring` engine: local files through io_uring.
 *
 * cuCascade builds it from an engine it obtained by name from kvikIO's engine
 * registry (see @c make_uring_ioctx_factory). Building it from a reactor
 * context instead creates the engine directly, which needs the engine plugin
 * at link time (`kvikio::engine_uring`); tests and benchmarks do that.
 */
class uring_ioctx : public engine_ioctx {
 public:
  /// Serve @p engine, which must be the `uring` engine.
  explicit uring_ioctx(std::shared_ptr<kvikio::io::engine> engine)
    : engine_ioctx(std::move(engine), io_context_type::uring)
  {
  }

  /// Build a pool of @p n_reactors reactors, all sharing @p ctx (one context
  /// per pool: it carries the per-reactor @c config and the staging source,
  /// which must outlive this ioctx).  Links the engine plugin.
  uring_ioctx(std::size_t n_reactors, std::shared_ptr<uring_reactor::reactor_context> ctx)
    : uring_ioctx(std::make_shared<kvikio::io::uring::uring_engine>(n_reactors, std::move(ctx)))
  {
  }

  /// One gauge snapshot per reactor, in reactor order.  Each call restarts the
  /// reactors' max-inflight and queue-delay windows.  Nothing samples them in
  /// the background; callers that want them poll this.
  [[nodiscard]] std::vector<uring_reactor::gauges> reactor_gauges() noexcept
  {
    auto* diagnostics = dynamic_cast<uring_diagnostics*>(&engine());
    return diagnostics == nullptr ? std::vector<uring_reactor::gauges>{}
                                  : diagnostics->reactor_gauges();
  }
};

}  // namespace cucascade::io::uring
