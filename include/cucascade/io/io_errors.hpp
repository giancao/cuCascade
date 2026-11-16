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

#include <kvikio/io/errors.hpp>

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace cucascade::io {

// The io error types live in kvikIO (kvikio/io/errors.hpp).
using kvikio::io::credential_error;
using kvikio::io::descriptor_mismatch_error;
using kvikio::io::not_implemented_error;
using kvikio::io::object_changed_error;

}  // namespace cucascade::io
