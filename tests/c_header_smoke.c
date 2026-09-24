#include <esdb/esdb.h>
#include <string.h>

int main(void) {
    esdb_open_options options;
    esdb_error error;
    esdb_open_options_init(&options);
    esdb_error_clear(&error);
    if (options.struct_size != sizeof(options)) return 1;
    if (esdb_abi_version() != ESDB_ABI_VERSION) return 2;
    if (strcmp(esdb_status_name(ESDB_OK), "ok") != 0) return 3;
    return 0;
}
