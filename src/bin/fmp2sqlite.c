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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

#include <sqlite3.h>

#include "../fmp.h"
#include "usage.h"

typedef struct fmp_sqlite_ctx_s {
    sqlite3 *db;
    struct fmp_sqlite_table_ctx_s *tables;
    size_t table_capacity;
    int announced_schema_complete;
} fmp_sqlite_ctx_t;

typedef struct fmp_sqlite_table_ctx_s {
    int table_index;
    sqlite3_stmt *insert_stmt;
    int last_row;
    int has_row;
} fmp_sqlite_table_ctx_t;

static fmp_sqlite_table_ctx_t *find_table(fmp_sqlite_ctx_t *ctx,
        fmp_table_t *table) {
    if (table->index < 0 || (size_t)table->index >= ctx->table_capacity)
        return NULL;
    fmp_sqlite_table_ctx_t *table_ctx = &ctx->tables[table->index];
    return table_ctx->table_index == table->index ? table_ctx : NULL;
}

fmp_handler_status_t handle_value(fmp_table_t *table, int row,
        fmp_column_t *column, const char *value, void *ctxp) {
    fmp_sqlite_ctx_t *ctx = (fmp_sqlite_ctx_t *)ctxp;
    fmp_sqlite_table_ctx_t *table_ctx = find_table(ctx, table);
    if (!table_ctx)
        return FMP_HANDLER_ABORT;
    if (table_ctx->has_row && table_ctx->last_row != row) {
        int rc = sqlite3_step(table_ctx->insert_stmt);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "Error inserting data into SQLite table: %s\n", sqlite3_errmsg(ctx->db));
            return FMP_HANDLER_ABORT;
        }
        rc = sqlite3_reset(table_ctx->insert_stmt);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Error resetting INSERT statement: %s\n", sqlite3_errmsg(ctx->db));
            return FMP_HANDLER_ABORT;
        }
        sqlite3_clear_bindings(table_ctx->insert_stmt);
    }
    int rc = sqlite3_bind_text(table_ctx->insert_stmt, column->index, value,
            strlen(value), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error binding parameter: %s\n", sqlite3_errmsg(ctx->db));
        return FMP_HANDLER_ABORT;
    }
    table_ctx->last_row = row;
    table_ctx->has_row = 1;
    return FMP_HANDLER_OK;
}

static size_t create_query_length(fmp_table_t *table, fmp_column_array_t *columns) {
    size_t len = 0;
    len += sizeof("CREATE TABLE \"\" ();");
    len += strlen(table->utf8_name);
    for (int j=0; j<columns->count; j++) {
        len += sizeof("\"\" TEXT")-1;
        len += strlen(columns->columns[j].utf8_name);
        if (j < columns->count) {
            len += sizeof(", ")-1;
        }
    }
    return len;
}

static size_t insert_query_length(fmp_table_t *table, fmp_column_array_t *columns) {
    size_t len = 0;
    len += sizeof("INSERT INTO \"\" () VALUES ();");
    len += strlen(table->utf8_name);
    for (int j=0; j<columns->count; j++) {
        len += sizeof("\"\"")-1;
        len += strlen(columns->columns[j].utf8_name);
        len += sizeof("\"\"")-1;
        len += sizeof("?NNNNN")-1;
        if (j < columns->count) {
            len += sizeof(", ")-1;
            len += sizeof(", ")-1;
        }
    }
    return len;
}

