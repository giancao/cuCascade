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
 * @file host_staging_source.hpp
 * @brief Staging memory for kvikIO engines, taken from a HOST memory space.
 */

#include <cucascade/memory/fixed_size_host_memory_resource.hpp>

#include <kvikio/io/staging_source.hpp>

#include <cstddef>

namespace cucascade::io {

/**
 * @brief Lends an engine the pinned blocks of a @c fixed_size_host_memory_resource.
 *
 * The blocks an engine stages through are therefore accounted in the HOST
 * tier's budget, like every other pinned allocation of cuCascade.
 *
 * @c acquire answers @c unavailable when the resource is out of memory, so the
 * engine can park the operation until blocks come back, and @c failed for any
 * other error. The resource must outlive this source and every lease it gave.
 */
class host_staging_source final : public kvikio::io::staging_source {
 public:
  explicit host_staging_source(memory::fixed_size_host_memory_resource& resource) noexcept
    : _resource(resource)
  {
  }

  [[nodiscard]] std::size_t block_size() const noexcept override
  {
    return _resource.get_block_size();
  }

  [[nodiscard]] kvikio::io::staging_result acquire(std::size_t bytes) noexcept override;

 private:
  memory::fixed_size_host_memory_resource& _resource;
};

}  // namespace cucascade::io
