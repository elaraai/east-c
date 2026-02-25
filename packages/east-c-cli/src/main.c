/*
 * east-c CLI — Run compiled East IR programs from the command line.
 *
 * Usage:
 *   east-c run <ir_file> [-p PACKAGE...] [-i FILE...] [-o FILE] [-v]
 *   east-c version [-p PACKAGE...]
 */

#include <east/east.h>
#include <east/eval_result.h>
#include <east/type_of_type.h>
#include <east_std/east_std.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double elapsed_ms(struct timespec *start, struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) * 1000.0
         + (double)(end->tv_nsec - start->tv_nsec) / 1e6;
}

#define CLI_VERSION "0.1.0"
#define RUNTIME_VERSION "0.1.0"
#define MAX_PACKAGES 16
#define MAX_INPUTS 64

/* ------------------------------------------------------------------ */
/*  Format detection                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    FMT_JSON,
    FMT_BEAST2,
    FMT_BEAST,
    FMT_EAST,
    FMT_UNKNOWN,
} FileFormat;

static const char *format_name(FileFormat fmt)
{
    switch (fmt) {
    case FMT_JSON:   return "json";
    case FMT_BEAST2: return "beast2";
    case FMT_BEAST:  return "beast";
    case FMT_EAST:   return "east";
    default:         return "unknown";
    }
}

static FileFormat detect_format(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return FMT_UNKNOWN;
    if (strcmp(dot, ".json") == 0) return FMT_JSON;
    if (strcmp(dot, ".beast2") == 0) return FMT_BEAST2;
    if (strcmp(dot, ".beast") == 0) return FMT_BEAST;
    if (strcmp(dot, ".east") == 0) return FMT_EAST;
    return FMT_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/*  File I/O helpers                                                   */
/* ------------------------------------------------------------------ */

static char *read_file_text(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open file: %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    buf[rd] = '\0';
    fclose(f);
    if (out_len) *out_len = rd;
    return buf;
}

static uint8_t *read_file_binary(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open file: %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (out_len) *out_len = rd;
    return buf;
}

static int write_file_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Error: Cannot write file: %s\n", path);
        return -1;
    }
    fputs(text, f);
    fclose(f);
    return 0;
}

static int write_file_binary(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Error: Cannot write file: %s\n", path);
        return -1;
    }
    fwrite(data, 1, len, f);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  IR / value loading and saving                                      */
/* ------------------------------------------------------------------ */

static EastValue *load_ir(const char *path, bool verbose)
{
    FileFormat fmt = detect_format(path);
    if (fmt == FMT_UNKNOWN) {
        fprintf(stderr, "Error: Unknown file extension for: %s\n"
                "Supported: .beast2, .beast, .east, .json\n", path);
        return NULL;
    }

    if (verbose) fprintf(stderr, "Loading IR from %s (format: %s)\n", path, format_name(fmt));

    if (fmt == FMT_JSON) {
        size_t len = 0;
        char *text = read_file_text(path, &len);
        if (!text) return NULL;
        EastValue *val = east_json_decode(text, east_ir_type);
        free(text);
        if (!val) fprintf(stderr, "Error: Failed to decode JSON IR from %s\n", path);
        return val;
    }
    if (fmt == FMT_BEAST2) {
        size_t len = 0;
        uint8_t *data = read_file_binary(path, &len);
        if (!data) return NULL;
        EastValue *val = east_beast2_decode_full(data, len, east_ir_type);
        free(data);
        if (!val) fprintf(stderr, "Error: Failed to decode Beast2 IR from %s\n", path);
        return val;
    }
    if (fmt == FMT_BEAST) {
        size_t len = 0;
        uint8_t *data = read_file_binary(path, &len);
        if (!data) return NULL;
        EastValue *val = east_beast_decode(data, len, east_ir_type);
        free(data);
        if (!val) fprintf(stderr, "Error: Failed to decode Beast IR from %s\n", path);
        return val;
    }
    if (fmt == FMT_EAST) {
        size_t len = 0;
        char *text = read_file_text(path, &len);
        if (!text) return NULL;
        EastValue *val = east_parse_value(text, east_ir_type);
        free(text);
        if (!val) fprintf(stderr, "Error: Failed to parse East IR from %s\n", path);
        return val;
    }
    return NULL;
}

