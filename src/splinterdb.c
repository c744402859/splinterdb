// Copyright 2021 VMware, Inc.
// SPDX-License-Identifier: Apache-2.0

/*
 *-----------------------------------------------------------------------------
 * splinterdb.c --
 *
 *     Implementation of SplinterDB's public API
 *
 *     The user must provide a data_config that encodes values into messages.
 *     A simple default data_config is available in default_data_config.h
 *
 *-----------------------------------------------------------------------------
 */

#include "platform.h"

#include "clockcache.h"
#include "splinterdb_private.h"
#include "rc_allocator.h"
#include "trunk.h"
#include "btree_private.h"
#include "shard_log.h"
#include "poison.h"

const char *BUILD_VERSION = "splinterdb_build_version " GIT_VERSION;
const char *
splinterdb_get_version()
{
   return BUILD_VERSION;
}

// A data config constructed by this layer, and passed down
// to lower layers.  Keys are fixed-length and functions will be
// called with key-length set to 0.
// This is a temporary shim until variable-length key support lands in trunk.
typedef struct {
   data_config super;
   // This is the data config provided by application, which assumes
   // all keys are variable-length, and the functions will be called
   // with the correct key lengths.
   const data_config *app_data_cfg;
} shim_data_config;

typedef struct splinterdb {
   task_system         *task_sys;
   io_config            io_cfg;
   platform_io_handle   io_handle;
   rc_allocator_config  allocator_cfg;
   rc_allocator         allocator_handle;
   clockcache_config    cache_cfg;
   clockcache           cache_handle;
   shard_log_config     log_cfg;
   allocator_root_id    trunk_id;
   trunk_config         trunk_cfg;
   trunk_handle        *spl;
   platform_heap_handle heap_handle; // for platform_buffer_create
   platform_heap_id     heap_id;
   shim_data_config     shim_data_cfg;
} splinterdb;


/*
 * Extract errno.h -style status int from a platform_status
 *
 * Note this currently relies on the implementation of the splinterdb
 * platform_linux. But at least it doesn't leak the dependency to callers.
 */
static inline int
platform_status_to_int(const platform_status status) // IN
{
   return status.r;
}

static void
splinterdb_config_set_defaults(splinterdb_config *cfg)
{
   if (!cfg->page_size) {
      cfg->page_size = LAIO_DEFAULT_PAGE_SIZE;
   }
   if (!cfg->extent_size) {
      cfg->extent_size = LAIO_DEFAULT_EXTENT_SIZE;
   }
   if (!cfg->io_flags) {
      cfg->io_flags = O_RDWR | O_CREAT;
   }
   if (!cfg->io_perms) {
      cfg->io_perms = 0755;
   }

   if (!cfg->io_async_queue_depth) {
      cfg->io_async_queue_depth = 256;
   }

   if (!cfg->btree_rough_count_height) {
      cfg->btree_rough_count_height = 1;
   }

   if (!cfg->filter_index_size) {
      cfg->filter_index_size = 256;
   }
   if (!cfg->filter_remainder_size) {
      cfg->filter_remainder_size = 6;
   }

   if (!cfg->memtable_capacity) {
      cfg->memtable_capacity = MiB_TO_B(24);
   }
   if (!cfg->fanout) {
      cfg->fanout = 8;
   }
   if (!cfg->max_branches_per_node) {
      cfg->max_branches_per_node = 24;
   }
   if (!cfg->reclaim_threshold) {
      cfg->reclaim_threshold = UINT64_MAX;
   }
}

static platform_status
splinterdb_validate_app_data_config(const data_config *cfg)
{
   platform_assert(cfg->key_size > 0);
   platform_assert(cfg->key_compare != NULL);
   platform_assert(cfg->key_hash != NULL);
   platform_assert(cfg->merge_tuples != NULL);
   platform_assert(cfg->merge_tuples_final != NULL);
   platform_assert(cfg->key_to_string != NULL);
   platform_assert(cfg->message_to_string != NULL);

   if (cfg->key_size > SPLINTERDB_MAX_KEY_SIZE) {
      platform_error_log("Invalid data_config: Specified key_size=%lu cannot "
                         "exceed SPLINTERDB_MAX_KEY_SIZE=%d.\n",
                         cfg->key_size,
                         SPLINTERDB_MAX_KEY_SIZE);
      return STATUS_BAD_PARAM;
   }

   platform_assert(cfg->max_key_length > 0,
                   "length of maximum key must be positive");
   platform_assert(cfg->max_key_length <= cfg->key_size,
                   "length of maximum key=%lu cannot exceed key_size=%lu",
                   cfg->max_key_length,
                   cfg->key_size);
   platform_assert(cfg->min_key_length <= cfg->key_size,
                   "length of minimum key=%lu cannot exceed key_size=%lu",
                   cfg->min_key_length,
                   cfg->key_size);

   int min_max_cmp =
      cfg->key_compare(cfg,
                       slice_create(cfg->min_key_length, cfg->min_key),
                       slice_create(cfg->max_key_length, cfg->max_key));
   platform_assert(min_max_cmp < 0, "min_key must compare < max_key");
   return STATUS_OK;
}

