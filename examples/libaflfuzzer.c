/* An in mmeory fuzzing example. Fuzzer for libpng library */

#include <stdio.h>
#include <signal.h>
#include "aflpp.h"

extern u8 *__afl_area_ptr;

bool got_sigchld = false;

int                       LLVMFuzzerTestOneInput(const uint8_t *, size_t);
__attribute__((weak)) int LLVMFuzzerInitialize(int *argc, char ***argv);

void sigchld_handler(int signal) {

  (void)(signal);
  got_sigchld = true;

}

afl_exit_t exec_debug(afl_executor_t *executor, uint8_t *data, size_t size) {

  (void)executor;
  u32 i;
  fprintf(stderr, "Enter harness function %p %lu\n", data, size);

  for (i = 0; i < MAP_SIZE; i++)
    if (__afl_area_ptr[i])
      fprintf(stderr, "Error: map unclean before harness: map[%04x]=0x%02x\n", i, __afl_area_ptr[i]);

  int ret = LLVMFuzzerTestOneInput(data, size);

  fprintf(stderr, "MAP:");
  for (i = 0; i < 65536; i++)
    if (__afl_area_ptr[i]) fprintf(stderr, " map[%04x]=0x%02x", i, __afl_area_ptr[i]);
  fprintf(stderr, "\n");

  return ret;

}

int exec(afl_executor_t *executor, const uint8_t *data, size_t size) {

  (void)executor;
  return LLVMFuzzerTestOneInput(data, size);

}

static afl_ret_t in_memory_fuzzer_start(afl_executor_t *executor) {

  in_memory_executor_t *in_memory_fuzzer = (in_memory_executor_t *)executor;

  /* TODO: Does the return code mean anything? */
  if (LLVMFuzzerInitialize) { LLVMFuzzerInitialize(&in_memory_fuzzer->argc, &in_memory_fuzzer->argv); }

  return AFL_RET_SUCCESS;

}

