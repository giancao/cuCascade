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

// The REST context served by kvikIO's built-in curl engine: the fallback for when the uring_http
// engine cannot be used.

#include "loopback_range_server.hpp"
#include "rest_contract_test_utils.hpp"

#include <cucascade/io/engine_ioctx.hpp>
#include <cucascade/io/io_errors.hpp>
#include <cucascade/io/object_listing.hpp>
#include <cucascade/io/rest/rest_ioctx.hpp>
#include <cucascade/io/uring/uring_ioctx.hpp>

#include <kvikio/io/engine_registry.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using cucascade::test::loopback_range_server;

namespace {

cucascade::io::object_store_config store_for(loopback_range_server const& server)
{
  cucascade::io::object_store_config store;
  store.endpoint   = server.endpoint();
  store.region     = "us-east-1";
  store.access_key = "test-access-key";
  store.secret_key = "test-secret-key";
  return store;
}

}  // namespace

TEST_CASE("the curl engine serves a REST context: open, read and list", "[rest][curl]")
{
  auto const payload = cucascade::test::rest_test_payload(256 * 1024 + 123);
  loopback_range_server server{
    payload, {}, {{"data/part.0.parquet", 100}, {"data/part.1.parquet", 200}}};

  auto context = cucascade::io::rest::make_curl_rest_ioctx(store_for(server), nullptr);
  REQUIRE(context != nullptr);
  CHECK(context->type() == cucascade::io::io_context_type::restful);
  CHECK(context->engine().name() == "curl");
  CHECK_FALSE(context->has_transport_api());
  CHECK(kvikio::io::engine_registry::instance().selected_name(kvikio::io::engine_route::s3) ==
        "curl");
  CHECK(context->supports("s3://bucket/key"));
  CHECK_FALSE(context->supports_write());

  // One HEAD gives the size; a known size sends none.
  auto object = context->open_io_object("s3://bucket/object.bin");
  CHECK(object->size() == payload.size());
  CHECK(server.head_count() == 1);
  auto sized = context->open_io_object("s3://bucket/object.bin", std::uint64_t{4096});
  CHECK(sized->size() == 4096);
  CHECK(server.head_count() == 1);

  std::vector<std::uint8_t> bytes(100 * 1024);
  auto future = context->host_read_async_io(*object, 777, bytes.size(), bytes.data());
  CHECK(std::move(future).get(30s) == bytes.size());
  CHECK(std::equal(bytes.begin(), bytes.end(), payload.begin() + 777));

  std::vector<std::uint8_t> head(16);
  CHECK(context->host_read_io(*object, 0, head.size(), head.data()) == head.size());
  CHECK(std::equal(head.begin(), head.end(), payload.begin()));

  auto const entries = context->list_objects("bucket", "data/");
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].key == "data/part.0.parquet");
  CHECK(entries[1].size == 200);
  CHECK(server.list_count() >= 1);

  // A host that only holds an ioctx lists through the engine's listing interface, whichever
  // engine serves the context; an interface the engine lacks is absent.  The interface shares
  // ownership of the context.
  {
    std::shared_ptr<cucascade::io::ioctx> const as_ioctx = context;
    auto const listing = cucascade::io::engine_api<cucascade::io::object_listing>(as_ioctx);
    REQUIRE(listing != nullptr);
    CHECK(as_ioctx.use_count() == 3);
    CHECK(listing->list_objects("bucket", "data/").size() == 2);
    CHECK(cucascade::io::engine_api<cucascade::io::uring::uring_diagnostics>(as_ioctx) == nullptr);
    CHECK(as_ioctx.use_count() == 3);
  }

  // What only the uring_http engine has: reported, not faked.
  std::vector<std::string> const paths{"s3://bucket/object.bin"};
  CHECK_THROWS_AS(context->resolve_footer_objects(
                    paths, [](cucascade::io::rest::footer_resolve_result const&) {}),
                  cucascade::io::not_implemented_error);
  CHECK(context->stats().gets == 0);
  CHECK(context->dispatch_fanout() == 1);

  // Destroying the context stops selecting the engine.
  context.reset();
  CHECK(
    kvikio::io::engine_registry::instance().selected_name(kvikio::io::engine_route::s3).empty());
}