// Variable-length key encoding and decoding virtual functions

// Length-prefix encoding of a variable-sized key (Disk-Resident Structure)
// We do this so that key comparison can be variable-length
typedef struct ONDISK {
   uint8 length;
   uint8 data[0];
} var_len_key_encoding;

static_assert((MAX_KEY_SIZE >= 8), "MAX_KEY_SIZE must be at least 8 bytes");
static_assert((MAX_KEY_SIZE <= 105),
              "Keys larger than 105 bytes are currently not supported");

static_assert((SPLINTERDB_MAX_KEY_SIZE + sizeof(var_len_key_encoding)
               == MAX_KEY_SIZE),
              "Variable-length key encoding header size mismatch");
static_assert((SPLINTERDB_MAX_KEY_SIZE <= UINT8_MAX),
              "Variable-length key support is currently cappted at 255 bytes");

static int
encode_key(uint64 out_key_buffer_len, void *out_key_buffer, slice in_key)
{
   if (slice_length(in_key) > SPLINTERDB_MAX_KEY_SIZE) {
      platform_error_log("splinterdb.encode_key requires "
                         "key_len (%lu) <= SPLINTERDB_MAX_KEY_SIZE (%u)\n",
                         slice_length(in_key),
                         SPLINTERDB_MAX_KEY_SIZE);
      return EINVAL;
   }
   platform_assert(out_key_buffer_len == MAX_KEY_SIZE,
                   "key buffer must always be of size MAX_KEY_SIZE");

   memset(out_key_buffer, 0, out_key_buffer_len);
   var_len_key_encoding *key_enc = (var_len_key_encoding *)out_key_buffer;
   key_enc->length               = (uint8)slice_length(in_key);
   if (slice_length(in_key) > 0) {
      memmove(key_enc->data, slice_data(in_key), slice_length(in_key));
   }
   return 0;
}


static int
splinterdb_shim_key_compare(const data_config *cfg,
                            slice              key1_raw,
                            slice              key2_raw)
{
   var_len_key_encoding *key1 = (var_len_key_encoding *)slice_data(key1_raw);
   var_len_key_encoding *key2 = (var_len_key_encoding *)slice_data(key2_raw);

   platform_assert(key1->length <= SPLINTERDB_MAX_KEY_SIZE);
   platform_assert(key2->length <= SPLINTERDB_MAX_KEY_SIZE);

   shim_data_config  *shim_data_cfg = (shim_data_config *)cfg;
   const data_config *app_cfg       = shim_data_cfg->app_data_cfg;
   return app_cfg->key_compare(app_cfg,
                               slice_create(key1->length, key1->data),
                               slice_create(key2->length, key2->data));
}

static int
splinterdb_shim_merge_tuple(const data_config *cfg,
                            slice              key_raw,
                            message            old_message,
                            merge_accumulator *new_message)
{
   shim_data_config     *shim_data_cfg = (shim_data_config *)cfg;
   const data_config    *app_cfg       = shim_data_cfg->app_data_cfg;
   var_len_key_encoding *key = (var_len_key_encoding *)slice_data(key_raw);

   platform_assert(key->length <= SPLINTERDB_MAX_KEY_SIZE);
   return app_cfg->merge_tuples(
      app_cfg, slice_create(key->length, key->data), old_message, new_message);
}

static int
splinterdb_shim_merge_tuple_final(const data_config *cfg,
                                  slice              key_raw,
                                  merge_accumulator *oldest_message)
{
   shim_data_config     *shim_data_cfg = (shim_data_config *)cfg;
   const data_config    *app_cfg       = shim_data_cfg->app_data_cfg;
   var_len_key_encoding *key = (var_len_key_encoding *)slice_data(key_raw);

   platform_assert(key->length <= SPLINTERDB_MAX_KEY_SIZE);
   return app_cfg->merge_tuples_final(
      app_cfg, slice_create(key->length, key->data), oldest_message);
}

