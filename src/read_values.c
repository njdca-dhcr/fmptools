/* FMP Tools - A library for reading FileMaker Pro databases
 * Copyright (c) 2020 Evan Miller (except where otherwise noted)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "fmp.h"
#include "fmp_internal.h"

typedef struct fmp_read_values_ctx_s {
    size_t current_row;
    size_t last_row;
    unsigned char *long_string_buf;
    size_t long_string_len;
    size_t long_string_used;
    unsigned char *utf8_buf;
    size_t utf8_len;
    size_t target_table_index;
    size_t last_column;
    size_t num_columns;
    int skip;
    fmp_column_array_t compact_columns;
    fmp_file_t *file;
    fmp_table_t *table;
    fmp_column_t *columns;
    fmp_value_handler handle_value;
    const fmp_database_handler_t *database_handler;
    void *user_ctx;
    fmp_error_t error;
} fmp_read_values_ctx_t;

typedef struct fmp_read_database_ctx_s {
    fmp_file_t *file;
    fmp_table_array_t *tables;
    fmp_read_values_ctx_t *table_contexts;
    fmp_read_values_ctx_t **table_by_index;
    size_t table_by_index_count;
    const fmp_database_handler_t *handler;
    fmp_error_t error;
} fmp_read_database_ctx_t;

static fmp_handler_status_t emit_value(fmp_read_values_ctx_t *ctx, int row,
        fmp_column_t *column, const char *value) {
    if (ctx->database_handler && ctx->database_handler->handle_value) {
        return ctx->database_handler->handle_value(
                ctx->table, row, column, value, ctx->database_handler->ctx);
    }
    if (ctx->handle_value)
        return ctx->handle_value(row, column, value, ctx->user_ctx);
    return FMP_HANDLER_OK;
}

static int ensure_buffer(unsigned char **buffer, size_t *capacity,
        size_t required) {
    if (required <= *capacity)
        return 1;
    size_t next = *capacity ? *capacity : 4096;
    while (next < required) {
        if (next > SIZE_MAX / 2) {
            next = required;
            break;
        }
        next *= 2;
    }
    unsigned char *resized = realloc(*buffer, next);
    if (!resized)
        return 0;
    *buffer = resized;
    *capacity = next;
    return 1;
}

static fmp_error_t convert_and_emit(fmp_read_values_ctx_t *ctx, int row,
        fmp_column_t *column, unsigned char *input, size_t input_len) {
    if (input_len > (SIZE_MAX - 1) / 4)
        return FMP_ERROR_MALLOC;
    size_t required = input_len * 4 + 1;
    if (!ensure_buffer(&ctx->utf8_buf, &ctx->utf8_len, required))
        return FMP_ERROR_MALLOC;
    convert(ctx->file->converter, ctx->file->xor_mask,
            (char *)ctx->utf8_buf, ctx->utf8_len, input, input_len);
    if (emit_value(ctx, row, column, (char *)ctx->utf8_buf) ==
            FMP_HANDLER_ABORT)
        return FMP_ERROR_USER_ABORTED;
    return FMP_OK;
}

static fmp_error_t flush_long_string(fmp_read_values_ctx_t *ctx) {
    if (!ctx->long_string_used)
        return FMP_OK;
    if (ctx->last_column == 0 || ctx->last_column > ctx->num_columns)
        return FMP_ERROR_READ;
    fmp_error_t error = convert_and_emit(ctx, ctx->current_row,
            &ctx->columns[ctx->last_column-1], ctx->long_string_buf,
            ctx->long_string_used);
    if (error == FMP_OK)
        ctx->long_string_used = 0;
    return error;
}

static int path_is_table_data(fmp_chunk_t *chunk) {
    return table_path_match_start1(chunk, 2, 5);
}

static int path_row(fmp_chunk_t *chunk) {
    if (chunk->version_num < 7)
        return path_value(chunk, chunk->path[1]);
    return path_value(chunk, chunk->path[2]);
}

static int path_is_long_string(fmp_chunk_t *chunk, fmp_read_values_ctx_t *ctx) {
    if (!table_path_match_start1(chunk, 3, 5))
        return 0;
    uint64_t column_index = path_value(chunk, chunk->path[chunk->version_num < 7 ? 2 : 3]);
    if (ctx->last_column == 0 || column_index < ctx->last_column) {
        return path_row(chunk) > ctx->last_row;
    }
    return path_row(chunk) == ctx->last_row;
}

static chunk_status_t process_value(fmp_chunk_t *chunk, fmp_read_values_ctx_t *ctx) {
    fmp_column_t *column = NULL;
    int long_string = 0;
    size_t column_index = 0;
    if (path_is_long_string(chunk, ctx)) {
        if (chunk->type == FMP_CHUNK_FIELD_REF_SIMPLE && chunk->ref_simple == 0)
            return CHUNK_NEXT; /* Rich-text formatting */
        long_string = 1;
        column_index = path_value(chunk, chunk->path[chunk->path_level-1]);
    } else if (path_is_table_data(chunk)) {
        if (chunk->type == FMP_CHUNK_FIELD_REF_SIMPLE && chunk->ref_simple <= ctx->num_columns
                && chunk->ref_simple != 252 /* Special metadata value? */) {
            column_index = chunk->ref_simple;
        } else if (chunk->type == FMP_CHUNK_DATA_SEGMENT && chunk->segment_index <= ctx->num_columns) {
            column_index = chunk->segment_index;
        }
    }
    if (column_index == 0 || column_index > ctx->num_columns)
        return CHUNK_NEXT;

    column = &ctx->columns[column_index-1];

    if (column->index != ctx->last_column && ctx->long_string_used) {
        if (ctx->handle_value || (ctx->database_handler &&
                    ctx->database_handler->handle_value)) {
            ctx->error = flush_long_string(ctx);
            if (ctx->error != FMP_OK)
                return CHUNK_ABORT;
        } else {
            ctx->long_string_used = 0;
        }
    }
    if (path_row(chunk) != ctx->last_row || column->index < ctx->last_column) {
        ctx->current_row++;
    }
    if (long_string) {
        if (chunk->data.len > SIZE_MAX - ctx->long_string_used - 1) {
            ctx->error = FMP_ERROR_MALLOC;
            return CHUNK_ABORT;
        }
        size_t required = ctx->long_string_used + chunk->data.len + 1;
        if (!ensure_buffer(&ctx->long_string_buf, &ctx->long_string_len,
                    required)) {
            ctx->error = FMP_ERROR_MALLOC;
            return CHUNK_ABORT;
        }
        memcpy(&ctx->long_string_buf[ctx->long_string_used], chunk->data.bytes, chunk->data.len);
        ctx->long_string_used += chunk->data.len;
        ctx->long_string_buf[ctx->long_string_used] = '\0';
    } else if (ctx->handle_value || (ctx->database_handler &&
                ctx->database_handler->handle_value)) {
        ctx->error = convert_and_emit(ctx, ctx->current_row, column,
                chunk->data.bytes, chunk->data.len);
        if (ctx->error != FMP_OK)
            return CHUNK_ABORT;
    }
    ctx->last_row = path_row(chunk);
    ctx->last_column = column->index;
    return CHUNK_NEXT;
}

