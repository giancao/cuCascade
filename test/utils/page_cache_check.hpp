/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

/**
 * @file page_cache_check.hpp
 * @brief Compares what the page cache holds for a file with what is on the disk.
 *
 * A file written partly with O_DIRECT and partly through the page cache can be left with pages
 * in the page cache that do not hold what is on the disk; a later buffered read returns them. A
 * write path that keeps the two apart never leaves such a page, which is what this checks: every
 * block is read once through the page cache and once with O_DIRECT, and the two must agree.
 */

#include <catch2/catch_all.hpp>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <system_error>

namespace cucascade {
namespace test {

struct page_cache_comparison {
  bool direct_io{true};        ///< false: the filesystem refused O_DIRECT, nothing was compared
  std::size_t blocks{0};       ///< 4 KiB blocks compared
  std::size_t stale{0};        ///< blocks a buffered read returns differently from a direct read
  std::size_t first_stale{0};  ///< index of the first such block
};

/// Read @p path block by block through the page cache and with O_DIRECT, and count the blocks
/// the two disagree on. The buffered read goes first, so it sees the pages the writer left.
[[nodiscard]] inline page_cache_comparison compare_page_cache_with_disk(std::string const& path)
{
  constexpr std::size_t block  = 4096;
  constexpr std::size_t window = 256 * block;
  struct descriptor {
    int fd;
    ~descriptor()
    {
      if (fd >= 0) { ::close(fd); }
    }
  };
  struct free_deleter {
    void operator()(void* p) const noexcept { std::free(p); }
  };
  auto const read_window = [](int fd, std::uint8_t* out, std::size_t at) -> ssize_t {
    std::size_t done = 0;
    while (done < window) {
      auto const n = ::pread(fd, out + done, window - done, static_cast<off_t>(at + done));
      if (n < 0) { return -1; }
      if (n == 0) { break; }
      done += static_cast<std::size_t>(n);
    }
    return static_cast<ssize_t>(done);
  };

  page_cache_comparison result;
  descriptor const buffered{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
  if (buffered.fd < 0) { throw std::system_error(errno, std::generic_category(), "open " + path); }
  descriptor const direct{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECT)};
  if (direct.fd < 0) {
    result.direct_io = false;
    return result;
  }
  std::unique_ptr<std::uint8_t, free_deleter> const cached{
    static_cast<std::uint8_t*>(std::aligned_alloc(block, window))};
  std::unique_ptr<std::uint8_t, free_deleter> const on_disk{
    static_cast<std::uint8_t*>(std::aligned_alloc(block, window))};
  for (std::size_t at = 0;; at += window) {
    auto const from_cache = read_window(buffered.fd, cached.get(), at);
    auto const from_disk  = read_window(direct.fd, on_disk.get(), at);
    if (from_cache < 0) { throw std::system_error(errno, std::generic_category(), "read " + path); }
    if (from_disk < 0) {
      result.direct_io = false;
      return result;
    }
    auto const bytes = static_cast<std::size_t>(std::max(from_cache, from_disk));
    for (std::size_t offset = 0; offset < bytes; offset += block) {
      auto const length = std::min(block, bytes - offset);
      // A block one read has and the other lacks counts as a difference.
      bool const same =
        offset + length <= static_cast<std::size_t>(std::min(from_cache, from_disk)) &&
        std::memcmp(cached.get() + offset, on_disk.get() + offset, length) == 0;
      if (!same) {
        if (result.stale == 0) { result.first_stale = (at + offset) / block; }
        ++result.stale;
      }
      ++result.blocks;
    }
    if (bytes < window) { break; }
  }
  return result;
}

/// CHECK that no block of @p path differs between the page cache and the disk.
inline void check_page_cache_matches_disk(std::string const& path)
{
  auto const result = compare_page_cache_with_disk(path);
  if (!result.direct_io) {
    WARN("the page cache of " << path << " was not compared with the disk: no O_DIRECT here");
    return;
  }
  INFO(path << ": " << result.stale << " of " << result.blocks
            << " blocks differ between the page cache and the disk, the first is block "
            << result.first_stale);
  CHECK(result.stale == 0);
}

}  // namespace test
}  // namespace cucascade
