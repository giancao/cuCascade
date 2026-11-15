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
 * @file detail.hpp
 * @brief Internals of the cuFile backend exposed for unit tests.
 *
 * The reactor's own seams live in kvikIO (`kvikio/io/cufile/detail.hpp`) and are
 * re-exported here; using the test seams needs the engine plugin at link time.
 * The path probe's verdict function is cuCascade's.
 */

#include <cucascade/io/cufile/platform.hpp>

#include <kvikio/io/cufile/detail.hpp>

namespace cucascade::io::cufile::detail {

/**
 * @brief The verdict of @ref probe_path from the evidence already in @p info.
 *
 * Reads the policy, platform, storage and opened-file fields and sets
 * @c capability, @c reason, @c matched_class, @c class_supported and
 * @c filesystem_supported.  Pure: no I/O, no cuFile calls.
 */
void decide_path_capability(path_info& info) noexcept;

using kvikio::io::cufile::detail::cufile_write_calls_for_testing;
using kvikio::io::cufile::detail::force_compat_env_on;
using kvikio::io::cufile::detail::live_registrations_for_testing;
using kvikio::io::cufile::detail::next_piece;
using kvikio::io::cufile::detail::registrations_created_for_testing;
using kvikio::io::cufile::detail::set_copy_event_record_failure_for_testing;
using kvikio::io::cufile::detail::set_cufile_transfer_hook_for_testing;
using kvikio::io::cufile::detail::set_cufile_writes_in_compat_for_testing;
using kvikio::io::cufile::detail::set_staging_copy_delay_for_testing;
using kvikio::io::cufile::detail::transfer_tracked_for_testing;

}  // namespace cucascade::io::cufile::detail
