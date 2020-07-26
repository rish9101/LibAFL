#include "libaflpp.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#define AFL_MAIN

#include "config.h"
#include "types.h"
#include "debug.h"
#include "alloc-inl.h"
#include "hash.h"
#include "common.h"
#include "sharedmem.h"
#include "libaflpp.h"
#include "libobservationchannel.h"
#include "libos.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>

#include <sys/wait.h>
#include <sys/time.h>
#ifndef USEMMAP
  #include <sys/shm.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>

#define MAP_SIZE 65536
#define MAX_PATH_LEN 100

typedef struct afl_forkserver {

  executor_t super;                      /* executer struct to inherit from */

  u8 *trace_bits;
      /* SHM with instrumentation bitmap  */  // Put it in map based obs channel
  u8 use_stdin;                         /* use stdin for sending data       */

  s32 fsrv_pid,                         /* PID of the fork server           */
      child_pid,                        /* PID of the fuzzed program        */
      child_status,                     /* waitpid result for the child     */
      out_dir_fd;                       /* FD of the lock file              */

  s32 out_fd,                           /* Persistent fd for fsrv->out_file */

      fsrv_ctl_fd,                      /* Fork server control pipe (write) */
      fsrv_st_fd;                       /* Fork server status pipe (read)   */

  u32 exec_tmout;                       /* Configurable exec timeout (ms)   */
  u32 map_size;                         /* map size used by the target      */

  u64 total_execs;                      /* How often run_target was called  */

  u8 *out_file,                         /* File to fuzz, if any             */
      *target_path;                     /* Path of the target               */

  u32 last_run_timed_out;               /* Traced process timed out?        */

  u8 last_kill_signal;                  /* Signal that killed the child     */

} afl_forkserver_t;

static u8 *file_data_read;  // When we read an input file, it goes here
static u32 in_len;          // File Input length

static u32 read_file(u8 *in_file);
static u32 read_s32_timed(s32 fd, s32 *buf, u32 timeout_ms);

static afl_forkserver_t *fsrv_init(u8 *target_path, u8 *out_file);
static exit_type_t       run_target(afl_forkserver_t *fsrv, u32 timeout);
static u8 place_inputs(afl_forkserver_t *fsrv, u8 *mem, size_t len);
static u8 fsrv_start(afl_forkserver_t *fsrv, char **extra_args);

static u32 read_s32_timed(s32 fd, s32 *buf, u32 timeout_ms) {

  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(fd, &readfds);
  struct timeval timeout;
  int            sret;
  ssize_t        len_read;

  timeout.tv_sec = (timeout_ms / 1000);
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
#if !defined(__linux__)
  u64 read_start = get_cur_time_us();
#endif

  /* set exceptfds as well to return when a child exited/closed the pipe. */
restart_select:
  sret = select(fd + 1, &readfds, NULL, NULL, &timeout);

  if (likely(sret > 0)) {

  restart_read:
    len_read = read(fd, (u8 *)buf, 4);

    if (likely(len_read == 4)) {  // for speed we put this first

#if defined(__linux__)
      u32 exec_ms = MIN(
          timeout_ms,
          ((u64)timeout_ms - (timeout.tv_sec * 1000 + timeout.tv_usec / 1000)));
#else
      u32 exec_ms = MIN(timeout_ms, get_cur_time_us() - read_start);
#endif

      // ensure to report 1 ms has passed (0 is an error)
      return exec_ms > 0 ? exec_ms : 1;

    } else if (unlikely(len_read == -1 && errno == EINTR)) {

      goto restart_read;

    } else if (unlikely(len_read < 4)) {

      return 0;

    }

  } else if (unlikely(!sret)) {

    *buf = -1;
    return timeout_ms + 1;

  } else if (unlikely(sret < 0)) {

    if (likely(errno == EINTR)) goto restart_select;

    *buf = -1;
    return 0;

  }

  return 0;  // not reached

}

/* This function simply reads data from a file and stores it in file_data_read,
 * length in in_len*/
