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
 * @file semi_future.hpp
 * @brief `promise`, `semi_future` and `future`.
 *
 * The implementation lives in kvikIO (`kvikio/exec/semi_future.hpp`), where the
 * I/O engines use it; this header re-exports it in `cucascade::exec`.
 */

#include <cucascade/exec/invocable.hpp>
#include <cucascade/exec/try.hpp>

#include <kvikio/exec/semi_future.hpp>

namespace cucascade::exec {

using kvikio::exec::broken_promise_error;
using kvikio::exec::executor_concept;
using kvikio::exec::executor_func;
using kvikio::exec::future;
using kvikio::exec::future_already_retrieved_error;
using kvikio::exec::future_invalid_error;
using kvikio::exec::inline_executor;
using kvikio::exec::make_semi_future;
using kvikio::exec::make_semi_future_with;
using kvikio::exec::promise;
using kvikio::exec::promise_already_satisfied_error;
using kvikio::exec::semi_future;
using kvikio::exec::tag;
using kvikio::exec::tag_t;

namespace detail {

// The shared state and its nodes, for code that builds futures by hand.
using kvikio::exec::detail::core;
using kvikio::exec::detail::deferred_state;
using kvikio::exec::detail::flat_map_state;
using kvikio::exec::detail::leaf_state;
using kvikio::exec::detail::ready_state;
using kvikio::exec::detail::state;

}  // namespace detail

}  // namespace cucascade::exec