static EastValue *load_value(const char *path, EastType *type)
{
    FileFormat fmt = detect_format(path);
    if (fmt == FMT_UNKNOWN) {
        fprintf(stderr, "Error: Unknown file extension for: %s\n", path);
        return NULL;
    }

    if (fmt == FMT_JSON) {
        size_t len = 0;
        char *text = read_file_text(path, &len);
        if (!text) return NULL;
        EastValue *val = east_json_decode(text, type);
        free(text);
        if (!val) fprintf(stderr, "Error: Failed to decode JSON from %s\n", path);
        return val;
    }
    if (fmt == FMT_BEAST2) {
        size_t len = 0;
        uint8_t *data = read_file_binary(path, &len);
        if (!data) return NULL;
        EastValue *val = east_beast2_decode_full(data, len, type);
        free(data);
        if (!val) fprintf(stderr, "Error: Failed to decode Beast2 from %s\n", path);
        return val;
    }
    if (fmt == FMT_BEAST) {
        size_t len = 0;
        uint8_t *data = read_file_binary(path, &len);
        if (!data) return NULL;
        EastValue *val = east_beast_decode(data, len, type);
        free(data);
        if (!val) fprintf(stderr, "Error: Failed to decode Beast from %s\n", path);
        return val;
    }
    if (fmt == FMT_EAST) {
        size_t len = 0;
        char *text = read_file_text(path, &len);
        if (!text) return NULL;
        EastValue *val = east_parse_value(text, type);
        free(text);
        if (!val) fprintf(stderr, "Error: Failed to parse East from %s\n", path);
        return val;
    }
    return NULL;
}

