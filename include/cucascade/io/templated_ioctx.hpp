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
 * @file templated_ioctx.hpp
 * @brief An @c ioctx in front of a pool of reactors.
 *
 * The dispatch logic (validating and clamping slices, resolving device ids and
 * default streams, choosing reactors, creating the coordinator, queueing) lives
 * in kvikIO as `kvikio::io::detail::reactor_dispatch`, shared with kvikIO's own
 * reactor engines. @ref cucascade::io::templated_ioctx connects it to the
 * @c ioctx interface and keeps the two overridable hooks, @c next_reactor and
 * @c on_device_dispatch_failure.
 */

#include <cucascade/exec/semi_future.hpp>
#include <cucascade/io/io_context.hpp>
#include <cucascade/io/io_request.hpp>
#include <cucascade/io/types.hpp>

#include <kvikio/io/detail/reactor_dispatch.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cucascade::io {

namespace detail {

using kvikio::io::detail::per_thread_stream_stand_in;
using kvikio::io::detail::resolve_default_streams;

}  // namespace detail

// The reactor contract (see kvikio/io/detail/reactor_dispatch.hpp). A reactor
// that specializes one of the traits does so in namespace kvikio::io::detail.
using kvikio::io::detail::io_reactor_c;
using kvikio::io::detail::io_write_reactor_c;
using kvikio::io::detail::reactor_declares_bulk_io_preference;
using kvikio::io::detail::reactor_declares_device_write_support;
using kvikio::io::detail::reactor_traits;
using kvikio::io::detail::reactor_write_traits;

