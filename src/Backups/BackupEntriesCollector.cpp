#include <Backups/BackupEntriesCollector.h>
#include <Backups/BackupEntryFromMemory.h>
#include <Backups/IBackupCoordination.h>
#include <Backups/BackupCoordinationHelpers.h>
#include <Backups/BackupUtils.h>
#include <Backups/DDLAdjustingForBackupVisitor.h>
#include <Databases/IDatabase.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/formatAST.h>
#include <Storages/IStorage.h>
#include <Access/Common/AccessEntityType.h>
#include <base/chrono_io.h>
#include <base/insertAtEnd.h>
#include <base/sleep.h>
#include <Common/escapeForFileName.h>
#include <boost/range/algorithm/copy.hpp>
#include <filesystem>

namespace fs = std::filesystem;


namespace DB
{

namespace ErrorCodes
{
    extern const int INCONSISTENT_METADATA_FOR_BACKUP;
    extern const int CANNOT_BACKUP_TABLE;
    extern const int TABLE_IS_DROPPED;
    extern const int UNKNOWN_TABLE;
    extern const int LOGICAL_ERROR;
}

namespace
{
    /// Finding all tables and databases which we're going to put to the backup and collecting their metadata.
    constexpr const char * kGatheringMetadataStatus = "gathering metadata";

    /// Making temporary hard links and prepare backup entries.
    constexpr const char * kExtractingDataFromTablesStatus = "extracting data from tables";

    /// Running special tasks for replicated tables which can also prepare some backup entries.
    constexpr const char * kRunningPostTasksStatus = "running post-tasks";

    /// Writing backup entries to the backup and removing temporary hard links.
    constexpr const char * kWritingBackupStatus = "writing backup";

    /// Error status.
    constexpr const char * kErrorStatus = BackupCoordinationStatusSync::kErrorStatus;

    /// Uppercases the first character of a passed string.
    String toUpperFirst(const String & str)
    {
        String res = str;
        res[0] = std::toupper(res[0]);
        return res;
    }

    /// Outputs "table <name>" or "temporary table <name>"
    String tableNameWithTypeToString(const String & database_name, const String & table_name, bool first_upper)
    {
        String str;
        if (database_name == DatabaseCatalog::TEMPORARY_DATABASE)
            str = fmt::format("temporary table {}", backQuoteIfNeed(table_name));
        else
            str = fmt::format("table {}.{}", backQuoteIfNeed(database_name), backQuoteIfNeed(table_name));
        if (first_upper)
            str[0] = std::toupper(str[0]);
        return str;
    }

