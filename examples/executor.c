#define AFL_MAIN

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <dirent.h>
#include <time.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>

#include <sys/wait.h>
#include <sys/time.h>
#ifndef USEMMAP
  #include <sys/shm.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>

#include "config.h"
#include "types.h"
#include "debug.h"
#include "xxh3.h"
#include "alloc-inl.h"
#include "aflpp.h"

#define SUPER_INTERESTING 0.5
#define VERY_INTERESTING 0.4
#define INTERESTING 0.3
#define MAP_CHANNEL_ID 0x1
#define TIMEOUT_CHANNEL_ID 0x2


/* We implement a simple map maximising feedback here. */
typedef struct maximize_map_feedback {

  feedback_t base;

  u8 *   virgin_bits;
  size_t size;

} maximize_map_feedback_t;

typedef struct timeout_obs_channel {

  observation_channel_t base;

  u32 last_run_time;
  u32 avg_exec_time;

} timeout_obs_channel_t;

typedef struct thread_instance_args {

  engine_t *           engine;
  char *               in_dir;
  llmp_client_state_t *client;

} thread_instance_args_t;


llmp_broker_state_t *llmp_broker;
int                  broker_port;
/* Functions related to the feedback defined above */
// static float coverage_fbck_is_interesting(feedback_t *feedback,
//                                           executor_t *fsrv);
static maximize_map_feedback_t *map_feedback_init(feedback_queue_t *queue,
                                                  size_t            size);
static float __attribute__((hot))
coverage_fbck_is_interesting(feedback_t *feedback, executor_t *fsrv);


/* Execute target application, monitoring for timeouts. Return status
   information. The called program will update afl->fsrv->trace_bits. */
static exit_type_t fsrv_run_target_custom(executor_t *fsrv_executor) {

  afl_forkserver_t *fsrv = (afl_forkserver_t *)fsrv_executor;

  s32 res;
  u32 exec_ms;
  u32 write_value = fsrv->last_run_timed_out;

  /* After this memset, fsrv->trace_bits[] are effectively volatile, so we
     must prevent any earlier operations from venturing into that
     territory. */

  memset(fsrv->trace_bits, 0, fsrv->map_size);

  MEM_BARRIER();

  /* we have the fork server (or faux server) up and running
  First, tell it if the previous run timed out. */

  if ((res = write(fsrv->fsrv_ctl_fd, &write_value, 4)) != 4) {

    RPFATAL(res, "Unable to request new process from fork server (OOM?)");

  }

  fsrv->last_run_timed_out = 0;

  if ((res = read(fsrv->fsrv_st_fd, &fsrv->child_pid, 4)) != 4) {

    RPFATAL(res, "Unable to request new process from fork server (OOM?)");

  }

  if (fsrv->child_pid <= 0) { FATAL("Fork server is misbehaving (OOM?)"); }

  exec_ms =
      read_s32_timed(fsrv->fsrv_st_fd, &fsrv->child_status, fsrv->exec_tmout);

  /* Update the timeout observation channel */
  timeout_obs_channel_t *timeout_channel =
      (timeout_obs_channel_t *)fsrv->base.funcs.get_observation_channels(
          &fsrv->base, 1);
  timeout_channel->last_run_time = exec_ms;

  if (exec_ms > fsrv->exec_tmout) {

    /* If there was no response from forkserver after timeout seconds,
    we kill the child. The forkserver should inform us afterwards */

    kill(fsrv->child_pid, SIGKILL);
    fsrv->last_run_timed_out = 1;
    if (read(fsrv->fsrv_st_fd, &fsrv->child_status, 4) < 4) { exec_ms = 0; }

  }

  if (!exec_ms) {}

  if (!WIFSTOPPED(fsrv->child_status)) { fsrv->child_pid = 0; }

  fsrv->total_execs++;
  if (!fsrv->use_stdin) { unlink(fsrv->out_file); }

  /* Any subsequent operations on fsrv->trace_bits must not be moved by the
     compiler below this point. Past this location, fsrv->trace_bits[]
     behave very normally and do not have to be treated as volatile. */

  MEM_BARRIER();

  /* Report outcome to caller. */

  if (WIFSIGNALED(fsrv->child_status)) {

    fsrv->last_kill_signal = WTERMSIG(fsrv->child_status);

    if (fsrv->last_run_timed_out && fsrv->last_kill_signal == SIGKILL) {

      return TIMEOUT;

    }

    return CRASH;

  }

  return NORMAL;

}

