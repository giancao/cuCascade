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

#include <cucascade/io/cufile/detail.hpp>
#include <cucascade/io/cufile/platform.hpp>
#include <cucascade/log/logging.hpp>
#include <cucascade/memory/topology_discovery.hpp>

#include <kvikio/io/cufile/cufile_options.hpp>
#include <kvikio/io/engine_registry.hpp>

#include <cufile.h>

#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace cucascade::io::cufile {

namespace {

using memory::path_storage_status;
using memory::storage_kind;

/// The @c CUfileDriverStatusFlags bit of @p cls, or -1 for @c storage_class::none.
[[nodiscard]] int status_bit(storage_class cls) noexcept
{
  switch (cls) {
    case storage_class::nvme: return CU_FILE_NVME_SUPPORTED;
    case storage_class::nvmeof: return CU_FILE_NVMEOF_SUPPORTED;
    case storage_class::nfs: return CU_FILE_NFS_SUPPORTED;
    case storage_class::lustre: return CU_FILE_LUSTRE_SUPPORTED;
    case storage_class::wekafs: return CU_FILE_WEKAFS_SUPPORTED;
    case storage_class::gpfs: return CU_FILE_GPFS_SUPPORTED;
    case storage_class::beegfs: return CU_FILE_BEEGFS_SUPPORTED;
    case storage_class::virtiofs: return CU_FILE_VIRTIOFS_SUPPORTED;
    case storage_class::none: break;
  }
  return -1;
}

[[nodiscard]] char const* class_name(storage_class cls) noexcept
{
  switch (cls) {
    case storage_class::nvme: return "NVMe";
    case storage_class::nvmeof: return "NVMe-oF";
    case storage_class::nfs: return "NFS";
    case storage_class::lustre: return "Lustre";
    case storage_class::wekafs: return "WekaFS";
    case storage_class::gpfs: return "GPFS";
    case storage_class::beegfs: return "BeeGFS";
    case storage_class::virtiofs: return "VirtioFS";
    case storage_class::none: break;
  }
  return "none";
}

/// The storage class of a network / shared filesystem type, or none.
[[nodiscard]] storage_class network_class(std::string_view fs_type) noexcept
{
  if (fs_type == "nfs" || fs_type == "nfs4" || fs_type == "vastnfs") return storage_class::nfs;
  if (fs_type == "lustre") return storage_class::lustre;
  if (fs_type == "wekafs") return storage_class::wekafs;
  if (fs_type == "gpfs") return storage_class::gpfs;
  if (fs_type == "beegfs") return storage_class::beegfs;
  if (fs_type == "virtiofs") return storage_class::virtiofs;
  return storage_class::none;
}

[[nodiscard]] char const* capability_name(path_capability capability) noexcept
{
  switch (capability) {
    case path_capability::unavailable: return "unavailable";
    case path_capability::compatibility_only: return "compatibility-only";
    case path_capability::direct_candidate: return "direct candidate";
    case path_capability::unknown: break;
  }
  return "unknown";
}

void set_verdict(path_info& info, path_capability capability, std::string reason)
{
  info.capability = capability;
  info.reason     = std::move(reason);
}

/// Log a verdict once per (device, verdict) pair: per mount, not per path.
void log_once(path_info const& info) noexcept
{
  try {
    static std::mutex mutex;
    static std::set<std::tuple<unsigned, unsigned, int>> logged;
    {
      std::lock_guard lock(mutex);
      if (!logged
             .emplace(info.storage.device_major,
                      info.storage.device_minor,
                      static_cast<int>(info.capability))
             .second) {
        return;
      }
    }
    CUCASCADE_LOG_INFO(
      "cufile: '{}' ({} on {}): GDS read capability {}: {}",
      info.storage.path,
      info.storage.filesystem_type.empty() ? "unknown fs" : info.storage.filesystem_type,
      info.storage.mount_point.empty() ? "unknown mount" : info.storage.mount_point,
      capability_name(info.capability),
      info.reason);
  } catch (...) {
    // Logging is best effort.
  }
}

void fill_policy(path_info& info, config const& cfg)
{
  info.compat_mode = cfg.compat_mode.value_or(compat::automatic);
  info.use_odirect = cfg.use_odirect;
  info.platform    = probe_platform();
}

void fill_from_object(path_info& info,
                      io_object const& object,
                      kvikio::io::cufile::cufile_object_view const& view)
{
  info.storage     = memory::topology_discovery::discover_storage_for_descriptor(view.buffered_fd(),
                                                                             object.object_path());
  info.opened      = true;
  info.direct_open = view.direct_fd() >= 0;
  info.registered  = view.cufile_registered();
}

}  // namespace

