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
 * @file invocable.hpp
 * @brief Move-only type-erased callable used across the exec layer.
 *
 * The implementation lives in kvikIO (`kvikio/exec/invocable.hpp`); this header
 * re-exports it as `cucascade::exec::invocable`. Two backing implementations
 * are selected at configure time: a minimal in-tree one (the default), or
 * `absl::AnyInvocable` when `CUCASCADE_USE_ABSEIL_INVOCABLE` is ON. cuCascade
 * passes that option on to the kvikIO it builds, so the library, kvikIO and
 * every consumer resolve the type to the same implementation.
 */

#include <kvikio/exec/invocable.hpp>

#if defined(CUCASCADE_USE_ABSEIL_INVOCABLE) != defined(KVIKIO_USE_ABSEIL_INVOCABLE)
#error "CUCASCADE_USE_ABSEIL_INVOCABLE and KVIKIO_USE_ABSEIL_INVOCABLE must be set alike: \
cucascade::exec::invocable is kvikio::exec::invocable"
#endif

namespace cucascade::exec {

using kvikio::exec::invocable;

}  // namespace cucascade::exec