/* Init function for the feedback */
static maximize_map_feedback_t *map_feedback_init(feedback_queue_t *queue,
                                                  size_t            size) {

  maximize_map_feedback_t *feedback =
      calloc(1, sizeof(maximize_map_feedback_t));
  if (!feedback) { return NULL; }
  afl_feedback_init(&feedback->base, queue);
  feedback->base.funcs.is_interesting = coverage_fbck_is_interesting;

  feedback->virgin_bits = calloc(1, size);
  if (!feedback->virgin_bits) {

    free(feedback);
    return NULL;

  }

  feedback->size = size;

  return feedback;

}

void timeout_channel_reset(observation_channel_t *obs_channel) {

  timeout_obs_channel_t *timeout_channel = (timeout_obs_channel_t *)obs_channel;

  timeout_channel->last_run_time = 0;

}

void timeout_channel_post_exec(observation_channel_t *obs_channel,
                               engine_t *             engine) {

  timeout_obs_channel_t *timeout_channel = (timeout_obs_channel_t *)obs_channel;

  timeout_channel->avg_exec_time =
      (timeout_channel->avg_exec_time + timeout_channel->last_run_time) /
      (engine->executions);

}

/* We'll implement a simple is_interesting function for the feedback, which
 * checks if new tuples have been hit in the map or hit count has increased*/

static float __attribute__((hot))
coverage_fbck_is_interesting(feedback_t *feedback, executor_t *fsrv) {

  maximize_map_feedback_t *map_feedback = (maximize_map_feedback_t *)feedback;

  /* First get the observation channel */

  if (feedback->observation_idx == -1) {

    for (size_t i = 0; i < fsrv->observors_num; ++i) {

      if (fsrv->observors[i]->channel_id == MAP_CHANNEL_ID) {

        feedback->observation_idx = i;
        break;

      }

    }

  }

  map_based_channel_t *obs_channel =
      (map_based_channel_t *)fsrv->funcs.get_observation_channels(
          fsrv, feedback->observation_idx);

#ifdef WORD_SIZE_64

  u64 *current = (u64 *)obs_channel->shared_map.map;
  u64 *virgin = (u64 *)map_feedback->virgin_bits;

  u32 i = (obs_channel->shared_map.map_size >> 3);

#else

  u32 *current = (u32 *)obs_channel->map.;
  u32 *virgin = (u32 *)virgin_map;

  u32 i = (obs_channel->shared_map.map_size >> 2);

#endif                                                     /* ^WORD_SIZE_64 */
  // the map size must be a minimum of 8 bytes.
  // for variable/dynamic map sizes this is ensured in the forkserver

  float ret = 0.0;

  while (i--) {

    /* Optimize for (*current & *virgin) == 0 - i.e., no bits in current bitmap
       that have not been already cleared from the virgin map - since this will
       almost always be the case. */

    // the (*current) is unnecessary but speeds up the overall comparison
    if (unlikely(*current) && unlikely(*current & *virgin)) {

      if (likely(ret < 2)) {

        u8 *cur = (u8 *)current;
        u8 *vir = (u8 *)virgin;

        /* Looks like we have not found any new bytes yet; see if any non-zero
           bytes in current[] are pristine in virgin[]. */

#ifdef WORD_SIZE_64

        if (*virgin == 0xffffffffffffffff || (cur[0] && vir[0] == 0xff) ||
            (cur[1] && vir[1] == 0xff) || (cur[2] && vir[2] == 0xff) ||
            (cur[3] && vir[3] == 0xff) || (cur[4] && vir[4] == 0xff) ||
            (cur[5] && vir[5] == 0xff) || (cur[6] && vir[6] == 0xff) ||
            (cur[7] && vir[7] == 0xff)) {

          ret = 1.0;

        } else {

          ret = 0.5;

        }

#else

        if (*virgin == 0xffffffff || (cur[0] && vir[0] == 0xff) ||
            (cur[1] && vir[1] == 0xff) || (cur[2] && vir[2] == 0xff) ||
            (cur[3] && vir[3] == 0xff))
          ret = 1.0;
        else
          ret = 0.5;

#endif                                                     /* ^WORD_SIZE_64 */

      }

      *virgin &= ~*current;

    }

    ++current;
    ++virgin;

  }

  if (((ret == 0.5) || (ret == 1.0)) && feedback->queue) {

    raw_input_t *input = fsrv->current_input->funcs.copy(fsrv->current_input);

    if (!input) { FATAL("Error creating a copy of input"); }

    queue_entry_t *new_entry = afl_queue_entry_create(input);
    // An incompatible ptr type warning has been suppresed here. We pass the
    // feedback queue to the add_to_queue rather than the base_queue
    feedback->queue->base.funcs.add_to_queue(&feedback->queue->base, new_entry);

    // Put the entry in the feedback queue and return 0.0 so that it isn't added
    // to the global queue too
    return 0.0;

  }

  return ret;

}

