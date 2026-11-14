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
 * @file io_request.hpp
 * @brief The request types a reactor queues and settles.
 *
 * The implementation lives in kvikIO (`kvikio/io/detail/io_request.hpp`); this
 * header re-exports it in `cucascade::io`. Its device copies use the CUDA
 * driver API: statuses are `CUresult` and events are `CUevent`, and a device
 * copy needs the device id of its destination (the dispatcher stamps it on the
 * submitting thread).
 */

#include <cucascade/exec/semi_future.hpp>
#include <cucascade/io/types.hpp>

#include <kvikio/io/detail/io_request.hpp>

#include <cuda_runtime_api.h>

namespace cucascade::io {

using kvikio::io::detail::device_cpy_request;
using kvikio::io::detail::grouped_coordinator;
using kvikio::io::detail::grouped_io_request;
using kvikio::io::detail::io_op_request;
using kvikio::io::detail::validate_write_slices;

// -- Transitional: reactors still written against the CUDA runtime ---------------
//
// The runtime's `cudaError_t` and the driver's `CUresult` are different
// enumerations with the same numeric values for the statuses they share. These
// two functions name the conversion at the places where a runtime status meets
// the request types; they go away as each reactor moves to the driver API.

static_assert(static_cast<int>(cudaSuccess) == static_cast<int>(CUDA_SUCCESS));
static_assert(static_cast<int>(cudaErrorInvalidValue) ==
              static_cast<int>(CUDA_ERROR_INVALID_VALUE));
static_assert(static_cast<int>(cudaErrorNotReady) == static_cast<int>(CUDA_ERROR_NOT_READY));
static_assert(static_cast<int>(cudaErrorNotPermitted) ==
              static_cast<int>(CUDA_ERROR_NOT_PERMITTED));
static_assert(static_cast<int>(cudaErrorUnknown) == static_cast<int>(CUDA_ERROR_UNKNOWN));

/// The driver status with the meaning of the runtime status @p status.
[[nodiscard]] constexpr CUresult to_driver_status(cudaError_t status) noexcept
{
  return static_cast<CUresult>(status);
}

/// The runtime status with the meaning of the driver status @p status.
[[nodiscard]] constexpr cudaError_t to_runtime_status(CUresult status) noexcept
{
  return static_cast<cudaError_t>(status);
}

}  // namespace cucascade::io
