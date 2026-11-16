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

// The REST context served by kvikIO's built-in curl engine: the fallback for when the uring_http
// engine cannot be used.

#include "loopback_range_server.hpp"
#include "rest_contract_test_utils.hpp"

#include <cucascade/io/cache/types.hpp>
#include <cucascade/io/engine_ioctx.hpp>
#include <cucascade/io/host_staging_source.hpp>
#include <cucascade/io/io_errors.hpp>
#include <cucascade/io/object_listing.hpp>
#include <cucascade/io/rest/rest_ioctx.hpp>
#include <cucascade/io/uring/uring_ioctx.hpp>
#include <cucascade/memory/fixed_size_host_memory_resource.hpp>
#include <cucascade/memory/numa_region_pinned_host_allocator.hpp>

#include <kvikio/io/engine_registry.hpp>

#include <rmm/cuda_stream.hpp>
#include <rmm/device_buffer.hpp>

#include <cuda_runtime_api.h>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using cucascade::test::loopback_range_server;

namespace {

cucascade::io::object_store_config store_for(loopback_range_server const& server)
{
  cucascade::io::object_store_config store;
  store.endpoint   = server.endpoint();
  store.region     = "us-east-1";
  store.access_key = "test-access-key";
  store.secret_key = "test-secret-key";
  return store;
}

}  // namespace

TEST_CASE("the curl engine serves a REST context: open, read and list", "[rest][curl]")
{
  auto const payload = cucascade::test::rest_test_payload(256 * 1024 + 123);
  loopback_range_server server{
    payload, {}, {{"data/part.0.parquet", 100}, {"data/part.1.parquet", 200}}};

  auto context = cucascade::io::rest::make_curl_rest_ioctx(store_for(server), nullptr);
  REQUIRE(context != nullptr);
  CHECK(context->type() == cucascade::io::io_context_type::restful);
  CHECK(context->engine().name() == "curl");
  CHECK_FALSE(context->has_transport_api());
  CHECK(kvikio::io::engine_registry::instance().selected_name(kvikio::io::engine_route::s3) ==
        "curl");
  CHECK(context->supports("s3://bucket/key"));
  CHECK_FALSE(context->supports_write());

  // One HEAD gives the size; a known size sends none.
  auto object = context->open_io_object("s3://bucket/object.bin");
  CHECK(object->size() == payload.size());
  CHECK(server.head_count() == 1);
  auto sized = context->open_io_object("s3://bucket/object.bin", std::uint64_t{4096});
  CHECK(sized->size() == 4096);
  CHECK(server.head_count() == 1);

  std::vector<std::uint8_t> bytes(100 * 1024);
  auto future = context->host_read_async_io(*object, 777, bytes.size(), bytes.data());
  CHECK(std::move(future).get(30s) == bytes.size());
  CHECK(std::equal(bytes.begin(), bytes.end(), payload.begin() + 777));

  std::vector<std::uint8_t> head(16);
  CHECK(context->host_read_io(*object, 0, head.size(), head.data()) == head.size());
  CHECK(std::equal(head.begin(), head.end(), payload.begin()));

  auto const entries = context->list_objects("bucket", "data/");
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].key == "data/part.0.parquet");
  CHECK(entries[1].size == 200);
  CHECK(server.list_count() >= 1);

  // A host that only holds an ioctx lists through the engine's listing interface, whichever
  // engine serves the context; an interface the engine lacks is absent.  The interface shares
  // ownership of the context.
  {
    std::shared_ptr<cucascade::io::ioctx> const as_ioctx = context;
    auto const listing = cucascade::io::engine_api<cucascade::io::object_listing>(as_ioctx);
    REQUIRE(listing != nullptr);
    CHECK(as_ioctx.use_count() == 3);
    CHECK(listing->list_objects("bucket", "data/").size() == 2);
    CHECK(cucascade::io::engine_api<cucascade::io::uring::uring_diagnostics>(as_ioctx) == nullptr);
    CHECK(as_ioctx.use_count() == 3);
  }

  // What only the uring_http engine has: reported, not faked.
  std::vector<std::string> const paths{"s3://bucket/object.bin"};
  CHECK_THROWS_AS(context->resolve_footer_objects(
                    paths, [](cucascade::io::rest::footer_resolve_result const&) {}),
                  cucascade::io::not_implemented_error);
  CHECK(context->stats().gets == 0);
  CHECK(context->dispatch_fanout() == 1);

  // Destroying the context stops selecting the engine.
  context.reset();
  CHECK(
    kvikio::io::engine_registry::instance().selected_name(kvikio::io::engine_route::s3).empty());
}

