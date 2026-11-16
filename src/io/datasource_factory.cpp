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

#include <cucascade/io/config.hpp>
#ifdef CUCASCADE_HAS_CUFILE
#include <cucascade/io/cufile/cufile_ioctx.hpp>
#endif
#include <cucascade/io/datasource_factory.hpp>
#include <cucascade/io/engine_ioctx.hpp>
#include <cucascade/io/host_staging_source.hpp>
#include <cucascade/io/io_context.hpp>
#include <cucascade/io/object_store_config.hpp>
#include <cucascade/io/rest/rest_ioctx.hpp>
#include <cucascade/io/uri_parser.hpp>
#include <cucascade/io/uring/uring_ioctx.hpp>
#include <cucascade/log/logging.hpp>
#include <cucascade/memory/fixed_size_host_memory_resource.hpp>
#include <cucascade/memory/memory_reservation_manager.hpp>
#include <cucascade/memory/memory_space.hpp>

#include <kvikio/io/cufile_posix_engine.hpp>
#include <kvikio/io/engine_registry.hpp>
#include <kvikio/io/errors.hpp>
#include <kvikio/io/uring/uring_engine.hpp>

#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace cucascade::io {

namespace {

/// Unified IO POC: NUMA-aware reactor placement (upstream PR #224) is not merged
/// into this branch, so every reactor stages from the first HOST space.
///
/// First HOST-tier pinned staging resource, or nullptr when none exists.  The
/// uring / rest reactors stage device reads through it and size their bounce
/// slots from its block size.
cucascade::memory::fixed_size_host_memory_resource* first_host_resource(
  cucascade::memory::memory_reservation_manager& rm)
{
  auto host_spaces = rm.get_memory_spaces_for_tier(cucascade::memory::Tier::HOST);
  if (host_spaces.empty()) { return nullptr; }
  return host_spaces.front()->get_memory_resource_of<cucascade::memory::Tier::HOST>();
}

/// Whether the object store is configured.  An empty endpoint, region or
/// credential disables the REST backend.
bool object_store_configured(const object_store_config& os)
{
  return !(os.endpoint.empty() || os.region.empty() || os.access_key.empty() ||
           os.secret_key.empty());
}

/// Whether this process may set up an io_uring ring at all (container seccomp
/// profiles, gVisor and kernel.io_uring_disabled can forbid it).  Probed once,
/// with the system call itself: the io library does not link liburing.
bool io_uring_usable() noexcept
{
  static bool const usable = [] {
    io_uring_params params{};
    auto const ring = ::syscall(__NR_io_uring_setup, 1U, &params);
    if (ring < 0) { return false; }
    ::close(static_cast<int>(ring));
    return true;
  }();
  return usable;
}

/// An s3:// URL: what the REST engine serves.
bool is_s3_url(std::string_view path)
{
  try {
    return parse(path).scheme == "s3";
  } catch (...) {
    return false;
  }
}

}  // namespace

using scheme_checker_type = io_context_registry::scheme_checker_type;
using factory_type        = io_context_registry::factory_type;

#ifdef CUCASCADE_HAS_CUFILE
factory_type make_cufile_ioctx_factory()
{
  return [](const io_config& config) -> std::shared_ptr<ioctx> {
    try {
      return cufile::make_cufile_ioctx(config.cufile_n_reactors, config.cufile);
    } catch (const std::exception& e) {
      CUCASCADE_LOG_ERROR("make_cufile_ioctx_factory: construction failed: {}", e.what());
      return nullptr;
    }
  };
}
#endif

/// A local regular file: what both local reactors and kvikIO's local engines serve.
bool is_local_regular_file(std::string_view path)
{
  std::error_code ec;
  return std::filesystem::is_regular_file(std::filesystem::path{path}, ec) && !ec;
}

factory_type make_cufile_posix_ioctx_factory(
  cucascade::memory::memory_reservation_manager& reservation_manager);

