#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fmp.h"

typedef struct test_ctx_s {
    int begin_count;
    int value_count;
    int end_count;
} test_ctx_t;

static fmp_handler_status_t begin_table(fmp_table_t *table,
        fmp_column_array_t *columns, void *ctxp) {
    test_ctx_t *ctx = (test_ctx_t *)ctxp;
    if (strcmp(table->utf8_name, "Data") != 0 || columns->count != 3)
        return FMP_HANDLER_ABORT;
    ctx->begin_count++;
    return FMP_HANDLER_OK;
}

static fmp_handler_status_t handle_value(fmp_table_t *table, int row,
        fmp_column_t *column, const char *value, void *ctxp) {
    test_ctx_t *ctx = (test_ctx_t *)ctxp;
    (void)row;
    if (strcmp(table->utf8_name, "Data") != 0 || column->index != 28 ||
            strcmp(value, "8665929235") != 0) {
        fprintf(stderr, "Unexpected value callback: table=%s column=%d value=%s\n",
                table->utf8_name, column->index, value);
        return FMP_HANDLER_ABORT;
    }
    ctx->value_count++;
    return FMP_HANDLER_OK;
}

static fmp_handler_status_t end_table(fmp_table_t *table, void *ctxp) {
    test_ctx_t *ctx = (test_ctx_t *)ctxp;
    if (strcmp(table->utf8_name, "Data") != 0)
        return FMP_HANDLER_ABORT;
    ctx->end_count++;
    return FMP_HANDLER_OK;
}

int main(void) {
    const char *srcdir = getenv("srcdir");
    if (!srcdir)
        srcdir = ".";
    char path[4096];
    int path_len = snprintf(path, sizeof(path),
            "%s/test/data/fmp12/PortableCode39-2.fmp12", srcdir);
    if (path_len < 0 || (size_t)path_len >= sizeof(path))
        return 2;
    fmp_error_t error = FMP_OK;
    fmp_file_t *file = fmp_open_file(path, &error);
    if (!file)
        return 2;
    test_ctx_t ctx = { 0 };
    fmp_database_handler_t handler = {
        .begin_table = begin_table,
        .handle_value = handle_value,
        .end_table = end_table,
        .ctx = &ctx
    };
    error = fmp_read_database(file, &handler);
    fmp_close_file(file);
    if (error != FMP_OK || ctx.begin_count != 1 || ctx.value_count != 1 ||
            ctx.end_count != 1) {
        fprintf(stderr, "Database reader lifecycle mismatch: error=%d begin=%d "
                "value=%d end=%d\n", error, ctx.begin_count,
                ctx.value_count, ctx.end_count);
        return 1;
    }
    return 0;
}