static u32 read_file(u8 *in_file) {

  struct stat st;
  s32         fd = open(in_file, O_RDONLY);

  if (fd < 0) { WARNF("Unable to open '%s'", in_file); }

  if (fstat(fd, &st) || !st.st_size) {

    WARNF("Zero-sized input file '%s'.", in_file);

  }

  in_len = st.st_size;
  file_data_read = ck_alloc_nozero(in_len);

  ck_read(fd, file_data_read, in_len, in_file);

  close(fd);

  return in_len;

}

afl_forkserver_t *fsrv_init(u8 *target_path, u8 *out_file) {

  afl_forkserver_t *fsrv = ck_alloc(
      sizeof(afl_forkserver_t));  // We use ck_alloc here since it's an example
                                  // and FATAL won't be an issue

  if (!afl_executor_init(&(fsrv->super))) {

    FATAL("SOMETHING WRONG WHILE INITIALIZING THE BASE EXECUTOR");

  }

  /* defining standard functions for the forkserver vtable */
  fsrv->super.funcs.init_cb = fsrv_start;
  fsrv->super.funcs.place_inputs_cb = place_inputs;
  fsrv->super.funcs.run_target_cb = run_target;

  fsrv->target_path = target_path;
  fsrv->out_file = out_file;

  /* FD for the stdin of the child process */
  if (!fsrv->out_file) {

    fsrv->out_fd = -1;

  } else {

    fsrv->out_fd = open(fsrv->out_file, O_WRONLY | O_CREAT, 0600);

  }

  fsrv->out_dir_fd = -1;

  /* Settings */
  fsrv->use_stdin = 1;

  /* exec related stuff */
  fsrv->child_pid = -1;
  fsrv->exec_tmout = 0;                          /* Default exec time in ms */

  return fsrv;

}

static u8 fsrv_start(afl_forkserver_t *fsrv, char **extra_args) {

  int st_pipe[2], ctl_pipe[2];
  s32 status;
  s32 rlen;

  ACTF("Spinning up the fork server...");

  if (pipe(st_pipe) || pipe(ctl_pipe)) { PFATAL("pipe() failed"); }

  fsrv->last_run_timed_out = 0;
  fsrv->fsrv_pid = fork();

  if (fsrv->fsrv_pid < 0) { PFATAL("fork() failed"); }

  if (!fsrv->fsrv_pid) {

    /* CHILD PROCESS */

    setsid();

    fsrv->out_fd = open(fsrv->out_file, O_RDONLY | O_CREAT, 0600);

    dup2(fsrv->out_fd, 0);
    close(fsrv->out_fd);

    /* Set up control and status pipes, close the unneeded original fds. */

    if (dup2(ctl_pipe[0], FORKSRV_FD) < 0) { PFATAL("dup2() failed"); }
    if (dup2(st_pipe[1], FORKSRV_FD + 1) < 0) { PFATAL("dup2() failed"); }

    close(ctl_pipe[0]);
    close(ctl_pipe[1]);
    close(st_pipe[0]);
    close(st_pipe[1]);

    execv(fsrv->target_path, extra_args);

    /* Use a distinctive bitmap signature to tell the parent about execv()
       falling through. */

    fsrv->trace_bits = (u8 *)0xdeadbeef;
    fprintf(stderr, "Error: execv to target failed\n");
    exit(0);

  }

  /* PARENT PROCESS */

  char pid_buf[16];
  sprintf(pid_buf, "%d", fsrv->fsrv_pid);
  /* Close the unneeded endpoints. */

  close(ctl_pipe[0]);
  close(st_pipe[1]);

  fsrv->fsrv_ctl_fd = ctl_pipe[1];
  fsrv->fsrv_st_fd = st_pipe[0];

  /* Wait for the fork server to come up, but don't wait too long. */

  rlen = 0;
  if (fsrv->exec_tmout) {

    u32 time_ms = read_s32_timed(fsrv->fsrv_st_fd, &status,
                                 fsrv->exec_tmout * FORK_WAIT_MULT);

    if (!time_ms) {

      kill(fsrv->fsrv_pid, SIGKILL);

    } else if (time_ms > fsrv->exec_tmout * FORK_WAIT_MULT) {

      fsrv->last_run_timed_out = 1;
      kill(fsrv->fsrv_pid, SIGKILL);

    } else {

      rlen = 4;

    }

  } else {

    rlen = read(fsrv->fsrv_st_fd, &status, 4);

  }

  /* If we have a four-byte "hello" message from the server, we're all set.
     Otherwise, try to figure out what went wrong. */

  if (rlen == 4) {

    OKF("All right - fork server is up.");

    return 0;

  }

  if (fsrv->trace_bits == (u8 *)0xdeadbeef) {

    FATAL("Unable to execute target application ('%s')", extra_args[0]);

  }

  FATAL("Fork server handshake failed");

};

