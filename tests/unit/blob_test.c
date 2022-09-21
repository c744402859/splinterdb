// Copyright 2021 VMware, Inc.
// SPDX-License-Identifier: Apache-2.0

/*
 * -----------------------------------------------------------------------------
 * blob_test.c --
 *
 *  Exercises the blob interfaces.
 * -----------------------------------------------------------------------------
 */
#include "ctest.h" // This is required for all test-case files.
#include "cache_test_common.h"
#include "test_data.h"
#include "blob_build.h"

/*
 * Global data declaration macro:
 */
CTEST_DATA(blob)
{
   master_config       master_cfg;
   data_config        *data_cfg;
   io_config           io_cfg;
   rc_allocator_config allocator_cfg;
   clockcache_config   cache_cfg;

   platform_heap_handle hh;
   platform_heap_id     hid;
   platform_io_handle   io;
   rc_allocator         al;
   clockcache           clock_cache;
};

blob_build_config cfg = {.extent_batch  = 0,
                         .page_batch    = 1,
                         .subpage_batch = 2,
                         .alignment     = 0};

// Optional setup function for suite, called before every test in suite
CTEST_SETUP(blob)
{
   config_set_defaults(&data->master_cfg);
   data->data_cfg = test_data_config;
   data->hid      = platform_get_heap_id();

   if (!SUCCESS(
          config_parse(&data->master_cfg, 1, Ctest_argc, (char **)Ctest_argv))
       || !init_data_config_from_master_config(data->data_cfg,
                                               &data->master_cfg)
       || !init_io_config_from_master_config(&data->io_cfg, &data->master_cfg)
       || !init_rc_allocator_config_from_master_config(
          &data->allocator_cfg, &data->master_cfg, &data->io_cfg)
       || !init_clockcache_config_from_master_config(
          &data->cache_cfg, &data->master_cfg, &data->io_cfg))
   {
      ASSERT_TRUE(FALSE, "Failed to parse args\n");
   }

   platform_module_id mid = platform_get_module_id();

   platform_status rc;

   rc = platform_heap_create(mid, 2 * GiB, &data->hh, &data->hid);
   platform_assert_status_ok(rc);

   rc = io_handle_init(&data->io, &data->io_cfg, data->hh, data->hid);
   platform_assert_status_ok(rc);

   rc_allocator_init(&data->al,
                     &data->allocator_cfg,
                     (io_handle *)&data->io,
                     data->hh,
                     data->hid,
                     mid);

   rc = clockcache_init(&data->clock_cache,
                        &data->cache_cfg,
                        (io_handle *)&data->io,
                        (allocator *)&data->al,
                        "test",
                        data->hh,
                        data->hid,
                        mid);
   platform_assert_status_ok(rc);
}

// Optional teardown function for suite, called after every test in suite
CTEST_TEARDOWN(blob)
{
   clockcache_deinit(&data->clock_cache);

   allocator *alp = (allocator *)&data->al;
   allocator_assert_noleaks(alp);
   rc_allocator_deinit(&data->al);

   io_handle_deinit(&data->io);

   platform_heap_destroy(&data->hh);
}

CTEST2(blob, build_unkeyed)
{
   mini_allocator  src;
   uint64          src_addr;
   mini_allocator  dst;
   uint64          dst_addr;
   platform_status rc;

   rc = allocator_alloc((allocator *)&data->al, &src_addr, PAGE_TYPE_MISC);
   platform_assert_status_ok(rc);

   rc = allocator_alloc((allocator *)&data->al, &dst_addr, PAGE_TYPE_MISC);
   platform_assert_status_ok(rc);

   mini_init(&src,
             (cache *)&data->clock_cache,
             data->data_cfg,
             src_addr,
             0,
             NUM_BLOB_BATCHES,
             PAGE_TYPE_MISC,
             FALSE);

   mini_init(&dst,
             (cache *)&data->clock_cache,
             data->data_cfg,
             dst_addr,
             0,
             NUM_BLOB_BATCHES,
             PAGE_TYPE_MISC,
             FALSE);

   writable_buffer original;
   writable_buffer blob;
   writable_buffer clone;
   writable_buffer materialized;

   writable_buffer_init(&original, NULL);
   writable_buffer_init(&blob, NULL);
   writable_buffer_init(&clone, NULL);
   writable_buffer_init(&materialized, NULL);

   for (int i = 0; i < 1000; i++) {
      for (int j = 0;
           j < 7 * cache_page_size((cache *)&data->clock_cache) / 10 / 19;
           j++)
      {
         writable_buffer_append(&original, 19, "this test is great!");
      }

      rc = blob_build(&cfg,
                      (cache *)&data->clock_cache,
                      &src,
                      NULL_SLICE,
                      writable_buffer_to_slice(&original),
                      PAGE_TYPE_MISC,
                      &blob);
      platform_assert_status_ok(rc);

      platform_assert(blob_length(writable_buffer_to_slice(&blob))
                      == writable_buffer_length(&original));

      rc = blob_materialize((cache *)&data->clock_cache,
                            writable_buffer_to_slice(&blob),
                            0,
                            blob_length(writable_buffer_to_slice(&blob)),
                            PAGE_TYPE_MISC,
                            &materialized);
      platform_assert_status_ok(rc);

      platform_assert(slice_lex_cmp(writable_buffer_to_slice(&original),
                                    writable_buffer_to_slice(&materialized))
                      == 0);

      rc = blob_clone(&cfg,
                      (cache *)&data->clock_cache,
                      &dst,
                      NULL_SLICE,
                      writable_buffer_to_slice(&blob),
                      PAGE_TYPE_MISC,
                      PAGE_TYPE_MISC,
                      &clone);
      platform_assert_status_ok(rc);

      rc = blob_materialize((cache *)&data->clock_cache,
                            writable_buffer_to_slice(&clone),
                            0,
                            blob_length(writable_buffer_to_slice(&clone)),
                            PAGE_TYPE_MISC,
                            &materialized);
      platform_assert_status_ok(rc);

      platform_assert(slice_lex_cmp(writable_buffer_to_slice(&original),
                                    writable_buffer_to_slice(&materialized))
                      == 0);
   }

   writable_buffer_deinit(&original);
   writable_buffer_deinit(&blob);
   writable_buffer_deinit(&clone);
   writable_buffer_deinit(&materialized);


   mini_release(&src, NULL_SLICE);
   mini_unkeyed_dec_ref(
      (cache *)&data->clock_cache, src_addr, PAGE_TYPE_MISC, FALSE);

   mini_release(&dst, NULL_SLICE);
   mini_unkeyed_dec_ref(
      (cache *)&data->clock_cache, dst_addr, PAGE_TYPE_MISC, FALSE);
}