// The contracts every engine keeps, checked for the curl engine through the REST context that
// cuCascade builds on it: a future that resolved means the bytes are in the destination, device
// memory included, and a read that failed leaves the destination alone.
TEST_CASE("the curl engine keeps the read contracts through a REST context", "[rest][curl]")
{
  using cucascade::test::range_fault_policy;
  using cucascade::test::require_rest_bytes_equal;
  using cucascade::test::rest_device_bytes;
  int devices = 0;
  if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
    SKIP("requires a CUDA device");
  }
  constexpr std::size_t block_size = 1UL << 20;
  constexpr std::size_t capacity   = 8 * block_size;
  cucascade::memory::numa_region_pinned_host_memory_resource upstream{0};
  cucascade::memory::fixed_size_host_memory_resource mr{
    0, upstream, capacity, capacity, block_size, 4, 1};
  auto const staging = std::make_shared<cucascade::io::host_staging_source>(mr);
  auto const payload = cucascade::test::rest_test_payload(3 * block_size + 777);

  SECTION("ranges are read into host memory")
  {
    loopback_range_server server{payload};
    auto context = cucascade::io::rest::make_curl_rest_ioctx(store_for(server), staging);
    auto object  = context->open_io_object("s3://bucket/object.bin", payload.size());
    std::vector<std::pair<std::size_t, std::size_t>> const ranges{
      {17, 257}, {64 * 1024 + 9, 1024}, {2 * block_size + 11, 4096}, {payload.size() - 333, 333}};
    std::vector<std::vector<std::uint8_t>> buffers;
    std::vector<cucascade::io::slice> segments;
    std::size_t total = 0;
    for (auto const& [offset, size] : ranges) {
      buffers.emplace_back(size);
      total += size;
    }
    for (std::size_t i = 0; i < ranges.size(); ++i) {
      segments.emplace_back(ranges[i].first, ranges[i].second, buffers[i].data());
    }
    REQUIRE(context->host_readv_async_io(*object, segments).get(30s) == total);
    for (std::size_t i = 0; i < ranges.size(); ++i) {
      require_rest_bytes_equal(buffers[i], {payload.data() + ranges[i].first, ranges[i].second});
    }
  }

  SECTION("device memory holds the bytes when the future resolves")
  {
    loopback_range_server server{payload};
    auto context = cucascade::io::rest::make_curl_rest_ioctx(store_for(server), staging);
    auto object  = context->open_io_object("s3://bucket/object.bin", payload.size());
    rmm::cuda_stream stream;
    rmm::device_buffer destination(payload.size(), stream);
    auto read = context->device_read_async_io(*object,
                                              0,
                                              payload.size(),
                                              static_cast<std::uint8_t*>(destination.data()),
                                              ::cuda::stream_ref{stream.value()});
    REQUIRE(std::move(read).get(30s) == payload.size());
    // Read back on another stream, without synchronizing the one the read used: the copies
    // must already have happened.
    rmm::cuda_stream other;
    require_rest_bytes_equal(rest_device_bytes(destination.data(), payload.size(), other.value()),
                             payload);
    // Every staging block went back to the HOST resource.
    CHECK(mr.get_free_blocks() == mr.get_total_blocks());
  }

  SECTION("a failed read fails the future and leaves the destinations alone")
  {
    range_fault_policy fault{};
    fault.fail_all_gets = true;
    fault.fail_status   = 404;
    loopback_range_server server{payload, fault};
    auto context = cucascade::io::rest::make_curl_rest_ioctx(store_for(server), staging);
    auto object  = context->open_io_object("s3://bucket/object.bin", payload.size());

    std::vector<std::uint8_t> host(64 * 1024, 0xEE);
    auto host_read = context->host_read_async_io(*object, 0, host.size(), host.data());
    CHECK_THROWS(std::move(host_read).get(30s));
    CHECK(std::all_of(host.begin(), host.end(), [](std::uint8_t b) { return b == 0xEE; }));

    rmm::cuda_stream stream;
    rmm::device_buffer destination(host.size(), stream);
    REQUIRE(cudaMemsetAsync(destination.data(), 0xEE, host.size(), stream.value()) == cudaSuccess);
    auto device_read = context->device_read_async_io(*object,
                                                     0,
                                                     host.size(),
                                                     static_cast<std::uint8_t*>(destination.data()),
                                                     ::cuda::stream_ref{stream.value()});
    CHECK_THROWS(std::move(device_read).get(30s));
    auto const after = rest_device_bytes(destination.data(), host.size(), stream.value());
    CHECK(std::all_of(after.begin(), after.end(), [](std::uint8_t b) { return b == 0xEE; }));
    CHECK(mr.get_free_blocks() == mr.get_total_blocks());
  }
}

