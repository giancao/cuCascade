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

#include <kvikio/compat_mode.hpp>

#include <catch2/catch_all.hpp>
#include <link.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::string> loaded_kvikio_libraries()
{
  std::vector<std::string> paths;
  dl_iterate_phdr(
    [](dl_phdr_info* info, std::size_t, void* data) {
      const std::string_view path{info->dlpi_name};
      const auto name = path.substr(path.find_last_of('/') + 1);
      if (name.starts_with("libkvikio.so")) {
        static_cast<std::vector<std::string>*>(data)->emplace_back(path);
      }
      return 0;
    },
    &paths);
  return paths;
}

}  // namespace

TEST_CASE("kvikio is loaded once and from the build cuCascade links", "[io][kvikio]")
{
  REQUIRE(kvikio::detail::parse_compat_mode_str("ON") == kvikio::CompatMode::ON);

  const auto loaded = loaded_kvikio_libraries();
  REQUIRE(loaded.size() == 1);
  INFO("loaded " << loaded.front());
  CHECK(std::filesystem::equivalent(loaded.front(), CUCASCADE_TEST_KVIKIO_LIBRARY));
}
