#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/fmp.h"

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

    if (!file->mapped_data || file->mapped_len == 0) {
        fprintf(stderr, "File-backed input has no mapping\n");
        return 1;
    }
    for (size_t i = 0; i < file->num_blocks; i++) {
        fmp_block_t *block = file->blocks[i];
        if (!block)
            continue;
        if (block->owns_payload || block->payload < file->mapped_data ||
                block->payload_len > file->mapped_len ||
                block->payload > file->mapped_data + file->mapped_len - block->payload_len) {
            fprintf(stderr, "Block payload %zu is not backed by the file mapping\n", i);
            return 1;
        }
    }

    fmp_close_file(file);
    return 0;
}
