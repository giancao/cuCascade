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
 * @file list_parser.hpp
 * @brief Parser of S3 ListObjectsV2 responses.
 *
 * The implementation lives in kvikIO (`kvikio/io/rest/s3/list_parser.hpp`); this
 * header re-exports it in `cucascade::io::rest::s3`.
 */

#include <kvikio/io/rest/s3/list_parser.hpp>

namespace cucascade::io::rest::s3 {

using kvikio::io::rest::s3::default_max_list_objects;
using kvikio::io::rest::s3::default_max_scanned_objects;
using kvikio::io::rest::s3::list_entry;
using kvikio::io::rest::s3::list_objects_v2_page;
using kvikio::io::rest::s3::parse_list_objects_v2;

}  // namespace cucascade::io::rest::s3
