/*
   american fuzzy lop++ - fuzzer header
   ------------------------------------

   Originally written by Michal Zalewski

   Now maintained by Marc Heuse <mh@mh-sec.de>,
                     Heiko Eißfeldt <heiko.eissfeldt@hexco.de>,
                     Andrea Fioraldi <andreafioraldi@gmail.com>,
                     Dominik Maier <mail@dmnk.co>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2020 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This is the Library based on AFL++ which can be used to build
   customized fuzzers for a specific target while taking advantage of
   a lot of features that AFL++ already provides.

 */

#ifndef LIBQUEUE_H
#define LIBQUEUE_H

#include <stdbool.h>
#include <limits.h>

#include "input.h"
#include "shmem.h"
#include "feedback.h"

/*
This is the generic interface implementation for the queue and queue entries.
We've tried to keep it generic and yet including, but if you want to extend the
queue/entry, simply "inherit" this struct by including it in your custom struct
and keeping it as the first member of your struct.
*/

typedef struct afl_queue    afl_queue_t;
typedef struct afl_entry    afl_entry_t;
typedef struct afl_feedback afl_feedback_t;

struct afl_entry_funcs {

  afl_input_t *(*get_input)(afl_entry_t *);
  bool (*is_on_disk)(afl_entry_t *);
  afl_entry_t *(*get_next)(afl_entry_t *);
  afl_entry_t *(*get_prev)(afl_entry_t *);
  afl_entry_t *(*get_parent)(afl_entry_t *);
  afl_entry_t *(*get_child)(afl_entry_t *, size_t);        /*TODO: Still need to add a base implementation for this.*/

};

typedef struct __attribute__((__packed__)) afl_entry_info {

  u64 hash, exec_us;
  u32 bytes_set, bits_set;
  u8  trimmed, has_new_coverage, variable, skip_entry;

} afl_entry_info_t;

struct afl_entry {

  afl_entry_info_t *info;
  afl_input_t *     input;
  u8 *              map;
  bool              on_disk, info_calloc;
  char              filename[FILENAME_LEN_MAX];
  struct afl_queue *queue;
  struct afl_entry *next;
  struct afl_entry *prev;
  struct afl_entry *parent;

  struct afl_entry_funcs funcs;

};

afl_ret_t afl_entry_init(afl_entry_t *, afl_input_t *, afl_entry_info_t *);
void      afl_entry_deinit(afl_entry_t *);

AFL_NEW_AND_DELETE_FOR_WITH_PARAMS(afl_entry, AFL_DECL_PARAMS(afl_input_t *input, afl_entry_info_t *info),
                                   AFL_CALL_PARAMS(input, info))
// AFL_NEW_AND_DELETE_FOR_WITH_PARAMS(afl_queue_feedback, AFL_DECL_PARAMS(afl_feedback_t *feedback, char *name),
//                                   AFL_CALL_PARAMS(feedback, name));

// Default implementations for the functions for queue_entry vtable
afl_input_t *afl_entry_get_input(afl_entry_t *entry);
afl_entry_t *afl_entry_get_next(afl_entry_t *entry);
afl_entry_t *afl_entry_get_prev(afl_entry_t *entry);
afl_entry_t *afl_entry_get_parent(afl_entry_t *entry);

typedef struct afl_queue afl_queue_t;

struct afl_queue_funcs {

  afl_ret_t (*insert)(afl_queue_t *, afl_entry_t *);
  void (*remove_from_queue)(afl_queue_t *);

  afl_entry_t *(*get)(afl_queue_t *);
  afl_entry_t *(*get_next_in_queue)(afl_queue_t *, int);
  afl_entry_t *(*get_queue_entry)(afl_queue_t *, u32);
  afl_entry_t *(*get_queue_base)(afl_queue_t *);
  size_t (*get_size)(afl_queue_t *);
  char *(*get_dirpath)(afl_queue_t *);
  size_t (*get_names_id)(afl_queue_t *);
  bool (*get_save_to_files)(afl_queue_t *);

