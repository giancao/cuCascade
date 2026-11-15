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

#include <cucascade/io/rest/rest_ioctx.hpp>
#include <cucascade/memory/net_topology.hpp>

#include <kvikio/io/curl_engine.hpp>
#include <kvikio/io/engine_registry.hpp>
#include <kvikio/io/rest/placement.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace cucascade::io::rest {

std::optional<kvikio::io::nic_topology> discover_egress_nic(std::string_view interface) noexcept
{
  try {
    auto const name = memory::select_egress_interface(interface);
    if (!name.has_value()) { return std::nullopt; }
    auto const found = memory::discover_nic(*name);
    if (!found.has_value()) { return std::nullopt; }
    kvikio::io::nic_topology nic;
    nic.interface   = found->interface;
    nic.pci_address = found->pci_address;
    nic.numa_node   = found->numa_node;
    nic.mtu         = found->mtu;
    nic.rx_queues   = found->rx_queues;
    nic.tx_queues   = found->tx_queues;
    nic.irq_cpus    = found->irq_cpus;
    nic.node_cpus   = found->node_cpus;
    nic.queue_irqs.reserve(found->queue_irqs.size());
    for (auto const& q : found->queue_irqs) {
      nic.queue_irqs.push_back(kvikio::io::nic_queue_irq{q.irq, q.queue, q.cpus, q.requested_cpus});
    }
    return nic;
  } catch (...) {
    return std::nullopt;  // no NIC information: the default count, unpinned
  }
}

rest_ioctx::rest_ioctx(std::shared_ptr<kvikio::io::engine> engine)
  : engine_ioctx(std::move(engine), io_context_type::restful),
    _listing(dynamic_cast<kvikio::io::object_listing*>(&this->engine())),
    _api(dynamic_cast<kvikio::io::rest::uring_http_api*>(&this->engine()))
{
  if (_listing == nullptr) {
    throw std::invalid_argument("rest_ioctx: the engine does not list objects");
  }
}

std::shared_ptr<rest_ioctx> make_rest_ioctx(std::size_t n_reactors,
                                            config cfg,
                                            object_store_config const& object_store,
                                            std::shared_ptr<kvikio::io::staging_source> staging)
{
  kvikio::io::rest::uring_http_options options;
  options.n_reactors   = n_reactors;
  options.nic          = discover_egress_nic(cfg.interface);
  options.cfg          = std::move(cfg);
  options.object_store = object_store;
  options.staging      = std::move(staging);
  auto engine          = kvikio::io::engine_registry::instance().select(
    kvikio::io::engine_route::s3, kvikio::io::rest::uring_http_engine_name, options);
  return std::make_shared<rest_ioctx>(std::move(engine));
}

std::shared_ptr<rest_ioctx> make_curl_rest_ioctx(
  object_store_config const& object_store, std::shared_ptr<kvikio::io::staging_source> staging)
{
  kvikio::io::curl_options options;
  options.object_store = object_store;
  options.staging      = std::move(staging);
  auto engine          = kvikio::io::engine_registry::instance().select(
    kvikio::io::engine_route::s3, kvikio::io::curl_engine_name, options);
  return std::make_shared<rest_ioctx>(std::move(engine));
}

std::vector<int> planned_reactor_cpus(std::size_t n_reactors, config const& cfg)
{
  return kvikio::io::rest::planned_reactor_cpus(
    n_reactors, cfg.pin_to_irq_cpus, discover_egress_nic(cfg.interface));
}

}  // namespace cucascade::io::rest