u8 place_inputs(afl_forkserver_t *fsrv, u8 *mem, size_t len) {

  int write_len = write(fsrv->out_fd, mem, len);

  if (write_len != len) { FATAL("Short Write"); }

  return write_len;

}

/* Execute target application, monitoring for timeouts. Return status
   information. The called program will update afl->fsrv->trace_bits. */

static exit_type_t run_target(afl_forkserver_t *fsrv, u32 timeout) {

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

  exec_ms = read_s32_timed(fsrv->fsrv_st_fd, &fsrv->child_status, timeout);

  SAYF("Child pid %d Exec ms %d Timeout %d\n", fsrv->child_pid, exec_ms,
       timeout);

  if (exec_ms > timeout) {

    /* If there was no response from forkserver after timeout seconds,
    we kill the child. The forkserver should inform us afterwards */

    kill(fsrv->child_pid, SIGKILL);
    fsrv->last_run_timed_out = 1;
    if (read(fsrv->fsrv_st_fd, &fsrv->child_status, 4) < 4) { exec_ms = 0; }

  }

  if (!exec_ms) {}

  if (!WIFSTOPPED(fsrv->child_status)) { fsrv->child_pid = 0; }

  fsrv->total_execs++;

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

int main(int argc, char **argv) {

  if (argc < 3) {

    FATAL(
        "Usage: ./executor_example /target/path /input/directory "
        "/out/file/path ");

  }

  DIR *          dir_in;
  struct dirent *dir_ent;
  u8 *           in_dir = argv[2];
  u8             infile[MAX_PATH_LEN] = {0};

  afl_forkserver_t *fsrv = fsrv_init(argv[1], argv[3]);

  /* Let's now create a simple map-based observation channel and add it to the
   * executor */

  map_based_channel_t *trace_bits_channel = afl_map_channel_init(MAP_SIZE);
  fsrv->super.funcs.add_observation_channel(fsrv, trace_bits_channel);

  /* for the fsrv to work, we also need a set __AFL_SHM_FD env variable */
  u8 *shm_str = alloc_printf("%d", trace_bits_channel->shared_map->shm_id);
  setenv("__AFL_SHM_ID", shm_str, 1);

  fsrv->trace_bits = trace_bits_channel->shared_map->map;

  fsrv->super.funcs.init_cb(fsrv, argv);

  if (!(dir_in = opendir(in_dir))) {

    PFATAL("cannot open directory %s", in_dir);

  }

  while ((dir_ent = readdir(dir_in))) {

    if (dir_ent->d_name[0] == '.') {

      continue;  // skip anything that starts with '.'

    }

    snprintf(infile, sizeof(infile), "%s/%s", in_dir, dir_ent->d_name);

    if (read_file(infile)) {

      fsrv->super.funcs.place_inputs_cb(fsrv, file_data_read, in_len);

      fsrv->super.funcs.run_target_cb(fsrv, 1000, NULL);

    }

  }

  OKF("Processed %llu input files.", fsrv->total_execs);

  closedir(dir_in);

  return 0;

}
