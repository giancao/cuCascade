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

#include <cucascade/io/engine_ioctx.hpp>
#include <cucascade/io/host_staging_source.hpp>

#include <rmm/error.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

namespace cucascade::io {

engine_ioctx::engine_ioctx(std::shared_ptr<kvikio::io::engine> engine, io_context_type type)
  : _engine(std::move(engine)), _type(type)
{
  if (_engine == nullptr) { throw std::invalid_argument("engine_ioctx: the engine is null"); }
  _caps = _engine->capabilities();
}

namespace {

class host_staging_lease final : public kvikio::io::staging_lease {
 public:
  using allocation_type = memory::fixed_size_host_memory_resource::fixed_multiple_blocks_allocation;

  explicit host_staging_lease(allocation_type allocation) noexcept
    : _allocation(std::move(allocation))
  {
  }

  [[nodiscard]] std::span<std::byte* const> blocks() const noexcept override
  {
    return _allocation->get_blocks();
  }

  [[nodiscard]] std::size_t block_size() const noexcept override
  {
    return _allocation->block_size();
  }

 private:
  allocation_type _allocation;
};

}  // namespace

kvikio::io::staging_result host_staging_source::acquire(std::size_t bytes) noexcept
{
  kvikio::io::staging_result result;
  try {
    auto allocation = _resource.allocate_multiple_blocks(bytes);
    if (allocation == nullptr || allocation->size_bytes() < bytes) { return result; }
    result.lease  = std::make_unique<host_staging_lease>(std::move(allocation));
    result.status = kvikio::io::staging_status::ok;
  } catch (rmm::out_of_memory const&) {
    result.status = kvikio::io::staging_status::unavailable;
  } catch (...) {
    result.status = kvikio::io::staging_status::failed;
  }
  return result;
}

}  // namespace cucascade::io
