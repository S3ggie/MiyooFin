#include "CatalogDb.hpp"
#include "CatalogDbInternal.hpp"
#include "CatalogDbSchema.hpp"

namespace miyoofin {
using namespace catalog_db_internal;

void CatalogDb::closeConnection()
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    if (!m_db) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connectionOpen = false;
        m_connectionWorkerOwned = false;
        m_preparedStatementCount = 0;
        return;
    }

    finalizeStatements();
    sqlite3_close(m_db);
    m_db = nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connectionOpen = false;
    m_connectionWorkerOwned = false;
    m_preparedStatementCount = 0;
}

void CatalogDb::finalizeStatements()
{
    // Future schema migrations must use this worker-only boundary, then run
    // BEGIN IMMEDIATE, migrate, set user_version, COMMIT, and rebuild the
    // registry before publishing the new schema-ready state.
    assert(std::this_thread::get_id() == m_worker.get_id());
    for (auto& entry : m_statements) {
        sqlite3_finalize(entry.second);
    }
    m_statements.clear();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_preparedStatementCount = 0;
}

bool CatalogDb::openConnection(const ScopeCommand& command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    const auto scopeStage = [&](const char* stage) {
        catalogDiagnostic(std::string("scope_stage=") + stage);
    };
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch != m_requestedEpoch || m_stopping) {
            return false;
        }
    }

    scopeStage("path_state_inspect_started");
    const std::string path = catalogPath(command.scopeKey);
    CatalogDbMigrationState migrationState = inspectMigrationState(command.scopeKey);
    scopeStage("path_state_inspect_completed");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            const bool attempted = m_migrationState.attempted;
            const bool succeeded = m_migrationState.succeeded;
            m_migrationState = migrationState;
            m_migrationState.attempted = attempted;
            m_migrationState.succeeded = succeeded;
        }
    }
    if (migrationState.pathError) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::CorruptOrIo;
            m_openState = CatalogDbOpenState::CorruptOrIo;
        }
        catalogDiagnostic("sqlite_open_skipped reason=path_error");
        catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                               CatalogDbErrorCategory::CorruptOrIo,
                               CatalogDbOpenState::CorruptOrIo);
        return false;
    }
    scopeStage("directory_prepare_started");
    const std::size_t slash = path.find_last_of('/');
    std::string error;
    if (slash == std::string::npos || !makeDirectories(path.substr(0, slash))) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::CorruptOrIo;
            m_openState = CatalogDbOpenState::CorruptOrIo;
        }
        catalogDiagnostic("sqlite_open_skipped reason=directory_create_failed");
        catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                               CatalogDbErrorCategory::CorruptOrIo,
                               CatalogDbOpenState::CorruptOrIo);
        return false;
    }
    scopeStage("directory_prepare_completed");

    bool bootstrapped = false;
    if (!migrationState.finalPresent) {
        std::string bootstrapError;
        if (!bootstrapFreshDatabaseForWorker(command, bootstrapError)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (command.epoch == m_requestedEpoch) {
                m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
                m_lastError = CatalogDbErrorCategory::CorruptOrIo;
                m_openState = CatalogDbOpenState::CorruptOrIo;
            }
            catalogDiagnostic(std::string("bootstrap_failed reason=") +
                              (bootstrapError.empty() ? "unknown" : bootstrapError));
            catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                                   CatalogDbErrorCategory::CorruptOrIo,
                                   CatalogDbOpenState::CorruptOrIo);
            return false;
        }
        bootstrapped = true;
        migrationState = inspectMigrationState(command.scopeKey);
        if (migrationState.pathError || !migrationState.finalPresent) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (command.epoch == m_requestedEpoch) {
                m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
                m_lastError = CatalogDbErrorCategory::CorruptOrIo;
                m_openState = CatalogDbOpenState::CorruptOrIo;
            }
            catalogDiagnostic("bootstrap_failed reason=promotion_not_visible");
            catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                                   CatalogDbErrorCategory::CorruptOrIo,
                                   CatalogDbOpenState::CorruptOrIo);
            return false;
        }
    }

    scopeStage("sqlite_open_started");
    catalogDiagnostic(std::string("sqlite_open_attempt db_dir=") +
                      scopeDirectory(command.scopeKey));
    sqlite3* db = nullptr;
    const int openRc =
        sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    {
        char line[128];
        const char* message = db ? sqlite3_errmsg(db) : "no_handle";
        std::snprintf(line, sizeof(line), "sqlite_open_returned rc=%d handle=%d errmsg=%s", openRc,
                      db != nullptr ? 1 : 0, message ? message : "none");
        catalogDiagnostic(line);
    }
    if (openRc != SQLITE_OK || !db) {
        if (db) {
            sqlite3_close(db);
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::CorruptOrIo;
            m_openState = CatalogDbOpenState::CorruptOrIo;
        }
        catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                               CatalogDbErrorCategory::CorruptOrIo,
                               CatalogDbOpenState::CorruptOrIo);
        return false;
    }

    sqlite3_extended_result_codes(db, 1);
    const int busyRc = sqlite3_busy_timeout(db, 250);
    catalogDiagnostic("busy_timeout rc=" + std::to_string(busyRc) + " ms=250");
    const auto setupPragma = [&](const char* sql, const char* name) {
        const int rc = execResult(db, sql, error);
        catalogDiagnostic(std::string("setup_pragma name=") + name + " rc=" + std::to_string(rc) +
                          " errmsg=" + (db ? sqlite3_errmsg(db) : "none"));
        return rc == SQLITE_OK;
    };
    if (!setupPragma("PRAGMA foreign_keys = ON;", "foreign_keys") ||
        !setupPragma("PRAGMA trusted_schema = OFF;", "trusted_schema") ||
        !setupPragma("PRAGMA journal_mode = DELETE;", "journal_mode") ||
        !setupPragma("PRAGMA synchronous = FULL;", "synchronous") ||
        !setupPragma("PRAGMA locking_mode = NORMAL;", "locking_mode")) {
        sqlite3_close(db);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::CorruptOrIo;
            m_openState = CatalogDbOpenState::CorruptOrIo;
        }
        catalogDiagnostic("sqlite_open_failed stage=configuration");
        catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                               CatalogDbErrorCategory::CorruptOrIo,
                               CatalogDbOpenState::CorruptOrIo);
        return false;
    }

    std::string foreignKeys;
    std::string trustedSchema;
    std::string journalMode;
    std::string synchronous;
    std::string lockingMode;
    if (!scalar(db, "PRAGMA foreign_keys;", foreignKeys, error) ||
        !scalar(db, "PRAGMA trusted_schema;", trustedSchema, error) ||
        !scalar(db, "PRAGMA journal_mode;", journalMode, error) ||
        !scalar(db, "PRAGMA synchronous;", synchronous, error) ||
        !scalar(db, "PRAGMA locking_mode;", lockingMode, error) || foreignKeys != "1" ||
        trustedSchema != "0" || journalMode != "delete" || synchronous != "2" ||
        lockingMode != "normal") {
        sqlite3_close(db);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::ConfigurationFailed;
        }
        catalogDiagnostic("sqlite_open_failed stage=configuration_validation");
        catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                               CatalogDbErrorCategory::ConfigurationFailed,
                               CatalogDbOpenState::NotAttempted);
        return false;
    }

    scopeStage("schema_validate_migrate_started");
    CatalogDbOpenState openState = CatalogDbOpenState::NotAttempted;
    bool needsBackfill = false;
    if (!ensureSchema(db, openState, needsBackfill, error)) {
        sqlite3_close(db);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_openState = openState;
            m_lastError = openState == CatalogDbOpenState::WrongApplicationId
                              ? CatalogDbErrorCategory::WrongApplicationId
                          : openState == CatalogDbOpenState::UnsupportedVersion
                              ? CatalogDbErrorCategory::UnsupportedVersion
                              : CatalogDbErrorCategory::CorruptOrIo;
        }
        const CatalogDbErrorCategory errorCategory =
            openState == CatalogDbOpenState::WrongApplicationId
                ? CatalogDbErrorCategory::WrongApplicationId
            : openState == CatalogDbOpenState::UnsupportedVersion
                ? CatalogDbErrorCategory::UnsupportedVersion
                : CatalogDbErrorCategory::CorruptOrIo;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "sqlite_open_failed stage=schema error_category=%s(%u) open_state=%s(%u)",
                      errorCategoryName(errorCategory), static_cast<unsigned>(errorCategory),
                      openStateName(openState), static_cast<unsigned>(openState));
        catalogDiagnostic(line);
        catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed, errorCategory, openState);
        return false;
    }
    catalogDiagnostic(std::string("schema_validate_migrate_completed open_state=") +
                      openStateName(openState));
    if (needsBackfill) {
        scopeStage("sort_key_backfill_started");
        if (!backfillOrganizationalSortKeys(db, error)) {
            sqlite3_close(db);
            return false;
        }
        scopeStage("sort_key_backfill_completed");
    } else {
        catalogDiagnostic("sort_key_backfill_skipped reason=schema_has_canonical_keys");
    }
    scopeStage("scope_setup_completed");
    if (bootstrapped)
        openState = CatalogDbOpenState::CreatedV3;

    if (migrationState.migratingPresent) {
        const std::string temporaryPath = migratingPath(command.scopeKey);
        const std::string sidecars[] = {temporaryPath, temporaryPath + "-journal",
                                        temporaryPath + "-wal", temporaryPath + "-shm"};
        for (const auto& candidate : sidecars) {
            if (std::remove(candidate.c_str()) != 0 && errno != ENOENT) {
                sqlite3_close(db);
                std::lock_guard<std::mutex> lock(m_mutex);
                if (command.epoch == m_requestedEpoch) {
                    m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
                    m_lastError = CatalogDbErrorCategory::CorruptOrIo;
                    m_openState = CatalogDbOpenState::CorruptOrIo;
                }
                catalogDiagnostic("sqlite_open_failed stage=stale_temp_cleanup");
                catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                                       CatalogDbErrorCategory::CorruptOrIo,
                                       CatalogDbOpenState::CorruptOrIo);
                return false;
            }
        }
        catalogDiagnostic("stale_temp_cleanup completed");
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch != m_requestedEpoch || m_stopping) {
            sqlite3_close(db);
            return false;
        }
        m_db = db;
        m_connectionOpen = true;
        m_connectionWorkerOwned = std::this_thread::get_id() == m_worker.get_id();
        m_activeScopeKey = command.scopeKey;
        m_scopeConfigured = true;
        m_scopeReady = true;
        m_scopeStatus = CatalogDbScopeStatus::Ready;
        m_lastError = CatalogDbErrorCategory::None;
        m_openState = openState;
    }
    catalogFinalDiagnostic(true, CatalogDbScopeStatus::Ready, CatalogDbErrorCategory::None,
                           openState);
    scopeStage("scope_ready");
    return true;
}