/// kvikIO's `uring` engine, selected by name in kvikIO's process-wide registry
/// (which loads its plugin) and staging through the first HOST space.
factory_type make_uring_ioctx_factory(
  cucascade::memory::memory_reservation_manager& reservation_manager)
{
  return [&reservation_manager](const io_config& config) -> std::shared_ptr<ioctx> {
    try {
      auto* host_mr = first_host_resource(reservation_manager);
      if (host_mr == nullptr) {
        CUCASCADE_LOG_ERROR(
          "make_uring_ioctx_factory: no HOST-tier memory resource for the reactor staging");
        return nullptr;
      }
      kvikio::io::uring::uring_options options;
      options.n_reactors = config.uring_n_reactors;
      options.cfg        = config.uring;
      options.staging    = std::make_shared<host_staging_source>(*host_mr);
      auto engine        = kvikio::io::engine_registry::instance().select(
        kvikio::io::engine_route::local, kvikio::io::uring::uring_engine_name, options);
      return std::make_shared<uring::uring_ioctx>(std::move(engine));
    } catch (kvikio::io::engine_selection_error const& e) {
      // Fall back only when the engine cannot be used in this process at all:
      // its plugin could not be loaded, or io_uring is not allowed here.  That
      // is reported, and local files are served by kvikIO's built-in engine
      // instead of not at all.  When the engine exists and io_uring works, the
      // failure is the configuration's or a shortage (no reactors, staging that
      // does not fit the HOST pool): report no backend, as before the engines
      // moved, so a host that requires this context fails instead of running on
      // an engine it did not ask for.
      if (kvikio::io::engine_registry::instance().has_factory(
            kvikio::io::uring::uring_engine_name) &&
          io_uring_usable()) {
        CUCASCADE_LOG_ERROR("make_uring_ioctx_factory: construction failed: {}", e.what());
        return nullptr;
      }
      CUCASCADE_LOG_ERROR(
        "make_uring_ioctx_factory: {}; local files are served by the cufile_posix engine",
        e.what());
      return make_cufile_posix_ioctx_factory(reservation_manager)(config);
    } catch (const std::exception& e) {
      CUCASCADE_LOG_ERROR("make_uring_ioctx_factory: construction failed: {}", e.what());
      return nullptr;
    }
  };
}

/// kvikIO's built-in engine for local files, behind engine_ioctx.  The engine
/// is selected in kvikIO's process-wide registry and stages device transfers
/// through the first HOST space, so that memory stays in cuCascade's budget.
factory_type make_cufile_posix_ioctx_factory(
  cucascade::memory::memory_reservation_manager& reservation_manager)
{
  return [&reservation_manager](const io_config& config) -> std::shared_ptr<ioctx> {
    try {
      kvikio::io::cufile_posix_options options;
      options.num_threads =
        static_cast<unsigned int>(std::max<std::size_t>(1, config.cufile_posix_n_threads));
      if (auto* host_mr = first_host_resource(reservation_manager); host_mr != nullptr) {
        options.staging = std::make_shared<host_staging_source>(*host_mr);
      }
      auto engine = kvikio::io::engine_registry::instance().select(
        kvikio::io::engine_route::local, kvikio::io::cufile_posix_engine_name, options);
      return std::make_shared<engine_ioctx>(std::move(engine), io_context_type::cufile_posix);
    } catch (const std::exception& e) {
      CUCASCADE_LOG_ERROR("make_cufile_posix_ioctx_factory: construction failed: {}", e.what());
      return nullptr;
    }
  };
}

factory_type make_rest_ioctx_factory(
  cucascade::memory::memory_reservation_manager& reservation_manager)
{
  return [&reservation_manager](const io_config& config) -> std::shared_ptr<ioctx> {
    try {
      if (!object_store_configured(config.object_store)) {
        CUCASCADE_LOG_WARN(
          "make_rest_ioctx_factory: object store not configured (endpoint / credentials / "
          "region missing); REST backend disabled");
        return nullptr;
      }
      auto* host_mr = first_host_resource(reservation_manager);
      auto rest_cfg = config.rest;
      // The object store owns the endpoint and its TLS trust; the reactor's
      // TLS connections must verify against the same CA bundle / policy the authorizer
      // presigns for, so source these from object_store rather than rest config.
      rest_cfg.ca_bundle_path = config.object_store.ca_bundle_path;
      rest_cfg.tls_verify     = config.object_store.tls_verify;
      // The engine signs for the object store itself (SigV4, in the form its
      // s3_signing_mode names) and is selected by name in kvikIO's registry.
      std::shared_ptr<kvikio::io::staging_source> staging;
      if (host_mr != nullptr) { staging = std::make_shared<host_staging_source>(*host_mr); }
      // A configuration that is invalid on its face disables the backend, as it
      // always did; only an engine that cannot be used falls back below.
      rest_cfg.validate();
      try {
        return rest::make_rest_ioctx(
          config.rest_n_reactors, std::move(rest_cfg), config.object_store, staging);
      } catch (kvikio::io::engine_selection_error const& e) {
        // Fall back only when the engine does not exist in this process (its
        // plugin could not be loaded).  When it exists but could not be created
        // or started, the cause may pass (no descriptors left, ...): report no
        // backend, as before, so the next lookup tries again instead of staying
        // on the slower engine.
        if (kvikio::io::engine_registry::instance().has_factory(
              kvikio::io::rest::uring_http_engine_name)) {
          throw;
        }
        CUCASCADE_LOG_ERROR(
          "make_rest_ioctx_factory: {}; object stores are served by the curl engine", e.what());
        return rest::make_curl_rest_ioctx(config.object_store, std::move(staging));
      }
    } catch (const std::exception& e) {
      CUCASCADE_LOG_ERROR("make_rest_ioctx_factory: construction failed: {}", e.what());
      return nullptr;
    }
  };
}

