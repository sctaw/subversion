/*
 * textbase.c:  wrappers around wc text-base functionality
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include "svn_path.h"

#include "private/svn_wc_private.h"

#include "client.h"

/* A baton for use with textbase_hydrate_cb(). */
typedef struct textbase_hydrate_baton_t
{
  apr_pool_t *result_pool;
  const char *base_abspath;
  svn_client_ctx_t *ctx;
  svn_ra_session_t *ra_session;
} textbase_hydrate_baton_t;

/* Implements svn_wc__textbase_hydrate_cb_t. */
static svn_error_t *
textbase_hydrate_cb(void *baton,
                    const char *repos_root_url,
                    const char *repos_relpath,
                    svn_revnum_t revision,
                    svn_stream_t *contents,
                    svn_cancel_func_t cancel_func,
                    void *cancel_baton,
                    apr_pool_t *scratch_pool)
{
  struct textbase_hydrate_baton_t *b = baton;
  const char *url;
  const char *old_url;

  url = svn_path_url_add_component2(repos_root_url, repos_relpath,
                                    scratch_pool);

  if (!b->ra_session)
    {
      svn_ra_session_t *session;

      SVN_ERR(svn_client__open_ra_session_internal(&session, NULL,
                                                   url, b->base_abspath,
                                                   NULL, TRUE, TRUE, b->ctx,
                                                   b->result_pool, scratch_pool));
      b->ra_session = session;
    }

  if (b->ctx->notify_func2)
    {
      svn_wc_notify_t *notify
        = svn_wc_create_notify(".", svn_wc_notify_hydrating_file,
                               scratch_pool);
      notify->revision = revision;
      notify->url = url;
      b->ctx->notify_func2(b->ctx->notify_baton2, notify, scratch_pool);
    }

  SVN_ERR(svn_client__ensure_ra_session_url(&old_url, b->ra_session, url,
                                            scratch_pool));
  SVN_ERR(svn_ra_fetch_file_contents(b->ra_session, "", revision, contents,
                                     scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__textbase_sync(svn_ra_session_t **ra_session_p,
                          const char *local_abspath,
                          svn_boolean_t allow_hydrate,
                          svn_boolean_t allow_dehydrate,
                          svn_client_ctx_t *ctx,
                          svn_ra_session_t *ra_session,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  textbase_hydrate_baton_t baton = {0};
  const char *old_session_url = NULL;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  /* A caller may want to reuse the RA session that we open internally.
     If that's the case, use the result pool.  Otherwise, the session
     is temporary, so use the scratch pool. */
  if (ra_session_p)
    baton.result_pool = result_pool;
  else
    baton.result_pool = scratch_pool;

  baton.base_abspath = local_abspath;
  baton.ctx = ctx;
  baton.ra_session = ra_session;

  if (ctx->notify_func2 && allow_hydrate)
    {
      svn_wc_notify_t *notify
        = svn_wc_create_notify(local_abspath, svn_wc_notify_hydrating_start,
                               scratch_pool);
      ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);
    }

  if (ra_session)
    SVN_ERR(svn_ra_get_session_url(ra_session, &old_session_url, scratch_pool));

  SVN_ERR(svn_wc__textbase_sync(ctx->wc_ctx, local_abspath,
                                allow_hydrate, allow_dehydrate,
                                textbase_hydrate_cb, &baton,
                                ctx->cancel_func, ctx->cancel_baton,
                                scratch_pool));

  if (ra_session)
    SVN_ERR(svn_ra_reparent(ra_session, old_session_url, scratch_pool));

  if (ctx->notify_func2 && allow_hydrate)
    {
      svn_wc_notify_t *notify
        = svn_wc_create_notify(local_abspath, svn_wc_notify_hydrating_end,
                               scratch_pool);
      ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);
    }

  if (ra_session_p)
    *ra_session_p = baton.ra_session;

  return SVN_NO_ERROR;
}
