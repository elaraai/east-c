/*
 * Parallel platform functions for East.
 *
 * parallel_map uses pthreads with Beast2 serialization for true
 * parallelism. Each worker thread gets an independent copy of the
 * function and input chunk (serialized/deserialized via Beast2),
 * so there is zero shared mutable state between threads.
 */

#include "east_std/east_std.h"
#include <east/values.h>
#include <east/eval_result.h>
#include <east/types.h>
#include <east/compiler.h>
#include <east/serialization.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

static _Thread_local EastType *s_input_type = NULL;
static _Thread_local EastType *s_result_type = NULL;
static EvalResult parallel_map_impl(EastValue **args, size_t num_args);

static PlatformFn parallel_map_factory(EastType **tp, size_t num_tp) {
    /* tp[0] = T (input element type), tp[1] = R (output element type) */
    if (num_tp >= 2) {
        s_input_type = tp[0];
        s_result_type = tp[1];
    }
    return parallel_map_impl;
}

/* ================================================================== */
/*  Worker thread data                                                 */
/* ================================================================== */

typedef struct {
    /* Input (owned by main thread, read-only for worker) */
    const uint8_t *fn_bytes;
    size_t fn_bytes_len;
    const uint8_t *chunk_bytes;
    size_t chunk_bytes_len;
    EastType *fn_type;       /* FunctionType([T], R) - shared, immutable */
    EastType *array_in_type; /* ArrayType(T) - shared, immutable */
    EastType *array_out_type;/* ArrayType(R) - shared, immutable */
    EastType *elem_out_type; /* R - shared, immutable */
    PlatformRegistry *platform;
    BuiltinRegistry *builtins;

    /* Output (written by worker, read by main after join) */
    uint8_t *result_bytes;
    size_t result_bytes_len;
    char *error_message;     /* NULL on success */
} WorkerData;

static void *worker_thread(void *arg) {
    WorkerData *wd = (WorkerData *)arg;

    /* Set thread-local context so Beast2 decode can find platform/builtins */
    east_set_thread_context(wd->platform, wd->builtins);

    /* Decode the function */
    EastValue *fn_val = east_beast2_decode(
        wd->fn_bytes, wd->fn_bytes_len, wd->fn_type);
    if (!fn_val || fn_val->kind != EAST_VAL_FUNCTION) {
        wd->error_message = strdup("Failed to decode function in worker");
        if (fn_val) east_value_release(fn_val);
        return NULL;
    }

    /* Decode the input chunk */
    EastValue *chunk = east_beast2_decode(
        wd->chunk_bytes, wd->chunk_bytes_len, wd->array_in_type);
    if (!chunk) {
        wd->error_message = strdup("Failed to decode input chunk in worker");
        east_value_release(fn_val);
        return NULL;
    }

    /* Apply function to each element */
    size_t len = east_array_len(chunk);
    EastValue *results = east_array_new(wd->elem_out_type);

    for (size_t i = 0; i < len; i++) {
        EastValue *item = east_array_get(chunk, i);
        east_value_retain(item);

        EastValue *call_args[] = { item };
        EvalResult r = east_call(fn_val->data.function.compiled, call_args, 1);
        east_value_release(item);

        if (r.status != EVAL_OK) {
            wd->error_message = r.error_message
                ? strdup(r.error_message) : strdup("Worker function error");
            eval_result_free(&r);
            east_value_release(results);
            east_value_release(chunk);
            east_value_release(fn_val);
            return NULL;
        }

        east_array_push(results, r.value);
        east_value_release(r.value);
        eval_result_free(&r);
    }

    /* Encode results */
    ByteBuffer *buf = east_beast2_encode(results, wd->array_out_type);
    if (buf) {
        wd->result_bytes = buf->data;
        wd->result_bytes_len = buf->len;
        /* Take ownership of data, free just the struct */
        buf->data = NULL;
        byte_buffer_free(buf);
    } else {
        wd->error_message = strdup("Failed to encode worker results");
    }

    east_value_release(results);
    east_value_release(chunk);
    east_value_release(fn_val);
    return NULL;
}

/* ================================================================== */
/*  parallel_map implementation                                        */
/* ================================================================== */

