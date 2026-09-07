/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Standalone benchmark driver for fzf-native's real multi-round session.
 *
 * The fzf-native module intentionally exposes its plain-C session internals to
 * its C test and fuzz targets.  Including the implementation here exercises
 * that same candidate arena, request coordinator, persistent worker pool,
 * caches, cancellation state, and result publication without putting Emacs or
 * terminal rendering inside the timing boundary.
 */

#define FZF_NATIVE_CTEST 1
#include "fzf-native-module.c"

#include <inttypes.h>
#include <limits.h>
#include <time.h>

#ifndef FZF_NATIVE_SOURCE_REVISION
#define FZF_NATIVE_SOURCE_REVISION "unknown"
#endif
#ifndef FZF_NATIVE_SOURCE_BUILD_ID
#define FZF_NATIVE_SOURCE_BUILD_ID "unknown"
#endif

enum { DEFAULT_TIMEOUT_MS = 120000 };

typedef struct {
  char **items;
  size_t count;
  size_t cap;
} QueryList;

typedef struct {
  uint64_t request_id;
  uint64_t elapsed_ns;
  uint64_t checksum;
  size_t matched;
  size_t emitted;
  size_t pool;
  bool filter_only;
  ScoredStr *results;
} RoundResult;

typedef struct {
  const char *input;
  QueryList queries;
  unsigned workers;
  size_t limit;
  uint64_t timeout_ms;
  size_t cache_entries;
  size_t batch_cache_bytes;
  size_t filter_only_min_pool;
  size_t filter_only_query_length;
  bool filter_only_logic_and;
  fzf_case_types case_mode;
  bool fuzzy;
} Options;

/* Focused regression seam for the untimed verification oracle. */
static bool verify_test_force_allocation_failure;

static void usage(FILE *out, const char *argv0) {
  fprintf(out,
          "Usage: %s --input FILE (--query QUERY | --queries FILE)... [OPTIONS]\n"
          "\n"
          "Required workload arguments:\n"
          "  --input FILE                 newline-delimited candidate corpus\n"
          "  --query QUERY                append one query round (repeatable)\n"
          "  --queries FILE               append query rounds from FILE, one per line\n"
          "\n"
          "Session options:\n"
          "  --workers N                  persistent scorer threads (default: online CPUs)\n"
          "  --limit N                    published top results; 0 means all (default: 10000)\n"
          "  --timeout-ms N               per-round timeout (default: 120000)\n"
          "  --case smart|ignore|respect  matching case mode (default: smart)\n"
          "  --exact                      use exact/substring matching instead of fuzzy\n"
          "  --cache-entries N             exact-result cache entries (default: 40)\n"
          "  --batch-cache-bytes N         stable-batch cache budget (default: 67108864)\n"
          "  --filter-only-min-pool N      pool trigger; 0 disables (default: 10000000)\n"
          "  --filter-only-query-length N  short-query trigger; 0 disables (default: 0)\n"
          "  --filter-only-logic or|and    trigger composition (default: or)\n",
          argv0);
}

static bool parse_size(const char *text, size_t *out) {
  if (!text || !*text || *text == '-') return false;
  errno = 0;
  char *end = NULL;
  uintmax_t value = strtoumax(text, &end, 10);
  if (errno || !end || *end || value > SIZE_MAX) return false;
  *out = (size_t)value;
  return true;
}

static bool parse_u64(const char *text, uint64_t *out) {
  if (!text || !*text || *text == '-') return false;
  errno = 0;
  char *end = NULL;
  uintmax_t value = strtoumax(text, &end, 10);
  if (errno || !end || *end || value > UINT64_MAX) return false;
  *out = (uint64_t)value;
  return true;
}