    /// How long we should sleep after finding an inconsistency error.
    std::chrono::milliseconds getSleepTimeAfterInconsistencyError(size_t pass)
    {
        size_t ms;
        if (pass == 1) /* pass is 1-based */
            ms = 0;
        else if ((pass % 10) != 1)
            ms = 0;
        else
            ms = 1000;
        return std::chrono::milliseconds{ms};
    }
}


BackupEntriesCollector::BackupEntriesCollector(
    const ASTBackupQuery::Elements & backup_query_elements_,
    const BackupSettings & backup_settings_,
    std::shared_ptr<IBackupCoordination> backup_coordination_,
    const ContextPtr & context_)
    : backup_query_elements(backup_query_elements_)
    , backup_settings(backup_settings_)
    , backup_coordination(backup_coordination_)
    , context(context_)
    , consistent_metadata_snapshot_timeout(context->getConfigRef().getUInt64("backups.consistent_metadata_snapshot_timeout", 300000))
    , log(&Poco::Logger::get("BackupEntriesCollector"))
{
}

BackupEntriesCollector::~BackupEntriesCollector() = default;

BackupEntries BackupEntriesCollector::run()
{
    try
    {
        /// run() can be called onle once.
        if (!current_status.empty())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Already making backup entries");

        /// Find other hosts working along with us to execute this ON CLUSTER query.
        all_hosts
            = BackupSettings::Util::filterHostIDs(backup_settings.cluster_host_ids, backup_settings.shard_num, backup_settings.replica_num);

        /// Do renaming in the create queries according to the renaming config.
        renaming_map = makeRenamingMapFromBackupQuery(backup_query_elements);

        /// Calculate the root path for collecting backup entries, it's either empty or has the format "shards/<shard_num>/replicas/<replica_num>/".
        calculateRootPathInBackup();

        /// Find databases and tables which we're going to put to the backup.
        gatherMetadataAndCheckConsistency();

        /// Make backup entries for the definitions of the found databases.
        makeBackupEntriesForDatabasesDefs();

        /// Make backup entries for the definitions of the found tables.
        makeBackupEntriesForTablesDefs();

        /// Make backup entries for the data of the found tables.
        setStatus(kExtractingDataFromTablesStatus);
        makeBackupEntriesForTablesData();

        /// Run all the tasks added with addPostCollectingTask().
        setStatus(kRunningPostTasksStatus);
        runPostTasks();

        /// No more backup entries or tasks are allowed after this point.
        setStatus(kWritingBackupStatus);

        return std::move(backup_entries);
    }
    catch (...)
    {
        try
        {
            setStatus(kErrorStatus, getCurrentExceptionMessage(false));
        }
        catch (...)
        {
        }
        throw;
    }
}

Strings BackupEntriesCollector::setStatus(const String & new_status, const String & message)
{
    if (new_status == kErrorStatus)
    {
        LOG_ERROR(log, "{} failed with error: {}", toUpperFirst(current_status), message);
        backup_coordination->setStatus(backup_settings.host_id, new_status, message);
        return {};
    }
    else
    {
        LOG_TRACE(log, "{}", toUpperFirst(new_status));
        current_status = new_status;
        if (new_status.starts_with(kGatheringMetadataStatus))
        {
            auto now = std::chrono::steady_clock::now();
            auto end_of_timeout = std::max(now, consistent_metadata_snapshot_start_time + consistent_metadata_snapshot_timeout);

            return backup_coordination->setStatusAndWaitFor(
                backup_settings.host_id,
                new_status,
                message,
                all_hosts,
                std::chrono::duration_cast<std::chrono::milliseconds>(end_of_timeout - now).count());
        }
        else
        {
            return backup_coordination->setStatusAndWait(backup_settings.host_id, new_status, message, all_hosts);
        }
    }
}

/// Calculates the root path for collecting backup entries,
/// it's either empty or has the format "shards/<shard_num>/replicas/<replica_num>/".
void BackupEntriesCollector::calculateRootPathInBackup()
{
    root_path_in_backup = "/";
    if (!backup_settings.host_id.empty())
    {
        auto [shard_num, replica_num]
            = BackupSettings::Util::findShardNumAndReplicaNum(backup_settings.cluster_host_ids, backup_settings.host_id);
        root_path_in_backup = root_path_in_backup / fs::path{"shards"} / std::to_string(shard_num) / "replicas" / std::to_string(replica_num);
    }
    LOG_TRACE(log, "Will use path in backup: {}", doubleQuoteString(String{root_path_in_backup}));
}

/// Finds databases and tables which we will put to the backup.
void BackupEntriesCollector::gatherMetadataAndCheckConsistency()
{
    consistent_metadata_snapshot_start_time = std::chrono::steady_clock::now();
    auto end_of_timeout = consistent_metadata_snapshot_start_time + consistent_metadata_snapshot_timeout;
    setStatus(fmt::format("{} ({})", kGatheringMetadataStatus, 1));

    for (size_t pass = 1;; ++pass)
    {
        String new_status = fmt::format("{} ({})", kGatheringMetadataStatus, pass + 1);
        std::optional<Exception> inconsistency_error;
        if (tryGatherMetadataAndCompareWithPrevious(inconsistency_error))
        {
            /// Gathered metadata and checked consistency, cool! But we have to check that other hosts cope with that too.
            auto all_hosts_results = setStatus(new_status, "consistent");

            std::optional<String> host_with_inconsistency;
            std::optional<String> inconsistency_error_on_other_host;
            for (size_t i = 0; i != all_hosts.size(); ++i)
            {
                if ((i < all_hosts_results.size()) && (all_hosts_results[i] != "consistent"))
                {
                    host_with_inconsistency = all_hosts[i];
                    inconsistency_error_on_other_host = all_hosts_results[i];
                    break;
                }
            }

            if (!host_with_inconsistency)
                break; /// All hosts managed to gather metadata and everything is consistent, so we can go further to writing the backup.

            inconsistency_error = Exception{
                ErrorCodes::INCONSISTENT_METADATA_FOR_BACKUP,
                "Found inconsistency on host {}: {}",
                *host_with_inconsistency,
                *inconsistency_error_on_other_host};
        }
        else
        {
            /// Failed to gather metadata or something wasn't consistent. We'll let other hosts know that and try again.
            setStatus(new_status, inconsistency_error->displayText());
        }

        /// Two passes is minimum (we need to compare with table names with previous ones to be sure we don't miss anything).
        if (pass >= 2)
        {
            if (std::chrono::steady_clock::now() > end_of_timeout)
                inconsistency_error->rethrow();
            else
                LOG_WARNING(log, "{}", inconsistency_error->displayText());
        }

        auto sleep_time = getSleepTimeAfterInconsistencyError(pass);
        if (sleep_time.count() > 0)
            sleepForNanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(sleep_time).count());
    }

