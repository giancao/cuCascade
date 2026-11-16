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

#include <cucascade/io/cufile/cufile_ioctx.hpp>

#include <kvikio/io/cufile/cufile_options.hpp>
#include <kvikio/io/engine_registry.hpp>

#include <cstddef>
#include <memory>
#include <stdexcept>

namespace cucascade::io::cufile {

namespace {

[[nodiscard]] kvikio::io::cufile::cufile_reactor_options engine_options(std::size_t n_reactors,
                                                                        config const& cfg)
{
  kvikio::io::cufile::cufile_reactor_options options;
  options.n_reactors = n_reactors;
  options.cfg        = cfg;
  return options;
}

}  // namespace

path_info cufile_ioctx::path_capability(io_object const& object) const
{
  auto const* view = dynamic_cast<kvikio::io::cufile::cufile_object_view const*>(&object);
  if (view == nullptr) {
    throw std::invalid_argument(
      "cufile_ioctx::path_capability: I/O object belongs to another backend");
  }
  return probe_object(object, *view, _config);
}

std::shared_ptr<cufile_ioctx> make_cufile_ioctx(std::size_t n_reactors, config const& cfg)
{
  auto engine =
    kvikio::io::engine_registry::instance().select(kvikio::io::engine_route::local,
                                                   kvikio::io::cufile::cufile_reactor_engine_name,
                                                   engine_options(n_reactors, cfg));
  return std::make_shared<cufile_ioctx>(std::move(engine), cfg);
}

std::shared_ptr<cufile_ioctx> make_private_cufile_ioctx(std::size_t n_reactors, config const& cfg)
{
  auto engine = kvikio::io::engine_registry::instance().create(
    kvikio::io::cufile::cufile_reactor_engine_name, engine_options(n_reactors, cfg));
  return std::make_shared<cufile_ioctx>(std::move(engine), cfg);
}

}  // namespace cucascade::io::cufile
