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
 * @file types.hpp
 * @brief The physical operation of the io_uring reactor and its planning helpers.
 *
 * They live in kvikIO's `uring` engine plugin (`kvikio/io/uring/types.hpp`);
 * this header re-exports them in `cucascade::io::uring`. Everything here is
 * header-only except `detail::set_copy_event_record_failure_for_testing`, which
 * needs the plugin at link time.
 */

#include <cucascade/io/io_request.hpp>

#include <kvikio/io/uring/types.hpp>

namespace cucascade::io::uring {

using kvikio::io::uring::max_dynamic_io_size;
using kvikio::io::uring::min_dynamic_io_size;
using kvikio::io::uring::uring_io_op;

namespace detail {

using kvikio::io::uring::detail::dynamic_io_target;
using kvikio::io::uring::detail::fill_remaining_iovecs;
using kvikio::io::uring::detail::is_odirect_compatible;
using kvikio::io::uring::detail::is_odirect_runtime_error;
using kvikio::io::uring::detail::odirect_available;
using kvikio::io::uring::detail::set_copy_event_record_failure_for_testing;
using kvikio::io::uring::detail::split_write_range;

}  // namespace detail

}  // namespace cucascade::io::uring
