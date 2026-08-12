#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "../src/read_values.c"

static const size_t long_value_len = 256 * 1024;
static int process_blocks_call;

uint64_t path_value(fmp_chunk_t *chunk, fmp_data_t *path) {
    (void)chunk;
    return path && path->len ? path->bytes[path->len - 1] : 0;
}

int table_path_match_start1(fmp_chunk_t *chunk, int depth, int value) {
    return chunk->path_level == (size_t)depth + 1 &&
        path_value(chunk, chunk->path[1]) == (uint64_t)value;
}

int table_path_match_start2(fmp_chunk_t *chunk, int depth, int first, int second) {
    return chunk->path_level == (size_t)depth + 1 &&
        path_value(chunk, chunk->path[1]) == (uint64_t)first &&
        path_value(chunk, chunk->path[2]) == (uint64_t)second;
}

void convert(iconv_t converter, uint8_t xor_mask, char *dst, size_t dst_len,
        uint8_t *src, size_t src_len) {
    (void)converter;
    (void)xor_mask;
    size_t copy_len = src_len < dst_len - 1 ? src_len : dst_len - 1;
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

fmp_table_array_t *fmp_list_tables(fmp_file_t *file, fmp_error_t *error) {
    (void)file;
    fmp_table_array_t *tables = calloc(1, sizeof(*tables));
    if (!tables) {
        *error = FMP_ERROR_MALLOC;
        return NULL;
    }
    tables->tables = calloc(1, sizeof(*tables->tables));
    if (!tables->tables) {
        free(tables);
        *error = FMP_ERROR_MALLOC;
        return NULL;
    }
    tables->count = 1;
    tables->tables[0].index = 1;
    strcpy(tables->tables[0].utf8_name, "Data");
    *error = FMP_OK;
    return tables;
}

void fmp_free_tables(fmp_table_array_t *tables) {
    if (tables) {
        free(tables->tables);
        free(tables);
    }
}

static fmp_chunk_t make_chunk(fmp_chunk_type_t type, fmp_data_t **path,
        size_t path_level, uint8_t *data, size_t data_len) {
    fmp_chunk_t chunk = { 0 };
    chunk.type = type;
    chunk.path = path;
    chunk.path_level = path_level;
    chunk.version_num = 12;
    chunk.data.bytes = data;
    chunk.data.len = data_len;
    return chunk;
}

fmp_error_t process_blocks(fmp_file_t *file, block_handler handle_block,
        chunk_handler handle_chunk, void *ctx) {
    (void)file;
    (void)handle_block;
    static uint8_t table_index[] = { 129 };
    static uint8_t schema[] = { 3 };
    static uint8_t fields[] = { 5 };
    static uint8_t row[] = { 1 };
    static uint8_t first_column[] = { 1 };
    static uint8_t second_column[] = { 2 };
    static uint8_t first_name[] = "first";
    static uint8_t second_name[] = "second";
    static uint8_t short_value[] = "y";
    static fmp_data_t table_path = { 1, table_index };
    static fmp_data_t schema_path = { 1, schema };
    static fmp_data_t fields_path = { 1, fields };
    static fmp_data_t row_path = { 1, row };
    static fmp_data_t first_column_path = { 1, first_column };
    static fmp_data_t second_column_path = { 1, second_column };

    process_blocks_call++;
    if (process_blocks_call == 1) {
        fmp_data_t *first_path[] = {
            &table_path, &schema_path, &fields_path, &first_column_path
        };
        fmp_data_t *second_path[] = {
            &table_path, &schema_path, &fields_path, &second_column_path
        };
        fmp_chunk_t first = make_chunk(FMP_CHUNK_FIELD_REF_SIMPLE,
                first_path, 4, first_name, sizeof(first_name) - 1);
        first.ref_simple = 16;
        fmp_chunk_t second = make_chunk(FMP_CHUNK_FIELD_REF_SIMPLE,
                second_path, 4, second_name, sizeof(second_name) - 1);
        second.ref_simple = 16;
        if (handle_chunk(&first, ctx) != CHUNK_NEXT ||
                handle_chunk(&second, ctx) != CHUNK_NEXT)
            return FMP_ERROR_USER_ABORTED;
        return FMP_OK;
    }

    uint8_t *long_value = malloc(long_value_len);
    if (!long_value)
        return FMP_ERROR_MALLOC;
    memset(long_value, 'x', long_value_len);
    fmp_data_t *long_path[] = {
        &table_path, &fields_path, &row_path, &first_column_path
    };
    fmp_data_t *short_path[] = { &table_path, &fields_path, &row_path };
    fmp_chunk_t first = make_chunk(FMP_CHUNK_DATA_SEGMENT,
            long_path, 4, long_value, long_value_len);
    first.segment_index = 1;
    fmp_chunk_t second = make_chunk(FMP_CHUNK_DATA_SEGMENT,
            short_path, 3, short_value, sizeof(short_value) - 1);
    second.segment_index = 2;
    chunk_status_t first_status = handle_chunk(&first, ctx);
    chunk_status_t second_status = first_status == CHUNK_NEXT ?
        handle_chunk(&second, ctx) : first_status;
    free(long_value);
    return second_status == CHUNK_NEXT ? FMP_OK : FMP_ERROR_USER_ABORTED;
}

typedef struct test_context_s {
    int begin_count;
    int value_count;
    int end_count;
} test_context_t;

static fmp_handler_status_t begin_table(fmp_table_t *table,
        fmp_column_array_t *columns, void *ctxp) {
    test_context_t *ctx = ctxp;
    if (strcmp(table->utf8_name, "Data") || columns->count != 2)
        return FMP_HANDLER_ABORT;
    ctx->begin_count++;
    return FMP_HANDLER_OK;
}

static fmp_handler_status_t handle_value(fmp_table_t *table, int row,
        fmp_column_t *column, const char *value, void *ctxp) {
    test_context_t *ctx = ctxp;
    (void)table;
    if (row != 1)
        return FMP_HANDLER_ABORT;
    if (column->index == 1) {
        if (strlen(value) != long_value_len || value[0] != 'x' ||
                value[long_value_len - 1] != 'x')
            return FMP_HANDLER_ABORT;
    } else if (column->index == 2) {
        if (strcmp(value, "y"))
            return FMP_HANDLER_ABORT;
    } else {
        return FMP_HANDLER_ABORT;
    }
    ctx->value_count++;
    return FMP_HANDLER_OK;
}

static fmp_handler_status_t end_table(fmp_table_t *table, void *ctxp) {
    test_context_t *ctx = ctxp;
    (void)table;
    ctx->end_count++;
    return FMP_HANDLER_OK;
}

int main(void) {
    struct rlimit stack_limit = { 512 * 1024, 512 * 1024 };
    if (setrlimit(RLIMIT_STACK, &stack_limit) != 0) {
        perror("setrlimit");
        return 2;
    }
    fmp_file_t file = { 0 };
    file.version_num = 12;
    test_context_t ctx = { 0 };
    fmp_database_handler_t handler = {
        .begin_table = begin_table,
        .handle_value = handle_value,
        .end_table = end_table,
        .ctx = &ctx
    };
    fmp_error_t error = fmp_read_database(&file, &handler);
    if (error != FMP_OK || ctx.begin_count != 1 || ctx.value_count != 2 ||
            ctx.end_count != 1) {
        fprintf(stderr, "Long-value lifecycle mismatch: error=%d begin=%d "
                "value=%d end=%d\n", error, ctx.begin_count,
                ctx.value_count, ctx.end_count);
        return 1;
    }
    return 0;
}
