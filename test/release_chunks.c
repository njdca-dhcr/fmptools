#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fmp.h"

static int tables_equal(fmp_table_array_t *left, fmp_table_array_t *right) {
    if (left->count != right->count)
        return 0;
    for (int i = 0; i < left->count; i++) {
        if (left->tables[i].index != right->tables[i].index ||
                strcmp(left->tables[i].utf8_name, right->tables[i].utf8_name) != 0)
            return 0;
    }
    return 1;
}

static int has_cached_chunks(fmp_file_t *file) {
    for (size_t i = 0; i < file->num_blocks; i++) {
        if (file->blocks[i] && file->blocks[i]->chunk)
            return 1;
    }
    return 0;
}

int main(void) {
    const char *srcdir = getenv("srcdir");
    char path[4096];
    snprintf(path, sizeof(path), "%s/test/data/fmp12/FMburgh_2012_11_07_Database.fmp12",
            srcdir ? srcdir : ".");
    fmp_error_t error = FMP_OK;
    fmp_file_t *file = fmp_open_file(path, &error);
    if (!file) {
        fprintf(stderr, "Could not open fixture: %d\n", error);
        return 2;
    }

    fmp_table_array_t *first = fmp_list_tables(file, &error);
    if (!first || error != FMP_OK) {
        fprintf(stderr, "Could not list tables: %d\n", error);
        return 2;
    }
    if (has_cached_chunks(file)) {
        fprintf(stderr, "Chunk chains remained cached after processing\n");
        return 1;
    }

    fmp_table_array_t *second = fmp_list_tables(file, &error);
    if (!second || error != FMP_OK || !tables_equal(first, second)) {
        fprintf(stderr, "Re-parsing released chunks changed table output\n");
        return 1;
    }
    if (has_cached_chunks(file)) {
        fprintf(stderr, "Chunk chains remained cached after re-processing\n");
        return 1;
    }

    fmp_free_tables(second);
    fmp_free_tables(first);
    fmp_close_file(file);
    return 0;
}
