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
 * @file cufile_reactor.hpp
 * @brief The cuFile / GDS reactor.
 *
 * It lives in kvikIO's `cufile_reactor` engine plugin
 * (`kvikio/io/cufile/cufile_reactor.hpp`, `libkvikio_engine_cufile_reactor.so`);
 * this header re-exports it in `cucascade::io::cufile`. cuCascade itself reaches
 * the engine by name through kvikIO's engine registry; code that uses the
 * reactor classes directly links the plugin (`kvikio::engine_cufile_reactor`).
 */

#include <cucascade/io/cufile/config.hpp>
#include <cucascade/io/io_request.hpp>
#include <cucascade/io/types.hpp>

#include <kvikio/io/cufile/cufile_reactor.hpp>

namespace cucascade::io::cufile {

using kvikio::io::cufile::cufile_io_object;
using kvikio::io::cufile::cufile_reactor;

}  // namespace cucascade::io::cufile
