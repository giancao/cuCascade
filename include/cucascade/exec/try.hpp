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
 * @file try.hpp
 * @brief `try_t`: a value or the exception that prevented it.
 *
 * The implementation lives in kvikIO (`kvikio/exec/try.hpp`); this header
 * re-exports it in `cucascade::exec`.
 */

#include <kvikio/exec/try.hpp>

namespace cucascade::exec {

using kvikio::exec::make_empty_try;
using kvikio::exec::make_try_with;
using kvikio::exec::try_empty_error;
using kvikio::exec::try_t;

}  // namespace cucascade::exec