static bool queries_push(QueryList *queries, const char *query) {
  if (queries->count == queries->cap) {
    size_t next_cap = queries->cap ? queries->cap * 2 : 8;
    char **next = realloc(queries->items, next_cap * sizeof *next);
    if (!next) return false;
    queries->items = next;
    queries->cap = next_cap;
  }
  char *copy = strdup(query);
  if (!copy) return false;
  queries->items[queries->count++] = copy;
  return true;
}

static bool queries_read_file(QueryList *queries, const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  char *line = NULL;
  size_t cap = 0;
  ssize_t got;
  bool ok = true;
  while ((got = getline(&line, &cap, file)) >= 0) {
    size_t len = (size_t)got;
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (strlen(line) != len || !queries_push(queries, line)) {
      ok = false;
      break;
    }
  }
  if (ferror(file)) ok = false;
  free(line);
  fclose(file);
  return ok;
}

static void queries_free(QueryList *queries) {
  for (size_t i = 0; i < queries->count; i++) free(queries->items[i]);
  free(queries->items);
}

static const char *option_value(int argc, char **argv, int *index) {
  if (*index + 1 >= argc) return NULL;
  return argv[++*index];
}

static bool parse_options(int argc, char **argv, Options *options) {
  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  *options = (Options){
      .workers = cpus > 0 ? (unsigned)cpus : 1,
      .limit = 10000,
      .timeout_ms = DEFAULT_TIMEOUT_MS,
      .cache_entries = 40,
      .batch_cache_bytes = 64 * 1024 * 1024,
      .filter_only_min_pool = 10000000,
      .case_mode = CaseSmart,
      .fuzzy = true,
  };
  if (options->workers > ASYNC_WORKER_LIMIT)
    options->workers = ASYNC_WORKER_LIMIT;

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    const char *value = NULL;
    size_t number = 0;
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      usage(stdout, argv[0]);
      exit(0);
    } else if (strcmp(arg, "--input") == 0) {
      if (!(value = option_value(argc, argv, &i))) return false;
      options->input = value;
    } else if (strcmp(arg, "--query") == 0) {
      if (!(value = option_value(argc, argv, &i)) ||
          !queries_push(&options->queries, value))
        return false;
    } else if (strcmp(arg, "--queries") == 0) {
      if (!(value = option_value(argc, argv, &i)) ||
          !queries_read_file(&options->queries, value))
        return false;
    } else if (strcmp(arg, "--workers") == 0) {
      if (!(value = option_value(argc, argv, &i)) ||
          !parse_size(value, &number) || number == 0 ||
          number > ASYNC_WORKER_LIMIT)
        return false;
      options->workers = (unsigned)number;
    } else if (strcmp(arg, "--limit") == 0) {
      if (!(value = option_value(argc, argv, &i)) ||
          !parse_size(value, &options->limit))
        return false;
    } else if (strcmp(arg, "--timeout-ms") == 0) {
      if (!(value = option_value(argc, argv, &i)) ||
          !parse_u64(value, &options->timeout_ms) || options->timeout_ms == 0 ||
          options->timeout_ms > UINT64_MAX / UINT64_C(1000000))
        return false;
    } else if (strcmp(arg, "--cache-entries") == 0) {
      if (!(value = option_value(argc, argv, &i)) ||
          !parse_size(value, &options->cache_entries) ||
          options->cache_entries == 0)
        return false;
    } else if (strcmp(arg, "--batch-cache-bytes") == 0) {
      if (!(value = option_value(argc, argv, &i)) ||
          !parse_size(value, &options->batch_cache_bytes))
        return false;
    } else if (strcmp(arg, "--filter-only-min-pool") == 0) {
      if (!(value = option_value(argc, argv, &i)) ||
          !parse_size(value, &options->filter_only_min_pool))
        return false;
    } else if (strcmp(arg, "--filter-only-query-length") == 0) {
      if (!(value = option_value(argc, argv, &i)) ||
          !parse_size(value, &options->filter_only_query_length))
        return false;
    } else if (strcmp(arg, "--case") == 0) {
      if (!(value = option_value(argc, argv, &i))) return false;
      if (strcmp(value, "smart") == 0)
        options->case_mode = CaseSmart;
      else if (strcmp(value, "ignore") == 0)
        options->case_mode = CaseIgnore;
      else if (strcmp(value, "respect") == 0)
        options->case_mode = CaseRespect;
      else
        return false;
    } else if (strcmp(arg, "--filter-only-logic") == 0) {
      if (!(value = option_value(argc, argv, &i))) return false;
      if (strcmp(value, "or") == 0)
        options->filter_only_logic_and = false;
      else if (strcmp(value, "and") == 0)
        options->filter_only_logic_and = true;
      else
        return false;
    } else if (strcmp(arg, "--exact") == 0) {
      options->fuzzy = false;
    } else {
      return false;
    }
  }
  return options->input && options->queries.count >= 1;
}

