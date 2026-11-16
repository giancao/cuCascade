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
 * @file uring_reactor.hpp
 * @brief The io_uring reactor.
 *
 * It lives in kvikIO's `uring` engine plugin (`kvikio/io/uring/uring_reactor.hpp`,
 * `libkvikio_engine_uring.so`); this header re-exports it in
 * `cucascade::io::uring`. cuCascade itself reaches the engine by name through
 * kvikIO's engine registry; code that uses the reactor classes directly links
 * the plugin (`kvikio::engine_uring`).
 *
 * A reactor pool stages through a `kvikio::io::staging_source`;
 * @ref cucascade::io::uring::make_reactor_context builds a context over a HOST
 * memory resource.
 */

#include <cucascade/io/host_staging_source.hpp>
#include <cucascade/io/types.hpp>
#include <cucascade/io/uring/config.hpp>
#include <cucascade/io/uring/types.hpp>

#include <kvikio/io/uring/uring_reactor.hpp>

#include <memory>

namespace cucascade::io::uring {

using kvikio::io::uring::local_io_object;
using kvikio::io::uring::uring_reactor;

namespace detail {

/// Holds the staging source of a @c resource_reactor_context; a base class so
/// that it is constructed before the context that points at it.
struct resource_staging_holder {
  host_staging_source source;
};

class resource_reactor_context final : private resource_staging_holder,
                                       public uring_reactor::reactor_context {
 public:
  resource_reactor_context(config cfg, memory::fixed_size_host_memory_resource& resource)
    : resource_staging_holder{host_staging_source{resource}},
      uring_reactor::reactor_context(cfg, &source)
  {
  }
};

}  // namespace detail

/**
 * @brief A reactor context staging through @p resource, which must outlive
 *        every reactor of the pool.  A null @p resource gives a context without
 *        staging, which a reactor refuses.
 */
[[nodiscard]] inline std::shared_ptr<uring_reactor::reactor_context> make_reactor_context(
  config cfg, memory::fixed_size_host_memory_resource* resource)
{
  if (resource == nullptr) {
    return std::make_shared<uring_reactor::reactor_context>(cfg, nullptr);
  }
  return std::make_shared<detail::resource_reactor_context>(cfg, *resource);
}

}  // namespace cucascade::io::uring