static int save_value(const char *path, EastValue *value, EastType *type)
{
    FileFormat fmt = detect_format(path);
    if (fmt == FMT_UNKNOWN) {
        fprintf(stderr, "Error: Unknown file extension for output: %s\n", path);
        return -1;
    }

    if (fmt == FMT_JSON) {
        char *text = east_json_encode(value, type);
        if (!text) { fprintf(stderr, "Error: JSON encode failed\n"); return -1; }
        int rc = write_file_text(path, text);
        free(text);
        return rc;
    }
    if (fmt == FMT_BEAST2) {
        ByteBuffer *buf = east_beast2_encode_full(value, type);
        if (!buf) { fprintf(stderr, "Error: Beast2 encode failed\n"); return -1; }
        int rc = write_file_binary(path, buf->data, buf->len);
        byte_buffer_free(buf);
        return rc;
    }
    if (fmt == FMT_BEAST) {
        ByteBuffer *buf = east_beast_encode(value, type);
        if (!buf) { fprintf(stderr, "Error: Beast encode failed\n"); return -1; }
        int rc = write_file_binary(path, buf->data, buf->len);
        byte_buffer_free(buf);
        return rc;
    }
    if (fmt == FMT_EAST) {
        char *text = east_print_value(value, type);
        if (!text) { fprintf(stderr, "Error: East print failed\n"); return -1; }
        int rc = write_file_text(path, text);
        free(text);
        return rc;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Package resolution                                                 */
/* ------------------------------------------------------------------ */

static bool is_std_package(const char *name)
{
    return strcmp(name, "east-c-std") == 0
        || strcmp(name, "std") == 0;
}

/* ------------------------------------------------------------------ */
/*  Commands                                                           */
/* ------------------------------------------------------------------ */

static int cmd_run(const char *ir_path,
                   const char **packages, int num_packages,
                   const char **input_files, int num_inputs,
                   const char *output_file,
                   bool verbose)
{
    /* Init type system */
    east_type_of_type_init();

    /* Create registries */
    BuiltinRegistry *builtins = builtin_registry_new();
    east_register_all_builtins(builtins);

    PlatformRegistry *platform = platform_registry_new();

    /* Register platform packages */
    for (int i = 0; i < num_packages; i++) {
        if (is_std_package(packages[i])) {
            if (verbose) fprintf(stderr, "Loading platform: %s\n", packages[i]);
            east_std_register_all(platform);
        } else {
            fprintf(stderr, "Error: Unknown platform package: %s\n"
                    "Available: east-c-std (or shorthand: std)\n",
                    packages[i]);
            platform_registry_free(platform);
            builtin_registry_free(builtins);
            return 1;
        }
    }

    struct timespec t0, t1, t2, t3, t4, t5;

    /* Load IR */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    EastValue *ir_val = load_ir(ir_path, verbose);
    if (!ir_val) {
        platform_registry_free(platform);
        builtin_registry_free(builtins);
        return 1;
    }

    IRNode *ir = east_ir_from_value(ir_val);
    east_value_release(ir_val);

    if (!ir) {
        fprintf(stderr, "Error: Failed to convert IR value to IR node\n");
        platform_registry_free(platform);
        builtin_registry_free(builtins);
        return 1;
    }

    /* Validate IR is a function */
    if (ir->kind != IR_FUNCTION && ir->kind != IR_ASYNC_FUNCTION) {
        fprintf(stderr, "Error: IR must be a Function or AsyncFunction node, got kind %d\n"
                "The IR file should contain compiled function IR.\n",
                ir->kind);
        ir_node_release(ir);
        platform_registry_free(platform);
        builtin_registry_free(builtins);
        return 1;
    }

    /* Extract function signature */
    EastType *fn_type = ir->type;
    if (!fn_type || (fn_type->kind != EAST_TYPE_FUNCTION && fn_type->kind != EAST_TYPE_ASYNC_FUNCTION)) {
        fprintf(stderr, "Error: IR function node has invalid type\n");
        ir_node_release(ir);
        platform_registry_free(platform);
        builtin_registry_free(builtins);
        return 1;
    }

    size_t num_params = fn_type->data.function.num_inputs;
    EastType **param_types = fn_type->data.function.inputs;
    EastType *return_type = fn_type->data.function.output;

    if (verbose) {
        fprintf(stderr, "Function: %zu inputs, %s\n",
                num_params,
                ir->kind == IR_ASYNC_FUNCTION ? "async" : "sync");
        char tbuf[256];
        for (size_t i = 0; i < num_params; i++) {
            east_type_print(param_types[i], tbuf, sizeof(tbuf));
            fprintf(stderr, "  param %zu: %s\n", i, tbuf);
        }
        east_type_print(return_type, tbuf, sizeof(tbuf));
        fprintf(stderr, "  return: %s\n", tbuf);
    }

    /* Validate input count */
    if ((size_t)num_inputs != num_params) {
        char sig_buf[1024];
        int off = 0;
        off += snprintf(sig_buf + off, sizeof(sig_buf) - (size_t)off, "(");
        for (size_t i = 0; i < num_params; i++) {
            if (i > 0) off += snprintf(sig_buf + off, sizeof(sig_buf) - (size_t)off, ", ");
            off += east_type_print(param_types[i], sig_buf + off, sizeof(sig_buf) - (size_t)off);
        }
        off += snprintf(sig_buf + off, sizeof(sig_buf) - (size_t)off, ") -> ");
        east_type_print(return_type, sig_buf + off, sizeof(sig_buf) - (size_t)off);

        fprintf(stderr, "Error: Function expects %zu inputs, got %d\nSignature: %s\n",
                num_params, num_inputs, sig_buf);
        ir_node_release(ir);
        platform_registry_free(platform);
        builtin_registry_free(builtins);
        return 1;
    }

    /* Load inputs with type-directed parsing */
    EastValue **args = NULL;
    if (num_inputs > 0) {
        args = calloc((size_t)num_inputs, sizeof(EastValue *));
        for (int i = 0; i < num_inputs; i++) {
            if (verbose) {
                char tbuf[256];
                east_type_print(param_types[i], tbuf, sizeof(tbuf));
                fprintf(stderr, "Loading input %d: %s as %s\n", i, input_files[i], tbuf);
            }
            args[i] = load_value(input_files[i], param_types[i]);
            if (!args[i]) {
                char tbuf[256];
                east_type_print(param_types[i], tbuf, sizeof(tbuf));
                fprintf(stderr, "Error: Failed to parse input %d (%s) as %s\n",
                        i, input_files[i], tbuf);
                for (int j = 0; j < i; j++) east_value_release(args[j]);
                free(args);
                ir_node_release(ir);
                platform_registry_free(platform);
                builtin_registry_free(builtins);
                return 1;
            }
        }
    }

    /* Compile */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (verbose) fprintf(stderr, "Compiling...\n");

    IRNode *body = ir->data.function.body;
    EastCompiledFn *fn = east_compile(body, platform, builtins);
    if (!fn) {
        fprintf(stderr, "Error: Failed to compile IR\n");
        for (int i = 0; i < num_inputs; i++) east_value_release(args[i]);
        free(args);
        ir_node_release(ir);
        platform_registry_free(platform);
        builtin_registry_free(builtins);
        return 1;
    }

    /* Set parameter names so east_call can bind arguments */
    fn->num_params = ir->data.function.num_params;
    if (fn->num_params > 0) {
        fn->param_names = calloc(fn->num_params, sizeof(char *));
        for (size_t i = 0; i < fn->num_params; i++) {
            fn->param_names[i] = strdup(ir->data.function.params[i].name);
        }
    }

    /* Execute */
    clock_gettime(CLOCK_MONOTONIC, &t2);
    if (verbose) fprintf(stderr, "Executing...\n");

    EvalResult result = east_call(fn, args, (size_t)num_inputs);
    clock_gettime(CLOCK_MONOTONIC, &t3);

    int exit_code = 0;

    if (result.status == EVAL_ERROR) {
        fprintf(stderr, "Error: %s\n",
                result.error_message ? result.error_message : "unknown error");
        for (size_t i = 0; i < result.num_locations; i++) {
            fprintf(stderr, "  at %s:%ld:%ld\n",
                    result.locations[i].filename ? result.locations[i].filename : "?",
                    (long)result.locations[i].line,
                    (long)result.locations[i].column);
        }
        exit_code = 1;
    } else {
        /* Save or print result */
        if (output_file) {
            if (verbose) {
                char tbuf[256];
                east_type_print(return_type, tbuf, sizeof(tbuf));
                fprintf(stderr, "Saving output to %s as %s\n", output_file, tbuf);
            }
            if (save_value(output_file, result.value, return_type) != 0) {
                exit_code = 1;
            }
        } else {
            /* Print as .east format to stdout */
            char *text = east_print_value(result.value, return_type);
            if (text) {
                printf("%s\n", text);
                free(text);
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t4);

    /* Cleanup */
    if (result.value) east_value_release(result.value);
    eval_result_free(&result);
    east_compiled_fn_free(fn);
    for (int i = 0; i < num_inputs; i++) east_value_release(args[i]);
    free(args);
    ir_node_release(ir);
    platform_registry_free(platform);
    builtin_registry_free(builtins);

    clock_gettime(CLOCK_MONOTONIC, &t5);

    if (verbose) {
        fprintf(stderr, "\nTiming:\n");
        fprintf(stderr, "  Load IR:    %8.1f ms\n", elapsed_ms(&t0, &t1));
        fprintf(stderr, "  Compile:    %8.1f ms\n", elapsed_ms(&t1, &t2));
        fprintf(stderr, "  Execute:    %8.1f ms\n", elapsed_ms(&t2, &t3));
        fprintf(stderr, "  Output:     %8.1f ms\n", elapsed_ms(&t3, &t4));
        fprintf(stderr, "  Cleanup:    %8.1f ms\n", elapsed_ms(&t4, &t5));
        fprintf(stderr, "  Total:      %8.1f ms\n", elapsed_ms(&t0, &t5));
    }

    return exit_code;
}

static int cmd_version(const char **packages, int num_packages)
{
    printf("east-c-cli %s\n", CLI_VERSION);
    printf("east-c %s\n", RUNTIME_VERSION);

    if (num_packages > 0) {
        printf("\nPlatforms:\n");
        for (int i = 0; i < num_packages; i++) {
            if (is_std_package(packages[i])) {
                /* Count functions by registering into a temp registry */
                PlatformRegistry *tmp = platform_registry_new();
                east_std_register_all(tmp);
                size_t fn_count = hashmap_count(tmp->functions)
                                + hashmap_count(tmp->generic_functions);
                printf("  east-c-std %s (%zu platform functions)\n", RUNTIME_VERSION, fn_count);
                platform_registry_free(tmp);
            } else {
                printf("  %s: not available\n", packages[i]);
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Usage / help                                                       */
/* ------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s run <ir_file> [-p PACKAGE...] [-i FILE...] [-o FILE] [-v]\n"
        "  %s version [-p PACKAGE...]\n"
        "\n"
        "Commands:\n"
        "  run      Run an East IR program\n"
        "  version  Show version information\n"
        "\n"
        "Options:\n"
        "  -p, --package PACKAGE   Platform package (e.g., std or east-c-std)\n"
        "  -i, --input FILE        Input data file (repeatable, order matches params)\n"
        "  -o, --output FILE       Output file for result\n"
        "  -v, --verbose           Enable verbose output\n"
        "\n"
        "Supported formats: .json, .beast2, .beast, .east\n",
        prog, prog);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *command = argv[1];

    /* Collect options */
    const char *packages[MAX_PACKAGES];
    int num_packages = 0;
    const char *input_files[MAX_INPUTS];
    int num_inputs = 0;
    const char *output_file = NULL;
    bool verbose = false;
    const char *ir_path = NULL;

    if (strcmp(command, "run") == 0) {
        /* Parse run arguments */
        int i = 2;

        /* First non-flag argument is the IR file */
        while (i < argc) {
            if (argv[i][0] != '-') {
                ir_path = argv[i];
                i++;
                break;
            }
            /* Handle flags before ir_file */
            if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--package") == 0) && i + 1 < argc) {
                if (num_packages >= MAX_PACKAGES) {
                    fprintf(stderr, "Error: Too many packages (max %d)\n", MAX_PACKAGES);
                    return 1;
                }
                packages[num_packages++] = argv[i + 1];
                i += 2;
            } else if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--input") == 0) && i + 1 < argc) {
                if (num_inputs >= MAX_INPUTS) {
                    fprintf(stderr, "Error: Too many inputs (max %d)\n", MAX_INPUTS);
                    return 1;
                }
                input_files[num_inputs++] = argv[i + 1];
                i += 2;
            } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
                output_file = argv[i + 1];
                i += 2;
            } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
                verbose = true;
                i++;
            } else {
                fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
                print_usage(argv[0]);
                return 1;
            }
        }

        if (!ir_path) {
            fprintf(stderr, "Error: Missing IR file argument\n");
            print_usage(argv[0]);
            return 1;
        }

        /* Parse remaining flags after ir_file */
        while (i < argc) {
            if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--package") == 0) && i + 1 < argc) {
                if (num_packages >= MAX_PACKAGES) {
                    fprintf(stderr, "Error: Too many packages (max %d)\n", MAX_PACKAGES);
                    return 1;
                }
                packages[num_packages++] = argv[i + 1];
                i += 2;
            } else if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--input") == 0) && i + 1 < argc) {
                if (num_inputs >= MAX_INPUTS) {
                    fprintf(stderr, "Error: Too many inputs (max %d)\n", MAX_INPUTS);
                    return 1;
                }
                input_files[num_inputs++] = argv[i + 1];
                i += 2;
            } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
                output_file = argv[i + 1];
                i += 2;
            } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
                verbose = true;
                i++;
            } else {
                fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
                print_usage(argv[0]);
                return 1;
            }
        }

        return cmd_run(ir_path, packages, num_packages, input_files, num_inputs, output_file, verbose);

    } else if (strcmp(command, "version") == 0) {
        /* Parse version arguments */
        for (int i = 2; i < argc; i++) {
            if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--package") == 0) && i + 1 < argc) {
                if (num_packages >= MAX_PACKAGES) {
                    fprintf(stderr, "Error: Too many packages (max %d)\n", MAX_PACKAGES);
                    return 1;
                }
                packages[num_packages++] = argv[i + 1];
                i++;
            } else {
                fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
                print_usage(argv[0]);
                return 1;
            }
        }

        return cmd_version(packages, num_packages);

    } else if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    } else {
        fprintf(stderr, "Error: Unknown command: %s\n", command);
        print_usage(argv[0]);
        return 1;
    }
}
