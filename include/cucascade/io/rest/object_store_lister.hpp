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
 * @file object_store_lister.hpp
 * @brief Paged listing of an object store, independent of the transport.
 *
 * The implementation lives in kvikIO (`kvikio/io/rest/object_store_lister.hpp`);
 * this header re-exports it in `cucascade::io::rest`.
 */

#include <cucascade/io/rest/s3/list_parser.hpp>

#include <kvikio/io/rest/object_store_lister.hpp>

namespace cucascade::io::rest {

using kvikio::io::rest::object_store_lister;

}  // namespace cucascade::io::rest
