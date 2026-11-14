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

// How many heap allocations the calling thread makes to submit one read. The
// numbers are a budget: a change that makes submission allocate more per
// request, or per fragment, fails here.

#include "utils/allocation_counter.hpp"

#include <cucascade/io/io_request.hpp>
#include <cucascade/io/types.hpp>
#include <cucascade/io/uring/config.hpp>
#include <cucascade/io/uring/uring_ioctx.hpp>
#include <cucascade/io/uring/uring_reactor.hpp>
#include <cucascade/memory/fixed_size_host_memory_resource.hpp>
#include <cucascade/memory/numa_region_pinned_host_allocator.hpp>

#include <catch2/catch_all.hpp>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using cucascade::io::fragment;
using cucascade::io::fragment_handle;
using cucascade::io::host_buffer;
using cucascade::io::prepared_io_completion;
using cucascade::io::prepared_io_slice;
using cucascade::io::range;
using cucascade::io::uring::uring_ioctx;
using cucascade::io::uring::uring_reactor;
using namespace std::chrono_literals;

constexpr std::size_t MiB = 1UL << 20;

struct fixture {
  static constexpr std::size_t block_size = MiB;
  static constexpr std::size_t file_bytes = 16 * MiB;

  fixture()
    : path(std::filesystem::temp_directory_path() /
           ("cucascade_uring_alloc_" + std::to_string(::getpid()) + ".bin")),
      mr{0, upstream, 128 * MiB, 128 * MiB, block_size, 64, 0},
      ctx{std::make_shared<uring_ioctx>(
        1, std::make_shared<uring_reactor::reactor_context>(cucascade::io::uring::config{}, &mr))}
  {
    std::vector<char> const bytes(file_bytes, 'x');
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    REQUIRE(out);
    ctx->start();
    object = ctx->open_io_object(path.string());
  }

  ~fixture()
  {
    object.reset();
    ctx->shutdown();
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  /// Submit @p slices and return the allocations the submitting thread made.
  std::size_t submit(std::vector<prepared_io_slice>&& slices, std::size_t expected_bytes)
  {
    cucascade::exec::semi_future<std::size_t> future;
    std::size_t allocations = 0;
    {
      cucascade::test::scoped_allocation_counter counter;
      future      = ctx->mixed_readv_async_io(*object, std::move(slices));
      allocations = counter.count();
    }
    REQUIRE(std::move(future).get(30s) == expected_bytes);
    return allocations;
  }

  std::filesystem::path path;
  cucascade::memory::numa_region_pinned_host_memory_resource upstream{0};
  cucascade::memory::fixed_size_host_memory_resource mr;
  std::shared_ptr<uring_ioctx> ctx;
  std::shared_ptr<cucascade::io::io_object> object;
};

/// @p n_slices contiguous host slices of one block each.
std::vector<prepared_io_slice> contiguous(std::vector<std::uint8_t>& memory, std::size_t n_slices)
{
  std::vector<prepared_io_slice> slices;
  slices.reserve(n_slices);
  for (std::size_t i = 0; i < n_slices; ++i) {
    slices.emplace_back(range{i * MiB, MiB}, host_buffer{memory.data() + i * MiB});
  }
  return slices;
}

/// @p n_slices fragmented slices of @p per_slice one-block fragments each.
std::vector<prepared_io_slice> fragmented(std::vector<std::uint8_t>& memory,
                                          std::size_t n_slices,
                                          std::size_t per_slice,
                                          std::shared_ptr<prepared_io_completion> const& done)
{
  std::vector<prepared_io_slice> slices;
  slices.reserve(n_slices);
  std::size_t block = 0;
  for (std::size_t i = 0; i < n_slices; ++i) {
    std::vector<fragment> fragments;
    fragments.reserve(per_slice);
    auto const first = block;
    for (std::size_t j = 0; j < per_slice; ++j, ++block) {
      auto* data = memory.data() + block * MiB;
      fragments.push_back(fragment{block * MiB, data, MiB, data});
    }
    slices.emplace_back(range{first * MiB, per_slice * MiB}, host_buffer{std::move(fragments)});
    slices.back().on_complete = done;
  }
  return slices;
}

}  // namespace

TEST_CASE("io_uring submission stays within its allocation budget", "[io][uring][allocations]")
{
  fixture f;
  std::vector<std::uint8_t> memory(fixture::file_bytes);
  std::atomic<std::size_t> handed_back{0};
  auto done = std::make_shared<prepared_io_completion>(
    [&handed_back](std::span<fragment_handle const> handles, bool ok) noexcept {
      if (ok) handed_back.fetch_add(handles.size());
    });

  // One request to warm up lazily created state.
  std::ignore = f.submit(contiguous(memory, 1), MiB);

  auto const one_slice    = f.submit(contiguous(memory, 1), MiB);
  auto const eight_slices = f.submit(contiguous(memory, 8), 8 * MiB);
  auto const one_fragment = f.submit(fragmented(memory, 1, 1, done), MiB);
  auto const eight_in_one = f.submit(fragmented(memory, 1, 8, done), 8 * MiB);
  auto const eight_by_one = f.submit(fragmented(memory, 8, 1, done), 8 * MiB);
  CHECK(handed_back.load() == 17);

  // The measured numbers, printed so a change to them is visible in the log.
  WARN("allocations on the submitting thread: 1 host slice "
       << one_slice << ", 8 host slices " << eight_slices << "; fragmented 1x1 " << one_fragment
       << ", 1x8 " << eight_in_one << ", 8x1 " << eight_by_one);

  // Submitting does not allocate per fragment.
  CHECK(eight_in_one == one_fragment);
  // A fragmented slice costs one allocation more than a contiguous one: the
  // dispatcher keeps the caller's slices until every queue entry exists, so it
  // clones each slice, and a clone copies the fragment list once.
  CHECK(one_fragment <= one_slice + 1);
  CHECK(eight_by_one <= eight_slices + 8);
  // The budget: the numbers measured when this test was written.
  CHECK(one_slice <= 11);
  CHECK(eight_slices <= 14);
}
