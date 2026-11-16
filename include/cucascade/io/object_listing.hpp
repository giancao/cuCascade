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
 * @file object_listing.hpp
 * @brief Listing the objects of a bucket.
 *
 * The interface lives in kvikIO (`kvikio/io/object_listing.hpp`) and is
 * implemented by the engines that can list (`uring_http`, `curl`); this header
 * re-exports it in `cucascade::io`. Reach the one of a context with
 * `engine_api<object_listing>(ctx)` (`cucascade/io/engine_ioctx.hpp`). The
 * page and entry types are `cucascade::io::rest::s3::list_objects_v2_page` and
 * `list_entry`.
 */

#include <cucascade/io/rest/forward.hpp>

#include <kvikio/io/object_listing.hpp>

namespace cucascade::io {

using kvikio::io::object_listing;

}  // namespace cucascade::io
