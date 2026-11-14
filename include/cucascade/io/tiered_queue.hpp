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
 * @file tiered_queue.hpp
 * @brief Two-tier (high / low priority) blocking queue of the reactors.
 *
 * The implementation lives in kvikIO (`kvikio/io/detail/tiered_queue.hpp`); this
 * header re-exports it in `cucascade::io`.
 */

#include <cucascade/io/concurrent_queue.hpp>
#include <cucascade/io/types.hpp>

#include <kvikio/io/detail/tiered_queue.hpp>

#include <type_traits>

namespace cucascade::io {

// kvikIO's queue aliases and cuCascade's must name the same moodycamel copy
// (KVIKIO_MOODYCAMEL_NAMESPACE == CUCASCADE_MOODYCAMEL_NAMESPACE).
static_assert(std::is_same_v<lightweight_semaphore, kvikio::io::detail::lightweight_semaphore>,
              "kvikIO and cuCascade use different moodycamel namespaces");

using kvikio::io::detail::tiered_blocking_queue;

}  // namespace cucascade::io
