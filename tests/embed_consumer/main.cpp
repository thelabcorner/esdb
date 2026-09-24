#include <esdb/esdb.h>

int main() {
    esdb_open_options options{};
    esdb_open_options_init(&options);
    return options.struct_size == sizeof(options) ? 0 : 1;
}
