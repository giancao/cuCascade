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
 * @file object_identity.hpp
 * @brief Evidence that two opens of a path see the same version of an object.
 *
 * The implementation lives in kvikIO (`kvikio/io/object_identity.hpp`); this
 * header re-exports it in `cucascade::io`. The open-generation counter is the
 * one in `libkvikio.so`, shared by every module of the process.
 */

#include <kvikio/io/object_identity.hpp>

namespace cucascade::io {

using kvikio::io::capture_local_identity;
using kvikio::io::identity_kind;
using kvikio::io::next_open_generation;
using kvikio::io::object_identity;

}  // namespace cucascade::io