/* Another feedback based on the exec time */

static float timeout_fbck_is_interesting(feedback_t *feedback,
                                         executor_t *executor) {

  afl_forkserver_t *fsrv = (afl_forkserver_t *)executor;
  u32               exec_timeout = fsrv->exec_tmout;

  // We find the related observation channel here
  if (feedback->observation_idx == -1) {

    for (size_t i = 0; i < executor->observors_num; ++i) {

      if (executor->observors[i]->channel_id == TIMEOUT_CHANNEL_ID) {

        feedback->observation_idx = i;
        break;

      }

    }

  }

  timeout_obs_channel_t *timeout_channel =
      (timeout_obs_channel_t *)fsrv->base.funcs.get_observation_channels(
          &fsrv->base, 1);

  u32 last_run_time = timeout_channel->last_run_time;

  if (last_run_time == exec_timeout) {

    raw_input_t *input =
        fsrv->base.current_input->funcs.copy(fsrv->base.current_input);
    if (!input) { FATAL("Error creating a copy of input"); }

    queue_entry_t *new_entry = afl_queue_entry_create(input);
    feedback->queue->base.funcs.add_to_queue(&feedback->queue->base, new_entry);
    return 0.0;

  }

  else {

    return 0.0;

  }

}

engine_t *initialize_engine_instance(char *target_path, char *in_dir,
                                     char **target_args) {

  /* Let's now create a simple map-based observation channel */
  map_based_channel_t *trace_bits_channel =
      afl_map_channel_create(MAP_SIZE, MAP_CHANNEL_ID);

  /* Another timing based observation channel */
  timeout_obs_channel_t *timeout_channel =
      calloc(1, sizeof(timeout_obs_channel_t));
  if (!timeout_channel) { FATAL("Error initializing observation channel"); }
  afl_observation_channel_init(&timeout_channel->base, TIMEOUT_CHANNEL_ID);
  timeout_channel->base.funcs.post_exec = timeout_channel_post_exec;
  timeout_channel->base.funcs.reset = timeout_channel_reset;

  /* We initialize the forkserver we want to use here. */
  // (void)out_file;
  char *output_file = calloc(50, 1);
  /* This rand is not good, we will want to use afl_rand_... functions
  after the engine is created */
  snprintf(output_file, 50, "out-%d", rand());
  afl_forkserver_t *fsrv = fsrv_init(target_path, target_args);
  fsrv->base.funcs.run_target_cb = fsrv_run_target_custom;
  if (!fsrv) { FATAL("Could not initialize forkserver!"); }
  fsrv->exec_tmout = 10000;
  fsrv->target_args = target_args;

  fsrv->base.funcs.add_observation_channel(&fsrv->base,
                                           &trace_bits_channel->base);
  fsrv->base.funcs.add_observation_channel(&fsrv->base, &timeout_channel->base);

  char shm_str[256];
  snprintf(shm_str, sizeof(shm_str), "%d",
           trace_bits_channel->shared_map.shm_id);
  setenv("__AFL_SHM_ID", (char *)shm_str, 1);
  fsrv->trace_bits = trace_bits_channel->shared_map.map;

  /* We create a simple feedback queue for coverage here*/
  feedback_queue_t *coverage_feedback_queue =
      afl_feedback_queue_create(NULL, (char *)"Coverage feedback queue");
  if (!coverage_feedback_queue) { FATAL("Error initializing feedback queue"); }

  /* Another feedback queue for timeout entries here */
  feedback_queue_t *timeout_feedback_queue =
      afl_feedback_queue_create(NULL, "Timeout feedback queue");
  if (!timeout_feedback_queue) { FATAL("Error initializing feedback queue"); }

  /* Global queue creation */
  global_queue_t *global_queue = afl_global_queue_create();
  if (!global_queue) { FATAL("Error initializing global queue"); }
  global_queue->extra_funcs.add_feedback_queue(global_queue,
                                               coverage_feedback_queue);
  global_queue->extra_funcs.add_feedback_queue(global_queue,
                                               timeout_feedback_queue);

  /* Coverage Feedback initialization */
  maximize_map_feedback_t *coverage_feedback = map_feedback_init(
      coverage_feedback_queue, trace_bits_channel->shared_map.map_size);
  if (!coverage_feedback) { FATAL("Error initializing feedback"); }
  coverage_feedback_queue->feedback = &coverage_feedback->base;

  /* Timeout Feedback initialization */
  feedback_t *timeout_feedback = afl_feedback_create(timeout_feedback_queue);
  if (!timeout_feedback) { FATAL("Error initializing feedback"); }
  timeout_feedback_queue->feedback = timeout_feedback;
  timeout_feedback->funcs.is_interesting = timeout_fbck_is_interesting;

  /* Let's build an engine now */
  engine_t *engine = afl_engine_create((executor_t *)fsrv, NULL, global_queue);
  engine->in_dir = in_dir;
  if (!engine) { FATAL("Error initializing Engine"); }
  engine->funcs.add_feedback(engine, (feedback_t *)coverage_feedback);
  engine->funcs.add_feedback(engine, timeout_feedback);

  fuzz_one_t *fuzz_one = afl_fuzz_one_create(engine);
  if (!fuzz_one) { FATAL("Error initializing fuzz_one"); }

  scheduled_mutator_t *mutators_havoc = afl_scheduled_mutator_create(NULL, 8);
  if (!mutators_havoc) { FATAL("Error initializing Mutators"); }

  mutators_havoc->extra_funcs.add_mutator(mutators_havoc, flip_byte_mutation);
  mutators_havoc->extra_funcs.add_mutator(mutators_havoc,
                                          flip_2_bytes_mutation);
  mutators_havoc->extra_funcs.add_mutator(mutators_havoc,
                                          flip_4_bytes_mutation);
  mutators_havoc->extra_funcs.add_mutator(mutators_havoc,
                                          delete_bytes_mutation);
  mutators_havoc->extra_funcs.add_mutator(mutators_havoc, clone_bytes_mutation);
  mutators_havoc->extra_funcs.add_mutator(mutators_havoc, flip_bit_mutation);
  mutators_havoc->extra_funcs.add_mutator(mutators_havoc, flip_2_bits_mutation);
  mutators_havoc->extra_funcs.add_mutator(mutators_havoc, flip_4_bits_mutation);
  mutators_havoc->extra_funcs.add_mutator(mutators_havoc,
                                          random_byte_add_sub_mutation);
  mutators_havoc->extra_funcs.add_mutator(mutators_havoc, random_byte_mutation);
  mutators_havoc->extra_funcs.add_mutator(mutators_havoc, splicing_mutation);

  fuzzing_stage_t *stage = afl_fuzzing_stage_create(engine);
  if (!stage) { FATAL("Error creating fuzzing stage"); }
  stage->funcs.add_mutator_to_stage(stage, &mutators_havoc->base);

  return engine;

}

