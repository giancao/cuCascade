#pragma once

#include <cucascade/io/cufile/config.hpp>
#include <cucascade/memory/topology_discovery.hpp>

#include <kvikio/io/cufile/cufile_options.hpp>
#include <kvikio/io/cufile/platform.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace cucascade::io::cufile {

// The process-wide cuFile snapshot lives with the reactor, in kvikIO.
using kvikio::io::cufile::gds_available;
using kvikio::io::cufile::platform_info;
using kvikio::io::cufile::probe_platform;

/**
 * @brief What GPUDirect Storage can do for reads of one file.
 *
 * Never a guarantee: the transfer mode cuFile picks also depends on the GPU, the
 * buffer and its alignment, cuFile's own configuration (@c cufile.json) and
 * driver state, none of which is observable before a read.
 */
enum class path_capability : std::uint8_t {
  /// cuFile cannot serve the file at all: libcufile or its driver is missing,
  /// the path does not name a readable regular file, or @c compat::off would
  /// refuse to open it.  (With @c compat::automatic reads still work, via POSIX.)
  unavailable,
  /// cuFile can only bounce through host memory, or is not used, for this file:
  /// policy (@c compat::on, @c use_odirect=false, @c CUFILE_FORCE_COMPAT_MODE),
  /// no storage class with a real GDS mode, an unsupported filesystem or storage
  /// class, or a file that could not be opened @c O_DIRECT / registered.
  compatibility_only,
  /// Every gate cuCascade can check passed: the driver reports a real GDS mode
  /// for the file's storage class, the filesystem is one GDS supports on it,
  /// and the file is open @c O_DIRECT and registered with cuFile.  cuFile may
  /// DMA directly, but may still fall back per transfer.
  direct_candidate,
  /// Not determined: the driver properties or the file's storage stack could
  /// not be read, or the stack is mixed / layered.  Not a negative answer.
  unknown,
};

/// The storage class whose @c dstatusflags bit decides a file's GDS mode.
enum class storage_class : std::uint8_t {
  none,      ///< No class applies (memory / pseudo filesystem, unknown stack).
  nvme,      ///< Local PCIe NVMe (@c CU_FILE_NVME_SUPPORTED).
  nvmeof,    ///< NVMe over fabrics (@c CU_FILE_NVMEOF_SUPPORTED).
  nfs,       ///< NFS (@c CU_FILE_NFS_SUPPORTED).
  lustre,    ///< Lustre (@c CU_FILE_LUSTRE_SUPPORTED).
  wekafs,    ///< WekaFS (@c CU_FILE_WEKAFS_SUPPORTED).
  gpfs,      ///< IBM Storage Scale / GPFS (@c CU_FILE_GPFS_SUPPORTED).
  beegfs,    ///< BeeGFS (@c CU_FILE_BEEGFS_SUPPORTED).
  virtiofs,  ///< VirtioFS (@c CU_FILE_VIRTIOFS_SUPPORTED).
};

/**
 * @brief Evidence and verdict of @ref probe_path for one file.
 *
 * A by-value snapshot: the passive storage discovery is not cached, and the
 * platform part is the process-wide @ref probe_platform snapshot.
 */
struct path_info {
  path_capability capability = path_capability::unknown;  ///< The verdict.
  std::string reason;                                     ///< Why: the deciding gate, in words.

  // -- policy ---------------------------------------------------------------
  compat compat_mode = compat::automatic;  ///< Effective configured policy.
  bool use_odirect{true};                  ///< Configured @c use_odirect.

  // -- platform -------------------------------------------------------------
  platform_info platform;  ///< The process-wide cuFile snapshot.

  // -- storage --------------------------------------------------------------
  memory::path_storage_info storage;                  ///< Passive mount / device discovery.
  storage_class matched_class = storage_class::none;  ///< Class the verdict checked.
  /// Whether the driver reports a real GDS mode for @c matched_class (its
  /// @c dstatusflags bit is set).  Never inferred from any other bit.
  bool class_supported{false};
  /// Whether the filesystem is one GDS serves directly on @c matched_class
  /// (ext4 / xfs on local NVMe; the network filesystem itself otherwise).
  bool filesystem_supported{false};

  // -- the opened file ------------------------------------------------------
  bool opened{false};       ///< The file was opened read-only.
  bool direct_open{false};  ///< An @c O_DIRECT descriptor is open.
  bool registered{false};   ///< @c cuFileHandleRegister succeeded for it.
  std::string open_error;   ///< Why opening failed, when it did.
};

/**
 * @brief Report what GPUDirect Storage can do for reads of @p path under @p cfg.
 *
 * Active, unlike passive storage discovery: it calls @ref probe_platform (loads
 * libcufile and opens the cuFile driver once per process, which can retain a
 * CUDA context per visible GPU and create @c cufile.log), and opens @p path
 * read-only the way the reactor would -- buffered and, policy permitting,
 * @c O_DIRECT, registered with cuFile -- then closes it.  It reads no data and
 * never writes.  Best effort and never throws: the file can change right after.
 *
 * Do not call it while holding a lock other threads route reads under; it can
 * take hundreds of milliseconds on first use.
 */
[[nodiscard]] path_info probe_path(std::string const& path, config const& cfg = {}) noexcept;

/**
 * @brief As @ref probe_path, for a file the reactor already opened with policy
 *        @p cfg -- so the verdict describes exactly the file that is read.
 *        Never throws.
 */
[[nodiscard]] path_info probe_object(io_object const& object,
                                     kvikio::io::cufile::cufile_object_view const& view,
                                     config const& cfg) noexcept;

}  // namespace cucascade::io::cufile