// ---------------------------------------------------------------------------
// datasource_registry
// ---------------------------------------------------------------------------

io_context_registry::io_context_registry(
  config_type config, cucascade::memory::memory_reservation_manager& reservation_manager)
  : _config(std::move(config)),
    _reservation_manager(reservation_manager),
    _local_type(_config.backend == io_backend::cufile         ? io_context_type::cufile
                : _config.backend == io_backend::cufile_posix ? io_context_type::cufile_posix
                                                              : io_context_type::uring)
{
#ifndef CUCASCADE_HAS_CUFILE
  if (_local_type == io_context_type::cufile) {
    throw std::invalid_argument(
      "io_context_registry: io_backend::cufile requested, but this cuCascade build has no cuFile "
      "backend (cufile.h was not found at configure time)");
  }
#endif
  // uring / rest claim paths via their reactor's static supports() (local
  // files and s3:// URLs respectively).  There is no catch-all: a path no
  // backend claims resolves to nothing.  cufile claims local regular files like
  // uring; lookup_path decides between them by the configured backend, never by
  // iteration order.
#ifdef CUCASCADE_HAS_CUFILE
  _entries.emplace(
    io_context_type::cufile,
    entry{io_context_type::cufile, &is_local_regular_file, make_cufile_ioctx_factory()});
#endif
  _entries.emplace(io_context_type::uring,
                   entry{io_context_type::uring,
                         &is_local_regular_file,
                         make_uring_ioctx_factory(_reservation_manager)});
  _entries.emplace(io_context_type::cufile_posix,
                   entry{io_context_type::cufile_posix,
                         &is_local_regular_file,
                         make_cufile_posix_ioctx_factory(_reservation_manager)});
  _entries.emplace(
    io_context_type::restful,
    entry{io_context_type::restful, &is_s3_url, make_rest_ioctx_factory(_reservation_manager)});
}

void io_context_registry::register_ioctx(io_context_type type,
                                         scheme_checker_type checker,
                                         factory_type factory)
{
  if (!checker) throw std::invalid_argument("datasource_registry: null scheme checker");
  if (!factory) throw std::invalid_argument("datasource_registry: null factory");
  std::lock_guard lk{_mtx};
  _entries[type] = {type, std::move(checker), std::move(factory)};
}

void io_context_registry::replace_ioctx(io_context_type old_type,
                                        io_context_type new_type,
                                        scheme_checker_type checker,
                                        factory_type factory)
{
  if (!checker) {
    throw std::invalid_argument("datasource_registry: replace_ioctx: null scheme checker");
  }
  if (!factory) { throw std::invalid_argument("datasource_registry: replace_ioctx: null factory"); }
  std::lock_guard lk{_mtx};
  if (_lookup_latched.load(std::memory_order_acquire)) {
    throw std::logic_error(
      "datasource_registry: replace_ioctx after the first lookup_path (bootstrap-only)");
  }
  if (!_entries.contains(old_type)) {
    throw std::invalid_argument("datasource_registry: replace_ioctx: old type not registered");
  }
  if (_entries.contains(new_type)) {
    throw std::invalid_argument("datasource_registry: replace_ioctx: new type already registered");
  }
  // Strong guarantee: the emplace is the only throwing step and precedes the
  // erase; erase by KEY, not by a pre-emplace iterator (emplace may rehash).
  _entries.emplace(new_type, entry{new_type, std::move(checker), std::move(factory)});
  _entries.erase(old_type);
}

std::optional<io_context_type> io_context_registry::lookup_path(
  std::string_view path) const noexcept
{
  std::shared_lock lk{_mtx};
  _lookup_latched.store(true, std::memory_order_release);
  // uring and rest claim disjoint paths (local files vs s3:// URLs), but both
  // cufile and uring answer true for a local file, and iteration order must not
  // decide between them: cufile is considered only when the config asked for
  // it, and then it displaces uring.  The rules, per configured backend:
  //   * cufile -- skip uring for local files (cufile wins); rest keeps s3://.
  //   * native -- skip cufile for local files (uring wins); rest keeps s3://.
  for (const auto& [type, entry] : _entries) {
    if (!entry.checker(path)) continue;
    bool const local = type == io_context_type::uring || type == io_context_type::cufile ||
                       type == io_context_type::cufile_posix;
    if (local && type != _local_type) { continue; }
    return type;
  }
  return std::nullopt;
}

std::shared_ptr<ioctx> io_context_registry::make_ioctx(io_context_type type) const noexcept
{
  std::shared_lock lk{_mtx};
  auto it = _entries.find(type);
  if (it == _entries.end()) return nullptr;
  return it->second.factory(_config);
}

void io_context_registry::clear()
{
  std::unique_lock lk{_mtx};
  _entries.clear();
}

}  // namespace cucascade::io
