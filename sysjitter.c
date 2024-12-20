#define SYSJITTER_VERSION  "1.4"
/*
 * sysjitter
 *
 * Copyright 2010-2017 David Riddoch <david@riddoch.org.uk>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Description:
 *
 * sysjitter measures the extent to which the system impacts on user-level
 * code by causing jitter.  It runs a thread on each processor core, and
 * when the thread is "knocked off" the core it measures how long for.  At
 * the end of the run it outputs some summary statistics for each core, and
 * optionally the full raw data.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <sched.h>
#include <stdbool.h>
#include <sys/sysinfo.h>
#include <sys/types.h>


/* Used as prefix for error and warning messages. */
#define APP_NAME  "sysjitter"


static void usage_msg(FILE* f)
{
  fprintf(f, "usage:\n");
  fprintf(f, "  %s [options] THRESHOLD_NSEC\n", APP_NAME);
  fprintf(f, "\n");
  fprintf(f, "options:\n");
  fprintf(f, "  --runtime SECONDS\n");
  fprintf(f, "  --raw FILENAME-PREFIX\n");
  fprintf(f, "  --cores COMMA-SEP-LIST-OF-CORES-OR-RANGES\n");
  fprintf(f, "  --sort\n");
  fprintf(f, "  --verbose\n");
  fprintf(f, "  --help\n");
  fprintf(f, "  --version\n");
}


static void usage_err(void)
{
  usage_msg(stderr);
  exit(1);
}


#  define relax()  sched_yield()

#ifdef __GNUC__
# define atomic_inc(ptr)   __sync_add_and_fetch((ptr), 1)
# if defined(__x86_64__)
static inline void frc(uint64_t* pval)
{
  uint32_t low, high;
  __asm__ __volatile__("rdtsc" : "=a" (low) , "=d" (high));
  *pval = ((uint64_t) high << 32) | low;
}
# elif defined(__i386__)
static inline void frc(uint64_t* pval)
{
  __asm__ __volatile__("rdtsc" : "=A" (*pval));
}
# elif defined(__PPC64__)
static inline void frc(uint64_t* pval)
{
  __asm__ __volatile__("mfspr %0, 268\n" : "=r" (*pval));
}
# elif defined(__aarch64__)
static inline void frc(uint64_t* pval)
{
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(*pval));
}
# else
#  error Need frc() for this platform.
# endif
#else
# error Need to add support for this compiler.
#endif


typedef uint64_t stamp_t;   /* timestamp */
typedef uint64_t cycles_t;  /* number of cycles */


enum command {
  WAIT,
  GO,
  STOP
};


struct interruption {
  stamp_t   ts;
  cycles_t  diff;
};


struct thread {
  int                  core_i;
  pthread_t            thread_id;

  /* Results generated during a test. */
  unsigned             cpu_mhz;
  struct interruption* interruptions;
  struct interruption* c_interruption;
  cycles_t             int_total;
  stamp_t              frc_start;
  stamp_t              frc_stop;

  /* Calculated by post-processing after the test. */
  struct interruption**sorted;
  cycles_t             runtime;
  unsigned             int_n;
  cycles_t             int_min;
  cycles_t             int_max;
  cycles_t             int_mean;
  cycles_t             int_median;
  cycles_t             int_90;
  cycles_t             int_99;
  cycles_t             int_999;
  cycles_t             int_9999;
  cycles_t             int_99999;
};


struct global {
  /* Configuration. */
  unsigned              max_interruptions;
  unsigned              runtime_secs;
  unsigned              threshold_nsec;
  unsigned              n_threads;
  struct timeval        tv_start;
  int                   sort_raw;
  int                   verbose;

  /* Mutable state. */
  volatile enum command cmd;
  volatile unsigned     n_threads_started;
  volatile unsigned     n_threads_ready;
  volatile unsigned     n_threads_running;
  volatile unsigned     n_threads_finished;
};


static struct global g;