static AsyncSession *session_create(const Options *options) {
  AsyncSession *session = calloc(1, sizeof *session);
  if (!session) return NULL;
  pthread_mutex_init(&session->mu, NULL);
  pthread_mutex_init(&session->child_mu, NULL);
  pthread_mutex_init(&session->score_req_mu, NULL);
  pthread_cond_init(&session->score_req_cond, NULL);
  pthread_mutex_init(&session->score_res_mu, NULL);
  atomic_store(&session->child_owner, AsyncChildUnclaimed);
  atomic_store(&session->producer_state, AsyncProducerComplete);
  atomic_store(&session->producer_error, 0);
  atomic_store(&session->producer_exit_status, 0);
  atomic_store(&session->reader_done, true);
  cache_init(&session->cache, options->cache_entries);
  batch_cache_init(&session->batch_cache, options->batch_cache_bytes);
  session->filter_only_min_pool = options->filter_only_min_pool;
  session->worker_pool = async_worker_pool_create(options->workers);
  if (!session->worker_pool) {
    async_session_destroy(session);
    return NULL;
  }
  session->worker_pool_owned = true;
  if (pthread_create(&session->score_thread, NULL, scoring_thread_fn,
                     session) != 0) {
    async_session_destroy(session);
    return NULL;
  }
  session->score_thread_started = true;
  return session;
}

static bool load_candidates(AsyncSession *session, const char *path,
                            size_t *out_count) {
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  char *line = NULL;
  size_t cap = 0;
  ssize_t got;
  bool ok = true;
  while ((got = getline(&line, &cap, file)) >= 0) {
    size_t len = (size_t)got;
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (strlen(line) != len) {
      ok = false;
      break;
    }
    len = async_strip_ansi(line, len);
    if (!async_append_candidate(session, line, len)) {
      ok = false;
      break;
    }
  }
  if (ferror(file)) ok = false;
  free(line);
  fclose(file);
  pthread_mutex_lock(&session->mu);
  *out_count = session->count;
  pthread_mutex_unlock(&session->mu);
  return ok && *out_count > 0;
}

static uint64_t monotonic_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
         (uint64_t)now.tv_nsec;
}

static uint64_t measurement_tick(void) {
#ifdef __APPLE__
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
  return monotonic_ns();
#endif
}

static uint64_t elapsed_ns(uint64_t start, uint64_t end) {
  if (!start || !end || end < start) return 0;
  return end - start;
}

