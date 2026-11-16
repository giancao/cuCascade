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
 * @file rest_reactor.hpp
 * @brief The REST reactor.
 *
 * Lives in kvikIO's `uring_http` engine plugin (`kvikio/io/rest/rest_reactor.hpp`);
 * see `cucascade/io/rest/forward.hpp`. Code that uses the reactor classes
 * directly links the plugin (`kvikio::engine_uring_http`).
 */

#include <cucascade/io/host_staging_source.hpp>
#include <cucascade/io/io_request.hpp>
#include <cucascade/io/rest/config.hpp>
#include <cucascade/io/rest/forward.hpp>
#include <cucascade/io/types.hpp>

#include <kvikio/io/rest/rest_reactor.hpp>

#include <memory>
#include <utility>

namespace cucascade::io::rest {

namespace staging_detail {

/// Holds the staging source of a @c resource_reactor_context; a base class so
/// that it is constructed before the context that points at it.
struct resource_staging_holder {
  host_staging_source source;
};

class resource_reactor_context final : private resource_staging_holder,
                                       public rest_reactor::reactor_context {
 public:
  resource_reactor_context(config cfg,
                           std::shared_ptr<request_authorizer> authorizer,
                           memory::fixed_size_host_memory_resource& resource)
    : resource_staging_holder{host_staging_source{resource}},
      rest_reactor::reactor_context(std::move(cfg), std::move(authorizer), &source)
  {
  }
};

}  // namespace staging_detail

/**
 * @brief A reactor context staging through @p resource, which must outlive
 *        every reactor of the pool.  A null @p resource gives a context without
 *        staging, on which staged device reads fail.
 */
[[nodiscard]] inline std::shared_ptr<rest_reactor::reactor_context> make_reactor_context(
  config cfg,
  std::shared_ptr<request_authorizer> authorizer,
  memory::fixed_size_host_memory_resource* resource = nullptr)
{
  if (resource == nullptr) {
    return std::make_shared<rest_reactor::reactor_context>(
      std::move(cfg), std::move(authorizer), nullptr);
  }
  return std::make_shared<staging_detail::resource_reactor_context>(
    std::move(cfg), std::move(authorizer), *resource);
}

}  // namespace cucascade::io::rest
