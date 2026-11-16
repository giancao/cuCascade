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

// engine_ioctx: an ioctx served by a kvikIO engine, and the staging source
// cuCascade injects into engines.

#include "utils/allocation_counter.hpp"

#include <cucascade/io/cache/types.hpp>
#include <cucascade/io/engine_ioctx.hpp>
#include <cucascade/io/host_staging_source.hpp>
#include <cucascade/io/io_errors.hpp>
#include <cucascade/memory/fixed_size_host_memory_resource.hpp>
#include <cucascade/memory/numa_region_pinned_host_allocator.hpp>

#include <kvikio/io/engine.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cucascade::io::engine_ioctx;
using cucascade::io::fragment_handle;
using cucascade::io::host_buffer;
using cucascade::io::io_context_type;
using cucascade::io::io_object;
using cucascade::io::prepared_io_completion;
using cucascade::io::prepared_io_slice;
using cucascade::io::range;
using cucascade::io::cache::cached_chunk;
using cucascade::io::cache::chunk_fill;
using cucascade::io::cache::chunk_state;

class fake_object final : public io_object {
 public:
  fake_object(std::string path, std::size_t size) : _path(std::move(path)), _size(size) {}
  [[nodiscard]] const std::string& raw_file_cache_id() const noexcept override { return _path; }
  [[nodiscard]] const std::string& object_path() const noexcept override { return _path; }
  [[nodiscard]] std::size_t size() const noexcept override { return _size; }

 private:
  std::string _path;
  std::size_t _size;
};

/// An engine that holds a read until the test steps it, one fragment per step,
/// the way a reactor completes one physical operation at a time.
class stepping_engine final : public kvikio::io::engine {
 public:
  stepping_engine()
  {
    caps.vector_host_read       = true;
    caps.host_to_device_read    = true;
    caps.fragmented_host_buffer = true;
    caps.prefers_bulk_io        = true;
    caps.min_alignment          = 4096;
    caps.merge_gap              = 1U << 20;
    caps.max_concurrent_scans   = 7;
    caps.staging_block_size     = 1U << 20;
    // Reserved up front: read() itself must not allocate for these.
    _slices.reserve(8);
  }

  [[nodiscard]] std::string_view name() const noexcept override { return "stepping"; }
  [[nodiscard]] kvikio::io::engine_capabilities const& capabilities() const noexcept override
  {
    return caps;
  }
  [[nodiscard]] bool supports(std::string_view path) const noexcept override
  {
    return path.starts_with("fake://");
  }
  void shutdown() noexcept override { ++shutdowns; }
  void warmup(std::string_view url) noexcept override { warmed = std::string(url); }

  [[nodiscard]] std::vector<cucascade::io::byte_range> align_and_coalesce(
    std::span<cucascade::io::byte_range const> ranges,
    std::optional<std::size_t> alignment) const noexcept override
  {
    seen_alignment = alignment;
    return {ranges.begin(), ranges.end()};
  }

  [[nodiscard]] std::shared_ptr<io_object> open(std::string path,
                                                kvikio::io::open_options const& options) override
  {
    last_open = options;
    return std::make_shared<fake_object>(std::move(path), options.known_size.value_or(1U << 20));
  }

  std::size_t host_read(io_object const&, std::size_t, std::size_t size, std::uint8_t* dst) override
  {
    std::fill_n(dst, size, std::uint8_t{0x11});
    return size;
  }

  [[nodiscard]] cucascade::exec::semi_future<std::size_t> read(
    io_object const&, std::vector<prepared_io_slice>&& slices) noexcept override
  {
    _slices  = std::move(slices);
    _promise = cucascade::exec::promise<std::size_t>{};
    _next    = 0;
    _bytes   = 0;
    return _promise.get_semi_future();
  }

  /// Complete the next fragment. Returns false when the request is finished.
  bool step(std::uint8_t value)
  {
    std::size_t index = 0;
    for (auto& slice : _slices) {
      for (auto const& fragment : slice.h_buffer.fragments()) {
        if (index++ != _next) { continue; }
        std::fill_n(fragment.data, fragment.size, value);
        _bytes += fragment.size;
        fragment_handle const handle = fragment.handle;
        (*slice.on_complete)(std::span<fragment_handle const>{&handle, 1}, true);
        ++_next;
        if (_next == total_fragments()) { _promise.set_value(_bytes); }
        return _next != total_fragments();
      }
    }
    return false;
  }

  [[nodiscard]] std::size_t total_fragments() const
  {
    std::size_t n = 0;
    for (auto const& slice : _slices) {
      n += slice.h_buffer.fragments().size();
    }
    return n;
  }

  kvikio::io::engine_capabilities caps;
  int shutdowns{0};
  std::string warmed;
  mutable std::optional<std::size_t> seen_alignment;
  kvikio::io::open_options last_open;

 private:
  std::vector<prepared_io_slice> _slices;
  cucascade::exec::promise<std::size_t> _promise;
  std::size_t _next{0};
  std::size_t _bytes{0};
};

constexpr std::size_t chunk_bytes = 4096;