afl_engine_t *initialize_fuzzer(int argc, char **argv, char *in_dir, char *queue_dirpath) {

  /* Let's create an in-memory executor */
  in_memory_executor_t *in_memory_executor = calloc(1, sizeof(in_memory_executor_t));
  if (!in_memory_executor) {

    FATAL("Error initi%s", afl_ret_stringify(AFL_RET_ALLOC));
    exit(-1);

  }

  in_memory_executor_init(in_memory_executor, exec_debug);

  in_memory_executor->argc = argc;
  in_memory_executor->argv = afl_argv_cpy_dup(argc, argv);
  if (!in_memory_executor->argv) { FATAL("Error allocating argv"); }
  in_memory_executor->base.funcs.init_cb = in_memory_fuzzer_start;

  /* Observation channel, map based, we initialize this ourselves since we don't
   * actually create a shared map */
  afl_observer_covmap_t *trace_bits_channel = afl_observer_covmap_new(MAP_SIZE);
  if (!trace_bits_channel) { PFATAL("Trace bits channel error"); }

  /* Since we don't use afl_observer_covmap_new function, we have to add reset
   * function manually */
  trace_bits_channel->base.funcs.reset = afl_observer_covmap_reset;
  trace_bits_channel->shared_map.map = __afl_area_ptr;  // Coverage map
  trace_bits_channel->shared_map.map_size = MAP_SIZE;
  trace_bits_channel->shared_map.shm_id = -1;  // Just a simple erronous value :)
  in_memory_executor->base.funcs.observer_add(&in_memory_executor->base, &trace_bits_channel->base);

  /* We create a simple feedback queue for coverage here*/
  afl_queue_feedback_t *coverage_feedback_queue = afl_queue_feedback_new(NULL, (char *)"Coverage feedback queue");
  if (!coverage_feedback_queue) {

    FATAL("Error initializing feedback queue");
    exit(-1);

  }

  coverage_feedback_queue->base.funcs.set_dirpath(&coverage_feedback_queue->base, queue_dirpath);

  /* Global queue creation */
  afl_queue_global_t *global_queue = afl_queue_global_new();
  if (!global_queue) {

    FATAL("Error initializing global queue");
    exit(-1);

  }

  global_queue->funcs.add_feedback_queue(global_queue, coverage_feedback_queue);
  global_queue->base.funcs.set_dirpath(&global_queue->base, queue_dirpath);

  /* Coverage Feedback initialization */
  afl_feedback_cov_t *coverage_feedback = afl_feedback_cov_new(coverage_feedback_queue, trace_bits_channel);
  if (!coverage_feedback) {

    FATAL("Error initializing feedback");
    exit(-1);

  }

  /* Let's build an engine now */
  afl_engine_t *engine = afl_engine_new(&in_memory_executor->base, NULL, global_queue);
  if (!engine) {

    FATAL("Error initializing Engine");
    exit(-1);

  }

  engine->funcs.add_feedback(engine, &coverage_feedback->base);
  engine->funcs.set_global_queue(engine, global_queue);
  engine->in_dir = in_dir;

  afl_fuzz_one_t *fuzz_one = afl_fuzz_one_new(engine);
  if (!fuzz_one) {

    FATAL("Error initializing fuzz_one");
    exit(-1);

  }

  // We also add the fuzzone to the engine here.
  engine->funcs.set_fuzz_one(engine, fuzz_one);

  afl_mutator_scheduled_t *mutators_havoc = afl_mutator_scheduled_new(NULL, 8);
  if (!mutators_havoc) {

    FATAL("Error initializing Mutators");
    exit(-1);

  }

  mutators_havoc->funcs.add_func(mutators_havoc, afl_mutfunc_flip_byte);
  mutators_havoc->funcs.add_func(mutators_havoc, afl_mutfunc_flip_2_bytes);
  mutators_havoc->funcs.add_func(mutators_havoc, afl_mutfunc_flip_4_bytes);
  mutators_havoc->funcs.add_func(mutators_havoc, afl_mutfunc_delete_bytes);
  mutators_havoc->funcs.add_func(mutators_havoc, afl_mutfunc_clone_bytes);
  mutators_havoc->funcs.add_func(mutators_havoc, afl_mutfunc_flip_bit);
  mutators_havoc->funcs.add_func(mutators_havoc, afl_mutfunc_flip_2_bits);
  mutators_havoc->funcs.add_func(mutators_havoc, afl_mutfunc_flip_4_bits);
  mutators_havoc->funcs.add_func(mutators_havoc, afl_mutfunc_random_byte_add_sub);
  mutators_havoc->funcs.add_func(mutators_havoc, afl_mutfunc_random_byte);

  afl_fuzzing_stage_t *stage = afl_fuzzing_stage_new(engine);
  if (!stage) {

    FATAL("Error creating fuzzing stage");
    exit(-1);

  }

  stage->funcs.add_mutator_to_stage(stage, &mutators_havoc->base);

  return engine;

}

void run_instance(llmp_client_t *llmp_client, void *data) {

  afl_engine_t *engine = (afl_engine_t *)data;
  engine->llmp_client = llmp_client;

  afl_observer_covmap_t *trace_bits_channel = (afl_observer_covmap_t *)engine->executor->observors[0];

  afl_fuzzing_stage_t *    stage = (afl_fuzzing_stage_t *)engine->fuzz_one->stages[0];
  afl_mutator_scheduled_t *mutators_havoc = (afl_mutator_scheduled_t *)stage->mutators[0];

  afl_feedback_cov_t *coverage_feedback = (afl_feedback_cov_t *)(engine->feedbacks[0]);

  /* Now we can simply load the testcases from the directory given */
  afl_ret_t ret = engine->funcs.load_testcases_from_dir(engine, engine->in_dir, NULL);
  if (ret != AFL_RET_SUCCESS) {

    PFATAL("Error loading testcase dir: %s", afl_ret_stringify(ret));
    exit(-1);

  }

  afl_ret_t fuzz_ret = engine->funcs.loop(engine);

  if (fuzz_ret != AFL_RET_SUCCESS) {

    PFATAL("Error fuzzing the target: %s", afl_ret_stringify(fuzz_ret));
    exit(-1);

  }

  SAYF("Fuzzing ends with all the queue entries fuzzed. No of executions %llu\n", engine->executions);

  /* Let's free everything now. Note that if you've extended any structure,
   * which now contains pointers to any dynamically allocated region, you have
   * to free them yourselves, but the extended structure itself can be de
   * initialized using the deleted functions provided */

  afl_executor_delete(engine->executor);
  afl_observer_covmap_delete(trace_bits_channel);
  afl_mutator_scheduled_delete(mutators_havoc);
  afl_fuzzing_stage_delete(stage);
  afl_fuzz_one_delete(engine->fuzz_one);
  afl_feedback_cov_delete(coverage_feedback);
  for (size_t i = 0; i < engine->feedbacks_count; ++i) {

    afl_feedback_delete((afl_feedback_t *)engine->feedbacks[i]);

  }

  for (size_t i = 0; i < engine->global_queue->feedback_queues_count; ++i) {

    afl_queue_feedback_delete(engine->global_queue->feedback_queues[i]);

  }

  afl_queue_global_delete(engine->global_queue);
  afl_engine_delete(engine);

}