    LOG_INFO(log, "Will backup {} databases and {} tables", database_infos.size(), table_infos.size());
}

bool BackupEntriesCollector::tryGatherMetadataAndCompareWithPrevious(std::optional<Exception> & inconsistency_error)
{
    try
    {
        /// Collect information about databases and tables specified in the BACKUP query.
        database_infos.clear();
        table_infos.clear();
        gatherDatabasesMetadata();
        gatherTablesMetadata();
    }
    catch (Exception & e)
    {
        if (e.code() != ErrorCodes::INCONSISTENT_METADATA_FOR_BACKUP)
            throw;

        inconsistency_error = e;
        return false;
    }

    /// We have to check consistency of collected information to protect from the case when some table or database is
    /// renamed during this collecting making the collected information invalid.
    return compareWithPrevious(inconsistency_error);
}

void BackupEntriesCollector::gatherDatabasesMetadata()
{
    /// Collect information about databases and tables specified in the BACKUP query.
    for (const auto & element : backup_query_elements)
    {
        switch (element.type)
        {
            case ASTBackupQuery::ElementType::TABLE:
            {
                gatherDatabaseMetadata(
                    element.database_name,
                    /* throw_if_database_not_found= */ true,
                    /* backup_create_database_query= */ false,
                    element.table_name,
                    /* throw_if_table_not_found= */ true,
                    element.partitions,
                    /* all_tables= */ false,
                    /* except_table_names= */ {});
                break;
            }

            case ASTBackupQuery::ElementType::TEMPORARY_TABLE:
            {
                gatherDatabaseMetadata(
                    DatabaseCatalog::TEMPORARY_DATABASE,
                    /* throw_if_database_not_found= */ true,
                    /* backup_create_database_query= */ false,
                    element.table_name,
                    /* throw_if_table_not_found= */ true,
                    element.partitions,
                    /* all_tables= */ false,
                    /* except_table_names= */ {});
                break;
            }

            case ASTBackupQuery::ElementType::DATABASE:
            {
                gatherDatabaseMetadata(
                    element.database_name,
                    /* throw_if_database_not_found= */ true,
                    /* backup_create_database_query= */ true,
                    /* table_name= */ {},
                    /* throw_if_table_not_found= */ false,
                    /* partitions= */ {},
                    /* all_tables= */ true,
                    /* except_table_names= */ element.except_tables);
                break;
            }

            case ASTBackupQuery::ElementType::ALL:
            {
                for (const auto & [database_name, database] : DatabaseCatalog::instance().getDatabases())
                {
                    if (!element.except_databases.contains(database_name))
                    {
                        gatherDatabaseMetadata(
                            database_name,
                            /* throw_if_database_not_found= */ false,
                            /* backup_create_database_query= */ true,
                            /* table_name= */ {},
                            /* throw_if_table_not_found= */ false,
                            /* partitions= */ {},
                            /* all_tables= */ true,
                            /* except_table_names= */ element.except_tables);
                    }
                }
                break;
            }
        }
    }
}