template <io_reactor_c Reactor>
class templated_ioctx
  : public ioctx,
    public kvikio::io::detail::reactor_dispatch<Reactor, templated_ioctx<Reactor>> {
  using dispatch_type = kvikio::io::detail::reactor_dispatch<Reactor, templated_ioctx<Reactor>>;
  friend dispatch_type;

 public:
  using reactor_type           = Reactor;
  using io_object_type         = typename Reactor::io_object_type;
  using reactor_config_type    = typename Reactor::reactor_config_type;
  using reactor_traits_t       = reactor_traits<Reactor>;
  using reactor_write_traits_t = reactor_write_traits<Reactor>;
  using io_op_type             = typename dispatch_type::io_op_type;

  explicit templated_ioctx(std::vector<std::unique_ptr<Reactor>> reactors)
    : dispatch_type(std::move(reactors))
  {
  }

  template <class Factory>
    requires std::invocable<Factory&> &&
             std::convertible_to<std::invoke_result_t<Factory&>, std::unique_ptr<Reactor>>
  templated_ioctx(std::size_t n_reactors, Factory factory)
    : dispatch_type(n_reactors, std::move(factory))
  {
  }

  ~templated_ioctx() override
  {
    this->pre_destroy();
    shutdown();
  }

  void start() override { this->start_reactors(); }

  void shutdown() noexcept override { this->shutdown_reactors(); }

  [[nodiscard]] bool supports(std::string_view path) const noexcept final
  {
    return Reactor::supports(path);
  }

  [[nodiscard]] bool supports_device_read() const noexcept final
  {
    return reactor_traits_t::supports_device_read;
  }

  [[nodiscard]] bool supports_host_to_device_read() const noexcept final
  {
    return reactor_traits_t::supports_host_to_device_read;
  }

  [[nodiscard]] bool supports_vector_host_read() const noexcept final
  {
    return reactor_traits_t::supports_vector_host_read;
  }

  [[nodiscard]] bool supports_device_range_read() const noexcept final
  {
    return reactor_traits_t::supports_device_range_read;
  }

  [[nodiscard]] bool prefers_bulk_io() const noexcept final
  {
    return reactor_traits_t::prefers_bulk_io;
  }

  [[nodiscard]] std::size_t min_alignment_requirement() const noexcept final
  {
    return this->_config.min_alignment_requirement();
  }

  [[nodiscard]] std::size_t merge_gap_size() const noexcept final
  {
    return this->_config.merge_gap_size();
  }

  [[nodiscard]] std::size_t n_max_concurrent_scans() const noexcept final
  {
    return this->_config.n_max_concurrent_scans;
  }

  /// Every reactor in the pool shares one reactor_context, so the first one's
  /// staging block size is the pool's.
  [[nodiscard]] std::size_t staging_block_size() const noexcept final
  {
    return this->_reactors.empty() ? 0 : this->_reactors.front()->staging_block_size();
  }

  [[nodiscard]] std::vector<byte_range> align_and_coalesce(
    std::span<byte_range const> ranges,
    std::optional<std::size_t> alignment = std::nullopt) const noexcept override
  {
    return Reactor::align_and_coalesce(ranges, alignment);
  }

  /**
   * @brief Select the reactors a request is split across.
   *
   * The default is the least-backlogged ones (see
   * `reactor_dispatch::default_next_reactor`). A backend overrides this to
   * route by object, operation or device.
   */
  virtual std::vector<Reactor*> next_reactor([[maybe_unused]] io_object_type const& object,
                                             [[maybe_unused]] std::size_t n_slices,
                                             [[maybe_unused]] io_op_type type,
                                             [[maybe_unused]] int device_id = -1)
  {
    return this->default_next_reactor();
  }

  std::size_t host_read_io(io_object const& object,
                           std::size_t offset,
                           std::size_t size,
                           std::uint8_t* destination) override
  {
    return this->dispatch_host_read(object, offset, size, destination);
  }

  exec::semi_future<std::size_t> mixed_readv_async_io(
    io_object const& object, std::vector<prepared_io_slice>&& input_slices) noexcept override
  {
    return this->dispatch_read(object, std::move(input_slices));
  }

  // -- Writes (only for reactors modelling io_write_reactor_c) -----------------

  /// Overridable so a derived context can gate writes at run time.
  [[nodiscard]] bool supports_write() const noexcept override
  {
    return reactor_write_traits_t::supports_write;
  }

  [[nodiscard]] bool supports_device_write() const noexcept override
  {
    return reactor_write_traits_t::supports_device_write;
  }

  std::size_t host_write_io(io_object const& object,
                            std::size_t offset,
                            std::size_t size,
                            std::uint8_t const* source) override
  {
    if constexpr (!reactor_write_traits_t::supports_write) {
      return ioctx::host_write_io(object, offset, size, source);
    } else {
      return this->dispatch_host_write(object, offset, size, source);
    }
  }

  exec::semi_future<std::size_t> mixed_writev_async_io(
    io_object const& object, std::vector<prepared_io_slice>&& input_slices) noexcept override
  {
    if constexpr (!reactor_write_traits_t::supports_write) {
      return ioctx::mixed_writev_async_io(object, std::move(input_slices));
    } else {
      return this->dispatch_write(object, std::move(input_slices));
    }
  }

  void flush_io(io_object const& object) override
  {
    if constexpr (!reactor_write_traits_t::supports_write) {
      ioctx::flush_io(object);
    } else {
      this->dispatch_flush(object);
    }
  }

  /// One flush slice, queued on the least-backlogged reactor.
  exec::semi_future<void> flush_async_io(io_object const& object,
                                         io_priority priority) noexcept override
  {
    if constexpr (!reactor_write_traits_t::supports_write) {
      return ioctx::flush_async_io(object, priority);
    } else {
      return this->dispatch_flush_async(object, priority);
    }
  }

 protected:
  /**
   * @brief Applies backend policy after synchronous device dispatch fails.
   *
   * Called from the exception handlers of the dispatch when the failed request
   * carried at least one device slice, before the exception is returned
   * through an errored future or reported to the request's coordinator.
   *
   * An S3-over-RDMA backend overrides this hook to check for a sticky CUDA
   * context error. Returning such an error as an ordinary request failure
   * could allow registered GPU memory to be reused or released before RDMA
   * writes and CUDA work are known to be quiescent. In that case the backend
   * must invoke its fatal policy instead of returning.
   *
   * The default implementation does nothing. An override must not throw or
   * re-enter this ioctx.
   */
  virtual void on_device_dispatch_failure() noexcept {}

  std::shared_ptr<io_object> create_io_object(std::string path) override
  {
    return std::shared_ptr<io_object>(Reactor::create_io_object(std::move(path)));
  }

  std::shared_ptr<io_object> create_io_object_for_write(std::string path,
                                                        write_open_options options) override
  {
    if constexpr (!reactor_write_traits_t::supports_write) {
      return ioctx::create_io_object_for_write(std::move(path), options);
    } else {
      return std::shared_ptr<io_object>(
        Reactor::create_io_object_for_write(std::move(path), options));
    }
  }
};

}  // namespace cucascade::io
