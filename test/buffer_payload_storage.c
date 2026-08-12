#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/fmp.h"

int main(void) {
    const char *srcdir = getenv("srcdir");
    char path[4096];
    snprintf(path, sizeof(path), "%s/test/data/fmp12/PortableCode39-2.fmp12",
            srcdir ? srcdir : ".");

    FILE *stream = fopen(path, "rb");
    if (!stream || fseek(stream, 0, SEEK_END) != 0)
        return 2;
    long length = ftell(stream);
    if (length <= 0 || fseek(stream, 0, SEEK_SET) != 0)
        return 2;
    uint8_t *buffer = malloc((size_t)length);
    if (!buffer || fread(buffer, (size_t)length, 1, stream) != 1)
        return 2;
    fclose(stream);

    fmp_error_t error = FMP_OK;
    fmp_file_t *file = fmp_open_buffer(buffer, (size_t)length, &error);
    if (!file) {
        fprintf(stderr, "Could not open buffer fixture: %d\n", error);
        return 2;
    }
    if (file->mapped_data || file->mapped_len) {
        fprintf(stderr, "Buffer-backed input unexpectedly owns a file mapping\n");
        return 1;
    }
    for (size_t i = 0; i < file->num_blocks; i++) {
        fmp_block_t *block = file->blocks[i];
        if (block && (!block->owns_payload || !block->payload)) {
            fprintf(stderr, "Buffer block %zu does not own its payload copy\n", i);
            return 1;
        }
    }

    fmp_table_array_t *tables = fmp_list_tables(file, &error);
    if (!tables || error != FMP_OK || tables->count == 0) {
        fprintf(stderr, "Could not parse tables from buffer: %d\n", error);
        return 1;
    }
    fmp_free_tables(tables);
    fmp_close_file(file);
    free(buffer);
    return 0;
}