static chunk_status_t handle_chunk_read_values_v3(fmp_chunk_t *chunk, fmp_read_values_ctx_t *ctx) {
    if (path_value(chunk, chunk->path[0]) > 5)
        return CHUNK_DONE;

    if (chunk->type != FMP_CHUNK_FIELD_REF_SIMPLE)
        return CHUNK_NEXT;

    if (table_path_match_start2(chunk, 3, 3, 5)) {
        fmp_data_t *column_path = chunk->path[chunk->path_level-1];
        size_t column_index = path_value(chunk, column_path);
        if (column_index > ctx->num_columns) {
            ctx->num_columns = column_index;
            ctx->columns = realloc(ctx->columns, ctx->num_columns * sizeof(fmp_column_t));
        }
        fmp_column_t *current_column = ctx->columns + column_index - 1;
        if (chunk->ref_simple == 1) {
            convert(ctx->file->converter, ctx->file->xor_mask,
                    current_column->utf8_name, sizeof(current_column->utf8_name),
                    chunk->data.bytes, chunk->data.len);
            current_column->index = column_index;
        } else if (chunk->ref_simple == 2) {
            if (chunk->data.bytes[1] <= FMP_COLUMN_TYPE_GLOBAL) {
                current_column->type = chunk->data.bytes[1];
            } else {
                current_column->type = FMP_COLUMN_TYPE_UNKNOWN;
            }
        }
        return CHUNK_NEXT;
    }
    return process_value(chunk, ctx);
}