static bool wait_for_publication(AsyncSession *session, uint64_t request_id,
                                 size_t pool, uint64_t timeout_ms,
                                 uint64_t timeout_start_ns,
                                 int initial_generation) {
  static const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000};
  bool first_check = true;
  int observed_generation = initial_generation;
  for (;;) {
    int generation = atomic_load_explicit(&session->gen, memory_order_acquire);
    if (generation != observed_generation || first_check) {
      pthread_mutex_lock(&session->score_res_mu);
      bool failed = session->score_error_id == request_id;
      bool complete = session->score_result_id == request_id &&
                      session->score_result_pool_gen == pool;
      pthread_mutex_unlock(&session->score_res_mu);
      if (failed) return false;
      if (complete) return true;
      observed_generation = generation;
      first_check = false;
    }

    uint64_t now = monotonic_ns();
    uint64_t waited_ns = elapsed_ns(timeout_start_ns, now);
    if (!now || waited_ns >= timeout_ms * UINT64_C(1000000))
      return false;
    /* The session's generation is the low-contention compatibility observer.
       A 1 us requested sleep avoids stealing scorer capacity while staying
       below the previous 50 us polling quantum on common schedulers. */
    nanosleep(&pause, NULL);
  }
}

static bool wait_for_idle(AsyncSession *session, uint64_t timeout_ms) {
  static const struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000};
  uint64_t start = monotonic_ns();
  for (;;) {
    pthread_mutex_lock(&session->score_req_mu);
    bool idle = session->score_req_id == 0 && session->score_current_id == 0;
    pthread_mutex_unlock(&session->score_req_mu);
    if (idle) return true;
    uint64_t now = monotonic_ns();
    if (!now || elapsed_ns(start, now) >= timeout_ms * UINT64_C(1000000))
      return false;
    nanosleep(&pause, NULL);
  }
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t len) {
  const unsigned char *bytes = data;
  for (size_t i = 0; i < len; i++) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t result_checksum(const ScoredStr *results, size_t count) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < count; i++) {
    hash = hash_bytes(hash, &results[i].idx, sizeof results[i].idx);
    hash = hash_bytes(hash, &results[i].score, sizeof results[i].score);
    hash = hash_bytes(hash, results[i].str, strlen(results[i].str) + 1);
  }
  return hash;
}

static int reference_cmp(const void *left, const void *right) {
  const ScoredStr *a = left;
  const ScoredStr *b = right;
  if (a->score != b->score) return a->score > b->score ? -1 : 1;
  if (a->idx != b->idx) return a->idx < b->idx ? -1 : 1;
  return 0;
}

static bool verify_round(AsyncSession *session, const Options *options,
                         const char *query, const RoundResult *round) {
  bool expected_filter_only = decide_filter_only(
      options->filter_only_min_pool, options->filter_only_query_length,
      options->filter_only_logic_and,
      utf8_character_count(query, strlen(query)), round->pool);
  if (round->filter_only != expected_filter_only) return false;

  char *mutable_query = *query ? strdup(query) : NULL;
  if (*query && !mutable_query) return false;
  fzf_pattern_t *pattern = mutable_query
                               ? fzf_parse_pattern(options->case_mode, false,
                                                   mutable_query, options->fuzzy)
                               : NULL;
  if (mutable_query && !pattern) {
    free(mutable_query);
    return false;
  }
  fzf_slab_t *slab = fzf_make_default_slab();
  ScoredStr *reference = malloc(round->pool * sizeof *reference);
  if ((!slab && pattern) || (round->pool && !reference)) {
    free(mutable_query);
    if (pattern) fzf_free_pattern(pattern);
    if (slab) fzf_free_slab(slab);
    free(reference);
    return false;
  }

  size_t matched = 0;
  bool matcher_allocation_failed = false;
  pthread_mutex_lock(&session->mu);
  for (size_t i = 0; i < round->pool; i++) {
    char *candidate = session->cands_top[i >> CANDS_BLOCK_SHIFT]
                                        [i & CANDS_BLOCK_MASK];
    int score = !pattern
                    ? 1
                    : round->filter_only
                          ? (fzf_has_match(candidate, pattern, slab) ? 1 : 0)
                          : fzf_get_score(candidate, pattern, slab);
    if (pattern &&
        (fzf_allocation_failed() || verify_test_force_allocation_failure)) {
      matcher_allocation_failed = true;
      break;
    }
    if (score > 0)
      reference[matched++] = (ScoredStr){
          .str = candidate, .score = score, .idx = (uint32_t)i};
  }
  pthread_mutex_unlock(&session->mu);

  size_t emitted = options->limit && options->limit < matched
                       ? options->limit
                       : matched;
  if (!matcher_allocation_failed && round->filter_only && pattern) {
    for (size_t i = 0; i < emitted; i++) {
      reference[i].score = fzf_get_score(reference[i].str, pattern, slab);
      if (fzf_allocation_failed() || verify_test_force_allocation_failure) {
        matcher_allocation_failed = true;
        break;
      }
    }
    if (!matcher_allocation_failed)
      qsort(reference, emitted, sizeof *reference, reference_cmp);
  } else if (!matcher_allocation_failed) {
    qsort(reference, matched, sizeof *reference, reference_cmp);
  }

  bool ok = !matcher_allocation_failed && round->matched == matched &&
            round->emitted == emitted;
  for (size_t i = 0; ok && i < emitted; i++) {
    ok = round->results[i].idx == reference[i].idx &&
         round->results[i].score == reference[i].score &&
         strcmp(round->results[i].str, reference[i].str) == 0;
  }

  free(reference);
  if (slab) fzf_free_slab(slab);
  if (pattern) fzf_free_pattern(pattern);
  free(mutable_query);
  return ok;
}

