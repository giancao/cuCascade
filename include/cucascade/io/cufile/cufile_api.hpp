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
 * @file cufile_api.hpp
 * @brief The cuFile functions the reactor loads at run time.
 *
 * Lives in kvikIO (`kvikio/io/cufile/cufile_api.hpp`, in `libkvikio.so`); this
 * header re-exports it in `cucascade::io::cufile`.
 */

#include <kvikio/io/cufile/cufile_api.hpp>

namespace cucascade::io::cufile {

using kvikio::io::cufile::cufile_api;

}  // namespace cucascade::io::cufile