// What the fs_cache relies on: the engine hands a chunk back when the range request that filled
// it finishes, not when the whole read does. One range is small enough to be answered at once,
// the other is held by the server halfway through its body.
TEST_CASE("the curl engine publishes each cache chunk when its own range request finishes",
          "[rest][curl]")
{
  using cucascade::io::cache::cached_chunk;
  using cucascade::io::cache::chunk_fill;
  using cucascade::io::cache::chunk_state;
  constexpr std::size_t small_bytes = 4096;
  constexpr std::size_t large_bytes = 64 * 1024;
  constexpr std::size_t large_at    = 128 * 1024;
  auto const payload                = cucascade::test::rest_test_payload(256 * 1024);
  // Declared before the context: they outlive it on every exit path.
  std::vector<std::uint8_t> small_memory(small_bytes, 0xEE);
  std::vector<std::uint8_t> large_memory(large_bytes, 0xEE);
  cached_chunk small{0};
  cached_chunk large{large_at};

  auto control = std::make_shared<cucascade::test::range_response_control>();
  cucascade::test::range_fault_policy fault{};
  fault.body =
    cucascade::test::response_body_policy{.hold_after_bytes = 2 * small_bytes, .control = control};
  loopback_range_server server{payload, fault};
  auto context = cucascade::io::rest::make_curl_rest_ioctx(store_for(server), nullptr);
  auto object  = context->open_io_object("s3://bucket/object.bin", payload.size());
  // Released before the context goes away, so a failed check cannot leave the read held.
  cucascade::test::response_release_guard const release{control};

  // Two chunks claimed for loading, as the fs_cache hands them to a backend.
  auto const claim = [](cached_chunk& chunk, std::uint8_t* memory) {
    chunk.data = memory;
    REQUIRE(chunk.state.mark_queued());
    REQUIRE(chunk.state.mark_allocated());
    chunk_fill fill;
    REQUIRE(chunk.state.take_loading_merging(chunk_fill::whole(), fill));
    return fill;
  };
  auto completion = std::make_shared<cucascade::io::prepared_io_completion>(
    [](std::span<cucascade::io::fragment_handle const> completed, bool ok) noexcept {
      cucascade::io::cache::publish_chunk_fill(completed, ok);
    });
  std::vector<cucascade::io::prepared_io_slice> slices;
  slices.emplace_back(cucascade::io::range{0, small_bytes},
                      cucascade::io::cache::make_fragment_buffer(
                        &small, claim(small, small_memory.data()), small_bytes));
  slices.back().on_complete = completion;
  slices.emplace_back(cucascade::io::range{large_at, large_bytes},
                      cucascade::io::cache::make_fragment_buffer(
                        &large, claim(large, large_memory.data()), large_bytes));
  slices.back().on_complete = completion;

  auto future = context->mixed_readv_async_io(*object, std::move(slices));

  // The small range is answered in full; the large one stops in the server.
  REQUIRE(control->wait_for_holds(1, 10s));
  auto const deadline = std::chrono::steady_clock::now() + 10s;
  while (small.state.get_state() != chunk_state::cached &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }
  CHECK(small.state.get_state() == chunk_state::cached);
  CHECK(std::equal(small_memory.begin(), small_memory.end(), payload.begin()));
  CHECK(large.state.get_state() == chunk_state::loading);
  CHECK_FALSE(future.is_ready());

  control->release_all();
  REQUIRE(std::move(future).get(30s) == small_bytes + large_bytes);
  CHECK(large.state.get_state() == chunk_state::cached);
  CHECK(std::equal(large_memory.begin(), large_memory.end(), payload.begin() + large_at));
  CHECK(server.get_count() == 2);
}