static chunk_status_t handle_chunk_read_values_v7(fmp_chunk_t *chunk, fmp_read_values_ctx_t *ctx) {
    if (path_value(chunk, chunk->path[0]) > ctx->target_table_index + 128)
        return CHUNK_DONE;
    if (path_value(chunk, chunk->path[0]) < ctx->target_table_index + 128)
        return CHUNK_NEXT;
    if (chunk->type != FMP_CHUNK_FIELD_REF_SIMPLE && chunk->type != FMP_CHUNK_DATA_SEGMENT)
        return CHUNK_NEXT;

    if (table_path_match_start2(chunk, 3, 3, 5)) {
        fmp_data_t *column_path = chunk->path[chunk->path_level-1];
        size_t column_index = path_value(chunk, column_path);
        if (column_index > ctx->num_columns) {
            ctx->num_columns = column_index;
            ctx->columns = realloc(ctx->columns, ctx->num_columns * sizeof(fmp_column_t));
        }
        fmp_column_t *current_column = ctx->columns + column_index - 1;
        if (chunk->ref_simple == 16) {
            convert(ctx->file->converter, ctx->file->xor_mask,
                    current_column->utf8_name, sizeof(current_column->utf8_name),
                    chunk->data.bytes, chunk->data.len);
            current_column->index = column_index;
        }
        return CHUNK_NEXT;
    }

    return process_value(chunk, ctx);
}

static chunk_status_t handle_chunk_read_values(fmp_chunk_t *chunk, void *ctx) {
    if (chunk->version_num >= 7)
        return handle_chunk_read_values_v7(chunk, ctx);
    return handle_chunk_read_values_v3(chunk, ctx);
}

fmp_error_t fmp_read_values(fmp_file_t *file, fmp_table_t *table, fmp_value_handler handle_value, void *user_ctx) {
    fmp_read_values_ctx_t *ctx = calloc(1, sizeof(fmp_read_values_ctx_t));
    ctx->target_table_index = table->index;
    ctx->handle_value = handle_value;
    ctx->file = file;
    ctx->table = table;
    ctx->user_ctx = user_ctx;
    fmp_error_t retval = process_blocks(file, NULL, handle_chunk_read_values, ctx);
    if (ctx->error != FMP_OK)
        retval = ctx->error;
    if (retval == FMP_OK && ctx->long_string_used && ctx->handle_value)
        retval = flush_long_string(ctx);
    free(ctx->long_string_buf);
    free(ctx->utf8_buf);
    free(ctx->columns);
    free(ctx);
    return retval;
}

static fmp_read_values_ctx_t *database_context_for_chunk(
        fmp_read_database_ctx_t *ctx, fmp_chunk_t *chunk) {
    if (ctx->tables->count == 0)
        return NULL;
    if (chunk->version_num < 7)
        return &ctx->table_contexts[0];
    if (chunk->path_level == 0)
        return NULL;
    uint64_t path_index = path_value(chunk, chunk->path[0]);
    if (path_index < 128)
        return NULL;
    size_t table_index = (size_t)(path_index - 128);
    if (table_index >= ctx->table_by_index_count)
        return NULL;
    return ctx->table_by_index[table_index];
}

