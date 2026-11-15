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

// Reopening a local file through its descriptor. The implementation lives in
// kvikIO (kvikio/io/detail/local_descriptor.hpp).

#include <kvikio/io/detail/local_descriptor.hpp>

namespace cucascade::io::detail {

using kvikio::io::detail::reopen_hook_for_testing;
using kvikio::io::detail::reopen_hook_for_testing_installed;
using kvikio::io::detail::reopen_local_descriptor;
using kvikio::io::detail::reopen_writable_direct;
using kvikio::io::detail::same_local_file;
using kvikio::io::detail::set_reopen_hook_for_testing;

}  // namespace cucascade::io::detail