void BackupEntriesCollector::gatherDatabaseMetadata(
    const String & database_name,
    bool throw_if_database_not_found,
    bool backup_create_database_query,
    const std::optional<String> & table_name,
    bool throw_if_table_not_found,
    const std::optional<ASTs> & partitions,
    bool all_tables,
    const std::set<DatabaseAndTableName> & except_table_names)
{
    auto it = database_infos.find(database_name);
    if (it == database_infos.end())
    {
        DatabasePtr database;
        if (throw_if_database_not_found)
        {
            database = DatabaseCatalog::instance().getDatabase(database_name);
        }
        else
        {
            database = DatabaseCatalog::instance().tryGetDatabase(database_name);
            if (!database)
                return;
        }

        DatabaseInfo new_database_info;
        new_database_info.database = database;
        it = database_infos.emplace(database_name, new_database_info).first;
    }

    DatabaseInfo & database_info = it->second;

    if (backup_create_database_query && !database_info.create_database_query && (database_name != DatabaseCatalog::TEMPORARY_DATABASE))
    {
        ASTPtr create_database_query;
        try
        {
            create_database_query = database_info.database->getCreateDatabaseQuery();
        }
        catch (...)
        {
            throw Exception(ErrorCodes::INCONSISTENT_METADATA_FOR_BACKUP, "Couldn't get a create query for database {}", database_name);
        }

        database_info.create_database_query = create_database_query;
        const auto & create = create_database_query->as<const ASTCreateQuery &>();

        if (create.getDatabase() != database_name)
            throw Exception(ErrorCodes::INCONSISTENT_METADATA_FOR_BACKUP, "Got a create query with unexpected name {} for database {}", backQuoteIfNeed(create.getDatabase()), backQuoteIfNeed(database_name));

        String new_database_name = renaming_map.getNewDatabaseName(database_name);
        database_info.metadata_path_in_backup = root_path_in_backup / "metadata" / (escapeForFileName(new_database_name) + ".sql");
    }

    if (table_name)
    {
        auto & table_params = database_info.tables[*table_name];
        if (throw_if_table_not_found)
            table_params.throw_if_table_not_found = true;
        if (partitions)
        {
            table_params.partitions.emplace();
            insertAtEnd(*table_params.partitions, *partitions);
        }
        database_info.except_table_names.emplace(*table_name);
    }

    if (all_tables)
    {
        database_info.all_tables = all_tables;
        for (const auto & except_table_name : except_table_names)
            if (except_table_name.first == database_name)
                database_info.except_table_names.emplace(except_table_name.second);
    }
}