static void
splinterdb_shim_key_to_string(const data_config *cfg,
                              slice              key_raw,
                              char              *str,
                              uint64             max_len)
{
   shim_data_config     *shim_data_cfg = (shim_data_config *)cfg;
   const data_config    *app_cfg       = shim_data_cfg->app_data_cfg;
   var_len_key_encoding *key = (var_len_key_encoding *)slice_data(key_raw);

   platform_assert(key->length <= SPLINTERDB_MAX_KEY_SIZE);
   app_cfg->key_to_string(
      app_cfg, slice_create(key->length, key->data), str, max_len);
}


// create a shim data_config that handles variable-length key encoding
// the output retains a reference to app_cfg
// so the lifetime of app_cfg must be at least as long as out_shim
static int
splinterdb_shim_data_config(const data_config *app_cfg,
                            shim_data_config  *out_shim)
{
   data_config shim = {0};
   shim.key_size    = app_cfg->key_size + sizeof(var_len_key_encoding);

   int rc = encode_key(sizeof(shim.min_key),
                       shim.min_key,
                       slice_create(app_cfg->min_key_length, app_cfg->min_key));
   if (rc != 0) {
      return rc;
   }
   shim.min_key_length = 0; // lower layer ignores this

   rc = encode_key(sizeof(shim.max_key),
                   shim.max_key,
                   slice_create(app_cfg->max_key_length, app_cfg->max_key));
   if (rc != 0) {
      return rc;
   }
   shim.max_key_length = 0; // lower layer ignores this

   shim.key_compare = splinterdb_shim_key_compare;

   // this fn signature doesn't support passing in a data_config, so there's no
   // way to shim it.  This might be a bug, in a corner case, but let's defer
   // it.
   shim.key_hash = app_cfg->key_hash;

   shim.merge_tuples       = splinterdb_shim_merge_tuple;
   shim.merge_tuples_final = splinterdb_shim_merge_tuple_final;
   shim.key_to_string      = splinterdb_shim_key_to_string;

   shim.message_to_string = app_cfg->message_to_string;
   out_shim->super        = shim;
   out_shim->app_data_cfg = app_cfg;
   return 0;
}


/*
 *-----------------------------------------------------------------------------
 * splinterdb_init_config --
 *
 *      Translate splinterdb_config to configs for individual subsystems.
 *
 *      The resulting splinterdb object will retain a reference to data_config
 *      So kvs_cfg->data_config must live at least that long.
 *
 * Results:
 *      STATUS_OK on success, appropriate error on failure.
 *
 * Side effects:
 *      None.
 *-----------------------------------------------------------------------------
 */
