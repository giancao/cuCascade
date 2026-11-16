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

#include <cucascade/error.hpp>
#include <cucascade/io/cache/config.hpp>
#include <cucascade/io/cache/fs_cache.hpp>
#include <cucascade/io/io_context.hpp>
#include <cucascade/io/io_errors.hpp>
#include <cucascade/io/io_request.hpp>
#include <cucascade/io/types.hpp>
#include <cucascade/log/logging.hpp>

#include <cuda_runtime_api.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cucascade::io {
namespace {

using nvtx_range = nvtx3::scoped_range_in<libcucascade_domain>;

struct io_read_to_host_message {
  static constexpr char const* message{"io:read_to_host"};
};

struct io_write_from_host_message {
  static constexpr char const* message{"io:write_from_host"};
};

[[noreturn]] void throw_errc(std::errc code, std::string const& what)
{
  throw std::system_error(std::make_error_code(code), what);
}

/// Name of a backend in error messages.
[[nodiscard]] std::string_view backend_name(io_context_type type) noexcept
{
  switch (type) {
    case io_context_type::uring: return "uring";
    case io_context_type::restful: return "rest";
    case io_context_type::s3rdma: return "s3rdma";
    case io_context_type::cufile: return "cufile";
    case io_context_type::cufile_posix: return "cufile_posix";
  }
  return "ioctx";
}

/// Throw unless @p ctx can write @p obj (with a device source when @p device).
/// A backend that cannot write at all reports @c not_implemented_error; one
/// that writes host sources only reports a plain @c not_supported for a device
/// source.
void require_writable(ioctx const& ctx, io_object const& obj, bool device, char const* operation)
{
  if (!ctx.supports_write()) {
    throw not_implemented_error(std::string(backend_name(ctx.type())) + ": " + operation);
  }
  if (device && !ctx.supports_device_write()) {
    throw_errc(std::errc::not_supported, "ioctx: backend cannot write from device memory");
  }
  if (!obj.writable()) {
    throw_errc(std::errc::bad_file_descriptor,
               "ioctx: '" + obj.object_path() + "' was not opened for writing");
  }
}

}  // namespace

/// What the front end keeps for one writable object of a backend that writes with O_DIRECT. It
/// is attached to the object when the object is opened (io_object::client_state), so finding it
/// takes no lock.
struct ioctx::write_edge_object {
  /// Serializes the partial-block work of this object's unaligned requests and the trimming of
  /// padding. Block-aligned requests never take it, and a completion only ever tries it.
  std::mutex mutex;
  std::atomic<bool> trim_pending{false};
  /// The largest end of any write submitted through the object, and the size the file had when
  /// none of this object's padding could be in it. The file is never cut below it.
  std::atomic<std::size_t> data_end{0};
  /// Unaligned requests that have not finished: their padding may be in the file.
  std::atomic<std::size_t> outstanding{0};
  // Under `mutex`: the block the file's data ended in when this object last wrote it.
  bool has_block{false};
  std::size_t block_offset{0};
  std::size_t block_data_end{0};
  std::unique_ptr<std::uint8_t, void (*)(void*)> image{nullptr, std::free};
};

/// Counters of how partial blocks were completed.
struct ioctx::write_edge_state {
  std::atomic<std::uint64_t> merged_from_memory{0};
  std::atomic<std::uint64_t> read_from_file{0};
  std::atomic<std::uint64_t> zero_filled{0};
};