void BackupEntriesCollector::gatherTablesMetadata()
{
    table_infos.clear();
    for (const auto & [database_name, database_info] : database_infos)
    {
        const auto & database = database_info.database;
        bool is_temporary_database = (database_name == DatabaseCatalog::TEMPORARY_DATABASE);

        auto filter_by_table_name = [database_info = &database_info](const String & table_name)
        {
            /// We skip inner tables of materialized views.
            if (table_name.starts_with(".inner_id."))
                return false;

            if (database_info->tables.contains(table_name))
                return true;

            if (database_info->all_tables)
                return !database_info->except_table_names.contains(table_name);

            return false;
        };

        auto db_tables = database->getTablesForBackup(filter_by_table_name, context);

        std::unordered_set<String> found_table_names;
        for (const auto & db_table : db_tables)
        {
            const auto & create_table_query = db_table.first;
            const auto & create = create_table_query->as<const ASTCreateQuery &>();
            found_table_names.emplace(create.getTable());

            if (is_temporary_database && !create.temporary)
                throw Exception(ErrorCodes::INCONSISTENT_METADATA_FOR_BACKUP, "Got a non-temporary create query for {}", tableNameWithTypeToString(database_name, create.getTable(), false));

            if (!is_temporary_database && (create.getDatabase() != database_name))
                throw Exception(ErrorCodes::INCONSISTENT_METADATA_FOR_BACKUP, "Got a create query with unexpected database name {} for {}", backQuoteIfNeed(create.getDatabase()), tableNameWithTypeToString(database_name, create.getTable(), false));
        }

        /// Check that all tables were found.
        for (const auto & [table_name, table_info] : database_info.tables)
        {
            if (table_info.throw_if_table_not_found && !found_table_names.contains(table_name))
                throw Exception(ErrorCodes::UNKNOWN_TABLE, "{} not found", tableNameWithTypeToString(database_name, table_name, true));
        }

        for (const auto & db_table : db_tables)
        {
            const auto & create_table_query = db_table.first;
            const auto & storage = db_table.second;
            const auto & create = create_table_query->as<const ASTCreateQuery &>();
            String table_name = create.getTable();

            fs::path metadata_path_in_backup, data_path_in_backup;
            auto table_name_in_backup = renaming_map.getNewTableName({database_name, table_name});
            if (table_name_in_backup.database == DatabaseCatalog::TEMPORARY_DATABASE)
            {
                metadata_path_in_backup = root_path_in_backup / "temporary_tables" / "metadata" / (escapeForFileName(table_name_in_backup.table) + ".sql");
                data_path_in_backup = root_path_in_backup / "temporary_tables" / "data" / escapeForFileName(table_name_in_backup.table);
            }
            else
            {
                metadata_path_in_backup
                    = root_path_in_backup / "metadata" / escapeForFileName(table_name_in_backup.database) / (escapeForFileName(table_name_in_backup.table) + ".sql");
                data_path_in_backup = root_path_in_backup / "data" / escapeForFileName(table_name_in_backup.database)
                    / escapeForFileName(table_name_in_backup.table);
            }

            /// Add information to `table_infos`.
            auto & res_table_info = table_infos[QualifiedTableName{database_name, table_name}];
            res_table_info.database = database;
            res_table_info.storage = storage;
            res_table_info.create_table_query = create_table_query;
            res_table_info.metadata_path_in_backup = metadata_path_in_backup;
            res_table_info.data_path_in_backup = data_path_in_backup;

            if (!backup_settings.structure_only)
            {
                auto it = database_info.tables.find(table_name);
                if (it != database_info.tables.end())
                {
                    const auto & partitions = it->second.partitions;
                    if (partitions && !storage->supportsBackupPartition())
                    {
                        throw Exception(
                            ErrorCodes::CANNOT_BACKUP_TABLE,
                            "Table engine {} doesn't support partitions, cannot backup {}",
                            storage->getName(),
                            tableNameWithTypeToString(database_name, table_name, false));
                    }
                    res_table_info.partitions = partitions;
                }
            }
        }
    }
}

void BackupEntriesCollector::lockTablesForReading()
{
    for (auto & [table_name, table_info] : table_infos)
    {
        auto storage = table_info.storage;
        TableLockHolder table_lock;
        if (storage)
        {
            try
            {
                table_lock = storage->lockForShare(context->getInitialQueryId(), context->getSettingsRef().lock_acquire_timeout);
            }
            catch (Exception & e)
            {
                if (e.code() != ErrorCodes::TABLE_IS_DROPPED)
                    throw;
                throw Exception(ErrorCodes::INCONSISTENT_METADATA_FOR_BACKUP, "{} is dropped", tableNameWithTypeToString(table_name.database, table_name.table, true));
            }
        }
    }
}

