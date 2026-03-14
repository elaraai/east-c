/*
 * Profile beast2 decode of a large file using native perf.
 * Usage: ./profile_beast2_decode /tmp/ui.beast2
 */
#include <east/east.h>
#include <east/type_of_type.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double elapsed_ms(struct timespec *a, struct timespec *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_nsec - a->tv_nsec) / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.beast2>\n", argv[0]);
        return 1;
    }

    /* Read file */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(fsize);
    fread(data, 1, fsize, f);
    fclose(f);
    fprintf(stderr, "File: %s (%ld bytes, %.1f MB)\n", argv[1], fsize, fsize / 1048576.0);

    /* Init type system */
    east_type_of_type_init();

    /* Decode */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    EastValue *val = east_beast2_decode_auto(data, fsize);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (!val) {
        fprintf(stderr, "Decode failed!\n");
        free(data);
        return 1;
    }

    fprintf(stderr, "Decode: %.1f ms\n", elapsed_ms(&t0, &t1));

    /* Release */
    struct timespec t2;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    east_value_release(val);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    fprintf(stderr, "Release: %.1f ms\n", elapsed_ms(&t0, &t2));

    free(data);
    return 0;
}