static platform_status
splinterdb_init_config(splinterdb_config *kvs_cfg, // IN
                       splinterdb        *kvs      // OUT
)
{
   platform_status rc = STATUS_OK;

   rc = splinterdb_validate_app_data_config(kvs_cfg->data_cfg);
   if (!SUCCESS(rc)) {
      return rc;
   }

   if (kvs_cfg->filename == NULL || kvs_cfg->cache_size == 0
       || kvs_cfg->disk_size == 0)
   {
      platform_error_log(
         "Expect filename, cache_size and disk_size to be set.\n");
      return STATUS_BAD_PARAM;
   }

   // mutable local config block, where we can set defaults
   splinterdb_config cfg = {0};
   memcpy(&cfg, kvs_cfg, sizeof(cfg));
   splinterdb_config_set_defaults(&cfg);

   // this line carries a reference, so kvs_cfg->data_cfg must live
   // at least as long as kvs does
   platform_assert(
      0 == splinterdb_shim_data_config(kvs_cfg->data_cfg, &kvs->shim_data_cfg),
      "error shimming data_config.  This is probably an invalid data_config");

   // Copy over handles to allocated (shared) memory so that in case the
   // system is run using shared memory we can deallocate the shared segment
   // when the Splinter instance is closed.
   kvs->heap_handle = cfg.heap_handle;
   kvs->heap_id     = cfg.heap_id;

   // Null out the memory handles off the config structure so that in the
   // running Splinter instance we are forced to use the memory handles off
   // of 'kvs'. (Also, this allows for a simple usage where application can
   // close and reopen Splinter, and not run into seg-faults due to stale
   // memory handles.)
   kvs_cfg->heap_handle = NULL;
   kvs_cfg->heap_id     = NULL;

   io_config_init(&kvs->io_cfg,
                  cfg.page_size,
                  cfg.extent_size,
                  cfg.io_flags,
                  cfg.io_perms,
                  cfg.io_async_queue_depth,
                  cfg.filename);

   // Validate IO-configuration parameters
   rc = laio_config_valid(&kvs->io_cfg);
   if (!SUCCESS(rc)) {
      return rc;
   }

   platform_default_log(
      "%s(): cfg.disk_size = %lu\n", __FUNCTION__, cfg.disk_size);
   rc_allocator_config_init(&kvs->allocator_cfg, &kvs->io_cfg, cfg.disk_size);

   clockcache_config_init(&kvs->cache_cfg,
                          &kvs->io_cfg,
                          cfg.cache_size,
                          cfg.cache_logfile,
                          cfg.use_stats);

   shard_log_config_init(
      &kvs->log_cfg, &kvs->cache_cfg.super, &kvs->shim_data_cfg.super);

   trunk_config_init(&kvs->trunk_cfg,
                     &kvs->cache_cfg.super,
                     &kvs->shim_data_cfg.super,
                     (log_config *)&kvs->log_cfg,
                     cfg.memtable_capacity,
                     cfg.fanout,
                     cfg.max_branches_per_node,
                     cfg.btree_rough_count_height,
                     cfg.filter_remainder_size,
                     cfg.filter_index_size,
                     cfg.reclaim_threshold,
                     cfg.use_log,
                     cfg.use_stats,
                     FALSE,
                     NULL);
   return STATUS_OK;
}


/*
 * Internal function for create or open
 */
