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
 * @file forward.hpp
 * @brief `cucascade::io::{rest,tls,http,net}` name kvikIO's REST engine.
 *
 * The object-store engine (REST reactor, HTTP codec, network transport, TLS,
 * S3 signing) lives in kvikIO's `uring_http` engine plugin
 * (`kvikio/io/rest`, `kvikio/io/tls`, `libkvikio_engine_uring_http.so`), in the
 * namespaces `kvikio::io::{rest,tls,http,net}`.
 *
 * Qualified lookup follows using-directives, so `cucascade::io::rest::config`,
 * `cucascade::io::rest::s3::list_entry` or `cucascade::io::tls::options` name
 * kvikIO's declarations, while cuCascade still declares what is its own
 * (@c rest_ioctx) in the namespace.
 *
 * cuCascade reaches the engine by name through kvikIO's engine registry; code
 * that uses the reactor, transport or TLS classes directly links the plugin
 * (`kvikio::engine_uring_http`).
 */

namespace kvikio::io::rest {
}  // namespace kvikio::io::rest
namespace kvikio::io::tls {
}
namespace kvikio::io::http {
}
namespace kvikio::io::net {
}

namespace cucascade::io::rest {
using namespace kvikio::io::rest;  // NOLINT(google-build-using-namespace)
}  // namespace cucascade::io::rest

namespace cucascade::io::tls {
using namespace kvikio::io::tls;  // NOLINT(google-build-using-namespace)
}  // namespace cucascade::io::tls

namespace cucascade::io::http {
using namespace kvikio::io::http;  // NOLINT(google-build-using-namespace)
}  // namespace cucascade::io::http

namespace cucascade::io::net {
using namespace kvikio::io::net;  // NOLINT(google-build-using-namespace)
}  // namespace cucascade::io::net
