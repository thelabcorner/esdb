#include "bench/orm_bench_common.hpp"
#include "examples/orm/user/generated/migrations/user_migrations.hpp"

#include <climits>

namespace {

class HandwrittenAdapter {
public:
    typedef esdb_orm_bench::Input InputType;

    static InputType make_input(int index) {
        return esdb_orm_bench::make_input(index);
    }

    struct Row {
        std::int64_t id;
        std::string name;
        std::string email;
        std::int64_t created_at;
    };

    explicit HandwrittenAdapter(esdb_database *database)
        : db_(esdb_orm_bench::native_handle(database)),
          insert_(NULL),
          find_(NULL),
          find_email_(NULL),
          update_(NULL),
          list_(NULL),
          delete_(NULL),
          valid_(false) {
        if (db_ == NULL) return;
        valid_ =
            prepare(&insert_, "INSERT INTO \"user\" (\"id\",\"name\",\"email\",\"created_at\") VALUES (?,?,?,?)") &&
            prepare(&find_, "SELECT \"id\",\"name\",\"email\",\"created_at\" FROM \"user\" WHERE \"id\" = ?") &&
            prepare(&find_email_, "SELECT \"id\",\"name\",\"email\",\"created_at\" FROM \"user\" WHERE \"email\" = ?") &&
            prepare(&update_, "UPDATE \"user\" SET \"name\" = ? WHERE \"id\" = ?") &&
            prepare(&list_, "SELECT \"id\",\"name\",\"email\",\"created_at\" FROM \"user\" ORDER BY \"user\".\"id\" ASC LIMIT ? OFFSET ?") &&
            prepare(&delete_, "DELETE FROM \"user\" WHERE \"id\" = ?");
        if (!valid_) finalize_all();
    }

    ~HandwrittenAdapter() { finalize_all(); }

    bool valid() const { return valid_; }

    int insert(const InputType &input) {
        if (!reset(insert_)) return -SQLITE_MISUSE;
        int rc = sqlite3_bind_int64(insert_, 1, static_cast<sqlite3_int64>(input.id));
        if (rc == SQLITE_OK) rc = bind_text(insert_, 2, input.name);
        if (rc == SQLITE_OK) rc = bind_text(insert_, 3, input.email);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(insert_, 4, static_cast<sqlite3_int64>(input.created_at));
        if (rc != SQLITE_OK) return -rc;
        rc = sqlite3_step(insert_);
        if (rc != SQLITE_DONE) return -rc;
        return static_cast<int>(sqlite3_changes64(db_));
    }

    int find(std::int64_t id, Row *out) {
        if (out == NULL || !reset(find_)) return -SQLITE_MISUSE;
        int rc = sqlite3_bind_int64(find_, 1, static_cast<sqlite3_int64>(id));
        if (rc != SQLITE_OK) return -rc;
        rc = sqlite3_step(find_);
        if (rc == SQLITE_ROW) {
            read_row(find_, out);
            return 1;
        }
        if (rc == SQLITE_DONE) return 0;
        return -rc;
    }

    int update_name(std::int64_t id, const std::string &name) {
        if (!reset(update_)) return -SQLITE_MISUSE;
        int rc = bind_text(update_, 1, name);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(update_, 2, static_cast<sqlite3_int64>(id));
        if (rc != SQLITE_OK) return -rc;
        rc = sqlite3_step(update_);
        if (rc != SQLITE_DONE) return -rc;
        return static_cast<int>(sqlite3_changes64(db_));
    }

    int list(std::int64_t limit, std::int64_t offset, std::vector<Row> *out) {
        if (out == NULL || !reset(list_)) return -SQLITE_MISUSE;
        int rc = sqlite3_bind_int64(list_, 1, static_cast<sqlite3_int64>(limit));
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(list_, 2, static_cast<sqlite3_int64>(offset));
        if (rc != SQLITE_OK) return -rc;
        int count = 0;
        for (;;) {
            rc = sqlite3_step(list_);
            if (rc == SQLITE_ROW) {
                Row row;
                read_row(list_, &row);
                out->push_back(row);
                ++count;
                continue;
            }
            if (rc == SQLITE_DONE) return count;
            return -rc;
        }
    }