int
splinterdb_create_or_open(splinterdb_config *kvs_cfg,      // IN
                          splinterdb       **kvs_out,      // OUT
                          bool               open_existing // IN
)
{
   splinterdb     *kvs = NULL;
   platform_status status;

   // Allocate a shared segment if so requested. For now, we hard-code
   // the required size big enough to run most tests. Eventually this
   // has to be calculated here based on other run-time params.
   // (Some tests externally create the platform_heap, so we should
   // only create one if it does not already exist.)
   if (kvs_cfg->use_shmem && (kvs_cfg->heap_handle == NULL)) {
      status = platform_heap_create(platform_get_module_id(),
                                    (2 * GiB),
                                    TRUE,
                                    &kvs_cfg->heap_handle,
                                    &kvs_cfg->heap_id);
      if (!SUCCESS(status)) {
         platform_error_log(
            "Shared memory creation failed. "
            "Failed to %s SplinterDB device '%s' with specified "
            "configuration: %s\n",
            (open_existing ? "open existing" : "initialize"),
            kvs_cfg->filename,
            platform_status_to_string(status));
         goto deinit_kvhandle;
      }

      // Setup global tracing booleans for shared memory usage
      if (kvs_cfg->trace_shmem_allocs) {
         Trace_shmem_allocs = TRUE;
      }
      if (kvs_cfg->trace_shmem_frees) {
         Trace_shmem_frees = TRUE;
      }
      if (kvs_cfg->trace_shmem) {
         Trace_shmem_allocs = TRUE;
         Trace_shmem_frees  = TRUE;
      }
   }

   platform_assert(kvs_out != NULL);

   kvs = TYPED_ZALLOC(kvs_cfg->heap_id, kvs);
   if (kvs == NULL) {
      status = STATUS_NO_MEMORY;
      return platform_status_to_int(status);
   }

   // All memory allocation after this call should -ONLY- use heap handles
   // from the handle to the running Splinter instance; i.e. 'kvs'. (The
   // input memory handles in kvs_cfg; i.e. kvs_cfg->heap_id, heap_handle will
   // be NULL'ed out after they are cp'ed to handles in kvs.)
   status = splinterdb_init_config(kvs_cfg, kvs);
   if (!SUCCESS(status)) {
      platform_error_log("Failed to %s SplinterDB device '%s' with specified "
                         "configuration: %s\n",
                         (open_existing ? "open existing" : "initialize"),
                         kvs_cfg->filename,
                         platform_status_to_string(status));
      goto deinit_kvhandle;
   }

   // Now that basic validation of configuration is completed, record the handle
   // to the running splinter in the shared segment created, if any. (This will
   // be used for testing & validation.)
   if (kvs->heap_handle) {
      platform_heap_set_splinterdb_handle(kvs->heap_handle, (void *)kvs);
   }

   status = io_handle_init(
      &kvs->io_handle, &kvs->io_cfg, kvs->heap_handle, kvs->heap_id);
   if (!SUCCESS(status)) {
      platform_error_log("Failed to initialize IO handle: %s\n",
                         platform_status_to_string(status));
      goto deinit_kvhandle;
   }

   uint8 num_bg_threads[NUM_TASK_TYPES] = {0}; // no bg threads

   status = task_system_create(kvs->heap_id,
                               &kvs->io_handle,
                               &kvs->task_sys,
                               TRUE,
                               FALSE,
                               num_bg_threads,
                               trunk_get_scratch_size());
   if (!SUCCESS(status)) {
      platform_error_log(
         "Failed to initialize SplinterDB task system state: %s\n",
         platform_status_to_string(status));
      goto deinit_iohandle;
   }

   if (open_existing) {
      status = rc_allocator_mount(&kvs->allocator_handle,
                                  &kvs->allocator_cfg,
                                  (io_handle *)&kvs->io_handle,
                                  kvs->heap_handle,
                                  kvs->heap_id,
                                  platform_get_module_id());
   } else {
      status = rc_allocator_init(&kvs->allocator_handle,
                                 &kvs->allocator_cfg,
                                 (io_handle *)&kvs->io_handle,
                                 kvs->heap_handle,
                                 kvs->heap_id,
                                 platform_get_module_id());
   }
   if (!SUCCESS(status)) {
      platform_error_log("Failed to %s SplinterDB allocator: %s\n",
                         (open_existing ? "mount existing" : "initialize"),
                         platform_status_to_string(status));
      goto deinit_system;
   }

   status = clockcache_init(&kvs->cache_handle,
                            &kvs->cache_cfg,
                            (io_handle *)&kvs->io_handle,
                            (allocator *)&kvs->allocator_handle,
                            "splinterdb",
                            kvs->heap_handle,
                            kvs->heap_id,
                            platform_get_module_id());
   if (!SUCCESS(status)) {
      platform_error_log("Failed to initialize SplinterDB cache: %s\n",
                         platform_status_to_string(status));
      goto deinit_allocator;
   }

   kvs->trunk_id = 1;
   if (open_existing) {
      kvs->spl = trunk_mount(&kvs->trunk_cfg,
                             (allocator *)&kvs->allocator_handle,
                             (cache *)&kvs->cache_handle,
                             kvs->task_sys,
                             kvs->trunk_id,
                             kvs->heap_id);
   } else {
      kvs->spl = trunk_create(&kvs->trunk_cfg,
                              (allocator *)&kvs->allocator_handle,
                              (cache *)&kvs->cache_handle,
                              kvs->task_sys,
                              kvs->trunk_id,
                              kvs->heap_id);
   }
   if (kvs->spl == NULL) {
      platform_error_log("Failed to %s SplinterDB instance.\n",
                         (open_existing ? "mount existing" : "initialize"));

      // Return a generic 'something went wrong' error
      status = STATUS_INVALID_STATE;
      goto deinit_cache;
   }

   *kvs_out = kvs;
   platform_default_log("Successfully %s SplinterDB instance at '%s'\n",
                        (open_existing ? "mounted existing" : "created new"),
                        kvs_cfg->filename);

   return platform_status_to_int(status);

deinit_cache:
   clockcache_deinit(&kvs->cache_handle);
deinit_allocator:
   rc_allocator_unmount(&kvs->allocator_handle);
deinit_system:
   task_system_destroy(kvs->heap_id, &kvs->task_sys);
deinit_iohandle:
   io_handle_deinit(&kvs->io_handle);
deinit_kvhandle:
   // Depending on the place where a configuration / setup error lead
   // us to here via a 'goto', the heap_id handle, if in use, may be
   // in a different location. Use one carefully, to avoid MSAN-errors.
   platform_free((kvs->heap_id ? kvs->heap_id : kvs_cfg->heap_id), kvs);

   return platform_status_to_int(status);
}

int
splinterdb_create(splinterdb_config *cfg, // IN
                  splinterdb       **kvs  // OUT
)
{
   return splinterdb_create_or_open(cfg, kvs, FALSE);
}

int
splinterdb_open(splinterdb_config *cfg, // IN
                splinterdb       **kvs  // OUT
)
{
   return splinterdb_create_or_open(cfg, kvs, TRUE);
}

