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
 * @file object_store_config.hpp
 * @brief Endpoint, credentials and transport of an object store.
 *
 * Lives in kvikIO (`kvikio/io/object_store_config.hpp`); this header
 * re-exports it in `cucascade::io`.
 */

#include <kvikio/io/object_store_config.hpp>

namespace cucascade::io {

using kvikio::io::enum_to_string;
using kvikio::io::object_store_config;
using kvikio::io::string_to_enum;

}  // namespace cucascade::io