  void (*set_dirpath)(afl_queue_t *, char *);
  void (*set_engine)(afl_queue_t *, afl_engine_t *);

};

struct afl_queue {

  afl_entry_t **         entries;
  size_t                 entries_count;
  afl_entry_t *          base;
  u64                    current;
  int                    engine_id;
  afl_engine_t *         engine;
  afl_entry_t *          end;
  char                   dirpath[PATH_MAX];
  size_t                 names_id;
  bool                   save_to_files;
  bool                   fuzz_started;
  struct afl_queue_funcs funcs;

};

/* TODO: Add the base  */

afl_ret_t afl_queue_init(afl_queue_t *);
void      afl_queue_deinit(afl_queue_t *);

afl_ret_t    afl_queue_insert(afl_queue_t *, afl_entry_t *);
size_t       afl_queue_get_size(afl_queue_t *);
char *       afl_queue_get_dirpath(afl_queue_t *);
size_t       afl_queue_get_names_id(afl_queue_t *);
bool         afl_queue_should_save_to_file(afl_queue_t *);
void         afl_queue_set_dirpath(afl_queue_t *, char *);
void         afl_queue_set_engine(afl_queue_t *queue, afl_engine_t *engine);
void         afl_queue_global_set_engine(afl_queue_t *, afl_engine_t *);
afl_entry_t *afl_queue_next_base_queue(afl_queue_t *queue, int engine_id);
afl_entry_t *afl_queue_get_entry(afl_queue_t *queue, u32 entry);

AFL_NEW_AND_DELETE_FOR(afl_queue)

typedef struct afl_queue_feedback {

  afl_queue_t base;  // Inheritence from base queue

  afl_feedback_t *feedback;
  char *          name;

} afl_queue_feedback_t;

afl_ret_t afl_queue_feedback_init(afl_queue_feedback_t *, afl_feedback_t *,
                                  char *);  // "constructor" for the above feedback queue

void afl_queue_feedback_deinit(afl_queue_feedback_t *);

AFL_NEW_AND_DELETE_FOR_WITH_PARAMS(afl_queue_feedback, AFL_DECL_PARAMS(afl_feedback_t *feedback, char *name),
                                   AFL_CALL_PARAMS(feedback, name));

typedef struct afl_queue_global afl_queue_global_t;

struct afl_queue_global_funcs {

  int (*schedule)(afl_queue_global_t *);
  afl_ret_t (*add_feedback_queue)(afl_queue_global_t *, afl_queue_feedback_t *);

};

struct afl_queue_global {

  afl_queue_t            base;
  afl_queue_feedback_t **feedback_queues;  // One global queue can have
                                           // multiple feedback queues

  size_t feedback_queues_count;

  struct afl_queue_global_funcs funcs;
  /*TODO: Add a map of Engine:feedback_queue
    UPDATE: Engine will have a ptr to current feedback queue rather than this*/

};

// Default implementations of global queue vtable functions
afl_ret_t afl_queue_global_add_feedback_queue(afl_queue_global_t *, afl_queue_feedback_t *);
int       afl_queue_global_schedule(afl_queue_global_t *);
void      afl_queue_global_set_engine(afl_queue_t *, afl_engine_t *);

// Function to get next entry from queue, we override the base_queue
// implementation
afl_entry_t *afl_queue_next_global_queue(afl_queue_t *queue, int engine_id);

/* Register this as global queue for the engine.
TODO: Make this a method of engine instead */
void afl_queue_global_set_engine(afl_queue_t *global_queue_base, afl_engine_t *engine);

/* TODO: ADD defualt implementation for the schedule function based on random.
 */

afl_ret_t afl_queue_global_init(afl_queue_global_t *);
void      afl_queue_global_deinit(afl_queue_global_t *);

AFL_NEW_AND_DELETE_FOR(afl_queue_global)

#endif