/*
 *-----------------------------------------------------------------------------
 * splinterdb_close --
 *
 *      Close a splinterdb, flushing to disk and releasing resources
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *-----------------------------------------------------------------------------
 */
void
splinterdb_close(splinterdb **kvs_in) // IN
{
   splinterdb *kvs = *kvs_in;
   platform_assert(kvs != NULL);

   trunk_unmount(&kvs->spl);
   clockcache_deinit(&kvs->cache_handle);
   rc_allocator_unmount(&kvs->allocator_handle);
   io_handle_deinit(&kvs->io_handle);
   task_system_destroy(kvs->heap_id, &kvs->task_sys);

   platform_heap_destroy(&kvs->heap_handle);
   *kvs_in = (splinterdb *)NULL;
}


/*
 *-----------------------------------------------------------------------------
 * splinterdb_register_thread --
 *
 *      Allocate scratch space and register the current thread.
 *
 *      Any thread, other than the initializing thread, must call this function
 *      exactly once before using the splinterdb.
 *
 *      Notes:
 *      - The task system imposes a limit of MAX_THREADS live at any time
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Allocates memory
 *-----------------------------------------------------------------------------
 */
void
splinterdb_register_thread(splinterdb *kvs) // IN
{
   platform_assert(kvs != NULL);

   size_t scratch_size = trunk_get_scratch_size();
   task_register_this_thread(kvs->task_sys, scratch_size);
}

/*
 *-----------------------------------------------------------------------------
 * splinterdb_deregister_thread --
 *
 *      Free scratch space.
 *      Call this function before exiting a registered thread.
 *      Otherwise, you'll leak memory.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Frees memory
 *-----------------------------------------------------------------------------
 */
void
splinterdb_deregister_thread(splinterdb *kvs)
{
   platform_assert(kvs != NULL);

   task_deregister_this_thread(kvs->task_sys);
}

static int
validate_key_length(const splinterdb *kvs, uint64 key_length)
{
   if (key_length > kvs->shim_data_cfg.app_data_cfg->key_size) {
      platform_error_log("key of size %lu exceeds data_config.key_size %lu",
                         key_length,
                         kvs->shim_data_cfg.app_data_cfg->key_size);
      return EINVAL;
   }
   return 0;
}

/*
 * -------------------------------------------------------------------------
 * External "APIs" provided mainly to invoke lower-level functions intended
 * for use -ONLY- as testing interfaces.
 * -------------------------------------------------------------------------
 */
void
splinterdb_cache_flush(const splinterdb *kvs)
{
   cache_flush(kvs->spl->cc);
}

/*
 * Validate that a key being inserted is within [min, max]-key range.
 */
bool
validate_key_in_range(const splinterdb *kvs, slice key)
{
   const data_config *cfg = kvs->shim_data_cfg.app_data_cfg;

   int cmp_rv = 0;

   // key to-be-inserted should be >= min-key
   cmp_rv = cfg->key_compare(
      cfg, slice_create(cfg->min_key_length, cfg->min_key), key);
   if (cmp_rv > 0) {
      platform_error_log(
         "Key '%s' is less than configured min-key '%s'.\n",
         key_string(cfg, key),
         key_string(cfg, slice_create(cfg->min_key_length, cfg->min_key)));
      return FALSE;
   }

   // key to-be-inserted should be <= max-key
   cmp_rv = cfg->key_compare(
      cfg, key, slice_create(cfg->max_key_length, cfg->max_key));
   if (cmp_rv > 0) {
      platform_error_log(
         "Key '%s' is greater than configured max-key '%s'.\n",
         key_string(cfg, key),
         key_string(cfg, slice_create(cfg->max_key_length, cfg->max_key)));
      return FALSE;
   }
   return TRUE;
}

/*
 *-----------------------------------------------------------------------------
 * splinterdb_insert_raw_message --
 *
 *      Insert a key and a raw message into splinter
 *
 * Results:
 *      0 on success, otherwise an errno
 *
 * Side effects:
 *      None.
 *-----------------------------------------------------------------------------
 */