namespace detail {

void decide_path_capability(path_info& info) noexcept
{
  try {
    auto const& storage       = info.storage;
    auto const& platform      = info.platform;
    info.matched_class        = storage_class::none;
    info.class_supported      = false;
    info.filesystem_supported = false;

    // The file itself.
    if (storage.status == path_storage_status::not_found ||
        storage.status == path_storage_status::not_regular_file ||
        storage.status == path_storage_status::inaccessible) {
      set_verdict(
        info, path_capability::unavailable, "not a readable regular file: " + storage.reason);
      return;
    }
    if (!info.opened) {
      set_verdict(info,
                  path_capability::unavailable,
                  "the cuFile reactor would not open the file: " + info.open_error);
      return;
    }

    // Policy.
    if (info.compat_mode == compat::on) {
      set_verdict(info,
                  path_capability::compatibility_only,
                  "compat_mode=on: cuFile is not used, every read goes through POSIX");
      return;
    }
    if (!info.use_odirect) {
      set_verdict(info,
                  path_capability::compatibility_only,
                  "use_odirect=false: the file is never registered with cuFile");
      return;
    }

    // Platform.
    if (!platform.library_available) {
      set_verdict(info, path_capability::unavailable, "libcufile cannot be loaded");
      return;
    }
    if (!platform.driver_opened) {
      set_verdict(info, path_capability::unavailable, "the cuFile driver did not open");
      return;
    }
    if (platform.forced_compat) {
      set_verdict(info,
                  path_capability::compatibility_only,
                  "CUFILE_FORCE_COMPAT_MODE forces cuFile compatibility mode");
      return;
    }
    if (!platform.properties_available) {
      set_verdict(info,
                  path_capability::unknown,
                  "cuFileDriverGetProperties failed: the driver's GDS modes are unknown");
      return;
    }
    if (platform.dstatusflags == 0) {
      set_verdict(info,
                  path_capability::compatibility_only,
                  "no storage class reports a real GDS mode (no nvidia-fs, P2PDMA or GDS-enabled "
                  "filesystem client): cuFile runs in compatibility mode");
      return;
    }

    // Storage.
    switch (storage.kind) {
      case storage_kind::local_block: {
        if (!storage.all_devices_nvme()) {
          set_verdict(info,
                      storage.mixed_devices() ? path_capability::unknown
                                              : path_capability::compatibility_only,
                      storage.mixed_devices()
                        ? "mixed device stack under " + storage.mount_point
                        : "the file is not on NVMe: no GDS storage class checked for it");
          return;
        }
        // NVMe namespaces reached without a PCI controller are fabrics-attached.
        bool pci = true;
        for (auto const& device : storage.devices) {
          if (device.pci_bus_id.empty()) pci = false;
        }
        info.matched_class = pci ? storage_class::nvme : storage_class::nvmeof;
        info.filesystem_supported =
          (storage.filesystem_type == "ext4" &&
           storage.super_options.find("data=journal") == std::string::npos) ||
          storage.filesystem_type == "xfs";
        break;
      }
      case storage_kind::network:
        info.matched_class        = network_class(storage.filesystem_type);
        info.filesystem_supported = info.matched_class != storage_class::none;
        if (info.matched_class == storage_class::none) {
          set_verdict(
            info,
            path_capability::compatibility_only,
            "network filesystem " + storage.filesystem_type + " has no GDS storage class");
          return;
        }
        break;
      case storage_kind::memory:
        set_verdict(info,
                    path_capability::compatibility_only,
                    "memory filesystem " + storage.filesystem_type + " has no GDS path");
        return;
      case storage_kind::unknown:
        info.matched_class        = network_class(storage.filesystem_type);
        info.filesystem_supported = info.matched_class != storage_class::none;
        if (info.matched_class == storage_class::none) {
          set_verdict(info,
                      path_capability::unknown,
                      "the file's storage is unknown: " +
                        (storage.reason.empty() ? std::string{"no detail"} : storage.reason));
          return;
        }
        break;
    }

    auto const bit = status_bit(info.matched_class);
    info.class_supported =
      bit >= 0 && (platform.dstatusflags & (1u << static_cast<unsigned>(bit))) != 0;
    std::string const cls = class_name(info.matched_class);
    if (!info.class_supported) {
      set_verdict(info,
                  path_capability::compatibility_only,
                  "the cuFile driver reports no GDS mode for " + cls);
      return;
    }
    if (!info.filesystem_supported) {
      set_verdict(info,
                  path_capability::compatibility_only,
                  "filesystem " + storage.filesystem_type + " is not served by GDS on " + cls);
      return;
    }
    if (!info.direct_open) {
      set_verdict(info,
                  path_capability::compatibility_only,
                  "the file has no O_DIRECT descriptor (refused by the filesystem)");
      return;
    }
    if (!info.registered) {
      set_verdict(info,
                  path_capability::compatibility_only,
                  "cuFileHandleRegister did not register the file");
      return;
    }
    set_verdict(info,
                path_capability::direct_candidate,
                "the cuFile driver reports GDS for " + cls + ", " + storage.filesystem_type +
                  " is supported there, and the file is registered O_DIRECT");
  } catch (...) {
    info.capability = path_capability::unknown;
  }
}

}  // namespace detail