static EvalResult parallel_map_impl(EastValue **args, size_t num_args) {
    (void)num_args;
    EastValue *array = args[0];
    EastValue *fn_val = args[1];
    size_t len = east_array_len(array);

    EastType *T = s_input_type ? s_input_type : &east_null_type;
    EastType *R = s_result_type ? s_result_type : &east_null_type;

    /* For small arrays, run sequentially (avoid thread overhead) */
    if (len <= 4) {
        EastValue *result = east_array_new(R);
        for (size_t i = 0; i < len; i++) {
            EastValue *item = east_array_get(array, i);
            east_value_retain(item);
            EastValue *call_args[] = { item };
            EvalResult r = east_call(fn_val->data.function.compiled, call_args, 1);
            east_value_release(item);
            if (r.status != EVAL_OK) {
                east_value_release(result);
                return r;
            }
            east_array_push(result, r.value);
            east_value_release(r.value);
            eval_result_free(&r);
        }
        return eval_ok(result);
    }

    /* Build types */
    EastType *fn_type = east_function_type(&T, 1, R);
    EastType *array_in_type = east_array_type(T);
    EastType *array_out_type = east_array_type(R);

    /* Encode the function once */
    ByteBuffer *fn_buf = east_beast2_encode(fn_val, fn_type);
    if (!fn_buf) {
        east_type_release(fn_type);
        east_type_release(array_in_type);
        east_type_release(array_out_type);
        return eval_error("Failed to encode function for parallel_map");
    }

    /* Determine number of workers */
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus < 1) ncpus = 1;
    size_t num_workers = (size_t)ncpus;
    if (num_workers > len) num_workers = len;

    /* Get current context for workers */
    PlatformRegistry *platform = east_current_platform();
    BuiltinRegistry *builtins = east_current_builtins();

    /* Split array into chunks and encode each */
    size_t chunk_size = (len + num_workers - 1) / num_workers;
    WorkerData *workers = calloc(num_workers, sizeof(WorkerData));
    pthread_t *threads = calloc(num_workers, sizeof(pthread_t));
    size_t actual_workers = 0;
    char *error = NULL;

    for (size_t w = 0; w < num_workers && !error; w++) {
        size_t start = w * chunk_size;
        if (start >= len) break;
        size_t end = start + chunk_size;
        if (end > len) end = len;
        size_t clen = end - start;

        /* Build chunk array */
        EastValue *chunk = east_array_new(T);
        for (size_t i = 0; i < clen; i++) {
            EastValue *item = east_array_get(array, start + i);
            east_value_retain(item);
            east_array_push(chunk, item);
            east_value_release(item);
        }

        /* Encode chunk */
        ByteBuffer *chunk_buf = east_beast2_encode(chunk, array_in_type);
        east_value_release(chunk);
        if (!chunk_buf) {
            error = strdup("Failed to encode chunk for parallel_map");
            break;
        }

        workers[w].fn_bytes = fn_buf->data;
        workers[w].fn_bytes_len = fn_buf->len;
        workers[w].chunk_bytes = chunk_buf->data;
        workers[w].chunk_bytes_len = chunk_buf->len;
        workers[w].fn_type = fn_type;
        workers[w].array_in_type = array_in_type;
        workers[w].array_out_type = array_out_type;
        workers[w].elem_out_type = R;
        workers[w].platform = platform;
        workers[w].builtins = builtins;
        workers[w].result_bytes = NULL;
        workers[w].result_bytes_len = 0;
        workers[w].error_message = NULL;

        /* Transfer chunk_buf data ownership to worker struct for later cleanup */
        chunk_buf->data = NULL;
        byte_buffer_free(chunk_buf);

        actual_workers = w + 1;
    }

    /* Spawn threads */
    size_t spawned = 0;
    if (!error) {
        for (size_t w = 0; w < actual_workers; w++) {
            if (pthread_create(&threads[w], NULL, worker_thread, &workers[w]) != 0) {
                error = strdup("Failed to create worker thread");
                break;
            }
            spawned = w + 1;
        }
    }

    /* Join all spawned threads */
    for (size_t w = 0; w < spawned; w++) {
        pthread_join(threads[w], NULL);
    }

    /* Check for worker errors */
    if (!error) {
        for (size_t w = 0; w < actual_workers; w++) {
            if (workers[w].error_message) {
                error = workers[w].error_message;
                workers[w].error_message = NULL;
                break;
            }
        }
    }

    /* Collect results */
    EastValue *result = NULL;
    if (!error) {
        result = east_array_new(R);
        for (size_t w = 0; w < actual_workers; w++) {
            EastValue *chunk_result = east_beast2_decode(
                workers[w].result_bytes, workers[w].result_bytes_len,
                array_out_type);
            if (!chunk_result) {
                error = strdup("Failed to decode worker results");
                east_value_release(result);
                result = NULL;
                break;
            }
            size_t clen = east_array_len(chunk_result);
            for (size_t i = 0; i < clen; i++) {
                EastValue *item = east_array_get(chunk_result, i);
                east_value_retain(item);
                east_array_push(result, item);
                east_value_release(item);
            }
            east_value_release(chunk_result);
        }
    }

    /* Cleanup */
    for (size_t w = 0; w < actual_workers; w++) {
        free((void *)workers[w].chunk_bytes);
        free(workers[w].result_bytes);
        free(workers[w].error_message);
    }
    free(workers);
    free(threads);
    byte_buffer_free(fn_buf);
    east_type_release(fn_type);
    east_type_release(array_in_type);
    east_type_release(array_out_type);

    if (error) {
        EvalResult err = eval_error(error);
        free(error);
        return err;
    }

    return eval_ok(result);
}

void east_std_register_parallel(PlatformRegistry *reg) {
    platform_registry_add_generic(reg, "parallel_map", parallel_map_factory, true);
}