static int
splinterdb_insert_message(const splinterdb *kvs, // IN
                          slice             key, // IN
                          message           msg  // IN
)
{
   platform_assert(kvs != NULL);
   int rc = validate_key_length(kvs, slice_length(key));
   if (rc != 0) {
      return rc;
   }

   debug_assert(validate_key_in_range(kvs, key),
                "Attempt to insert key outside configured min/max key-range");

   char key_buffer[MAX_KEY_SIZE] = {0};
   rc = encode_key(sizeof(key_buffer), key_buffer, key);
   if (rc != 0) {
      return rc;
   }

   platform_status status = trunk_insert(kvs->spl, key_buffer, msg);
   return platform_status_to_int(status);
}

int
splinterdb_insert(const splinterdb *kvsb, slice key, slice value)
{
   message msg = message_create(MESSAGE_TYPE_INSERT, value);
   return splinterdb_insert_message(kvsb, key, msg);
}

int
splinterdb_delete(const splinterdb *kvsb, slice key)
{
   return splinterdb_insert_message(kvsb, key, DELETE_MESSAGE);
}

int
splinterdb_update(const splinterdb *kvsb, slice key, slice update)
{
   message msg = message_create(MESSAGE_TYPE_UPDATE, update);
   return splinterdb_insert_message(kvsb, key, msg);
}

/*
 *-----------------------------------------------------------------------------
 * _splinterdb_lookup_result structure --
 *-----------------------------------------------------------------------------
 */
typedef struct {
   merge_accumulator value;
} _splinterdb_lookup_result;

_Static_assert(sizeof(_splinterdb_lookup_result)
                  <= sizeof(splinterdb_lookup_result),
               "sizeof(splinterdb_lookup_result) is too small");

_Static_assert(alignof(splinterdb_lookup_result)
                  == alignof(_splinterdb_lookup_result),
               "mismatched alignment for splinterdb_lookup_result");

void
splinterdb_lookup_result_init(const splinterdb         *kvs,        // IN
                              splinterdb_lookup_result *result,     // IN/OUT
                              uint64                    buffer_len, // IN
                              char                     *buffer      // IN
)
{
   _splinterdb_lookup_result *_result = (_splinterdb_lookup_result *)result;
   merge_accumulator_init_with_buffer(&_result->value,
                                      NULL,
                                      buffer_len,
                                      buffer,
                                      WRITABLE_BUFFER_NULL_LENGTH,
                                      MESSAGE_TYPE_INVALID);
}

void
splinterdb_lookup_result_deinit(splinterdb_lookup_result *result) // IN
{
   _splinterdb_lookup_result *_result = (_splinterdb_lookup_result *)result;
   merge_accumulator_deinit(&_result->value);
}

bool
splinterdb_lookup_found(const splinterdb_lookup_result *result) // IN
{
   _splinterdb_lookup_result *_result = (_splinterdb_lookup_result *)result;
   return trunk_lookup_found(&_result->value);
}

int
splinterdb_lookup_result_value(const splinterdb_lookup_result *result, // IN
                               slice                          *value)
{
   _splinterdb_lookup_result *_result = (_splinterdb_lookup_result *)result;

   if (!splinterdb_lookup_found(result)) {
      return EINVAL;
   }

   *value = merge_accumulator_to_value(&_result->value);
   return 0;
}

/*
 *-----------------------------------------------------------------------------
 * splinterdb_lookup --
 *
 *      Lookup a single tuple
 *
 *      result must have been initialized via splinterdb_lookup_result_init()
 *
 *      Use splinterdb_lookup_result_parse to interpret the result
 *
 *      A single result may be used for multiple lookups
 *
 * Results:
 *      0 on success (including key not found), otherwise an error number.
 *      Check for not-found via splinterdb_lookup_result_parse
 *
 * Side effects:
 *      None.
 *-----------------------------------------------------------------------------
 */
int
splinterdb_lookup(const splinterdb         *kvs, // IN
                  slice                     key,
                  splinterdb_lookup_result *result) // IN/OUT
{
   int rc = validate_key_length(kvs, slice_length(key));
   if (rc != 0) {
      return rc;
   }

   platform_status            status;
   _splinterdb_lookup_result *_result = (_splinterdb_lookup_result *)result;

   platform_assert(kvs != NULL);
   char key_buffer[MAX_KEY_SIZE] = {0};

   rc = encode_key(sizeof(key_buffer), key_buffer, key);
   if (rc != 0) {
      return rc;
   }

   status = trunk_lookup(kvs->spl, key_buffer, &_result->value);
   return platform_status_to_int(status);
}


struct splinterdb_iterator {
   trunk_range_iterator sri;
   platform_status      last_rc;
   const splinterdb    *parent;
};