/// A chunk claimed for loading, as the fs_cache hands it to a backend.
chunk_fill claim(cached_chunk& chunk, std::uint8_t* memory)
{
  chunk.data = memory;
  REQUIRE(chunk.state.mark_queued());
  REQUIRE(chunk.state.mark_allocated());
  chunk_fill fill;
  REQUIRE(chunk.state.take_loading_merging(chunk_fill::whole(), fill));
  return fill;
}

}  // namespace

TEST_CASE("engine_ioctx reports the engine's capabilities and hints", "[io][engine_ioctx]")
{
  auto engine = std::make_shared<stepping_engine>();
  engine_ioctx ctx{engine, io_context_type::restful};

  CHECK(ctx.type() == io_context_type::restful);
  CHECK(&ctx.engine() == engine.get());
  CHECK(ctx.supports("fake://x"));
  CHECK_FALSE(ctx.supports("s3://x"));
  CHECK(ctx.supports_vector_host_read());
  CHECK(ctx.supports_host_to_device_read());
  CHECK_FALSE(ctx.supports_device_read());
  CHECK_FALSE(ctx.supports_write());
  CHECK(ctx.can_use_fs_cache());
  CHECK(ctx.prefers_bulk_io());
  CHECK(ctx.min_alignment_requirement() == 4096);
  CHECK(ctx.merge_gap_size() == (1U << 20));
  CHECK(ctx.n_max_concurrent_scans() == 7);
  CHECK(ctx.staging_block_size() == (1U << 20));

  ctx.warmup("https://bucket.example");
  CHECK(engine->warmed == "https://bucket.example");

  std::array<cucascade::io::byte_range, 1> ranges{cucascade::io::byte_range{0, 10}};
  CHECK(ctx.align_and_coalesce(ranges, 512).size() == 1);
  CHECK(engine->seen_alignment == std::optional<std::size_t>{512});

  ctx.shutdown();
  CHECK(engine->shutdowns == 1);
}

TEST_CASE("engine_ioctx cannot use the cache of an engine without fragment support",
          "[io][engine_ioctx]")
{
  auto engine                         = std::make_shared<stepping_engine>();
  engine->caps.fragmented_host_buffer = false;
  engine_ioctx ctx{engine, io_context_type::uring};
  CHECK_FALSE(ctx.supports_vector_host_read());
  CHECK_FALSE(ctx.supports_host_to_device_read());
  CHECK_FALSE(ctx.can_use_fs_cache());
}

TEST_CASE("engine_ioctx rejects a null engine", "[io][engine_ioctx]")
{
  CHECK_THROWS_AS((engine_ioctx{nullptr, io_context_type::uring}), std::invalid_argument);
}

TEST_CASE("engine_ioctx passes open hints and known sizes to the engine", "[io][engine_ioctx]")
{
  auto engine = std::make_shared<stepping_engine>();
  engine_ioctx ctx{engine, io_context_type::restful};

  auto plain = ctx.open_io_object("fake://a");
  CHECK(engine->last_open.hint == cucascade::io::open_hint::generic);
  CHECK_FALSE(engine->last_open.known_size.has_value());

  auto hinted = ctx.open_io_object("fake://b", cucascade::io::open_hint::parquet_footer_probe);
  CHECK(engine->last_open.hint == cucascade::io::open_hint::parquet_footer_probe);

  auto sized = ctx.open_io_object("fake://c", std::uint64_t{12345});
  CHECK(engine->last_open.known_size == std::optional<std::uint64_t>{12345});
  CHECK(sized->size() == 12345);
}

TEST_CASE("engine_ioctx serves synchronous host reads on the calling thread", "[io][engine_ioctx]")
{
  auto engine = std::make_shared<stepping_engine>();
  engine_ioctx ctx{engine, io_context_type::uring};
  auto object = ctx.open_io_object("fake://a");
  std::array<std::uint8_t, 8> bytes{};
  CHECK(ctx.host_read_io(*object, 0, bytes.size(), bytes.data()) == bytes.size());
  CHECK(bytes[7] == 0x11);
}

TEST_CASE("engine_ioctx reports writes of a read-only engine as not implemented",
          "[io][engine_ioctx]")
{
  auto engine = std::make_shared<stepping_engine>();
  engine_ioctx ctx{engine, io_context_type::restful};
  auto object = ctx.open_io_object("fake://a");
  std::uint8_t byte{};
  CHECK_THROWS_AS(ctx.host_write_io(*object, 0, 1, &byte), cucascade::io::not_implemented_error);
  CHECK_THROWS_AS(ctx.flush_io(*object), cucascade::io::not_implemented_error);
  CHECK_THROWS_AS(ctx.open_io_object_for_write("fake://new"), cucascade::io::not_implemented_error);
}