#define TEST(x)                                 \
  do {                                          \
    if( ! (x) )                                 \
      test_fail(#x, __LINE__);                  \
  } while( 0 )

#define TEST0(x)  TEST((x) == 0)


static void test_fail(const char* what, int line)
{
  fprintf(stderr, "ERROR: Internal error in %s\n", APP_NAME);
  fprintf(stderr, "ERROR: TEST(%s)\n", what);
  fprintf(stderr, "ERROR: at line=%d errno=%d (%s)\n",
          line, errno, strerror(errno));
  exit(1);
}


static int move_to_core(int core_i)
{
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  CPU_SET(core_i, &cpus);
  return sched_setaffinity(0, sizeof(cpus), &cpus);
}


static cycles_t __measure_cpu_hz(void)
{
  struct timeval tvs, tve;
  stamp_t s, e;
  double sec;

  frc(&s);
  e = s;
  gettimeofday(&tvs, NULL);
  while( e - s < 1000000 )
    frc(&e);
  gettimeofday(&tve, NULL);
  sec = tve.tv_sec - tvs.tv_sec + (tve.tv_usec - tvs.tv_usec) / 1e6;
  return (cycles_t) ((e - s) / sec);
}


static unsigned measure_cpu_mhz(void)
{
  cycles_t m, mprev, d;

  mprev = __measure_cpu_hz();
  do {
    m = __measure_cpu_hz();
    if( m > mprev )  d = m - mprev;
    else             d = mprev - m;
    mprev = m;
  } while( d > m / 1000 );

  return (unsigned) (m / 1000000);
}


static void thread_init(struct thread* t)
{
  int bytes = g.max_interruptions * sizeof(struct interruption);
  TEST(t->interruptions = malloc(bytes));
  memset(t->interruptions, 0, bytes);  /* touch to fault in */
  t->c_interruption = t->interruptions;
  TEST(t->sorted = malloc(g.max_interruptions * sizeof(t->sorted[0])));
}


static uint64_t cycles_to_ns(const struct thread* t, uint64_t cycles)
{
  return cycles * 1000 / t->cpu_mhz;
}


static uint64_t cycles_to_us(const struct thread* t, uint64_t cycles)
{
  return cycles / t->cpu_mhz;
}


static float cycles_to_sec_f(const struct thread* t, uint64_t cycles)
{
  return cycles / (t->cpu_mhz * 1e6);
}


static void doit(struct thread* t, cycles_t threshold_cycles)
{
  struct interruption* i = t->interruptions;
  struct interruption* i_end = t->interruptions + g.max_interruptions;
  stamp_t prev_ts;
  cycles_t int_total = 0;

  frc(&prev_ts);
  while( g.cmd == GO ) {
    frc(&i->ts);
    i->diff = i->ts - prev_ts;
    prev_ts = i->ts;
    if( i->diff >= threshold_cycles ) {
      int_total += i->diff;
      ++i;
      if( i == i_end )
        break;
    }
  }

  t->c_interruption = i;
  t->int_total = int_total;
}


static void* thread_main(void* arg)
{
  /* Important thing to note here is that once we start bashing the CPU, we
   * need to keep doing so to prevent the core from changing frequency or
   * dropping into a low power state.
   */
  struct thread* t = arg;

  /* Alloc memory in the thread itself after setting affinity to get the
   * best chance of getting numa-local memory.  Doesn't matter so much for
   * the "struct thread" since we expect that to stay cache resident.
   */
  TEST(move_to_core(t->core_i) == 0);
  thread_init(t);

  /* Don't bash the cpu until all threads have got going. */
  atomic_inc(&g.n_threads_started);
  while( g.cmd == WAIT )
    usleep(1000);

  t->cpu_mhz = measure_cpu_mhz();

  /* Ensure we all start at the same time. */
  atomic_inc(&g.n_threads_running);
  while( g.n_threads_running != g.n_threads )
    relax();

  frc(&t->frc_start);
  doit(t, (cycles_t) g.threshold_nsec * t->cpu_mhz / 1000);
  frc(&t->frc_stop);

  /* Wait for everyone to finish so we don't disturb them by exiting and
   * waking the main thread.
   */
  atomic_inc(&g.n_threads_finished);
  while( g.n_threads_finished != g.n_threads )
    relax();

  return NULL;
}


static int qsort_cmp_interruption(const void* oa, const void* ob)
{
  const struct interruption*const* a = oa;
  const struct interruption*const* b = ob;
  return (int) ((*a)->diff - (*b)->diff);
}


static void unsort_interruptions(struct thread* t)
{
  int i, n = t->c_interruption - t->interruptions;
  for( i = 0; i < n; ++i )
    t->sorted[i] = &t->interruptions[i];
}


static void sort_interruptions(struct thread* t)
{
  int n = t->c_interruption - t->interruptions;
  unsort_interruptions(t);
  qsort(t->sorted, n, sizeof(t->sorted[0]), qsort_cmp_interruption);
}


static void thread_calc_stats(struct thread* t)
{
  struct interruption* i;
  uint64_t sum;

  t->runtime = t->frc_stop - t->frc_start;
  t->int_n = t->c_interruption - t->interruptions;
  if( t->int_n ) {
    sort_interruptions(t);
    t->int_min = t->sorted[0]->diff;
    t->int_max = t->sorted[t->int_n - 1]->diff;
    t->int_median = t->sorted[t->int_n / 2]->diff;
    t->int_90 = t->sorted[(int) (t->int_n * 0.9)]->diff;
    t->int_99 = t->sorted[(int) (t->int_n * 0.99)]->diff;
    t->int_999 = t->sorted[(int) (t->int_n * 0.999)]->diff;
    t->int_9999 = t->sorted[(int) (t->int_n * 0.9999)]->diff;
    t->int_99999 = t->sorted[(int) (t->int_n * 0.99999)]->diff;
    sum = 0;
    for( i = t->interruptions; i != t->c_interruption; ++i )
      sum += i->diff;
    t->int_mean = sum / t->int_n;
  }
  else {
    t->int_min = 0;
    t->int_max = 0;
    t->int_median = 0;
    t->int_90 = 0;
    t->int_99 = 0;
    t->int_999 = 0;
    t->int_9999 = 0;
    t->int_99999 = 0;
    t->int_mean = 0;
  }
}


static void post_test_checks(struct thread* threads)
{
  struct thread* t;
  int early = 0;
  int i;

  for( i = 0; i < g.n_threads; ++i ) {
    t = &(threads[i]);
    if( t->c_interruption - t->interruptions == g.max_interruptions ) {
      early = 1;
      fprintf(stderr, "%s: ERROR: Thread %d finished at %.1fs (max=%d)\n",
              APP_NAME, i, cycles_to_sec_f(t, t->frc_stop - t->frc_start),
              g.max_interruptions);
    }
  }

  if( early ) {
    fprintf(stderr, "%s: You probably need to increase the interruption "
            "threshold.\n", APP_NAME);
    exit(2);
  }
}


static void write_thread_raw(struct thread* t, FILE* f)
{
  int j, n_interruptions = (int) (t->c_interruption - t->interruptions);
  const struct interruption* i;
  const struct interruption* prev;
  cycles_t delta;

  fprintf(f, "# cpu_mhz: %u\n", t->cpu_mhz);
  fprintf(f, "# threshold: %uns\n", g.threshold_nsec);
  fprintf(f, "# n_interruptions: %d\n", n_interruptions);
  if( n_interruptions == 0 )
    return;
  delta = t->frc_stop - t->frc_start;
  fprintf(f, "# interruption: %f%%\n", 100.0 * t->int_total / delta);
  fprintf(f, "# total_interruption: %"PRId64" cycles\n", t->int_total);
  fprintf(f, "# total_runtime: %"PRIu64" cycles\n", delta);
  fprintf(f, "# total_interruption: %.9f seconds\n",
          cycles_to_sec_f(t, t->int_total));
  fprintf(f, "# total_runtime: %.9f seconds\n", cycles_to_sec_f(t, delta));
  fprintf(f, "#\n");

  if( ! g.sort_raw ) {
    fprintf(f, "#      Timestamp      delta   <== interruption =>\n");
    fprintf(f, "#         (nsec)     (usec)   (cycles)     (nsec)\n");
    /*         "1234567890123456 1234567890 1234567890 1234567890" */

    i = prev = t->interruptions;
    for( ; i < t->c_interruption; prev = i, ++i ) {
      delta = i->ts - prev->ts;
      fprintf(f, "%16"PRIu64" %10"PRIu64" %10"PRId64" %10"PRIu64"\n",
              cycles_to_ns(t, i->ts - t->frc_start),
              cycles_to_us(t, delta), i->diff, cycles_to_ns(t, i->diff));
    }
  }
  else {
    fprintf(f, "#      Timestamp   <== interruption =>\n");
    fprintf(f, "#         (nsec)   (cycles)     (nsec)\n");
    /*         "1234567890123456 1234567890 1234567890" */

    sort_interruptions(t);
    for( j = 0; j < n_interruptions; ++j ) {
      i = t->sorted[j];
      fprintf(f, "%16"PRIu64" %10"PRId64" %10"PRIu64"\n",
              cycles_to_ns(t, i->ts - t->frc_start),
              i->diff, cycles_to_ns(t, i->diff));
    }
  }
}


static int write_raw(struct thread* threads, const char* outf)
{
  char fname[strlen(outf) + 10];
  FILE* f;
  int i, core_digits, max_core_i = -1;
  int rc = 0;

  /* Find out max core_i so we can pad the core_i in the filename to the
   * appropriate width.
   */
  for( i = 0; i < g.n_threads; ++i )
    if( threads[i].core_i > max_core_i )
      max_core_i = threads[i].core_i;
  sprintf(fname, "%d%n", max_core_i, &core_digits);

  for( i = 0; i < g.n_threads; ++i ) {
    sprintf(fname, "%s.%0*d", outf, core_digits, threads[i].core_i);
    if( (f = fopen(fname, "w")) == NULL ) {
      fprintf(stderr, "%s: ERROR: Could not open '%s' for writing (%s)\n",
              APP_NAME, fname, strerror(errno));
      rc = 3;
      continue;
    }
    write_thread_raw(&(threads[i]), f);
    fclose(f);
  }
  return rc;
}


#define _putfield(label, val, fmt) do {         \
  printf("%s:", label);                         \
  for( i = 0; i < g.n_threads; ++i )            \
    printf(" %"fmt, val);                       \
  printf("\n");                                 \
} while( 0 )