namespace {

using write_edge_object = ioctx::write_edge_object;

[[nodiscard]] std::unique_ptr<std::uint8_t, void (*)(void*)> make_block_image(std::size_t block)
{
  auto* memory = static_cast<std::uint8_t*>(std::aligned_alloc(block, block));
  if (memory == nullptr) throw std::bad_alloc();
  return {memory, std::free};
}

void store_max(std::atomic<std::size_t>& target, std::size_t value) noexcept
{
  auto seen = target.load(std::memory_order_relaxed);
  while (seen < value && !target.compare_exchange_weak(seen, value, std::memory_order_acq_rel)) {}
}

/// Cut the file back to the end of its data when all that follows is the zero padding of a last
/// block that was written whole. Called with the object's mutex held.
void trim_padding(const io_object& obj, write_edge_object& state, std::size_t block) noexcept
{
  try {
    auto const on_disk = obj.sync_size();
    auto const data    = state.data_end.load(std::memory_order_acquire);
    // More than one block past the data is not this object's padding: leave it alone.
    if (on_disk > data && on_disk <= (data + block - 1) / block * block) { obj.resize(data); }
  } catch (std::exception const& e) {
    CUCASCADE_LOG_ERROR(
      "ioctx: cannot trim the block padding of '{}': {}", obj.object_path(), e.what());
  } catch (...) {
    CUCASCADE_LOG_ERROR("ioctx: cannot trim the block padding of '{}'", obj.object_path());
  }
}

/// Run a requested trim if the mutex is free; whoever holds it runs this after releasing it.
void drain_trim(const io_object& obj, write_edge_object& state, std::size_t block) noexcept
{
  while (state.trim_pending.load(std::memory_order_acquire) && state.mutex.try_lock()) {
    std::lock_guard held(state.mutex, std::adopt_lock);
    if (state.trim_pending.exchange(false, std::memory_order_acq_rel)) {
      trim_padding(obj, state, block);
    }
  }
}

/// An unaligned request is over, successfully or not: its padding can go. Never blocks, so it
/// can run on the thread that completes the request.
void finish_unaligned_write(const io_object& obj,
                            write_edge_object& state,
                            std::size_t block) noexcept
{
  state.outstanding.fetch_sub(1, std::memory_order_acq_rel);
  state.trim_pending.store(true, std::memory_order_release);
  drain_trim(obj, state, block);
}

}  // namespace

ioctx::ioctx() : _write_edges(std::make_unique<write_edge_state>()) {}
ioctx::~ioctx() = default;

ioctx::write_edge_counters ioctx::write_edge_stats() const noexcept
{
  return {_write_edges->merged_from_memory.load(std::memory_order_relaxed),
          _write_edges->read_from_file.load(std::memory_order_relaxed),
          _write_edges->zero_filled.load(std::memory_order_relaxed)};
}

