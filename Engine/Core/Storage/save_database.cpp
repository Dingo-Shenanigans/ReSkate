#include "save_database.h"
#include <sqlite3.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace dingosdk::storage {
namespace {
constexpr int application_id = 0x52534b54; // RSKT
constexpr std::size_t max_bytes = 64 * 1024 * 1024, max_record = 4 * 1024 * 1024, max_nodes = 262144;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void sql_check(int code, sqlite3* db) {
    if (code != SQLITE_OK) throw std::runtime_error(std::string("Save database: ") + sqlite3_errmsg(db));
}
void execute(sqlite3* db, const char* sql) { sql_check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), db); }
class Connection {
public:
    sqlite3* db{};
    Connection(const std::filesystem::path& path, int flags) {
        const auto name = path.u8string();
        const auto code = sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &db, flags | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (code != SQLITE_OK) {
            const std::string error = db ? sqlite3_errmsg(db) : "Cannot allocate SQLite connection";
            if (db) sqlite3_close(db);
            db = nullptr; throw std::runtime_error("Save database: " + error);
        }
        sqlite3_extended_result_codes(db, 1);
        sqlite3_limit(db, SQLITE_LIMIT_LENGTH, static_cast<int>(max_record + 8192));
        sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, 16384);
        sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
        // ReSkate owns its writers. External contention fails promptly without
        // pausing a native callback for a long database busy timeout.
        sqlite3_busy_timeout(db, 0);
    }
    ~Connection() { if (db) sqlite3_close(db); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
};
class Statement {
public:
    sqlite3_stmt* statement{};
    sqlite3* db{};
    Statement(sqlite3* connection, const char* sql) : db(connection) {
        sql_check(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr), db);
    }
    ~Statement() { sqlite3_finalize(statement); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bind(int index, std::string_view value) {
        check(value.size() <= max_record, "Save database record exceeds limit");
        sql_check(sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT), db);
    }
    void bind(int index, const Json& value) {
        if (value.is_string()) bind(index, std::string_view(value.string()));
        else if (value.is_null()) sql_check(sqlite3_bind_null(statement, index), db);
        else if (value.is_boolean()) sql_check(sqlite3_bind_int(statement, index, value.get<bool>() ? 1 : 0), db);
        else if (value.is_number_integer()) {
            check(!value.is_number_unsigned() || value.get<std::uint64_t>() <= INT64_MAX, "SQL integer overflow");
            sql_check(sqlite3_bind_int64(statement, index, value.get<std::int64_t>()), db);
        } else if (value.is_number_float()) sql_check(sqlite3_bind_double(statement, index, value.get<double>()), db);
        else throw std::runtime_error("SQL columns require scalar values");
    }
    Json value(int column) const {
        switch (sqlite3_column_type(statement, column)) {
        case SQLITE_TEXT: return text(column);
        case SQLITE_INTEGER: {
            const auto n = sqlite3_column_int64(statement, column);
            return n >= 0 ? Json(static_cast<std::uint64_t>(n)) : Json(n);
        }
        case SQLITE_FLOAT: return sqlite3_column_double(statement, column);
        case SQLITE_NULL: return nullptr;
        default: throw std::runtime_error("Unexpected SQL column type");
        }
    }
    bool row() {
        const auto code = sqlite3_step(statement);
        if (code == SQLITE_ROW) return true;
        if (code == SQLITE_DONE) return false;
        sql_check(code, db); return false;
    }
    std::string text(int column) const {
        check(sqlite3_column_type(statement, column) == SQLITE_TEXT, "Invalid save database record type");
        const auto* data = sqlite3_column_text(statement, column);
        const auto size = sqlite3_column_bytes(statement, column);
        check(data && size >= 0 && static_cast<std::size_t>(size) <= max_record, "Invalid save database text");
        return {reinterpret_cast<const char*>(data), static_cast<std::size_t>(size)};
    }
    void run() {
        check(!row(), "Unexpected save database result");
        sql_check(sqlite3_reset(statement), db); sql_check(sqlite3_clear_bindings(statement), db);
    }
};
class Transaction {
    sqlite3* db_;
    bool committed_{};
public:
    explicit Transaction(sqlite3* db, bool write) : db_(db) { execute(db_, write ? "BEGIN IMMEDIATE" : "BEGIN"); }
    ~Transaction() { if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { execute(db_, "COMMIT"); committed_ = true; }
};
int integer(sqlite3* db, const char* query) {
    Statement s(db, query); check(s.row(), "Missing save database metadata");
    const auto value = sqlite3_column_int(s.statement, 0);
    check(!s.row(), "Duplicate save database metadata"); return value;
}
void verify(sqlite3* db, std::string_view kind) {
    check(integer(db, "PRAGMA application_id") == application_id && integer(db, "PRAGMA user_version") == 1,
        "Unsupported ReSkate save database; original file retained");
    Statement type(db, "SELECT value FROM metadata WHERE key='kind'");
    check(type.row() && type.text(0) == kind && !type.row(), "Wrong ReSkate save database kind");
    Statement integrity(db, "PRAGMA quick_check(1)");
    check(integrity.row() && integrity.text(0) == "ok" && !integrity.row(), "Save database integrity check failed");
}
// Check complete trees without serializing the entire profile on each save.
// Bounds also cover arrays stored as one record and future extension members.
void bounds(const Json& value, std::size_t& nodes, std::size_t& bytes, unsigned depth = 0) {
    check(depth <= 32 && ++nodes <= max_nodes, "Save database tree exceeds complexity limit");
    bytes += 32;
    if (value.is_string()) bytes += value.string().size();
    else if (value.is_number_float()) check(std::isfinite(value.get<double>()), "Non-finite save value");
    else if (value.is_object()) for (const auto& [key, child] : value.items()) {
        bytes += key.size(); bounds(child, nodes, bytes, depth + 1);
    }
    else if (value.is_array()) for (const auto& child : value) bounds(child, nodes, bytes, depth + 1);
    check(bytes <= max_bytes, "Save database tree exceeds size limit");
}
void validate_tree(const Json& value) {
    check(value.is_object(), "Save database requires an object root");
    std::size_t nodes{}, bytes{}; bounds(value, nodes, bytes);
}
std::vector<std::string> columns(const SaveTable& table) {
    std::vector<std::string> result;
    std::string_view names = table.columns;
    while (!names.empty()) {
        const auto comma = names.find(','); result.emplace_back(names.substr(0, comma));
        if (comma == std::string_view::npos) break; names.remove_prefix(comma + 1);
    }
    check(!result.empty() && table.key_columns > 0 && table.key_columns <= result.size(), "Invalid save schema");
    return result;
}
void validate_tables(const Json& value, SaveSchema schema) {
    validate_tree(value);
    check(value.size() == schema.tables.size(), "Unexpected save tables");
    for (const auto& table : schema.tables) {
        check(value.contains(table.name) && value.at(table.name).is_object(), "Missing save table");
        const auto width = columns(table).size();
        for (const auto& [key, row] : value.at(table.name).items()) {
            check(row.is_array() && row.size() == width && row_key(row, table.key_columns) == key, "Invalid save row");
            for (const auto& cell : row) check(!cell.is_object() && !cell.is_array(), "Invalid SQL cell");
        }
    }
}
Json load(sqlite3* db, SaveSchema schema) {
    Json root = Json::object(); std::size_t count{}, bytes{};
    for (const auto& table : schema.tables) {
        const auto names = columns(table);
        auto& rows = root[table.name]; rows = Json::object();
        const auto sql = "SELECT " + std::string(table.columns) + " FROM " + std::string(table.name);
        Statement records(db, sql.c_str());
        while (records.row()) {
            check(++count <= max_nodes, "Too many save rows");
            Json row = Json::array();
            for (std::size_t i = 0; i < names.size(); ++i) {
                auto value = records.value(static_cast<int>(i));
                bytes += value.is_string() ? value.string().size() : 16;
                check(bytes <= max_bytes, "Save database exceeds limit"); row.push_back(std::move(value));
            }
            const auto key = row_key(row, table.key_columns);
            check(!rows.contains(key), "Duplicate save database key"); rows[key] = std::move(row);
        }
    }
    validate_tables(root, schema); return root;
}
void apply_changes(sqlite3* db, SaveSchema schema, const Json* before, const Json& after) {
    for (const auto& table : schema.tables) {
        const auto& next = after.at(table.name);
        const Json* old = before ? &before->at(table.name) : nullptr;
        if (old && *old == next) continue;
        const auto names = columns(table);
        std::string insert = "INSERT INTO " + std::string(table.name) + '(' + std::string(table.columns) + ") VALUES(";
        std::string remove = "DELETE FROM " + std::string(table.name) + " WHERE ";
        for (std::size_t i = 0; i < names.size(); ++i) { if (i) insert += ','; insert += '?'; }
        insert += ") ON CONFLICT(";
        for (unsigned i = 0; i < table.key_columns; ++i) {
            if (i) { insert += ','; remove += " AND "; }
            insert += names[i]; remove += names[i] + "=?";
        }
        insert += ')';
        if (table.key_columns == names.size()) insert += " DO NOTHING";
        else {
            insert += " DO UPDATE SET ";
            for (std::size_t i = table.key_columns; i < names.size(); ++i) {
                if (i != table.key_columns) insert += ',';
                insert += names[i] + "=excluded." + names[i];
            }
        }
        Statement erase(db, remove.c_str()), upsert(db, insert.c_str());
        if (old) for (const auto& [key, row] : old->items()) if (!next.contains(key)) {
            for (unsigned i = 0; i < table.key_columns; ++i) erase.bind(static_cast<int>(i + 1), row[i]);
            erase.run();
        }
        for (const auto& [key, row] : next.items()) {
            if (old && old->contains(key) && old->at(key) == row) continue;
            for (std::size_t i = 0; i < names.size(); ++i) upsert.bind(static_cast<int>(i + 1), row[i]);
            upsert.run();
        }
    }
}
void configure_writer(sqlite3* db) {
    Statement mode(db, "PRAGMA journal_mode=WAL");
    check(mode.row() && mode.text(0) == "wal" && !mode.row(), "Cannot enable save database WAL");
    // NORMAL, not FULL: in WAL mode a commit then no longer waits for the disk. A transaction is
    // still kept when the game crashes (it is in the operating system's hands); only a power
    // loss or a crash of Windows itself can take back the last commits, never corrupt the file.
    // FULL made every commit wait for a flush, on the game's own threads: each play event the
    // game saves, each option, under locks the game's hooks wait on (2026-10-15).
    execute(db, "PRAGMA foreign_keys=ON; PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=256; PRAGMA journal_size_limit=1048576; PRAGMA max_page_count=16384;");
}
void check_size(const std::filesystem::path& path) { check(std::filesystem::file_size(path) <= max_bytes, "Save database exceeds size limit"); }
void clear_staging(const std::filesystem::path& path) {
    // These are unpublished files owned by the locked writer. Remove journals
    // too, so a retry cannot recover an abandoned transaction into a new file.
    for (const auto* suffix : {L"", L"-journal", L"-wal", L"-shm"}) {
        const auto file = path.wstring() + suffix;
        if (std::filesystem::exists(file)) check(!!DeleteFileW(file.c_str()), "Cannot clear interrupted save staging");
    }
}
}
std::string row_key(const Json& row, unsigned key_columns) {
    check(row.is_array() && row.size() >= key_columns, "Invalid save key");
    Json key = Json::array(); for (unsigned i = 0; i < key_columns; ++i) key.push_back(row[i]);
    return key.dump();
}
std::filesystem::path database_path(std::filesystem::path path) {
    if (path.extension() == L".json") path.replace_extension(L".sqlite3"); return path;
}
std::filesystem::path json_source_path(std::filesystem::path path) { return path.replace_extension(L".json"); }
struct SaveDatabase::Impl {
    std::filesystem::path path;
    SaveSchema schema;
    std::size_t nodes{}, bytes{};
    std::vector<HANDLE> locks;
    std::unique_ptr<Connection> connection;
    Json value;
    int version{};
    ~Impl() { connection.reset(); for (const auto handle : locks) CloseHandle(handle); }
    void lock(const std::filesystem::path& target) {
        const auto handle = CreateFileW((target.wstring() + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "Save is already in use or inaccessible; close the game before editing");
        try { locks.push_back(handle); } catch (...) { CloseHandle(handle); throw; }
    }
};
SaveDatabase::SaveDatabase(std::filesystem::path path, SaveSchema schema) : impl_(std::make_unique<Impl>()) {
    auto& s = *impl_; s.path = database_path(std::move(path)); s.schema = schema;
    if (!s.path.parent_path().empty()) std::filesystem::create_directories(s.path.parent_path());
    s.lock(s.path); s.lock(json_source_path(s.path)); // Excludes older JSON writers too.
    const auto legacy = s.path.parent_path() / L"profile.rsp";
    if (schema.name == "profile" && std::filesystem::exists(legacy)) s.lock(legacy);
    if (!std::filesystem::exists(s.path)) {
        check(!std::filesystem::exists(s.path.wstring() + L".bak") && !std::filesystem::exists(s.path.wstring() + L"-wal") &&
            !std::filesystem::exists(s.path.wstring() + L"-shm") && !std::filesystem::exists(s.path.wstring() + L"-journal"), "Save database missing while recovery files exist"); return;
    }
    check_size(s.path);
    s.connection = std::make_unique<Connection>(s.path, SQLITE_OPEN_READWRITE);
    Transaction read(s.connection->db, false); verify(s.connection->db, schema.name); s.value = load(s.connection->db, schema);
    s.version = integer(s.connection->db, "PRAGMA data_version"); read.commit();
    bounds(s.value, s.nodes, s.bytes);
}
SaveDatabase::~SaveDatabase() = default;
bool SaveDatabase::exists() const { return !!impl_->connection; }
const Json& SaveDatabase::document() const { check(exists(), "Save database is not initialized"); return impl_->value; }
void SaveDatabase::initialize(Json value) {
    check(!exists(), "Save database is already initialized"); validate_tables(value, impl_->schema);
    auto& s = *impl_;
    const auto temporary = s.path.wstring() + L".migrating";
    // Unpublished staging belongs to this writer. Retrying a crashed migration
    // never replaces the active database or the retained original JSON.
    clear_staging(temporary);
    {
        Connection initial(temporary, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        execute(initial.db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA synchronous=EXTRA; PRAGMA max_page_count=16384;");
        Transaction transaction(initial.db, true);
        execute(initial.db, "PRAGMA application_id=1381190484; PRAGMA user_version=1;"
            "CREATE TABLE metadata(key TEXT PRIMARY KEY NOT NULL,value TEXT NOT NULL) STRICT, WITHOUT ROWID;");
        for (const auto& table : s.schema.tables) execute(initial.db, std::string(table.create_sql).c_str());
        Statement type(initial.db, "INSERT INTO metadata VALUES('kind',?1)"); type.bind(1, s.schema.name); type.run();
        apply_changes(initial.db, s.schema, nullptr, value); transaction.commit();
    }
    check(!!MoveFileExW(temporary.c_str(), s.path.c_str(), MOVEFILE_WRITE_THROUGH), "Cannot publish migrated save database");
    s.connection = std::make_unique<Connection>(s.path, SQLITE_OPEN_READWRITE);
    verify(s.connection->db, s.schema.name); configure_writer(s.connection->db);
    s.version = integer(s.connection->db, "PRAGMA data_version"); s.value = std::move(value);
    s.nodes = s.bytes = 0; bounds(s.value, s.nodes, s.bytes);
}
void SaveDatabase::commit(Json value) {
    auto& s = *impl_; check(exists(), "Save database is not initialized"); validate_tables(value, s.schema);
    if (s.value == value) return;
    Transaction transaction(s.connection->db, true);
    check(integer(s.connection->db, "PRAGMA data_version") == s.version, "Save database edited externally; restart to load changes");
    apply_changes(s.connection->db, s.schema, &s.value, value); transaction.commit();
    s.value = std::move(value); // Memory is published only after durable COMMIT.
    s.nodes = s.bytes = 0; bounds(s.value, s.nodes, s.bytes);
}
void SaveDatabase::patch(Json patch) {
    auto& s = *impl_;
    check(exists() && patch.is_object(), "Invalid save patch");
    Json before = Json::object(), after = Json::object();
    for (const auto& table : s.schema.tables) {
        before[table.name] = Json::object(); after[table.name] = Json::object();
    }
    auto nodes = s.nodes, bytes = s.bytes;
    struct Publication {
        Json::Object* rows;
        Json::Object::iterator old;
        Json::Object::node_type replacement;
    };
    std::vector<Publication> publications;
    for (auto& [table_name, changes] : patch.items()) {
        const auto table = std::find_if(s.schema.tables.begin(), s.schema.tables.end(),
            [&](const auto& item) { return item.name == table_name; });
        check(table != s.schema.tables.end() && changes.is_object(), "Unknown save patch table");
        const auto width = columns(*table).size();
        auto& rows = s.value.at(table_name).items();
        for (auto it = changes.items().begin(); it != changes.items().end();) {
            auto current = it++;
            const auto& key = current->first;
            const auto& row = current->second;
            auto old = rows.find(key);
            if ((old == rows.end() && row.is_null()) ||
                (old != rows.end() && old->second == row)) continue;
            if (old != rows.end()) {
                std::size_t old_nodes{}, old_bytes{};
                bounds(old->second, old_nodes, old_bytes, 2);
                nodes -= old_nodes; bytes -= old_bytes + key.size();
                before[table_name][key] = old->second;
            }
            if (!row.is_null()) {
                check(row.is_array() && row.size() == width &&
                    row_key(row, table->key_columns) == key, "Invalid save patch row");
                for (const auto& cell : row) check(!cell.is_object() && !cell.is_array(), "Invalid SQL cell");
                std::size_t new_nodes{}, new_bytes{};
                bounds(row, new_nodes, new_bytes, 2);
                nodes += new_nodes; bytes += new_bytes + key.size();
                after[table_name][key] = row;
            }
            publications.push_back({&rows, old, {}});
            if (!row.is_null()) publications.back().replacement = changes.items().extract(current);
        }
    }
    if (publications.empty()) return;
    check(nodes <= max_nodes && bytes <= max_bytes, "Save patch exceeds database limits");
    Transaction transaction(s.connection->db, true);
    check(integer(s.connection->db, "PRAGMA data_version") == s.version,
        "Save database edited externally; restart to load changes");
    const auto changed_before = sqlite3_total_changes64(s.connection->db);
    apply_changes(s.connection->db, s.schema, &before, after);
    check(sqlite3_total_changes64(s.connection->db) - changed_before == static_cast<sqlite3_int64>(publications.size()),
        "Save patch changed rows outside its declared records");
    transaction.commit();
    // Every replacement map node was allocated before COMMIT. Publishing the
    // cache uses only erasure/node transfer, so allocation failures cannot leave
    // a successfully saved transaction absent from this session's read model.
    for (auto& item : publications) {
        if (item.old != item.rows->end()) item.rows->erase(item.old);
        if (!item.replacement.empty()) item.rows->insert(std::move(item.replacement));
    }
    s.nodes = nodes; s.bytes = bytes;
}
void SaveDatabase::backup() {
    auto& s = *impl_; check(exists(), "Save database is not initialized");
    const auto temporary = s.path.wstring() + L".bak.pending";
    clear_staging(temporary);
    {
        Transaction read(s.connection->db, false);
        check(integer(s.connection->db, "PRAGMA data_version") == s.version, "Save database changed during load");
        Connection destination(temporary, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        auto* backup = sqlite3_backup_init(destination.db, "main", s.connection->db, "main");
        check(backup != nullptr, "Cannot initialize save backup");
        const auto result = sqlite3_backup_step(backup, -1); const auto finish = sqlite3_backup_finish(backup);
        check(result == SQLITE_DONE && finish == SQLITE_OK, "Cannot complete save backup");
        execute(destination.db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=EXTRA;"); read.commit();
    }
    check(!!MoveFileExW(temporary.c_str(), (s.path.wstring() + L".bak").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
        "Cannot publish save backup");
    configure_writer(s.connection->db);
    check(integer(s.connection->db, "PRAGMA data_version") == s.version, "Save database changed during backup");
}
}
