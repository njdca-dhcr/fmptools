#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#define main fmp2sqlite_main
#include "../src/bin/fmp2sqlite.c"
#undef main

static int read_values_called;

void print_usage_and_exit(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    exit(2);
}

fmp_file_t *fmp_open_file(const char *path, fmp_error_t *errorCode) {
    (void)path;
    if (errorCode)
        *errorCode = FMP_OK;
    return calloc(1, sizeof(fmp_file_t));
}

fmp_table_array_t *fmp_list_tables(fmp_file_t *file, fmp_error_t *errorCode) {
    (void)file;
    fmp_table_array_t *tables = calloc(1, sizeof(fmp_table_array_t));
    tables->count = 1;
    tables->tables = calloc(1, sizeof(fmp_table_t));
    tables->tables[0].index = 1;
    snprintf(tables->tables[0].utf8_name,
            sizeof(tables->tables[0].utf8_name), "blank");
    if (errorCode)
        *errorCode = FMP_OK;
    return tables;
}

fmp_column_array_t *fmp_list_columns(fmp_file_t *file, fmp_table_t *table,
        fmp_error_t *errorCode) {
    (void)file;
    (void)table;
    if (errorCode)
        *errorCode = FMP_OK;
    return calloc(1, sizeof(fmp_column_array_t));
}

fmp_error_t fmp_read_values(fmp_file_t *file, fmp_table_t *table,
        fmp_value_handler handle_value, void *ctx) {
    (void)file;
    (void)table;
    (void)handle_value;
    (void)ctx;
    read_values_called = 1;
    return FMP_OK;
}

void fmp_free_columns(fmp_column_array_t *columns) {
    free(columns);
}

void fmp_free_tables(fmp_table_array_t *tables) {
    free(tables->tables);
    free(tables);
}

void fmp_close_file(fmp_file_t *file) {
    free(file);
}

int main(void) {
    char output[] = "/tmp/fmptools-zero-columns-XXXXXX";
    int fd = mkstemp(output);
    if (fd < 0)
        return 2;
    close(fd);
    unlink(output);

    char *argv[] = { "fmp2sqlite", "unused.fmp12", output, NULL };
    int result = fmp2sqlite_main(3, argv);
    unlink(output);

    if (result != 0 || read_values_called) {
        fprintf(stderr, "Zero-column table was not skipped\n");
        return 1;
    }
    return 0;
}