    int delete_by_id(std::int64_t id) {
        if (!reset(delete_)) return -SQLITE_MISUSE;
        int rc = sqlite3_bind_int64(delete_, 1, static_cast<sqlite3_int64>(id));
        if (rc != SQLITE_OK) return -rc;
        rc = sqlite3_step(delete_);
        if (rc != SQLITE_DONE) return -rc;
        return static_cast<int>(sqlite3_changes64(db_));
    }

private:
    sqlite3 *db_;
    sqlite3_stmt *insert_;
    sqlite3_stmt *find_;
    sqlite3_stmt *find_email_;
    sqlite3_stmt *update_;
    sqlite3_stmt *list_;
    sqlite3_stmt *delete_;
    bool valid_;

    bool prepare(sqlite3_stmt **out, const char *sql) {
        return sqlite3_prepare_v3(db_, sql, -1, SQLITE_PREPARE_PERSISTENT, out, NULL) == SQLITE_OK;
    }

    static bool reset(sqlite3_stmt *statement) {
        if (statement == NULL) return false;
        (void)sqlite3_reset(statement);
        return sqlite3_clear_bindings(statement) == SQLITE_OK;
    }

    static int bind_text(sqlite3_stmt *statement, int index, const std::string &value) {
        if (value.size() > static_cast<std::size_t>(INT_MAX)) return SQLITE_TOOBIG;
        return sqlite3_bind_text(
            statement,
            index,
            value.c_str(),
            static_cast<int>(value.size()),
            SQLITE_TRANSIENT);
    }

    static std::string read_text(sqlite3_stmt *statement, int column) {
        const unsigned char *text = sqlite3_column_text(statement, column);
        const int bytes = sqlite3_column_bytes(statement, column);
        return text == NULL || bytes <= 0
            ? std::string()
            : std::string(reinterpret_cast<const char *>(text), static_cast<std::size_t>(bytes));
    }

    static void read_row(sqlite3_stmt *statement, Row *out) {
        out->id = static_cast<std::int64_t>(sqlite3_column_int64(statement, 0));
        out->name = read_text(statement, 1);
        out->email = read_text(statement, 2);
        out->created_at = static_cast<std::int64_t>(sqlite3_column_int64(statement, 3));
    }

    void finalize_all() {
        sqlite3_finalize(insert_);
        sqlite3_finalize(find_);
        sqlite3_finalize(find_email_);
        sqlite3_finalize(update_);
        sqlite3_finalize(list_);
        sqlite3_finalize(delete_);
        insert_ = find_ = find_email_ = update_ = list_ = delete_ = NULL;
    }
};

}  // namespace

int main() {
    esdb_database *database = NULL;
    esdb_open_options options;
    esdb_error error;

    esdb_open_options_init(&options);
    options.journal_mode = ESDB_JOURNAL_WAL;
    options.synchronous = ESDB_SYNCHRONOUS_NORMAL;

    const char *path = "esdb-orm-bench.sqlite";
    (void)std::remove(path);
    (void)std::remove("esdb-orm-bench.sqlite-wal");
    (void)std::remove("esdb-orm-bench.sqlite-shm");

    const std::uint64_t startup_start = esdb_orm_bench::now_ns();
    esdb_error_clear(&error);
    if (esdb_open(path, &options, &database, &error) != ESDB_OK) {
        std::fprintf(stderr, "esdb_open failed: %s\n", error.message);
        return 1;
    }
    esdb_error_clear(&error);
    if (esdb_generated_user_migrations::migrate(database, &error) != ESDB_OK) {
        std::fprintf(stderr, "migration failed: %s\n", error.message);
        esdb_close(database);
        return 1;
    }
    const std::uint64_t startup_end = esdb_orm_bench::now_ns();

    const esdb_orm_bench::Result result = esdb_orm_bench::run<HandwrittenAdapter>(
        "handwritten",
        database,
        static_cast<double>(startup_end - startup_start));

    esdb_orm_bench::print_json(result);
    esdb_close(database);
    return result.statement_reuse ? 0 : 15;
}