// The engine hands fragments back one physical operation at a time; each chunk
// becomes readable when its own operation finishes, not when the request does.
TEST_CASE("engine_ioctx publishes cache chunks as their fragments complete", "[io][engine_ioctx]")
{
  auto engine = std::make_shared<stepping_engine>();
  engine_ioctx ctx{engine, io_context_type::uring};
  auto object = ctx.open_io_object("fake://a");

  std::array<std::uint8_t, 3 * chunk_bytes> memory{};
  std::array<cached_chunk, 3> chunks{
    cached_chunk{0}, cached_chunk{chunk_bytes}, cached_chunk{2 * chunk_bytes}};
  auto completion = std::make_shared<prepared_io_completion>(
    [](std::span<fragment_handle const> completed, bool ok) noexcept {
      cucascade::io::cache::publish_chunk_fill(completed, ok);
    });

  std::vector<prepared_io_slice> slices;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    auto const fill = claim(chunks[i], memory.data() + i * chunk_bytes);
    slices.emplace_back(range{i * chunk_bytes, chunk_bytes},
                        cucascade::io::cache::make_fragment_buffer(&chunks[i], fill, chunk_bytes));
    slices.back().on_complete = completion;
  }
  // The fragment carries the chunk's file range and memory; the chunk is its handle.
  auto const& first = slices[1].h_buffer.fragments()[0];
  CHECK(first.offset == chunk_bytes);
  CHECK(first.size == chunk_bytes);
  CHECK(first.data == memory.data() + chunk_bytes);
  CHECK(cucascade::io::cache::chunk_of(first.handle) == &chunks[1]);

  auto future = ctx.mixed_readv_async_io(*object, std::move(slices));
  for (auto const& chunk : chunks) {
    CHECK(chunk.state.get_state() == chunk_state::loading);
  }

  REQUIRE(engine->step(0xa1));
  CHECK(chunks[0].state.get_state() == chunk_state::cached);
  CHECK(chunks[1].state.get_state() == chunk_state::loading);
  CHECK(chunks[2].state.get_state() == chunk_state::loading);
  CHECK_FALSE(future.is_ready());

  REQUIRE(engine->step(0xa2));
  CHECK(chunks[1].state.get_state() == chunk_state::cached);
  CHECK(chunks[2].state.get_state() == chunk_state::loading);
  CHECK_FALSE(future.is_ready());

  CHECK_FALSE(engine->step(0xa3));
  CHECK(chunks[2].state.get_state() == chunk_state::cached);
  REQUIRE(future.is_ready());
  CHECK(std::move(future).get() == 3 * chunk_bytes);
  CHECK(memory[0] == 0xa1);
  CHECK(memory[chunk_bytes] == 0xa2);
  CHECK(memory[2 * chunk_bytes] == 0xa3);
}

// The front end forwards a request: it adds no allocation of its own.
TEST_CASE("engine_ioctx adds no allocation to a read", "[io][engine_ioctx][allocations]")
{
  auto engine = std::make_shared<stepping_engine>();
  engine_ioctx ctx{engine, io_context_type::uring};
  auto object = ctx.open_io_object("fake://a");
  std::array<std::uint8_t, 2 * chunk_bytes> memory{};

  auto make_slices = [&] {
    std::vector<prepared_io_slice> slices;
    slices.emplace_back(range{0, chunk_bytes}, host_buffer{memory.data()});
    slices.emplace_back(range{chunk_bytes, chunk_bytes}, host_buffer{memory.data() + chunk_bytes});
    return slices;
  };

  auto direct_slices = make_slices();
  std::size_t direct = 0;
  {
    cucascade::test::scoped_allocation_counter counter;
    auto future = engine->read(*object, std::move(direct_slices));
    direct      = counter.count();
  }

  auto forwarded_slices = make_slices();
  std::size_t forwarded = 0;
  {
    cucascade::test::scoped_allocation_counter counter;
    auto future = ctx.mixed_readv_async_io(*object, std::move(forwarded_slices));
    forwarded   = counter.count();
  }
  CHECK(forwarded == direct);
}

TEST_CASE("host_staging_source lends blocks and reports exhaustion as unavailable",
          "[io][engine_ioctx][staging]")
{
  constexpr std::size_t block_size = 1UL << 20;
  constexpr std::size_t capacity   = 4UL << 20;
  cucascade::memory::numa_region_pinned_host_memory_resource upstream{0};
  cucascade::memory::fixed_size_host_memory_resource mr{
    0, upstream, capacity, capacity, block_size, 4, 0};
  cucascade::io::host_staging_source source{mr};

  CHECK(source.block_size() == block_size);

  auto first = source.acquire(2 * block_size + 1);
  REQUIRE(first.status == kvikio::io::staging_status::ok);
  REQUIRE(first.lease != nullptr);
  CHECK(first.lease->block_size() == block_size);
  CHECK(first.lease->blocks().size() == 3);
  CHECK(first.lease->size_bytes() == 3 * block_size);
  CHECK(first.lease->blocks()[0] != nullptr);

  // More than what is left: the engine is told to wait, not that the read failed.
  auto second = source.acquire(2 * block_size);
  CHECK(second.status == kvikio::io::staging_status::unavailable);
  CHECK(second.lease == nullptr);

  // Returning the lease makes the blocks available again.
  first.lease.reset();
  auto third = source.acquire(2 * block_size);
  CHECK(third.status == kvikio::io::staging_status::ok);
}