#define putfield(fn, fmt)  _putfield(#fn, t[i].fn, fmt)

#define putu(fn)    putfield(fn, "u")
#define put_frc(fn)  putfield(fn, PRIx64)
#define put_cycles(fn)                                          \
  _putfield(#fn"(ns)", cycles_to_ns(&(t[i]), t[i].fn), PRIu64)
#define put_cycles_s(fn)                                        \
  _putfield(#fn"(s)", cycles_to_sec_f(&(t[i]), t[i].fn), ".3f")
#define put_percent(a, b)                                               \
  _putfield(#a"(%)", (t[i].b ? (t[i].a * 1e2 / t[i].b) : 0.0), ".3f")


static void write_summary(struct thread* t, FILE* f)
{
  int i;

  for( i = 0; i < g.n_threads; ++i )
    thread_calc_stats(&(t[i]));

  putu(core_i);
  _putfield("threshold(ns)", g.threshold_nsec, "u");
  putu(cpu_mhz);
  put_cycles(runtime);
  put_cycles_s(runtime);
  putu(int_n);
  _putfield("int_n_per_sec",
            t[i].int_n / cycles_to_sec_f(&(t[i]), t[i].runtime), ".3f");
  put_cycles(int_min);
  put_cycles(int_median);
  put_cycles(int_mean);
  put_cycles(int_90);
  put_cycles(int_99);
  put_cycles(int_999);
  put_cycles(int_9999);
  put_cycles(int_99999);
  put_cycles(int_max);
  put_cycles(int_total);
  put_percent(int_total, runtime);
  if( g.verbose ) {
    put_frc(frc_start);
    put_frc(frc_stop);
  }
}


static void run_expt(struct thread* threads, int runtime_secs)
{
  int i;

  g.runtime_secs = runtime_secs;
  g.n_threads_started = 0;
  g.n_threads_ready = 0;
  g.n_threads_running = 0;
  g.n_threads_finished = 0;
  g.cmd = WAIT;

  for( i = 0; i < g.n_threads; ++i )
    TEST0(pthread_create(&(threads[i].thread_id), NULL,
                         thread_main, &(threads[i])));
  while( g.n_threads_started != g.n_threads )
    usleep(1000);
  gettimeofday(&g.tv_start, NULL);
  g.cmd = GO;

  alarm(g.runtime_secs);

  /* Go to sleep until the threads have done their stuff. */
  for( i = 0; i < g.n_threads; ++i )
    pthread_join(threads[i].thread_id, NULL);
  post_test_checks(threads);
}


static void cleanup_expt(struct thread* threads)
{
  int i;
  for( i = 0; i < g.n_threads; ++i ) {
    free(threads[i].interruptions);
    threads[i].interruptions = NULL;
    free(threads[i].sorted);
    threads[i].sorted = NULL;
  }
}


static void calc_max_interruptions(struct thread* threads, int runtime)
{
  /* Calculate how big max_interruptions needs to be for real run of
   * [runtime] seconds.
   */
  struct thread* t;
  int i, max = 0, per_sec;

  for( i = 0; i < g.n_threads; ++i ) {
    t = &(threads[i]);
    t->int_n = t->c_interruption - t->interruptions;
    if( t->int_n > max )
      max = t->int_n;
  }

  /* If getting a low number of interruptions per second then variance may be
   * quite high.  So
   */
  per_sec = max / g.runtime_secs;
  if( per_sec < 1000 )
    per_sec = 1000;
  g.max_interruptions = per_sec * 2 * runtime;
}


static void handle_alarm(int code)
{
  g.cmd = STOP;
}


static void append_int(int** list, int* list_len, int val)
{
  int idx = (*list_len)++;
  TEST( *list = realloc(*list, *list_len * sizeof(int)) );
  (*list)[idx] = val;
}


static bool parse_comma_sep_ranges(const char* csr_in,
                                   int** list, int* list_len)
{
  char* csr = strdupa(csr_in);
  char *saveptr = NULL, *t;
  unsigned low, high;
  char dummy;

  *list = NULL;
  *list_len = 0;

  while( (t = strtok_r(csr, ",", &saveptr)) != NULL ) {
    csr = NULL;
    if( sscanf(t, "%u - %u%c", &low, &high, &dummy) == 2 )
      for( ; low <= high; ++low )
        append_int(list, list_len, low);
    else if( sscanf(t, "%u%c", &low, &dummy) == 1 )
      append_int(list, list_len, low);
    else
      return false;
  }
  return true;
}


int main(int argc, char* argv[])
{
  struct thread* threads;
  const char* raw_prefix = NULL;
  const char* cores_opt = NULL;
  char dummy;
  int n_cores, runtime = 70;
  int* cores;

  g.max_interruptions = 1000000;

  --argc; ++argv;
  for( ; argc; --argc, ++argv ) {
    if( argv[0][0] != '-' ) {
      break;
    }
    else if( strcmp(argv[0], "--max") == 0 && argc > 1 &&
             sscanf(argv[1], "%u%c", &g.max_interruptions, &dummy) == 1 ) {
      --argc, ++argv;
    }
    else if( strcmp(argv[0], "--raw") == 0 && argc > 1 ) {
      raw_prefix = argv[1];
      --argc, ++argv;
    }
    else if( strcmp(argv[0], "--cores") == 0 && argc > 1 ) {
      cores_opt = argv[1];
      --argc, ++argv;
    }
    else if( strcmp(argv[0], "--runtime") == 0 && argc > 1 &&
             sscanf(argv[1], "%u%c", &runtime, &dummy) == 1 ) {
      --argc, ++argv;
    }
    else if( strcmp(argv[0], "--sort") == 0 ) {
      g.sort_raw = 1;
    }
    else if( strcmp(argv[0], "--verbose") == 0 ) {
      g.verbose = 1;
    }
    else if( strcmp(argv[0], "--help") == 0 ) {
      usage_msg(stdout);
      exit(0);
    }
    else if( strcmp(argv[0], "--version") == 0 ) {
      printf("%s\n", SYSJITTER_VERSION);
      exit(0);
    }
    else {
      usage_err();
    }
  }

  if( argc != 1  ||
      sscanf(argv[0], "%u%c", &g.threshold_nsec, &dummy) != 1 )
    usage_err();

  int nprocs = get_nprocs_conf();
  cpu_set_t cpus;
  sched_getaffinity(getpid(), sizeof cpus, &cpus);

  if( cores_opt == NULL ) {
    n_cores = CPU_COUNT(&cpus);
  }
  else {
    if( ! parse_comma_sep_ranges(cores_opt, &cores, &n_cores) ) {
      fprintf(stderr, "%s: ERROR: badly formatted --cores arg\n", APP_NAME);
      exit(2);
    }
  }

  /* Check which cores we can use by trying to set affinity to each. */
  TEST( threads = malloc(n_cores * sizeof(threads[0])) );
  g.n_threads = n_cores;

  /* FIXME This ignores any input to --cores from the user */
  int cur_proc = 0;
  for (int i = 0; i != n_cores; ++i, ++cur_proc)
  {
    while (!CPU_ISSET(cur_proc, &cpus) && cur_proc != nprocs)
        ++cur_proc;

    if (cur_proc != nprocs)
      threads[i].core_i = cur_proc;
    else
    {
      puts("error: reached nprocs limit");
      exit(1);
    }
  }

  move_to_core(0);  // put the main thread on core 0
  signal(SIGALRM, handle_alarm);

  run_expt(threads, 1);
  calc_max_interruptions(threads, runtime);
  cleanup_expt(threads);
  run_expt(threads, runtime);

  /* NB. Important to write raw results first, as write_summary() sorts the
   * interruptions.
   */
  int err = 0;
  if( raw_prefix )
    err = write_raw(threads, raw_prefix);
  write_summary(threads, stdout);
  return err;
}