path_info probe_object(io_object const& object,
                       kvikio::io::cufile::cufile_object_view const& view,
                       config const& cfg) noexcept
{
  path_info info;
  try {
    fill_policy(info, cfg);
    fill_from_object(info, object, view);
    detail::decide_path_capability(info);
    log_once(info);
  } catch (...) {
    info.capability = path_capability::unknown;
    info.reason     = "probing the file failed";
  }
  return info;
}

path_info probe_path(std::string const& path, config const& cfg) noexcept
{
  path_info info;
  try {
    fill_policy(info, cfg);
    // The file is opened the way the engine opens it, by a short-lived engine
    // of its own: the reactor lives in kvikIO's plugin, which is loaded by name.
    std::shared_ptr<io_object> object;
    std::shared_ptr<kvikio::io::engine> engine;
    try {
      kvikio::io::cufile::cufile_reactor_options options;
      options.n_reactors = 1;
      options.cfg        = cfg;
      engine             = kvikio::io::engine_registry::instance().create(
        kvikio::io::cufile::cufile_reactor_engine_name, options);
      object = engine->open(path, open_options{});
    } catch (std::exception const& e) {
      info.open_error = e.what();
    } catch (...) {
      info.open_error = "unknown error";
    }
    auto const* view = dynamic_cast<kvikio::io::cufile::cufile_object_view const*>(object.get());
    if (view != nullptr) {
      fill_from_object(info, *object, *view);
    } else {
      info.storage = memory::topology_discovery::discover_storage_for_path(path);
    }
    object.reset();
    if (engine != nullptr) { engine->shutdown(); }
    detail::decide_path_capability(info);
    log_once(info);
  } catch (...) {
    info.capability = path_capability::unknown;
    info.reason     = "probing the file failed";
  }
  return info;
}

}  // namespace cucascade::io::cufile
