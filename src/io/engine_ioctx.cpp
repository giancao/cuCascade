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

#include <cucascade/io/engine_ioctx.hpp>
#include <cucascade/io/host_staging_source.hpp>
#include <cucascade/log/logging.hpp>

#include <kvikio/io/engine_registry.hpp>
#include <kvikio/logger.hpp>

#include <rmm/error.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cucascade::io {

namespace {

/// kvikIO's logger calls this for every record it lets through.
void deliver_engine_record(int lvl, char const* message)
{
  if (message == nullptr) { return; }
  std::string_view text{message};
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  // rapids_logger's levels are numbered like cuCascade's (trace = 0 ... off = 6).
  auto const level =
    static_cast<log::level>(std::clamp(lvl, 0, static_cast<int>(log::level::fatal)));
  log::detail::deliver(level, std::source_location::current(), text);
}

}  // namespace

void forward_engine_logs() noexcept
{
  try {
    // The user asked for kvikIO's own logging: leave it alone.
    if (std::getenv("KVIKIO_LOG_LEVEL") != nullptr) { return; }
    auto& logger = kvikio::default_logger();
    static std::once_flag attached;
    std::call_once(attached, [&logger] {
      // cuCascade's sink adds the timestamp and level itself.
      logger.set_pattern("%v");
      logger.sinks().push_back(
        std::make_shared<rapids_logger::callback_sink_mt>(&deliver_engine_record));
    });
    logger.set_level(static_cast<rapids_logger::level_enum>(
      std::min(static_cast<int>(log::get_min_level()), static_cast<int>(log::level::off))));
  } catch (...) {
    // Logging never fails the caller.
  }
}

engine_ioctx::engine_ioctx(std::shared_ptr<kvikio::io::engine> engine, io_context_type type)
  : _engine(std::move(engine)), _type(type)
{
  if (_engine == nullptr) { throw std::invalid_argument("engine_ioctx: the engine is null"); }
  forward_engine_logs();
  _caps = _engine->capabilities();
}

engine_ioctx::~engine_ioctx()
{
  this->pre_destroy();
  _engine->shutdown();
  // The registry keeps a selected engine alive; this context was its user.
  kvikio::io::engine_registry::instance().release(_engine.get());
}

namespace {

class host_staging_lease final : public kvikio::io::staging_lease {
 public:
  using allocation_type = memory::fixed_size_host_memory_resource::fixed_multiple_blocks_allocation;

  explicit host_staging_lease(allocation_type allocation) noexcept
    : _allocation(std::move(allocation))
  {
  }

  [[nodiscard]] std::span<std::byte* const> blocks() const noexcept override
  {
    return _allocation->get_blocks();
  }

  [[nodiscard]] std::size_t block_size() const noexcept override
  {
    return _allocation->block_size();
  }

 private:
  allocation_type _allocation;
};

}  // namespace

kvikio::io::staging_result host_staging_source::acquire(std::size_t bytes) noexcept
{
  kvikio::io::staging_result result;
  try {
    auto allocation = _resource.allocate_multiple_blocks(bytes);
    if (allocation == nullptr || allocation->size_bytes() < bytes) { return result; }
    result.lease  = std::make_unique<host_staging_lease>(std::move(allocation));
    result.status = kvikio::io::staging_status::ok;
  } catch (rmm::out_of_memory const&) {
    result.status = kvikio::io::staging_status::unavailable;
  } catch (...) {
    result.status = kvikio::io::staging_status::failed;
  }
  return result;
}

std::exception_ptr host_staging_source::exhausted_error(char const* what) const noexcept
{
  try {
    return std::make_exception_ptr(rmm::out_of_memory(what != nullptr ? what : ""));
  } catch (...) {
    return std::current_exception();
  }
}

}  // namespace cucascade::io