/// Check consistency of collected information about databases and tables.
bool BackupEntriesCollector::compareWithPrevious(std::optional<Exception> & inconsistency_error)
{
    /// We need to scan tables at least twice to be sure that we haven't missed any table which could be renamed
    /// while we were scanning.
    std::vector<std::pair<String, String>> databases_metadata;
    std::vector<std::pair<QualifiedTableName, String>> tables_metadata;
    databases_metadata.reserve(database_infos.size());
    tables_metadata.reserve(table_infos.size());
    for (const auto & [database_name, database_info] : database_infos)
        databases_metadata.emplace_back(database_name, database_info.create_database_query ? serializeAST(*database_info.create_database_query) : "");
    for (const auto & [table_name, table_info] : table_infos)
        tables_metadata.emplace_back(table_name, serializeAST(*table_info.create_table_query));

    /// We need to sort the lists to make the comparison below correct.
    ::sort(databases_metadata.begin(), databases_metadata.end());
    ::sort(tables_metadata.begin(), tables_metadata.end());

    SCOPE_EXIT({
        previous_databases_metadata = std::move(databases_metadata);
        previous_tables_metadata = std::move(tables_metadata);
    });

    /// Databases must be the same as during the previous scan.
    if (databases_metadata != previous_databases_metadata)
    {
        std::vector<std::pair<String, String>> difference;
        difference.reserve(databases_metadata.size());
        std::set_difference(databases_metadata.begin(), databases_metadata.end(), previous_databases_metadata.begin(),
                            previous_databases_metadata.end(), std::back_inserter(difference));

        if (!difference.empty())
        {
            inconsistency_error = Exception{
                ErrorCodes::INCONSISTENT_METADATA_FOR_BACKUP,
                "Database {} were created or changed its definition during scanning",
                backQuoteIfNeed(difference[0].first)};
            return false;
        }

        difference.clear();
        difference.reserve(previous_databases_metadata.size());
        std::set_difference(previous_databases_metadata.begin(), previous_databases_metadata.end(), databases_metadata.begin(),
                            databases_metadata.end(), std::back_inserter(difference));

        if (!difference.empty())
        {
            inconsistency_error = Exception{
                ErrorCodes::INCONSISTENT_METADATA_FOR_BACKUP,
                "Database {} were removed or changed its definition during scanning",
                backQuoteIfNeed(difference[0].first)};
            return false;
        }
    }

    /// Tables must be the same as during the previous scan.
    if (tables_metadata != previous_tables_metadata)
    {
        std::vector<std::pair<QualifiedTableName, String>> difference;
        difference.reserve(tables_metadata.size());
        std::set_difference(tables_metadata.begin(), tables_metadata.end(), previous_tables_metadata.begin(),
                            previous_tables_metadata.end(), std::back_inserter(difference));

        if (!difference.empty())
        {
            inconsistency_error = Exception{
                ErrorCodes::INCONSISTENT_METADATA_FOR_BACKUP,
                "{} were created or changed its definition during scanning",
                tableNameWithTypeToString(difference[0].first.database, difference[0].first.table, true)};
            return false;
        }

        difference.clear();
        difference.reserve(previous_tables_metadata.size());
        std::set_difference(previous_tables_metadata.begin(), previous_tables_metadata.end(), tables_metadata.begin(),
                            tables_metadata.end(), std::back_inserter(difference));

        if (!difference.empty())
        {
            inconsistency_error = Exception{
                ErrorCodes::INCONSISTENT_METADATA_FOR_BACKUP,
                "{} were removed or changed its definition during scanning",
                tableNameWithTypeToString(difference[0].first.database, difference[0].first.table, true)};
            return false;
        }
    }

    return true;
}

/// Make backup entries for all the definitions of all the databases found.
void BackupEntriesCollector::makeBackupEntriesForDatabasesDefs()
{
    for (const auto & [database_name, database_info] : database_infos)
    {
        if (!database_info.create_database_query)
            continue; /// We store CREATE DATABASE queries only if there was BACKUP DATABASE specified.

        LOG_TRACE(log, "Adding definition of database {}", backQuoteIfNeed(database_name));

        ASTPtr new_create_query = database_info.create_database_query;
        adjustCreateQueryForBackup(new_create_query, context->getGlobalContext(), nullptr);
        renameDatabaseAndTableNameInCreateQuery(new_create_query, renaming_map, context->getGlobalContext());

        const String & metadata_path_in_backup = database_info.metadata_path_in_backup;
        backup_entries.emplace_back(metadata_path_in_backup, std::make_shared<BackupEntryFromMemory>(serializeAST(*new_create_query)));
    }
}