static fmp_handler_status_t begin_table(fmp_table_t *table,
        fmp_column_array_t *columns, void *ctxp) {
    fmp_sqlite_ctx_t *ctx = (fmp_sqlite_ctx_t *)ctxp;
    if (!ctx->announced_schema_complete) {
        fprintf(stderr, "PHASE schema-discovery complete; creating tables and extracting rows\n");
        ctx->announced_schema_complete = 1;
    }
    if (columns->count == 0) {
        fprintf(stderr, "SKIP TABLE \"%s\" (no columns)\n", table->utf8_name);
        return FMP_HANDLER_SKIP;
    }

    size_t create_query_len = create_query_length(table, columns);
    size_t insert_query_len = insert_query_length(table, columns);
    char *create_query = malloc(create_query_len);
    char *insert_query = malloc(insert_query_len);
    if (!create_query || !insert_query) {
        free(create_query);
        free(insert_query);
        return FMP_HANDLER_ABORT;
    }

    char *p = create_query;
    char *q = insert_query;
    p += snprintf(p, create_query_len, "CREATE TABLE \"%s\" (", table->utf8_name);
    q += snprintf(q, insert_query_len, "INSERT INTO \"%s\" (", table->utf8_name);
    for (size_t j=0; j<columns->count; j++) {
        fmp_column_t *column = &columns->columns[j];
        char *colname = strdup(column->utf8_name);
        if (!colname) {
            free(create_query);
            free(insert_query);
            return FMP_HANDLER_ABORT;
        }
        size_t colname_len = strlen(colname);
        for (size_t k=0; k<colname_len; k++) {
            if (colname[k] == ' ')
                colname[k] = '_';
        }
        p += snprintf(p, create_query_len - (size_t)(p - create_query),
                "\"%s\" TEXT", colname);
        q += snprintf(q, insert_query_len - (size_t)(q - insert_query),
                "\"%s\"", colname);
        if (j < columns->count - 1) {
            p += snprintf(p, create_query_len - (size_t)(p - create_query), ", ");
            q += snprintf(q, insert_query_len - (size_t)(q - insert_query), ", ");
        }
        free(colname);
    }
    snprintf(p, create_query_len - (size_t)(p - create_query), ");");
    q += snprintf(q, insert_query_len - (size_t)(q - insert_query), ") VALUES (");
    for (size_t j=0; j<columns->count; j++) {
        fmp_column_t *column = &columns->columns[j];
        q += snprintf(q, insert_query_len - (size_t)(q - insert_query),
                "?%d", column->index);
        if (j < columns->count - 1)
            q += snprintf(q, insert_query_len - (size_t)(q - insert_query), ", ");
    }
    snprintf(q, insert_query_len - (size_t)(q - insert_query), ");");

    fprintf(stderr, "CREATE TABLE \"%s\"\n", table->utf8_name);
    char *error_message = NULL;
    int rc = sqlite3_exec(ctx->db, create_query, NULL, NULL, &error_message);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error creating SQL table: %s\n", error_message);
        fprintf(stderr, "Statement was: %s\n", create_query);
        sqlite3_free(error_message);
        free(create_query);
        free(insert_query);
        return FMP_HANDLER_ABORT;
    }

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(ctx->db, insert_query, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error preparing SQL statement: %d\n", rc);
        fprintf(stderr, "Statement was: %s\n", insert_query);
        free(create_query);
        free(insert_query);
        return FMP_HANDLER_ABORT;
    }
    free(create_query);
    free(insert_query);

    if (table->index < 0) {
        sqlite3_finalize(stmt);
        return FMP_HANDLER_ABORT;
    }
    size_t required = (size_t)table->index + 1;
    if (required > ctx->table_capacity) {
        fmp_sqlite_table_ctx_t *tables = realloc(ctx->tables,
                required * sizeof(fmp_sqlite_table_ctx_t));
        if (!tables) {
            sqlite3_finalize(stmt);
            return FMP_HANDLER_ABORT;
        }
        memset(tables + ctx->table_capacity, 0,
                (required - ctx->table_capacity) *
                sizeof(fmp_sqlite_table_ctx_t));
        ctx->tables = tables;
        ctx->table_capacity = required;
    }
    ctx->tables[table->index] = (fmp_sqlite_table_ctx_t) {
        .table_index = table->index, .insert_stmt = stmt, .last_row = 0,
        .has_row = 0
    };
    return FMP_HANDLER_OK;
}

static fmp_handler_status_t end_table(fmp_table_t *table, void *ctxp) {
    fmp_sqlite_ctx_t *ctx = (fmp_sqlite_ctx_t *)ctxp;
    fmp_sqlite_table_ctx_t *table_ctx = find_table(ctx, table);
    if (!table_ctx)
        return FMP_HANDLER_ABORT;
    if (table_ctx->has_row) {
        int rc = sqlite3_step(table_ctx->insert_stmt);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "Error inserting data into SQLite table: %s\n",
                    sqlite3_errmsg(ctx->db));
            return FMP_HANDLER_ABORT;
        }
    }
    if (sqlite3_finalize(table_ctx->insert_stmt) != SQLITE_OK)
        return FMP_HANDLER_ABORT;
    table_ctx->insert_stmt = NULL;
    return FMP_HANDLER_OK;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        print_usage_and_exit(argc, argv);
    }

    sqlite3 *db = NULL;
    char *zErrMsg = NULL;
    fmp_error_t error = FMP_OK;
    fprintf(stderr, "PHASE sector-index start\n");
    fmp_file_t *file = fmp_open_file(argv[1], &error);
    if (!file) {
        fprintf(stderr, "Error code: %d\n", error);
        return 1;
    }
    fprintf(stderr, "PHASE sector-index complete; schema-discovery start\n");

    int rc = sqlite3_open_v2(argv[2], &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error opening SQLite file\n");
        return 1;
    }

    rc = sqlite3_exec(db, "PRAGMA journal_mode = OFF;\n", NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error setting journal_mode = OFF\n");
        return 1;
    }

    rc = sqlite3_exec(db, "PRAGMA synchronous = 0;\n", NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error setting synchronous = 0\n");
        return 1;
    }
    rc = sqlite3_exec(db, "BEGIN TRANSACTION;\n", NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error beginning SQLite transaction: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        return 1;
    }

    fmp_sqlite_ctx_t ctx = { .db = db, .tables = NULL,
        .table_capacity = 0, .announced_schema_complete = 0 };
    fmp_database_handler_t handler = {
        .begin_table = begin_table,
        .handle_value = handle_value,
        .end_table = end_table,
        .ctx = &ctx
    };
    error = fmp_read_database(file, &handler);
    for (size_t i=0; i<ctx.table_capacity; i++) {
        if (ctx.tables[i].insert_stmt)
            sqlite3_finalize(ctx.tables[i].insert_stmt);
    }
    free(ctx.tables);
    if (error != FMP_OK) {
        fprintf(stderr, "Error code: %d\n", error);
        sqlite3_exec(db, "ROLLBACK;\n", NULL, NULL, NULL);
        sqlite3_close(db);
        return 1;
    }
    fprintf(stderr, "PHASE data-extraction complete; committing SQLite transaction\n");
    rc = sqlite3_exec(db, "COMMIT;\n", NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error committing SQLite transaction: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
        sqlite3_close(db);
        return 1;
    }
    if (sqlite3_close(db) != SQLITE_OK)
        return 1;

    fmp_close_file(file);

    return 0;
}