void thread_run_instance(llmp_client_state_t *client, void *data) {

  engine_t *engine = (engine_t *)data;

  engine->llmp_client = client;

  afl_forkserver_t *   fsrv = (afl_forkserver_t *)engine->executor;
  map_based_channel_t *trace_bits_channel =
      (map_based_channel_t *)fsrv->base.observors[0];
  timeout_obs_channel_t *timeout_channel =
      (timeout_obs_channel_t *)fsrv->base.observors[1];

  fuzzing_stage_t *    stage = (fuzzing_stage_t *)engine->fuzz_one->stages[0];
  scheduled_mutator_t *mutators_havoc =
      (scheduled_mutator_t *)stage->mutators[0];

  maximize_map_feedback_t *coverage_feedback =
      (maximize_map_feedback_t *)(engine->feedbacks[0]);

  /* Let's reduce the timeout initially to fill the queue */
  fsrv->exec_tmout = 20;
  /* Now we can simply load the testcases from the directory given */
  afl_ret_t ret =
      engine->funcs.load_testcases_from_dir(engine, engine->in_dir, NULL);
  if (ret != AFL_RET_SUCCESS) {

    PFATAL("Error loading testcase dir: %s", afl_ret_stringify(ret));

  }

  OKF("Processed %llu input files.", engine->executions);

  afl_ret_t fuzz_ret = engine->funcs.loop(engine);

  if (fuzz_ret != AFL_RET_SUCCESS) {

    PFATAL("Error fuzzing the target: %s", afl_ret_stringify(fuzz_ret));

  }

  SAYF(
      "Fuzzing ends with all the queue entries fuzzed. No of executions %llu\n",
      engine->executions);

  /* Let's free everything now. Note that if you've extended any structure,
   * which now contains pointers to any dynamically allocated region, you have
   * to free them yourselves, but the extended structure itself can be de
   * initialized using the deleted functions provided */

  afl_executor_delete(&fsrv->base);
  afl_map_channel_delete(trace_bits_channel);
  afl_observation_channel_delete(&timeout_channel->base);
  afl_scheduled_mutator_delete(mutators_havoc);
  afl_fuzz_stage_delete(stage);
  afl_fuzz_one_delete(engine->fuzz_one);
  free(coverage_feedback->virgin_bits);
  for (size_t i = 0; i < engine->feedbacks_num; ++i) {

    afl_feedback_delete((feedback_t *)engine->feedbacks[i]);

  }

  for (size_t i = 0; i < engine->global_queue->feedback_queues_num; ++i) {

    afl_feedback_queue_delete(engine->global_queue->feedback_queues[i]);

  }

  afl_global_queue_delete(engine->global_queue);
  afl_engine_delete(engine);
  return;

}