void ioctx::write_edge_blocks(const io_object& obj,
                              write_edge_object& state,
                              std::vector<write_edge_piece>& pieces,
                              std::size_t request_begin,
                              std::size_t request_end)
{
  auto const block = write_alignment_requirement();
  std::ranges::sort(pieces, {}, &write_edge_piece::offset);
  {
    std::unique_lock lock(state.mutex);
    // A trim that was left for the holder of the mutex: do it before looking at the file.
    if (state.trim_pending.exchange(false, std::memory_order_acq_rel)) {
      trim_padding(obj, state, block);
    }
    // The file as it is now (not this object's view of it): bytes a failed write or another
    // writer left there are the file's, and are read back rather than replaced.
    auto const file_size = obj.sync_size();
    // With no unaligned request of this object in flight the file holds none of its padding, so
    // the file ends where its data ends.
    bool const alone = state.outstanding.load(std::memory_order_acquire) == 0;
    if (alone) store_max(state.data_end, file_size);
    auto const data_before = alone ? file_size : state.data_end.load(std::memory_order_acquire);
    // The request itself starts inside a block. The bytes in front of it are not the request's
    // and are never invented: they must be the file's. Checked before anything is registered or
    // written.
    if (auto const head = pieces.front().offset;
        head == request_begin && head % block != 0 && head > data_before) {
      throw std::invalid_argument(
        "ioctx: O_DIRECT write to '" + obj.object_path() + "' starts at offset " +
        std::to_string(head) + ", inside a " + std::to_string(block) +
        "-byte block and past the end of the file's data (" + std::to_string(data_before) +
        "): the bytes in front of it do not exist. Start on a block boundary, or continue where "
        "the data ends");
    }
    store_max(state.data_end, request_end);
    state.outstanding.fetch_add(1, std::memory_order_acq_rel);
    try {
      auto const data_after = std::max(data_before, request_end);
      auto image            = make_block_image(block);
      for (std::size_t i = 0; i < pieces.size();) {
        auto const at   = pieces[i].offset - pieces[i].offset % block;
        auto const head = pieces[i].offset;
        if (alone && state.has_block && state.block_offset == at &&
            state.block_data_end == file_size && head == file_size) {
          // The write continues exactly where this object left the file, and the file still
          // ends there: the block is the one remembered.
          std::memcpy(image.get(), state.image.get(), block);
          _write_edges->merged_from_memory.fetch_add(1, std::memory_order_relaxed);
        } else if (at < file_size) {
          // Complete the block from the file. A whole block is read the way it was written (the
          // backend's asynchronous read is O_DIRECT for an aligned block); the plain read serves
          // a file that ends inside the block, and a backend whose reactors cannot run.
          std::memset(image.get(), 0, block);
          auto const present = std::min(block, file_size - at);
          std::size_t got    = 0;
          bool have          = false;
          if (present == block) {
            try {
              got  = host_read_async_io(obj, at, block, image.get()).get();
              have = true;
            } catch (...) {
              have = false;
            }
          }
          if (!have) got = host_read_io(obj, at, present, image.get());
          if (got != present) {
            throw_errc(std::errc::io_error, "ioctx: short read completing a partial block");
          }
          _write_edges->read_from_file.fetch_add(1, std::memory_order_relaxed);
        } else {
          // Past the end of the file and behind the request's first byte: a gap inside the
          // request, or what follows its last byte. Zeros, as a hole would read.
          std::memset(image.get(), 0, block);
          _write_edges->zero_filled.fetch_add(1, std::memory_order_relaxed);
        }
        for (; i < pieces.size() && pieces[i].offset < at + block; ++i) {
          std::memcpy(image.get() + (pieces[i].offset - at), pieces[i].src, pieces[i].size);
        }
        if (host_write_io(obj, at, block, image.get()) != block) {
          throw_errc(std::errc::io_error, "ioctx: short write of a completed block");
        }
        if (data_after > at && data_after < at + block) {
          // The file's data now ends inside this block: remember it for a write that continues.
          if (state.image == nullptr) state.image = make_block_image(block);
          std::memcpy(state.image.get(), image.get(), block);
          state.block_offset   = at;
          state.block_data_end = data_after;
          state.has_block      = true;
        } else if (state.has_block && state.block_offset == at) {
          std::memcpy(state.image.get(), image.get(), block);  // keep the copy current
        }
      }
    } catch (...) {
      lock.unlock();
      finish_unaligned_write(obj, state, block);
      throw;
    }
  }
  drain_trim(obj, state, block);
}

void ioctx::initialize_cache(
  cucascade::memory::memory_reservation_manager& reservation_manager,
  io::cache::config const& cache_config,
  std::shared_ptr<const cucascade::memory::topology_index> topology_index) noexcept
{
  // One-shot.  Repeated calls are silent no-ops so callers can be
  // robust to multiple wiring sites.
  if (_cache) {
    CUCASCADE_LOG_WARN("ioctx::initialize_cache() called but fs_cache already present");
    return;
  }
  if (!can_use_fs_cache()) {
    CUCASCADE_LOG_WARN(
      "ioctx::initialize_cache() called but backend does not support vector host read");
    return;
  }
  try {
    _cache = std::make_unique<cache::fs_cache>(
      reservation_manager, this, cache_config, std::move(topology_index));
  } catch (const std::exception& e) {
    CUCASCADE_LOG_ERROR("fs_cache construction failed: {}", e.what());
    _cache.reset();
  } catch (...) {
    CUCASCADE_LOG_ERROR("fs_cache construction failed: unknown error");
    _cache.reset();
  }
  // The reactors plan a fragmented fill's extent with
  // cache::fill_span(fill, chunk->offset, their own staging block size).  A
  // cache whose chunks are a different size makes every partial fill compute
  // the wrong extent -- with the larger staging block that is an out-of-bounds
  // write past the end of a pinned chunk.  The two are equal today only because
  // both read the same front HOST arena; refuse the cache rather than let a
  // future split of those resources corrupt the heap silently.
  if (_cache && staging_block_size() != 0 && staging_block_size() != _cache->chunk_size()) {
    CUCASCADE_LOG_ERROR(
      "ioctx::initialize_cache: backend {} stages in {}-byte blocks but the prefetching cache "
      "chunk is {} bytes; the two must match because fragmented fills are planned with the "
      "staging block size -- running without a cache",
      static_cast<int>(type()),
      staging_block_size(),
      _cache->chunk_size());
    _cache.reset();
  }
}

