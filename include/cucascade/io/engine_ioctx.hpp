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
 * @file engine_ioctx.hpp
 * @brief An @c ioctx served by a kvikIO engine.
 */

#include <cucascade/io/io_context.hpp>

#include <kvikio/io/engine.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cucascade::io {

/**
 * @brief The cache-aware front end of one kvikIO engine.
 *
 * The @c ioctx base keeps what belongs to cuCascade: the @c fs_cache, the
 * metadata store, and the cache-aware read and write entry points. Everything
 * below them (the backend hooks) is forwarded to the engine unchanged, so a
 * request costs one more virtual call than it did when the reactors lived in
 * cuCascade. This class adds no locks, allocations or copies of its own.
 *
 * The engine is already created (and normally started and selected through
 * @c kvikio::io::engine_registry) when it is handed over; @ref start starts it
 * again only if the caller asks, and @ref shutdown shuts it down.
 */
class engine_ioctx : public ioctx {
 public:
  /**
   * @param engine the engine that serves this context; must not be null
   * @param type   what kind of backend the engine is, for callers that choose a
   *               strategy by backend kind
   */
  engine_ioctx(std::shared_ptr<kvikio::io::engine> engine, io_context_type type);

  /// The engine serving this context.
  [[nodiscard]] kvikio::io::engine& engine() const noexcept { return *_engine; }

  [[nodiscard]] io_context_type type() const noexcept override { return _type; }

  /// Shuts the cache and the engine down, and stops selecting the engine in
  /// kvikIO's registry if this context's creator selected it there.
  ~engine_ioctx() override;

  /// Starts the engine. One obtained from kvikIO's registry is already
  /// started; starting an engine again does nothing.
  void start() override { _engine->start(); }

  void shutdown() noexcept override { _engine->shutdown(); }

  void warmup(std::string_view bucket_url) noexcept override { _engine->warmup(bucket_url); }

  [[nodiscard]] bool supports(std::string_view path) const noexcept override
  {
    return _engine->supports(path);
  }

  // -- Capabilities: the engine's, read once at construction ---------------------

  [[nodiscard]] bool supports_device_read() const noexcept override { return _caps.device_read; }
  [[nodiscard]] bool supports_host_to_device_read() const noexcept override
  {
    return _caps.fragmented_host_buffer && _caps.host_to_device_read;
  }
  [[nodiscard]] bool supports_vector_host_read() const noexcept override
  {
    return _caps.fragmented_host_buffer && _caps.vector_host_read;
  }
  [[nodiscard]] bool supports_device_range_read() const noexcept override
  {
    return _caps.device_range_read;
  }
  [[nodiscard]] bool supports_write() const noexcept override { return _caps.write; }
  [[nodiscard]] bool supports_device_write() const noexcept override { return _caps.device_write; }
  [[nodiscard]] bool prefers_bulk_io() const noexcept override { return _caps.prefers_bulk_io; }
  [[nodiscard]] std::size_t min_alignment_requirement() const noexcept override
  {
    return _caps.min_alignment;
  }
  [[nodiscard]] std::size_t merge_gap_size() const noexcept override { return _caps.merge_gap; }
  [[nodiscard]] std::size_t n_max_concurrent_scans() const noexcept override
  {
    return _caps.max_concurrent_scans;
  }
  [[nodiscard]] std::size_t staging_block_size() const noexcept override
  {
    return _caps.staging_block_size;
  }

  [[nodiscard]] std::vector<byte_range> align_and_coalesce(
    std::span<const byte_range> ranges,
    std::optional<std::size_t> alignment = std::nullopt) const noexcept override
  {
    return _engine->align_and_coalesce(ranges, alignment);
  }

  // -- Backend hooks: forwarded --------------------------------------------------

  std::size_t host_read_io(const io_object& obj,
                           std::size_t offset,
                           std::size_t size,
                           std::uint8_t* dst) override
  {
    return _engine->host_read(obj, offset, size, dst);
  }

  exec::semi_future<std::size_t> mixed_readv_async_io(
    const io_object& obj, std::vector<prepared_io_slice>&& slices) noexcept override
  {
    return _engine->read(obj, std::move(slices));
  }

  std::size_t host_write_io(const io_object& obj,
                            std::size_t offset,
                            std::size_t size,
                            const std::uint8_t* src) override
  {
    return _engine->host_write(obj, offset, size, src);
  }

  exec::semi_future<std::size_t> mixed_writev_async_io(
    const io_object& obj, std::vector<prepared_io_slice>&& slices) noexcept override
  {
    return _engine->write(obj, std::move(slices));
  }

  void flush_io(const io_object& obj) override { _engine->flush(obj); }

  exec::semi_future<void> flush_async_io(const io_object& obj,
                                         io_priority priority) noexcept override
  {
    return _engine->flush_async(obj, priority);
  }

 protected:
  std::shared_ptr<io_object> create_io_object(std::string path) override
  {
    return _engine->open(std::move(path), open_options{});
  }

  std::shared_ptr<io_object> create_io_object(std::string path, open_hint hint) override
  {
    open_options options;
    options.hint = hint;
    return _engine->open(std::move(path), options);
  }

  std::shared_ptr<io_object> create_io_object(std::string path, std::uint64_t known_size) override
  {
    open_options options;
    options.known_size = known_size;
    return _engine->open(std::move(path), options);
  }

  std::shared_ptr<io_object> create_io_object_for_write(std::string path,
                                                        write_open_options options) override
  {
    return _engine->open_for_write(std::move(path), options);
  }

 private:
  std::shared_ptr<kvikio::io::engine> _engine;
  kvikio::io::engine_capabilities _caps;
  io_context_type _type;
};

}  // namespace cucascade::io