static chunk_status_t ensure_database_column(fmp_read_database_ctx_t *database,
        fmp_read_values_ctx_t *table, size_t column_index) {
    if (column_index == 0 || column_index <= table->num_columns)
        return CHUNK_NEXT;
    if (column_index > SIZE_MAX / sizeof(fmp_column_t)) {
        database->error = FMP_ERROR_MALLOC;
        return CHUNK_ABORT;
    }
    fmp_column_t *columns = realloc(table->columns,
            column_index * sizeof(fmp_column_t));
    if (!columns) {
        database->error = FMP_ERROR_MALLOC;
        return CHUNK_ABORT;
    }
    memset(columns + table->num_columns, 0,
            (column_index - table->num_columns) * sizeof(fmp_column_t));
    table->columns = columns;
    table->num_columns = column_index;
    return CHUNK_NEXT;
}

static chunk_status_t handle_chunk_collect_columns(fmp_chunk_t *chunk,
        void *ctxp) {
    fmp_read_database_ctx_t *database = (fmp_read_database_ctx_t *)ctxp;
    fmp_read_values_ctx_t *table = database_context_for_chunk(database, chunk);
    if (!table || chunk->type != FMP_CHUNK_FIELD_REF_SIMPLE)
        return CHUNK_NEXT;
    if (!table_path_match_start2(chunk, 3, 3, 5))
        return CHUNK_NEXT;

    fmp_data_t *column_path = chunk->path[chunk->path_level-1];
    size_t column_index = path_value(chunk, column_path);
    if ((chunk->version_num >= 7 && chunk->ref_simple == 16) ||
            (chunk->version_num < 7 && chunk->ref_simple == 1)) {
        chunk_status_t status = ensure_database_column(
                database, table, column_index);
        if (status != CHUNK_NEXT)
            return status;
        fmp_column_t *column = &table->columns[column_index-1];
        convert(database->file->converter, database->file->xor_mask,
                column->utf8_name, sizeof(column->utf8_name),
                chunk->data.bytes, chunk->data.len);
        column->index = column_index;
    } else if (chunk->version_num < 7 && chunk->ref_simple == 2 &&
            column_index > 0 && column_index <= table->num_columns) {
        fmp_column_t *column = &table->columns[column_index-1];
        if (chunk->data.len > 1 && chunk->data.bytes[1] <= FMP_COLUMN_TYPE_GLOBAL)
            column->type = chunk->data.bytes[1];
        else
            column->type = FMP_COLUMN_TYPE_UNKNOWN;
        if (chunk->data.len > 3)
            column->collation = chunk->data.bytes[3];
    }
    return CHUNK_NEXT;
}

static fmp_error_t build_compact_columns(fmp_read_values_ctx_t *ctx) {
    size_t count = 0;
    for (size_t i=0; i<ctx->num_columns; i++) {
        if (ctx->columns[i].index)
            count++;
    }
    ctx->compact_columns.count = count;
    if (!count)
        return FMP_OK;
    ctx->compact_columns.columns = calloc(count, sizeof(fmp_column_t));
    if (!ctx->compact_columns.columns)
        return FMP_ERROR_MALLOC;
    size_t next = 0;
    for (size_t i=0; i<ctx->num_columns; i++) {
        if (ctx->columns[i].index)
            ctx->compact_columns.columns[next++] = ctx->columns[i];
    }
    return FMP_OK;
}

static chunk_status_t handle_chunk_read_database(fmp_chunk_t *chunk,
        void *ctxp) {
    fmp_read_database_ctx_t *database = (fmp_read_database_ctx_t *)ctxp;
    fmp_read_values_ctx_t *table = database_context_for_chunk(database, chunk);
    if (!table || table->skip)
        return CHUNK_NEXT;
    if (chunk->version_num < 7)
        return handle_chunk_read_values_v3(chunk, table);
    if (chunk->type != FMP_CHUNK_FIELD_REF_SIMPLE &&
            chunk->type != FMP_CHUNK_DATA_SEGMENT)
        return CHUNK_NEXT;
    if (table_path_match_start2(chunk, 3, 3, 5))
        return CHUNK_NEXT;
    return process_value(chunk, table);
}