void ioctx::shutdown_cache() noexcept { _cache.reset(); }

std::shared_ptr<io_object> ioctx::create_io_object(std::string path, open_hint /*hint*/)
{
  return create_io_object(std::move(path));
}

std::shared_ptr<io_object> ioctx::create_io_object(std::string path, std::uint64_t /*known_size*/)
{
  return create_io_object(std::move(path));
}

size_t ioctx::host_read(const io_object& obj,
                        size_t offset,
                        size_t size,
                        uint8_t* dst,
                        cache::cache_handle* handle,
                        io_priority priority)
{
  auto const& message =
    nvtx3::registered_string_in<libcucascade_domain>::get<io_read_to_host_message>();
  nvtx_range const read_range{message, nvtx3::payload{static_cast<std::uint64_t>(size)}};
  // Without a cache the backend reads synchronously on this thread, bypassing
  // the reactor queues, so the priority only matters on the cache path.
  if (uses_fs_cache()) { return _cache->host_read(obj, offset, size, dst, handle, priority); }
  return host_read_io(obj, offset, size, dst);
}

exec::semi_future<size_t> ioctx::host_read_async(const io_object& obj,
                                                 size_t offset,
                                                 size_t size,
                                                 uint8_t* dst,
                                                 cache::cache_handle* handle,
                                                 io_priority priority)
{
  if (uses_fs_cache()) { return _cache->host_read_async(obj, offset, size, dst, handle, priority); }
  return host_read_async_io(obj, offset, size, dst, priority);
}

exec::semi_future<size_t> ioctx::device_read_async(const io_object& obj,
                                                   size_t offset,
                                                   size_t size,
                                                   uint8_t* dst,
                                                   ::cuda::stream_ref stream,
                                                   cache::cache_handle* handle,
                                                   io_priority priority)
{
  if (uses_fs_cache()) {
    return _cache->device_read_async(obj, offset, size, dst, stream, handle, priority);
  }
  return device_read_async_io(obj, offset, size, dst, stream, priority);
}

exec::semi_future<size_t> ioctx::host_read_async_io(
  const io_object& obj, size_t offset, size_t size, uint8_t* dst, io_priority priority) noexcept
{
  if (size == 0) return exec::make_semi_future<size_t>(0);
  try {
    if (dst == nullptr) throw std::invalid_argument("host read destination is null");
    std::vector<prepared_io_slice> slices;
    slices.emplace_back(range{offset, size}, host_buffer{dst});
    slices.front().priority = resolve_priority(priority, io_priority::high);
    return host_device_readv_async_io(obj, std::move(slices));
  } catch (...) {
    return exec::make_semi_future<size_t>(std::current_exception());
  }
}

exec::semi_future<size_t> ioctx::device_read_async_io(const io_object& obj,
                                                      size_t offset,
                                                      size_t size,
                                                      uint8_t* dst,
                                                      ::cuda::stream_ref stream,
                                                      io_priority priority) noexcept
{
  if (size == 0) return exec::make_semi_future<size_t>(0);
  try {
    if (dst == nullptr) throw std::invalid_argument("device read destination is null");
    std::vector<prepared_io_slice> slices;
    slices.emplace_back(range{offset, size}, device_buffer{dst, stream.get()});
    slices.front().priority = resolve_priority(priority, io_priority::high);
    return host_device_readv_async_io(obj, std::move(slices));
  } catch (...) {
    return exec::make_semi_future<size_t>(std::current_exception());
  }
}

exec::semi_future<size_t> ioctx::host_readv_async_io(const io_object& obj,
                                                     std::span<const slice> slices,
                                                     io_priority priority) noexcept
{
  if (slices.empty()) return exec::make_semi_future<size_t>(0);
  try {
    auto const resolved = resolve_priority(priority, io_priority::high);
    std::vector<prepared_io_slice> prepared_slices;
    prepared_slices.reserve(slices.size());
    for (auto const& current : slices) {
      if (current.size() == 0) continue;
      if (current.dst == nullptr) throw std::invalid_argument("host readv destination is null");
      prepared_slices.emplace_back(range{current.offset(), current.size()},
                                   host_buffer{current.dst});
      prepared_slices.back().priority = resolved;
    }
    return host_device_readv_async_io(obj, std::move(prepared_slices));
  } catch (...) {
    return exec::make_semi_future<size_t>(std::current_exception());
  }
}