int
splinterdb_iterator_init(const splinterdb     *kvs,      // IN
                         splinterdb_iterator **iter,     // OUT
                         slice                 start_key // IN
)
{
   splinterdb_iterator *it = TYPED_MALLOC(kvs->spl->heap_id, it);
   if (it == NULL) {
      platform_error_log("TYPED_MALLOC error\n");
      return platform_status_to_int(STATUS_NO_MEMORY);
   }
   it->last_rc = STATUS_OK;

   trunk_range_iterator *range_itor = &(it->sri);

   char start_key_buffer[MAX_KEY_SIZE] = {0};
   bool start_key_is_null              = slice_is_null(start_key);
   if (!start_key_is_null) {
      int rc = encode_key(MAX_KEY_SIZE, start_key_buffer, start_key);
      if (rc != 0) {
         return rc;
      }
   }

   platform_status rc =
      trunk_range_iterator_init(kvs->spl,
                                range_itor,
                                (start_key_is_null ? NULL : start_key_buffer),
                                NULL,
                                UINT64_MAX);
   if (!SUCCESS(rc)) {
      trunk_range_iterator_deinit(range_itor);
      platform_free(kvs->spl->heap_id, *iter);
      return platform_status_to_int(rc);
   }
   it->parent = kvs;

   *iter = it;
   return EXIT_SUCCESS;
}

void
splinterdb_iterator_deinit(splinterdb_iterator *iter)
{
   trunk_range_iterator *range_itor = &(iter->sri);
   trunk_range_iterator_deinit(range_itor);

   trunk_handle *spl = range_itor->spl;
   platform_free(spl->heap_id, range_itor);
}

bool
splinterdb_iterator_valid(splinterdb_iterator *kvi)
{
   if (!SUCCESS(kvi->last_rc)) {
      return FALSE;
   }
   bool      at_end;
   iterator *itor = &(kvi->sri.super);
   kvi->last_rc   = iterator_at_end(itor, &at_end);
   if (!SUCCESS(kvi->last_rc)) {
      return FALSE;
   }
   return !at_end;
}

void
splinterdb_iterator_next(splinterdb_iterator *kvi)
{
   iterator *itor = &(kvi->sri.super);
   kvi->last_rc   = iterator_advance(itor);
}

int
splinterdb_iterator_status(const splinterdb_iterator *iter)
{
   return platform_status_to_int(iter->last_rc);
}

void
splinterdb_iterator_get_current(splinterdb_iterator *iter, // IN
                                slice               *key,  // OUT
                                slice               *value // OUT
)
{
   slice     key_slice;
   message   msg;
   iterator *itor = &(iter->sri.super);

   iterator_get_curr(itor, &key_slice, &msg);

   var_len_key_encoding *kenc = (var_len_key_encoding *)(slice_data(key_slice));
   platform_assert(kenc->length <= SPLINTERDB_MAX_KEY_SIZE);

   *key   = slice_create(kenc->length, kenc->data);
   *value = message_slice(msg);
}

void
splinterdb_stats_print_insertion(const splinterdb *kvs)
{
   trunk_print_insertion_stats(Platform_default_log_handle, kvs->spl);
}

void
splinterdb_stats_print_lookup(const splinterdb *kvs)
{
   trunk_print_lookup_stats(Platform_default_log_handle, kvs->spl);
}

void
splinterdb_stats_reset(splinterdb *kvs)
{
   trunk_reset_stats(kvs->spl);

/*
 * -----------------------------------------------------------------------------
 * External accessor APIs, mainly provided for use as testing hooks.
 * -----------------------------------------------------------------------------
 */
void *
splinterdb_get_heap_handle(const splinterdb *kvs)
{
   return (void *)kvs->heap_handle;
}

const void *
splinterdb_get_task_system_handle(const splinterdb *kvs)
{
   return (void *)kvs->task_sys;
}

const void *
splinterdb_get_io_handle(const splinterdb *kvs)
{
   return (void *)&kvs->io_handle;
}

const void *
splinterdb_get_allocator_handle(const splinterdb *kvs)
{
   return (void *)&kvs->allocator_handle;
}

const void *
splinterdb_get_cache_handle(const splinterdb *kvs)
{
   return (void *)&kvs->cache_handle;
}

const void *
splinterdb_get_trunk_handle(const splinterdb *kvs)
{
   return (void *)kvs->spl;
>>>>>>> 20fc47a (Add extern APIs to support new test test_data_structures_handles.)
}
