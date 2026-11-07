// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pineforge/run_failure.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

struct sqlite3;

namespace pineforge::live {

enum class LedgerBackupFailureReason { space, io, verification, incomplete };
const char* ledger_backup_failure_reason_name(LedgerBackupFailureReason reason) noexcept;

class LedgerBackupFailed final : public coded<std::runtime_error> {
public:
    LedgerBackupFailed(LedgerBackupFailureReason reason, const char* operation);
    LedgerBackupFailureReason reason() const noexcept { return reason_; }
private:
    LedgerBackupFailureReason reason_;
};

// The caller holds the runner's exclusive lock and owns source_fd throughout.
// Inspection and online backup share one read-only SQLite transaction. No
// source checkpoint, writer connection or schema work belongs in this class.
class LedgerBackupSource {
public:
    LedgerBackupSource(const std::string& canonical_path, int source_fd,
                       std::function<bool()> cancelled = {});
    ~LedgerBackupSource();
    LedgerBackupSource(const LedgerBackupSource&) = delete;
    LedgerBackupSource& operator=(const LedgerBackupSource&) = delete;
    sqlite3* database() const noexcept;
    // Returns only after a source-verified image and completion manifest have
    // been flushed and published, or an existing completion revalidates.
    void complete();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pineforge::live