bool CatalogDb::bootstrapFreshDatabaseForWorker(const ScopeCommand& command, std::string& error)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    const auto bootstrapStage = [&](const char* stage) {
        catalogDiagnostic(std::string("bootstrap_stage=") + stage);
    };
    const auto scopeIsCurrent = [&] {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_stopping && command.epoch == m_requestedEpoch &&
               command.scopeKey == m_requestedScopeKey;
    };
    const std::string finalPath = catalogPath(command.scopeKey);
    const std::string temporaryPath = migratingPath(command.scopeKey);
    const std::string sidecars[] = {temporaryPath, temporaryPath + "-journal",
                                    temporaryPath + "-wal", temporaryPath + "-shm"};
    const auto removeTemporaryFamily = [&] {
        for (const auto& candidate : sidecars) {
            if (std::remove(candidate.c_str()) != 0 && errno != ENOENT) {
                error = "could not remove stale temporary catalog";
                return false;
            }
        }
        return true;
    };
    const CatalogDbMigrationState initialState = inspectMigrationState(command.scopeKey);
    if (initialState.pathError) {
        error = "catalog database paths are not accessible";
        return false;
    }
    if (initialState.finalPresent) {
        return true;
    }
    if (!scopeIsCurrent()) {
        error = "fresh catalog bootstrap scope was superseded";
        return false;
    }
    if (!removeTemporaryFamily()) {
        return false;
    }

    const std::size_t slash = temporaryPath.find_last_of('/');
    if (slash == std::string::npos || !makeDirectories(temporaryPath.substr(0, slash))) {
        error = "could not create catalog database directory";
        return false;
    }
    catalogDiagnostic("bootstrap_temp_open_started");
    sqlite3* temporary = nullptr;
    const int openRc = sqlite3_open_v2(temporaryPath.c_str(), &temporary,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (openRc != SQLITE_OK || !temporary) {
        if (temporary) {
            sqlite3_close(temporary);
        }
        char line[96];
        std::snprintf(line, sizeof(line), "bootstrap_temp_create_failed rc=%d", openRc);
        catalogDiagnostic(line);
        error = "could not open temporary catalog database";
        return false;
    }
    catalogDiagnostic("bootstrap_temp_created");
    sqlite3_extended_result_codes(temporary, 1);
    const auto closeTemporary = [&] {
        if (temporary) {
            sqlite3_close(temporary);
            temporary = nullptr;
        }
    };
    const int busyRc = sqlite3_busy_timeout(temporary, 250);
    catalogDiagnostic("bootstrap_busy_timeout rc=" + std::to_string(busyRc) + " ms=250");
    const auto setupPragma = [&](const char* sql, const char* name) {
        const int rc = execResult(temporary, sql, error);
        catalogDiagnostic(std::string("bootstrap_pragma name=") + name +
                          " rc=" + std::to_string(rc) + " errmsg=" + sqlite3_errmsg(temporary));
        return rc == SQLITE_OK;
    };
    if (!setupPragma("PRAGMA foreign_keys = ON;", "foreign_keys") ||
        !setupPragma("PRAGMA trusted_schema = OFF;", "trusted_schema") ||
        !setupPragma("PRAGMA journal_mode = DELETE;", "journal_mode") ||
        !setupPragma("PRAGMA synchronous = FULL;", "synchronous") ||
        !setupPragma("PRAGMA locking_mode = NORMAL;", "locking_mode")) {
        closeTemporary();
        removeTemporaryFamily();
        return false;
    }
    bootstrapStage("schema_validate_migrate_started");
    CatalogDbOpenState openState = CatalogDbOpenState::NotAttempted;
    bool needsBackfill = false;
    if (!ensureSchema(temporary, openState, needsBackfill, error) ||
        openState != CatalogDbOpenState::CreatedV3) {
        if (error.empty()) {
            error = "fresh catalog schema creation failed";
        }
        closeTemporary();
        removeTemporaryFamily();
        return false;
    }
    bootstrapStage("schema_validate_migrate_completed");
    std::string foreignKeys;
    std::string trustedSchema;
    std::string journalMode;
    std::string synchronous;
    std::string lockingMode;
    if (!scalar(temporary, "PRAGMA foreign_keys;", foreignKeys, error) ||
        !scalar(temporary, "PRAGMA trusted_schema;", trustedSchema, error) ||
        !scalar(temporary, "PRAGMA journal_mode;", journalMode, error) ||
        !scalar(temporary, "PRAGMA synchronous;", synchronous, error) ||
        !scalar(temporary, "PRAGMA locking_mode;", lockingMode, error) || foreignKeys != "1" ||
        trustedSchema != "0" || journalMode != "delete" || synchronous != "2" ||
        lockingMode != "normal") {
        if (error.empty()) {
            error = "fresh catalog configuration validation failed";
        }
        closeTemporary();
        removeTemporaryFamily();
        return false;
    }
    std::string quickCheck;
    if (!scalar(temporary, "PRAGMA quick_check;", quickCheck, error) || quickCheck != "ok") {
        if (error.empty()) {
            error = "fresh catalog quick_check failed";
        }
        closeTemporary();
        removeTemporaryFamily();
        return false;
    }
    sqlite3_stmt* foreignKeyCheck = nullptr;
    const bool foreignKeyPrepared = sqlite3_prepare_v2(temporary, "PRAGMA foreign_key_check;", -1,
                                                       &foreignKeyCheck, nullptr) == SQLITE_OK;
    const int foreignKeyRc = foreignKeyPrepared ? sqlite3_step(foreignKeyCheck) : SQLITE_ERROR;
    if (foreignKeyCheck) {
        sqlite3_finalize(foreignKeyCheck);
    }
    if (!foreignKeyPrepared || foreignKeyRc != SQLITE_DONE) {
        error = "fresh catalog foreign_key_check failed";
        closeTemporary();
        removeTemporaryFamily();
        return false;
    }
    if (sqlite3_close(temporary) != SQLITE_OK) {
        temporary = nullptr;
        error = "temporary catalog database did not close cleanly";
        removeTemporaryFamily();
        return false;
    }
    temporary = nullptr;
    const int temporaryFd = ::open(temporaryPath.c_str(), O_RDONLY);
    if (temporaryFd < 0 || ::fsync(temporaryFd) != 0) {
        if (temporaryFd >= 0) {
            ::close(temporaryFd);
        }
        error = "temporary catalog fsync failed";
        removeTemporaryFamily();
        return false;
    }
    ::close(temporaryFd);
    if (!scopeIsCurrent()) {
        error = "fresh catalog bootstrap scope was superseded";
        removeTemporaryFamily();
        return false;
    }
    const CatalogDbMigrationState beforePromotion = inspectMigrationState(command.scopeKey);
    if (beforePromotion.pathError) {
        error = "catalog database paths became inaccessible";
        removeTemporaryFamily();
        return false;
    }
    if (beforePromotion.finalPresent) {
        return removeTemporaryFamily();
    }
    if (::rename(temporaryPath.c_str(), finalPath.c_str()) != 0) {
        const int renameError = errno;
        char line[128];
        std::snprintf(line, sizeof(line), "bootstrap_temp_finalize_failed errno=%d", renameError);
        catalogDiagnostic(line);
        error = "fresh catalog promotion failed";
        removeTemporaryFamily();
        return false;
    }
    const int directoryFd =
        ::open(finalPath.substr(0, finalPath.find_last_of('/')).c_str(), O_RDONLY);
    if (directoryFd >= 0) {
        ::fsync(directoryFd);
        ::close(directoryFd);
    }
    catalogDiagnostic("bootstrap_temp_finalized final_present=1");
    return true;
}

} // namespace miyoofin
