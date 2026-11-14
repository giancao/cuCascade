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

#include <cucascade/exec/invocable.hpp>
#include <cucascade/io/byte_range.hpp>
#include <cucascade/io/object_identity.hpp>

#include <kvikio/io/types.hpp>

#include <cuda/stream>
#include <cuda_runtime.h>

#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cucascade::io::cache {
struct cached_chunk;
class metadata_store;
}  // namespace cucascade::io::cache

namespace cucascade::io {
class ioctx;
}  // namespace cucascade::io

namespace cucascade::io {

using kvikio::io::IO_BLOCK_SIZE;
using kvikio::io::write_granule;

/**
 * @brief RAII wrapper for a POSIX file descriptor.
 *
 * Non-copyable, movable. Closes the underlying fd on destruction.
 */
struct file_descriptor {
  int fd{-1};
  file_descriptor() = default;
  explicit file_descriptor(int f) noexcept : fd(f) {}
  ~file_descriptor() noexcept
  {
    if (fd >= 0) ::close(fd);
  }
  file_descriptor(file_descriptor const&)            = delete;
  file_descriptor& operator=(file_descriptor const&) = delete;
  file_descriptor(file_descriptor&& o) noexcept : fd(std::exchange(o.fd, -1)) {}
  file_descriptor& operator=(file_descriptor&& o) noexcept
  {
    if (this != &o) {
      if (fd >= 0) ::close(fd);
      fd = std::exchange(o.fd, -1);
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return fd; }
  [[nodiscard]] int native_handle() const noexcept { return fd; }
  explicit operator bool() const noexcept { return fd >= 0; }
};

// ---------------------------------------------------------------------------
// Request vocabulary
// ---------------------------------------------------------------------------
//
// The types an io request is made of live in kvikIO (kvikio/io/types.hpp), where
// the I/O engines use them; they are re-exported here under their cuCascade
// names. Two things to know when reading older code:
//
//  - A fragmented host_buffer holds io::fragment (file offset, memory, opaque
//    handle), not cache chunks. The fs_cache builds fragments from the chunks it
//    claimed (see cache::make_fragment) and gets the handles back through
//    prepared_io_completion, once per physical operation.
//  - host_buffer and prepared_io_slice are move-only; clone() is the explicit
//    copy. device_buffer::stream is the raw stream handle (cudaStream_t).

using kvikio::io::device_buffer;
using kvikio::io::fragment;
using kvikio::io::fragment_handle;
using kvikio::io::host_buffer;
using kvikio::io::intersect;
using kvikio::io::io_access;
using kvikio::io::io_object;
using kvikio::io::io_op_kind;
using kvikio::io::io_priority;
using kvikio::io::make_device_write_slice;
using kvikio::io::make_flush_slice;
using kvikio::io::make_host_write_slice;
using kvikio::io::open_hint;
using kvikio::io::open_options;
using kvikio::io::prepared_io_completion;
using kvikio::io::prepared_io_slice;
using kvikio::io::range;
using kvikio::io::resolve_priority;
using kvikio::io::slice;
using kvikio::io::write_mode;
using kvikio::io::write_open_options;
using kvikio::io::write_slice;

/// Raise @p target to @p value if it is smaller (an atomic maximum).
inline void atomic_store_max(std::atomic<std::size_t>& target, std::size_t value) noexcept
{
  auto current = target.load(std::memory_order_relaxed);
  while (current < value &&
         !target.compare_exchange_weak(
           current, value, std::memory_order_acq_rel, std::memory_order_relaxed)) {}
}

class io_object_metadata {
 public:
  virtual ~io_object_metadata() = default;
};

}  // namespace cucascade::io