static bool capture_round(AsyncSession *session, const Options *options,
                          const char *query, size_t pool,
                          RoundResult *round) {
  char *owned_query = strdup(query);
  if (!owned_query) return false;
  int initial_generation =
      atomic_load_explicit(&session->gen, memory_order_acquire);
  uint64_t timeout_start = monotonic_ns();
  uint64_t start = measurement_tick();
  if (!timeout_start || !start) {
    free(owned_query);
    return false;
  }
  uint64_t request_id = async_submit_request_resolved(
      session, owned_query, strlen(owned_query), options->limit,
      options->case_mode, options->fuzzy,
      options->filter_only_query_length, options->filter_only_logic_and);
  if (!request_id || !wait_for_publication(session, request_id, pool,
                                            options->timeout_ms, timeout_start,
                                            initial_generation))
    return false;
  uint64_t end = measurement_tick();
  if (!end || end < start) return false;

  size_t limit = 0, progress_completed = 0, progress_total = 0;
  size_t filtered = 0, total = 0;
  uint64_t result_id = 0, generation = 0, error_id = 0;
  AsyncResultObservation result_observation = {0};
  char *result_filter = NULL, *error = NULL;
  fzf_case_types case_mode = CaseSmart;
  bool fuzzy = true;
  bool allocation_failed = false;
  ScoredStr *results = async_copy_public_result(
      session, true, &round->emitted, &result_observation, &result_filter,
      &limit, &case_mode, &fuzzy, &round->filter_only, &generation,
      &progress_completed, &progress_total, &error_id, &error, &filtered,
      &total, &allocation_failed);
  result_id = result_observation.request_id;
  round->pool = result_observation.pool_generation;
  bool ok = result_id == request_id && result_filter &&
            strcmp(result_filter, query) == 0 && !error &&
            round->pool == pool &&
            total == pool && !allocation_failed &&
            (!round->emitted || results) &&
            progress_completed == progress_total &&
            case_mode == options->case_mode && fuzzy == options->fuzzy &&
            limit == options->limit && (!limit || round->emitted <= limit);
  free(result_filter);
  free(error);
  if (!ok || !wait_for_idle(session, options->timeout_ms)) {
    async_free_public_result(results, round->emitted);
    return false;
  }
  round->request_id = request_id;
  round->elapsed_ns = elapsed_ns(start, end);
  round->matched = filtered;
  round->results = results;
  round->checksum = result_checksum(results, round->emitted);
  return true;
}

static void emit_error(const char *code) {
  printf("{\"event\":\"error\",\"protocol\":1,\"code\":\"%s\"}\n", code);
  fflush(stdout);
}