void *run_broker_thread(void *data) {

  (void)data;
  llmp_broker_run(llmp_broker);
  return 0;

}

/* Main entry point function */
int main(int argc, char **argv) {

  if (argc < 4) {

    FATAL(
        "Usage: ./executor /input/directory number_of_threads "
        "target [target_args]");

  }

  char *in_dir = argv[1];
  char *target_path = argv[3];
  int   thread_count = atoi(argv[2]);

  if (thread_count <= 0) {

    FATAL("Number of threads should be greater than 0");

  }

  // Time for llmp POC :)
  broker_port = 0XAF1;
  llmp_broker = llmp_broker_new();
  if (!llmp_broker) { FATAL("Broker creation failed"); }
  if (!llmp_broker_register_local_server(llmp_broker, broker_port)) {

    FATAL("Broker register failed");

  }

  OKF("Broker created now");

  for (int i = 0; i < thread_count; ++i) {

    char **target_args = afl_argv_cpy_dup(argc, argv);

    engine_t *engine =
        initialize_engine_instance(target_path, in_dir, target_args);

    if (!llmp_broker_register_threaded_clientloop(
            llmp_broker, thread_run_instance, engine)) {

      FATAL("Error registering client");

    };

    if (afl_register_fuzz_worker(engine) != AFL_RET_SUCCESS) {

      FATAL("Error registering fuzzing instance");

    }

  }

  u64 time_elapsed = 1;

  pthread_t p1;

  int s = pthread_create(&p1, NULL, run_broker_thread, NULL);

  if (!s) { OKF("Broker started running"); }

  while (true) {

    sleep(1);
    u64 execs = 0;
    u64 crashes = 0;
    for (size_t i = 0; i < fuzz_workers_count; ++i) {

      execs += registered_fuzz_workers[i]->executions;
      crashes += registered_fuzz_workers[i]->crashes;

    }

    SAYF(
        "Execs: %8llu\tCrashes: %4llu\tExecs per second: %5llu  time elapsed: "
        "%8llu\r",
        execs, crashes, execs / time_elapsed, time_elapsed);
    time_elapsed++;
    fflush(0);

  }

}

