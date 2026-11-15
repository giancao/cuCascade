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
 * @file cufile_ioctx.hpp
 * @brief The @c ioctx served by kvikIO's `cufile_reactor` engine.
 */

#include <cucascade/io/cufile/config.hpp>
#include <cucascade/io/cufile/cufile_reactor.hpp>
#include <cucascade/io/cufile/platform.hpp>
#include <cucascade/io/engine_ioctx.hpp>

#include <kvikio/io/cufile/cufile_engine.hpp>

#include <cstddef>
#include <memory>
#include <utility>

namespace cucascade::io::cufile {

/**
 * @brief An @c engine_ioctx over the `cufile_reactor` engine: device
 *        destinations through cuFile / GPUDirect Storage, the rest through POSIX.
 *
 * The engine cannot fill fragmented host buffers, so the prefetching cache is
 * never armed on this context.
 *
 * Build one with @ref make_cufile_ioctx, which gets the engine by name from
 * kvikIO's engine registry.
 */
class cufile_ioctx : public engine_ioctx {
 public:
  /// Serve @p engine, which must be the `cufile_reactor` engine running @p cfg.
  cufile_ioctx(std::shared_ptr<kvikio::io::engine> engine, config cfg)
    : engine_ioctx(std::move(engine), io_context_type::cufile), _config(cfg)
  {
  }

  /// Build a pool of @p n_reactors reactors, all sharing @p ctx.  Creates the
  /// engine directly, which needs the engine plugin at link time
  /// (`kvikio::engine_cufile_reactor`); tests do that.
  cufile_ioctx(std::size_t n_reactors, std::shared_ptr<cufile_reactor::reactor_context> const& ctx)
    : cufile_ioctx(std::make_shared<kvikio::io::cufile::cufile_reactor_engine>(n_reactors, ctx),
                   ctx->cfg())
  {
  }

  /**
   * @brief What GPUDirect Storage can do for reads of @p object, an object this
   *        ioctx opened, under this ioctx's configuration (see @ref probe_object).
   *
   * Advisory: computed on each call from the object's own descriptors and
   * registration (passive storage discovery is not cached), and logged once
   * per device and verdict.  Reads never depend on it.
   *
   * @throws std::invalid_argument when @p object was not opened by a cuFile ioctx.
   */
  [[nodiscard]] path_info path_capability(io_object const& object) const;

  /// The configuration the engine's reactors run with.
  [[nodiscard]] config const& configuration() const noexcept { return _config; }

 private:
  config _config;
};

/**
 * @brief Select kvikIO's `cufile_reactor` engine for local files, with
 *        @p n_reactors reactors configured by @p cfg, and serve it.
 *
 * The engine is selected in kvikIO's process-wide registry, which loads its
 * plugin, and is already started.
 *
 * @throws kvikio::io::engine_selection_error when the engine cannot be
 *         selected: its plugin is missing, or it cannot be created with this
 *         configuration.
 */
[[nodiscard]] std::shared_ptr<cufile_ioctx> make_cufile_ioctx(std::size_t n_reactors,
                                                              config const& cfg);

/// As @ref make_cufile_ioctx, with an engine of its own that is not selected
/// in the registry: for a component that wants its own reactors.
[[nodiscard]] std::shared_ptr<cufile_ioctx> make_private_cufile_ioctx(std::size_t n_reactors,
                                                                      config const& cfg);

}  // namespace cucascade::io::cufile