/// Calls IDatabase::backupTable() for all the tables found to make backup entries for tables.
void BackupEntriesCollector::makeBackupEntriesForTablesDefs()
{
    for (auto & [table_name, table_info] : table_infos)
    {
        LOG_TRACE(log, "Adding definition of {}", tableNameWithTypeToString(table_name.database, table_name.table, false));

        ASTPtr new_create_query = table_info.create_table_query;
        adjustCreateQueryForBackup(new_create_query, context->getGlobalContext(), &table_info.replicated_table_shared_id);
        renameDatabaseAndTableNameInCreateQuery(new_create_query, renaming_map, context->getGlobalContext());

        const String & metadata_path_in_backup = table_info.metadata_path_in_backup;
        backup_entries.emplace_back(metadata_path_in_backup, std::make_shared<BackupEntryFromMemory>(serializeAST(*new_create_query)));
    }
}

void BackupEntriesCollector::makeBackupEntriesForTablesData()
{
    if (backup_settings.structure_only)
        return;

    for (const auto & [table_name, table_info] : table_infos)
    {
        const auto & storage = table_info.storage;
        const auto & data_path_in_backup = table_info.data_path_in_backup;
        if (storage)
        {
            LOG_TRACE(log, "Adding data of {}", tableNameWithTypeToString(table_name.database, table_name.table, false));
            storage->backupData(*this, data_path_in_backup, table_info.partitions);
        }
        else
        {
            /// Storage == null means this storage exists on other replicas but it has not been created on this replica yet.
            /// If this table is replicated in this case we call IBackupCoordination::addReplicatedDataPath() which will cause
            /// other replicas to fill the storage's data in the backup.
            /// If this table is not replicated we'll do nothing leaving the storage's data empty in the backup.
            if (table_info.replicated_table_shared_id)
                backup_coordination->addReplicatedDataPath(*table_info.replicated_table_shared_id, data_path_in_backup);
        }
    }
}

void BackupEntriesCollector::addBackupEntry(const String & file_name, BackupEntryPtr backup_entry)
{
    if (current_status == kWritingBackupStatus)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Adding backup entries is not allowed");
    backup_entries.emplace_back(file_name, backup_entry);
}

void BackupEntriesCollector::addBackupEntry(const std::pair<String, BackupEntryPtr> & backup_entry)
{
    addBackupEntry(backup_entry.first, backup_entry.second);
}

void BackupEntriesCollector::addBackupEntries(const BackupEntries & backup_entries_)
{
    if (current_status == kWritingBackupStatus)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Adding backup entries is not allowed");
    insertAtEnd(backup_entries, backup_entries_);
}

void BackupEntriesCollector::addBackupEntries(BackupEntries && backup_entries_)
{
    if (current_status == kWritingBackupStatus)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Adding backup entries is not allowed");
    insertAtEnd(backup_entries, std::move(backup_entries_));
}

void BackupEntriesCollector::addPostTask(std::function<void()> task)
{
    if (current_status == kWritingBackupStatus)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Adding post tasks is not allowed");
    post_tasks.push(std::move(task));
}

/// Runs all the tasks added with addPostCollectingTask().
void BackupEntriesCollector::runPostTasks()
{
    /// Post collecting tasks can add other post collecting tasks, our code is fine with that.
    while (!post_tasks.empty())
    {
        auto task = std::move(post_tasks.front());
        post_tasks.pop();
        std::move(task)();
    }
}

size_t BackupEntriesCollector::getAccessCounter(AccessEntityType type)
{
    access_counters.resize(static_cast<size_t>(AccessEntityType::MAX));
    return access_counters[static_cast<size_t>(type)]++;
}

}