static void free_database_context(fmp_read_database_ctx_t *ctx) {
    if (ctx->table_contexts) {
        for (size_t i=0; i<ctx->tables->count; i++) {
            free(ctx->table_contexts[i].long_string_buf);
            free(ctx->table_contexts[i].utf8_buf);
            free(ctx->table_contexts[i].columns);
            free(ctx->table_contexts[i].compact_columns.columns);
        }
    }
    free(ctx->table_by_index);
    free(ctx->table_contexts);
    fmp_free_tables(ctx->tables);
}

fmp_error_t fmp_read_database(fmp_file_t *file,
        const fmp_database_handler_t *handler) {
    fmp_error_t retval = FMP_OK;
    fmp_read_database_ctx_t ctx = { .file = file, .handler = handler,
        .error = FMP_OK };
    ctx.tables = fmp_list_tables(file, &retval);
    if (!ctx.tables)
        return retval;
    ctx.table_contexts = calloc(ctx.tables->count,
            sizeof(fmp_read_values_ctx_t));
    if (ctx.tables->count && !ctx.table_contexts) {
        free_database_context(&ctx);
        return FMP_ERROR_MALLOC;
    }

    size_t max_table_index = 0;
    for (size_t i=0; i<ctx.tables->count; i++) {
        fmp_table_t *table = &ctx.tables->tables[i];
        if (table->index > 0 && (size_t)table->index > max_table_index)
            max_table_index = table->index;
    }
    ctx.table_by_index_count = max_table_index + 1;
    ctx.table_by_index = calloc(ctx.table_by_index_count,
            sizeof(fmp_read_values_ctx_t *));
    if (ctx.table_by_index_count && !ctx.table_by_index) {
        free_database_context(&ctx);
        return FMP_ERROR_MALLOC;
    }
    for (size_t i=0; i<ctx.tables->count; i++) {
        fmp_table_t *table = &ctx.tables->tables[i];
        fmp_read_values_ctx_t *table_ctx = &ctx.table_contexts[i];
        table_ctx->target_table_index = table->index;
        table_ctx->file = file;
        table_ctx->table = table;
        table_ctx->database_handler = handler;
        if (table->index >= 0 &&
                (size_t)table->index < ctx.table_by_index_count)
            ctx.table_by_index[table->index] = table_ctx;
    }

    retval = process_blocks(file, NULL, handle_chunk_collect_columns, &ctx);
    if (ctx.error != FMP_OK)
        retval = ctx.error;
    if (retval != FMP_OK) {
        free_database_context(&ctx);
        return retval;
    }

    for (size_t i=0; i<ctx.tables->count; i++) {
        fmp_read_values_ctx_t *table_ctx = &ctx.table_contexts[i];
        retval = build_compact_columns(table_ctx);
        if (retval != FMP_OK) {
            free_database_context(&ctx);
            return retval;
        }
        if (handler && handler->begin_table) {
            fmp_handler_status_t status = handler->begin_table(
                    table_ctx->table, &table_ctx->compact_columns,
                    handler->ctx);
            if (status == FMP_HANDLER_ABORT) {
                free_database_context(&ctx);
                return FMP_ERROR_USER_ABORTED;
            }
            table_ctx->skip = status == FMP_HANDLER_SKIP;
        }
    }

    retval = process_blocks(file, NULL, handle_chunk_read_database, &ctx);
    for (size_t i=0; i<ctx.tables->count; i++) {
        if (ctx.table_contexts[i].error != FMP_OK) {
            retval = ctx.table_contexts[i].error;
            break;
        }
    }
    if (retval == FMP_OK) {
        for (size_t i=0; i<ctx.tables->count && retval == FMP_OK; i++) {
            fmp_read_values_ctx_t *table_ctx = &ctx.table_contexts[i];
            if (!table_ctx->skip)
                retval = flush_long_string(table_ctx);
            if (retval == FMP_OK && !table_ctx->skip && handler &&
                    handler->end_table &&
                    handler->end_table(table_ctx->table, handler->ctx) ==
                    FMP_HANDLER_ABORT)
                retval = FMP_ERROR_USER_ABORTED;
        }
    }
    free_database_context(&ctx);
    return retval;
}
