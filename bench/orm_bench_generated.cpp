#include "bench/orm_bench_common.hpp"
#include "examples/orm/user/generated/cpp/user_repository.hpp"
#include "examples/orm/user/generated/migrations/user_migrations.hpp"

namespace {

class GeneratedAdapter {
public:
    typedef esdb_generated_user::UserRow Row;
    typedef esdb_generated_user::UserInsert InputType;

    static InputType make_input(int index) {
        const esdb_orm_bench::Input base = esdb_orm_bench::make_input(index);
        InputType value;
        value.id = base.id;
        value.name = base.name;
        value.email = base.email;
        value.created_at = base.created_at;
        return value;
    }

    explicit GeneratedAdapter(esdb_database *database)
        : repository_(database) {}

    bool valid() { return repository_.valid(); }

    int insert(const InputType &value) {
        return repository_.insert(value);
    }

    int find(std::int64_t id, Row *out) {
        return repository_.find_by_id(id, out);
    }

    int update_name(std::int64_t id, const std::string &name) {
        return repository_.update_name(id, name);
    }

    int list(std::int64_t limit, std::int64_t offset, std::vector<Row> *out) {
        return repository_.list(limit, offset, out);
    }

    int delete_by_id(std::int64_t id) {
        return repository_.delete_by_id(id);
    }

private:
    esdb_generated_user::UserRepository repository_;
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

    const esdb_orm_bench::Result result = esdb_orm_bench::run<GeneratedAdapter>(
        "generated",
        database,
        static_cast<double>(startup_end - startup_start));

    esdb_orm_bench::print_json(result);
    esdb_close(database);
    return result.statement_reuse ? 0 : 15;
}
