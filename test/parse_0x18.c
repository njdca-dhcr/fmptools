#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "../src/fmp.h"
#include "../src/fmp_internal.h"

void debug(const char *fmt, ...) {
    (void)fmt;
}

int main(void) {
    static uint8_t payload[] = {
        0x18, 0x04, 0xE0, 0x0C, 0x34, 0xDA,
        0x18, 0x04, 0xE0, 0x0C, 0x3A, 0x10,
        0x40,
    };
    fmp_file_t file = { 0 };
    fmp_block_t *block = calloc(1, sizeof(fmp_block_t));
    if (!block)
        return 2;

    file.version_num = 12;
    block->payload_len = sizeof(payload);
    block->payload = payload;

    if (process_block(&file, block) != FMP_OK)
        return 1;

    fmp_chunk_t *first = block->chunk;
    if (!first || first->code != 0x18 || first->type != FMP_CHUNK_FIELD_REF_LONG ||
            first->ref_long.len != 3 || first->data.len != 2 ||
            memcmp(first->ref_long.bytes, &payload[1], 3) != 0 ||
            memcmp(first->data.bytes, &payload[4], 2) != 0)
        return 1;

    fmp_chunk_t *second = first->next;
    if (!second || second->code != 0x18 || second->type != FMP_CHUNK_FIELD_REF_LONG ||
            second->ref_long.len != 3 || second->data.len != 2 ||
            memcmp(second->ref_long.bytes, &payload[7], 3) != 0 ||
            memcmp(second->data.bytes, &payload[10], 2) != 0)
        return 1;

    fmp_chunk_t *third = second->next;
    if (!third || third->code != 0x40 || third->type != FMP_CHUNK_PATH_POP || third->next)
        return 1;

    while (block->chunk) {
        fmp_chunk_t *chunk = block->chunk;
        block->chunk = chunk->next;
        free(chunk);
    }
    free(block);
    return 0;
}