int main(int argc, char **argv) {

  if (argc < 4) {

    FATAL(
        "Usage: %s number_of_threads /path/to/input/dir "
        "/path/to/queue/dir",
        argv[0]);
    exit(-1);

  }

  bool debug = false;
  if (getenv("DEBUG") || getenv("AFL_DEBUG") || getenv("LIBAFL_DEBUG")) {

    debug = true;
    fprintf(stderr, "Map ptr: %p\n", __afl_area_ptr);

  }

  int   pid, i;
  char *in_dir = argv[2];
  int   client_count = atoi(argv[1]);
  char *queue_dirpath = argv[3];

  if (client_count <= 0) {

    FATAL("Number of threads should be greater than 0");
    exit(-1);

  }

  int            broker_port = 0xAF1;
  llmp_broker_t *llmp_broker = llmp_broker_new();
  if (!llmp_broker) {

    FATAL("Broker creation failed");
    exit(-1);

  }

  if (!llmp_broker_register_local_server(llmp_broker, broker_port)) {

    FATAL("Broker register on port %d/tcp failed", broker_port);
    exit(-1);

  }

  OKF("Broker created.");

  (void)signal(SIGCHLD, sigchld_handler);
  u32 clients_started = 0;
  // u64 time_elapsed = 1;

  while (1) {

    for (i = clients_started; i < client_count; i++) {

      if ((pid = fork()) < 0) {

        PFATAL("fork failed.");
        exit(-1);

      }

      if (!pid) {  // child

        llmp_client_t *llmp_client = llmp_client_new(broker_port);

        if (!llmp_client) {

          FATAL("Error registering client");
          exit(-1);

        }

        if (!debug) {

          s32 dev_null_fd = open("/dev/null", O_WRONLY);
          dup2(dev_null_fd, 2);
          dup2(dev_null_fd, 1);
          dup2(dev_null_fd, 0);

        }

        afl_engine_t *engine = initialize_fuzzer(argc, argv, in_dir, queue_dirpath);

        run_instance(llmp_client, engine);

        llmp_client_delete(llmp_client);

        exit(0);

      }

    }

    volatile bool loop = 1;
    while (loop) {

      sleep(1);
      /*
            u64 execs = 0;
            u64 crashes = 0;
            for (i = 0; i < fuzz_workers_count; ++i) {

              execs += registered_fuzz_workers[i]->executions;
              crashes += registered_fuzz_workers[i]->crashes;

            }

            u64 paths =
         registered_fuzz_workers[0]->global_queue->feedback_queues_count;

            SAYF("execs=%llu  execs/s=%llu  paths=%llu  crashes=%llu
         elapsed=%llu\r", execs, execs / time_elapsed, paths, crashes,
         time_elapsed); time_elapsed++; fflush(0);
      */

    }

    clients_started--;  // sigchild makes us exit the loop

  }

  return 0;

}