exec::semi_future<size_t> ioctx::device_readv_async_io(const io_object& obj,
                                                       std::span<const slice> slices,
                                                       ::cuda::stream_ref stream,
                                                       io_priority priority) noexcept
{
  if (slices.empty()) return exec::make_semi_future<size_t>(0);
  try {
    auto const resolved = resolve_priority(priority, io_priority::high);
    std::vector<prepared_io_slice> prepared_slices;
    prepared_slices.reserve(slices.size());
    for (auto const& current : slices) {
      if (current.size() == 0) continue;
      if (current.dst == nullptr) throw std::invalid_argument("device readv destination is null");
      prepared_slices.emplace_back(range{current.offset(), current.size()},
                                   device_buffer{current.dst, stream.get()});
      prepared_slices.back().priority = resolved;
    }
    return host_device_readv_async_io(obj, std::move(prepared_slices));
  } catch (...) {
    return exec::make_semi_future<size_t>(std::current_exception());
  }
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

std::shared_ptr<io_object> ioctx::open_io_object_for_write(std::string path,
                                                           write_open_options options)
{
  auto object = create_io_object_for_write(strip_file_scheme(path), options);
  if (object == nullptr) return object;
  // A backend that writes with O_DIRECT: the state of the object's unaligned writes.
  if (write_alignment_requirement() > 1 && object->client_state() == nullptr) {
    object->set_client_state(std::make_shared<write_edge_object>());
  }
  // Truncating (or replacing) the file changes its bytes as much as a write.
  if (options.mode == write_mode::create_or_truncate || options.mode == write_mode::create_new) {
    invalidate_cached(object->object_path());
  }
  return stamped(std::move(object));
}

void ioctx::invalidate_cached(std::string_view object_path) noexcept
{
  if (_cache) { _cache->invalidate(object_path); }
  _metadata_store.invalidate_written(object_path);
}

std::size_t ioctx::host_write(const io_object& obj,
                              std::size_t offset,
                              std::size_t size,
                              const std::uint8_t* src)
{
  auto const& message =
    nvtx3::registered_string_in<libcucascade_domain>::get<io_write_from_host_message>();
  nvtx_range const write_range{message, nvtx3::payload{static_cast<std::uint64_t>(size)}};
  if (size == 0) return 0;
  if (src == nullptr) throw std::invalid_argument("host write source is null");
  constexpr auto max_end = static_cast<std::size_t>(std::numeric_limits<off_t>::max());
  if (size > max_end || offset > max_end - size) {
    throw std::invalid_argument("host write range exceeds the largest file offset");
  }
  require_writable(*this, obj, false, "host_write");

  // Drop cached bytes of the path before and after the write: the second pass
  // catches a prefetch that read pre-write bytes while the write ran.
  invalidate_cached(obj.object_path());
  try {
    std::size_t written = 0;
    auto const block    = write_alignment_requirement();
    auto state =
      block > 1 ? std::static_pointer_cast<write_edge_object>(obj.client_state()) : nullptr;
    if (block > 1 && (offset % block != 0 || size % block != 0)) {
      // Never hand the backend less than a block: see write_edge_blocks.
      if (state == nullptr) state = std::make_shared<write_edge_object>();
      auto const end = offset + size;
      auto const first =
        std::min(end, offset % block == 0 ? offset : offset - offset % block + block);
      auto const last = std::max(first, end - end % block);
      std::vector<write_edge_piece> pieces;
      if (first > offset) pieces.push_back({offset, first - offset, src});
      if (end > last) pieces.push_back({last, end - last, src + (last - offset)});
      write_edge_blocks(obj, *state, pieces, offset, end);
      try {
        if (last > first) host_write_io(obj, first, last - first, src + (first - offset));
      } catch (...) {
        finish_unaligned_write(obj, *state, block);
        throw;
      }
      finish_unaligned_write(obj, *state, block);
      written = size;
    } else {
      if (state != nullptr) store_max(state->data_end, offset + size);
      written = host_write_io(obj, offset, size, src);
    }
    invalidate_cached(obj.object_path());
    return written;
  } catch (...) {
    invalidate_cached(obj.object_path());
    throw;
  }
}

exec::semi_future<std::size_t> ioctx::host_write_async(const io_object& obj,
                                                       std::size_t offset,
                                                       std::size_t size,
                                                       const std::uint8_t* src,
                                                       io_priority priority) noexcept
{
  write_slice const slice{offset, size, src};
  return submit_write(
    obj, std::span{&slice, 1}, {}, ::cuda::stream_ref{cudaStream_t{nullptr}}, priority);
}

exec::semi_future<std::size_t> ioctx::device_write_async(const io_object& obj,
                                                         std::size_t offset,
                                                         std::size_t size,
                                                         const std::uint8_t* src,
                                                         ::cuda::stream_ref stream,
                                                         io_priority priority) noexcept
{
  write_slice const slice{offset, size, src};
  return submit_write(obj, {}, std::span{&slice, 1}, stream, priority);
}

exec::semi_future<std::size_t> ioctx::host_writev_async(const io_object& obj,
                                                        std::span<const write_slice> slices,
                                                        io_priority priority) noexcept
{
  return submit_write(obj, slices, {}, ::cuda::stream_ref{cudaStream_t{nullptr}}, priority);
}

exec::semi_future<std::size_t> ioctx::device_writev_async(const io_object& obj,
                                                          std::span<const write_slice> slices,
                                                          ::cuda::stream_ref stream,
                                                          io_priority priority) noexcept
{
  return submit_write(obj, {}, slices, stream, priority);
}

exec::semi_future<std::size_t> ioctx::mixed_writev_async(const io_object& obj,
                                                         std::span<const write_slice> host_slices,
                                                         std::span<const write_slice> device_slices,
                                                         ::cuda::stream_ref stream,
                                                         io_priority priority) noexcept
{
  return submit_write(obj, host_slices, device_slices, stream, priority);
}

exec::semi_future<std::size_t> ioctx::submit_write(const io_object& obj,
                                                   std::span<const write_slice> host_slices,
                                                   std::span<const write_slice> device_slices,
                                                   ::cuda::stream_ref stream,
                                                   io_priority priority) noexcept
{
  try {
    auto const resolved = resolve_priority(priority, io_priority::high);
    // A backend that writes with O_DIRECT is handed whole blocks only; the bytes
    // that share a block with something else go through write_edge_blocks.
    auto const block =
      write_alignment_requirement() > 1 ? write_alignment_requirement() : std::size_t{1};
    struct cut {
      range head, whole, tail;
    };
    auto const cut_of = [block](range rng) {
      auto const end   = rng.end();
      auto const first = std::min(
        end, rng.offset % block == 0 ? rng.offset : rng.offset - rng.offset % block + block);
      auto const last = std::max(first, end - end % block);
      return cut{
        range{rng.offset, first - rng.offset}, range{first, last - first}, range{last, end - last}};
    };
    std::vector<prepared_io_slice> slices;
    slices.reserve(host_slices.size() + device_slices.size());
    std::vector<write_edge_piece> pieces;
    std::vector<range> whole_ranges;
    std::size_t edge_bytes = 0;
    for (auto const& current : host_slices) {
      if (current.size() == 0) continue;
      if (current.src == nullptr) throw std::invalid_argument("host write source is null");
      auto const parts = cut_of(current.rng);
      for (auto const& part : {parts.head, parts.tail}) {
        if (part.size == 0) continue;
        pieces.push_back(
          {part.offset, part.size, current.src + (part.offset - current.rng.offset)});
        edge_bytes += part.size;
      }
      if (parts.whole.size == 0) continue;
      whole_ranges.push_back(parts.whole);
      slices.push_back(make_host_write_slice(
        parts.whole, current.src + (parts.whole.offset - current.rng.offset), resolved));
    }
    bool const has_device = std::ranges::any_of(
      device_slices, [](write_slice const& current) { return current.size() != 0; });
    // The partial blocks of device slices are copied to the host first, in stream order.
    std::vector<std::uint8_t> device_edges;
    std::vector<std::pair<std::size_t, const std::uint8_t*>> device_copies;  // piece index, source
    for (auto const& current : device_slices) {
      if (current.size() == 0) continue;
      if (current.src == nullptr) throw std::invalid_argument("device write source is null");
      auto const parts = cut_of(current.rng);
      for (auto const& part : {parts.head, parts.tail}) {
        if (part.size == 0) continue;
        device_copies.emplace_back(pieces.size(), current.src + (part.offset - current.rng.offset));
        pieces.push_back({part.offset, part.size, nullptr});
        edge_bytes += part.size;
      }
      if (parts.whole.size == 0) continue;
      whole_ranges.push_back(parts.whole);
      slices.push_back(
        make_device_write_slice(parts.whole,
                                current.src + (parts.whole.offset - current.rng.offset),
                                stream.get(),
                                -1,
                                resolved));
    }
    if (slices.empty() && pieces.empty()) return exec::make_semi_future<std::size_t>(0);
    require_writable(*this, obj, has_device, "write");
    if (!pieces.empty()) {
      // The backend validates what it is handed, but the partial blocks are written here, before
      // it sees anything: check the request as the caller wrote it first (overlaps, ranges).
      std::vector<prepared_io_slice> original;
      original.reserve(host_slices.size() + device_slices.size());
      for (auto const& current : host_slices) {
        if (current.size() != 0)
          original.push_back(make_host_write_slice(current.rng, current.src));
      }
      for (auto const& current : device_slices) {
        if (current.size() != 0) {
          original.push_back(make_device_write_slice(current.rng, current.src, stream.get(), -1));
        }
      }
      std::ignore = validate_write_slices(original);
    }
    if (!device_copies.empty()) {
      std::size_t total = 0;
      for (auto const& [index, source] : device_copies)
        total += pieces[index].size;
      device_edges.resize(total);
      std::size_t used = 0;
      for (auto const& [index, source] : device_copies) {
        CUCASCADE_CUDA_TRY(cudaMemcpyAsync(device_edges.data() + used,
                                           source,
                                           pieces[index].size,
                                           cudaMemcpyDeviceToHost,
                                           stream.get()));
        pieces[index].src = device_edges.data() + used;
        used += pieces[index].size;
      }
      CUCASCADE_CUDA_TRY(cudaStreamSynchronize(stream.get()));
    }

    auto const& path = obj.object_path();
    invalidate_cached(path);
    cache::fs_cache::pending_invalidation pending;
    if (_cache) { pending = _cache->invalidate_later(path); }
    // Set when the request has partial blocks: its padding is trimmed when it is over.
    std::shared_ptr<write_edge_object> edges;
    std::shared_ptr<const io_object> owner;
    if (block > 1) {
      auto state                = std::static_pointer_cast<write_edge_object>(obj.client_state());
      std::size_t request_begin = std::numeric_limits<std::size_t>::max();
      std::size_t request_end   = 0;
      for (auto const& piece : pieces) {
        request_begin = std::min(request_begin, piece.offset);
        request_end   = std::max(request_end, piece.offset + piece.size);
      }
      for (auto const& whole : whole_ranges) {
        request_begin = std::min(request_begin, whole.offset);
        request_end   = std::max(request_end, whole.end());
      }
      if (pieces.empty()) {
        if (state != nullptr) store_max(state->data_end, request_end);
      } else {
        try {
          owner = obj.weak_from_this().lock();  // null only for an object no backend accepts
          edges = state != nullptr ? std::move(state) : std::make_shared<write_edge_object>();
          write_edge_blocks(obj, *edges, pieces, request_begin, request_end);
        } catch (...) {
          pending.fire();
          _metadata_store.invalidate_written(path);
          throw;
        }
      }
    }
    if (slices.empty()) {
      if (edges != nullptr) finish_unaligned_write(obj, *edges, block);
      pending.fire();
      _metadata_store.invalidate_written(path);
      return exec::make_semi_future<std::size_t>(std::size_t{edge_bytes});
    }
    try {
      // Everything the completion needs is allocated BEFORE the write is
      // dispatched, so nothing can fail between starting physical work and
      // attaching the second invalidation pass.  That pass runs once the write
      // settled, before the caller sees it: a prefetch that raced the write may
      // have cached pre-write bytes.  It runs on the completing thread; `pending`
      // keeps the cache alive until then, and the metadata store is a member of
      // this ioctx, which outlives every completion of its own backend (reactors
      // are joined in its destructor).
      exec::promise<std::size_t> bridge;
      auto result = bridge.get_semi_future();
      // The future's exact callback type, so installing it moves it in without
      // allocating an adapter.
      using terminal_type = exec::invocable<void(exec::try_t<std::size_t>&&)>;
      terminal_type terminal{
        [pending  = std::move(pending),
         metadata = &_metadata_store,
         path     = std::string{path},
         edge_bytes,
         edges,
         owner = std::move(owner),
         block,
         bridge = std::move(bridge)](exec::try_t<std::size_t>&& outcome) mutable {
          pending.fire();
          metadata->invalidate_written(path);
          // Before the caller sees the request: the file ends where its data ends again.
          if (edges != nullptr) {
            if (owner != nullptr) {
              finish_unaligned_write(*owner, *edges, block);
            } else {
              edges->outstanding.fetch_sub(1, std::memory_order_acq_rel);
            }
          }
          // The partial blocks were written before the request was dispatched.
          if (edge_bytes != 0 && outcome.has_value()) {
            auto const whole = std::move(outcome).get();
            outcome.emplace(whole + edge_bytes);
          }
          bridge.set_try(std::move(outcome));
        }};

      auto backend = mixed_writev_async_io(obj, std::move(slices));
      try {
        std::move(backend).install_callback(std::move(terminal));
      } catch (...) {
        // Not expected (the hook returns a valid future and the install does not
        // allocate), but never let the caller observe the write before the final
        // invalidations: wait for it here and run them.
        if (!backend.valid() || !terminal) throw;
        terminal(std::move(backend).get_try());
      }
      return result;
    } catch (...) {
      // The completion never ran: the request is over here.
      if (edges != nullptr) finish_unaligned_write(obj, *edges, block);
      throw;
    }
  } catch (...) {
    return exec::make_semi_future<std::size_t>(std::current_exception());
  }
}

void ioctx::flush(const io_object& obj)
{
  require_writable(*this, obj, false, "flush");
  flush_io(obj);
}

exec::semi_future<void> ioctx::flush_async(const io_object& obj, io_priority priority) noexcept
{
  try {
    require_writable(*this, obj, false, "flush_async");
  } catch (...) {
    return exec::make_semi_future<void>(exec::try_t<void>(std::current_exception()));
  }
  return flush_async_io(obj, resolve_priority(priority, io_priority::high));
}

// -- Default write hooks: not implemented ---------------------------------------

std::shared_ptr<io_object> ioctx::create_io_object_for_write(std::string /*path*/,
                                                             write_open_options /*options*/)
{
  throw not_implemented_error(std::string(backend_name(type())) + ": open_io_object_for_write");
}

std::size_t ioctx::host_write_io(const io_object& /*obj*/,
                                 std::size_t /*offset*/,
                                 std::size_t /*size*/,
                                 const std::uint8_t* /*src*/)
{
  throw not_implemented_error(std::string(backend_name(type())) + ": host_write");
}

exec::semi_future<std::size_t> ioctx::mixed_writev_async_io(
  const io_object& /*obj*/, std::vector<prepared_io_slice>&& /*slices*/) noexcept
{
  try {
    throw not_implemented_error(std::string(backend_name(type())) + ": mixed_writev_async");
  } catch (...) {
    return exec::make_semi_future<std::size_t>(std::current_exception());
  }
}

void ioctx::flush_io(const io_object& /*obj*/)
{
  throw not_implemented_error(std::string(backend_name(type())) + ": flush");
}

exec::semi_future<void> ioctx::flush_async_io(const io_object& /*obj*/,
                                              io_priority /*priority*/) noexcept
{
  try {
    throw not_implemented_error(std::string(backend_name(type())) + ": flush_async");
  } catch (...) {
    return exec::make_semi_future<void>(exec::try_t<void>(std::current_exception()));
  }
}

}  // namespace cucascade::io
