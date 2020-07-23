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


 */

#ifndef MUTATOR_FILE_INCLUDED
#define MUTATOR_FILE_INCLUDED

#include "libinput.h"
#include "list.h"

// Mutator struct will have many internal functions like mutate, trimming etc.
// This is based on both the FFF prototype and the custom mutators that we have
// in AFL++ without the AFL++ specific parts

typedef struct mutator mutator_t;

struct mutator_functions {

  void (*init)(mutator_t *);  // Sort of like the afl_custom_init we have for
                              // custom mutators?

  size_t (*trim)(mutator_t *, u8 *,
                 u8 *);  // The params here are in_buf and out_buf.

  size_t (*mutate)(mutator_t *, raw_input_t *, size_t);  // Mutate function

  stage_t *(*get_stage)(mutator_t *);

};

struct mutator {

  stage_t *stage;

  struct mutator_functions funcs;

};


void mutator_init_default(mutator_t *);
size_t trim_default(mutator_t *, u8 *, u8 *);
size_t mutate_default(mutator_t *, raw_input_t *, size_t);
stage_t *get_mutator_stage_default(mutator_t *);


void _afl_mutator_init_(mutator_t *, stage_t *);
void afl_mutator_deinit(mutator_t *);

// A simple scheduled mutator based on the above mutator. Will act something
// similar to the havoc stage

static inline mutator_t *afl_mutator_init(mutator_t *mutator, stage_t *stage) {

  mutator_t *new_mutator = mutator;

  if (mutator)
    _afl_mutator_init_(mutator, stage);

  else {

    new_mutator = calloc(1, sizeof(mutator_t));
    if (!new_mutator) return NULL;
    _afl_mutator_init_(new_mutator, stage);

  }

  return new_mutator;

}

#define AFL_MUTATOR_DEINIT(mutator) afl_mutator_deinit(mutator);

typedef void (*mutator_func_type)(mutator_t *, raw_input_t *);

typedef struct scheduled_mutator scheduled_mutator_t;

struct scheduled_mutator_functions {

  int (*schedule)(scheduled_mutator_t *);
  void (*add_mutator)(scheduled_mutator_t *, mutator_func_type);
  int (*iterations)(scheduled_mutator_t *);

};

struct scheduled_mutator {

  mutator_t super;
  list_t    mutations;

  struct scheduled_mutator_functions extra_funcs;

};

/* TODO add implementation for the _schedule_ and _iterations_ functions, need a
 * random list element pop type implementation for this */
int  iterations_default(scheduled_mutator_t *);
void add_mutator_default(scheduled_mutator_t *, mutator_func_type);
int schedule_default(scheduled_mutator_t *);

scheduled_mutator_t *afl_scheduled_mutator_init(stage_t *);
void                 afl_scheduled_mutator_deinit(scheduled_mutator_t *);

#endif