int main(int argc, char **argv) {
  verify_test_force_allocation_failure =
      getenv("FZF_NATIVE_DRIVER_TEST_VERIFY_OOM") != NULL;
  Options options;
  if (!parse_options(argc, argv, &options)) {
    usage(stderr, argv[0]);
    emit_error("invalid-arguments");
    queries_free(&options.queries);
    return 2;
  }

  AsyncSession *session = session_create(&options);
  size_t item_count = 0;
  if (!session || !load_candidates(session, options.input, &item_count)) {
    emit_error("input-load-failed-or-empty");
    if (session) async_session_destroy(session);
    queries_free(&options.queries);
    return 1;
  }

  printf("{\"event\":\"ready\",\"protocol\":1,\"items\":%zu,"
         "\"rounds\":%zu,\"workers\":%u,\"limit\":%zu,"
         "\"source_revision\":\"%s\",\"source_build_id\":\"%s\"}\n",
         item_count, options.queries.count, options.workers, options.limit,
         FZF_NATIVE_SOURCE_REVISION, FZF_NATIVE_SOURCE_BUILD_ID);
  fflush(stdout);

  RoundResult *rounds = calloc(options.queries.count, sizeof *rounds);
  bool ok = rounds != NULL;
  for (size_t i = 0; ok && i < options.queries.count; i++)
    ok = capture_round(session, &options, options.queries.items[i],
                       item_count, &rounds[i]);
  for (size_t i = 0; ok && i < options.queries.count; i++)
    ok = verify_round(session, &options, options.queries.items[i], &rounds[i]);

  if (!ok) {
    emit_error("session-or-verification-failed");
    for (size_t i = 0; rounds && i < options.queries.count; i++)
      async_free_public_result(rounds[i].results, rounds[i].emitted);
    free(rounds);
    async_session_destroy(session);
    queries_free(&options.queries);
    return 1;
  }

  uint64_t total_ns = 0;
  uint64_t aggregate = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < options.queries.count; i++) {
    uint64_t query_hash = hash_bytes(UINT64_C(1469598103934665603),
                                     options.queries.items[i],
                                     strlen(options.queries.items[i]));
    total_ns += rounds[i].elapsed_ns;
    aggregate = hash_bytes(aggregate, &rounds[i].checksum,
                           sizeof rounds[i].checksum);
    printf("{\"event\":\"round\",\"protocol\":1,\"round\":%zu,"
           "\"request_id\":%" PRIu64 ",\"query_hash\":\"%016" PRIx64
           "\",\"elapsed_ns\":%" PRIu64 ",\"matched\":%zu,"
           "\"emitted\":%zu,\"items\":%zu,\"filter_only\":%s,"
           "\"checksum\":\"%016" PRIx64 "\",\"verified\":true}\n",
           i + 1, rounds[i].request_id, query_hash, rounds[i].elapsed_ns,
           rounds[i].matched, rounds[i].emitted, rounds[i].pool,
           rounds[i].filter_only ? "true" : "false", rounds[i].checksum);
  }
  printf("{\"event\":\"complete\",\"protocol\":1,\"items\":%zu,"
         "\"rounds\":%zu,\"total_elapsed_ns\":%" PRIu64
         ",\"aggregate_checksum\":\"%016" PRIx64
         "\",\"source_revision\":\"%s\",\"source_build_id\":\"%s\","
         "\"verified\":true}\n",
         item_count, options.queries.count, total_ns, aggregate,
         FZF_NATIVE_SOURCE_REVISION, FZF_NATIVE_SOURCE_BUILD_ID);
  fflush(stdout);

  for (size_t i = 0; i < options.queries.count; i++)
    async_free_public_result(rounds[i].results, rounds[i].emitted);
  free(rounds);
  async_session_destroy(session);
  queries_free(&options.queries);
  return 0;
}
