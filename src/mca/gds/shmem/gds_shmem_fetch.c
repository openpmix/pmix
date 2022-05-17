/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "gds_shmem_fetch.h"
#include "gds_shmem_utils.h"

#include "src/util/pmix_hash.h"

#include "src/mca/bfrops/bfrops.h"
#include "src/mca/pcompress/base/base.h"

/**
 * Fetches all node info from a given nodeinfo.
 */
// TODO(skg) This may be a candidate for shared-memory storage.
static pmix_status_t
fetch_all_node_info(
    char *key,
    pmix_gds_shmem_nodeinfo_t *nodeinfo,
    pmix_list_t *kvs
) {
    size_t i = 0;

    pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
    kv->key = key;
    kv->value = (pmix_value_t *)malloc(sizeof(pmix_value_t));
    if (NULL == kv->value) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOMEM;
    }
    size_t nds = pmix_list_get_size(&nodeinfo->info);
    // TODO(skg) Why increment? And why increment twice if a node has both
    // IDs? Ask Ralph.
    if (NULL != nodeinfo->hostname) {
        ++nds;
    }
    if (UINT32_MAX != nodeinfo->nodeid) {
        ++nds;
    }
    // Create the data array.
    pmix_data_array_t *darray;
    PMIX_DATA_ARRAY_CREATE(darray, nds, PMIX_INFO);
    if (NULL == darray) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOMEM;
    }
    pmix_info_t *dainfo = (pmix_info_t *)darray->array;
    if (NULL != nodeinfo->hostname) {
        PMIX_INFO_LOAD(
            &dainfo[i++], PMIX_HOSTNAME, nodeinfo->hostname, PMIX_STRING
        );
    }
    if (UINT32_MAX != nodeinfo->nodeid) {
        PMIX_INFO_LOAD(
            &dainfo[i++], PMIX_NODEID, &nodeinfo->nodeid, PMIX_UINT32
        );
    }
    pmix_kval_t *kvi;
    PMIX_LIST_FOREACH (kvi, &nodeinfo->info, pmix_kval_t) {
        PMIX_GDS_SHMEM_VOUT(
            "%s:%s adding key=%s", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid), kvi->key
        );
        PMIX_LOAD_KEY(dainfo[i].key, kvi->key);
        pmix_status_t rc = PMIx_Value_xfer(&dainfo[i].value, kvi->value);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_ARRAY_FREE(darray);
            PMIX_RELEASE(kv);
            return rc;
        }
        i++;
    }
    kv->value->data.darray = darray;
    kv->value->type = PMIX_DATA_ARRAY;
    pmix_list_append(kvs, &kv->super);
    return PMIX_SUCCESS;
}

/**
 * Fetches all node info from a given list of node infos.
 */
// TODO(skg) This may be a candidate for shared-memory storage.
static pmix_status_t
fetch_all_node_info_from_list(
    pmix_gds_shmem_job_t *job,
    pmix_list_t *nodeinfos,
    pmix_list_t *kvs
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_gds_shmem_nodeinfo_t *ni;
    PMIX_LIST_FOREACH (ni, nodeinfos, pmix_gds_shmem_nodeinfo_t) {
        char *key = NULL;
        // If the proc's version is earlier than v3.1, then the info must be
        // provided as a data_array with a key of the node's name as earlier
        // versions don't understand node_info arrays.
        if (job->nspace->version.major < 3 ||
            (3 == job->nspace->version.major &&
             0 == job->nspace->version.minor)) {
            if (NULL == ni->hostname) {
                // Skip this one.
                continue;
            }
            key = strdup(ni->hostname);
        }
        else {
            // Everyone else uses a node_info array.
            key = strdup(PMIX_NODE_INFO_ARRAY);
        }
        rc = fetch_all_node_info(key, ni, kvs);
        if (PMIX_SUCCESS != rc) {
            free(key);
            break;
        }
    }
    return rc;
}

pmix_status_t
pmix_gds_shmem_fetch_nodeinfo(
    const char *key,
    pmix_gds_shmem_job_t *job,
    pmix_list_t *nodeinfos,
    pmix_info_t *info,
    size_t ninfo,
    pmix_list_t *kvs
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_nodeinfo_t *nodeinfo = NULL;
    uint32_t nid = UINT32_MAX;
    char *hostname = NULL;
    bool found = false;

    PMIX_GDS_SHMEM_VOUT(
        "%s FETCHING NODE INFO key=%s",
        PMIX_NAME_PRINT(&pmix_globals.myid),
        (NULL == key) ? "NULL" : key
    );
    // Scan for the nodeID or hostname to identify
    // which node they are asking about.
    for (size_t n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_NODEID)) {
            PMIX_VALUE_GET_NUMBER(rc, &info[n].value, nid, uint32_t);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            found = true;
            break;
        }
        else if (PMIX_CHECK_KEY(&info[n], PMIX_HOSTNAME)) {
            hostname = info[n].value.data.string;
            found = true;
            break;
        }
    }
    if (!found) {
        // If the key is NULL, then they want all the info from all nodes.
        if (NULL == key) {
            return fetch_all_node_info_from_list(job, nodeinfos, kvs);
        }
        // Else assume they want it from this node.
        hostname = pmix_globals.hostname;
    }
    // Scan the list of nodes to find the matching entry.
    if (UINT32_MAX != nid) {
        pmix_gds_shmem_nodeinfo_t *ndi;
        PMIX_LIST_FOREACH (ndi, nodeinfos, pmix_gds_shmem_nodeinfo_t) {
            if (UINT32_MAX != ndi->nodeid &&
                nid == ndi->nodeid) {
                nodeinfo = ndi;
                break;
            }
        }
    }
    else if (NULL != hostname) {
        nodeinfo = pmix_gds_shmem_get_nodeinfo_by_nodename(nodeinfos, hostname);
    }

    if (NULL == nodeinfo) {
        if (!found) {
            // They didn't specify, so it is optional.
            return PMIX_ERR_DATA_VALUE_NOT_FOUND;
        }
        return PMIX_ERR_NOT_FOUND;
    }
    // If they want it all, give it to them.
    if (NULL == key) {
        // The key used for the node info.
        char *nikey = NULL;
        // If the proc's version is earlier than v3.1, then the info must be
        // provided as a data_array with a key of the node's name as earlier
        // versions don't understand node_info arrays.
        if (job->nspace->version.major < 3 ||
            (3 == job->nspace->version.major &&
             0 == job->nspace->version.minor)) {
            if (NULL == nodeinfo->hostname) {
                nikey = strdup(pmix_globals.hostname);
            }
            else {
                nikey = strdup(nodeinfo->hostname);
            }
        }
        else {
            // Everyone else uses a node_info array.
            nikey = strdup(PMIX_NODE_INFO_ARRAY);
        }
        rc = fetch_all_node_info(nikey, nodeinfo, kvs);
        if (PMIX_SUCCESS != rc) {
            free(nikey);
        }
        return rc;
    }
    // If we are here, then they want a specific key/value pair. So, scan the
    // info list of this node to find the key they want.
    rc = PMIX_ERR_NOT_FOUND;
    pmix_kval_t *kvi;
    PMIX_LIST_FOREACH (kvi, &nodeinfo->info, pmix_kval_t) {
        if (!PMIX_CHECK_KEY(kvi, key)) {
            continue;
        }
        PMIX_GDS_SHMEM_VOUT(
            "%s:%s: adding key=%s",
            PMIX_NAME_PRINT(&pmix_globals.myid),
            __func__, kvi->key
        );
        // Since they only asked for one key, return just that value.
        pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup(kvi->key);
        kv->value = NULL;
        PMIX_VALUE_XFER(rc, kv->value, kvi->value);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kv);
            return rc;
        }
        pmix_list_append(kvs, &kv->super);
        break;
    }
    return rc;
}

static pmix_status_t
fetch_all_app_info(
    pmix_list_t *apps,
    pmix_list_t *kvs
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_gds_shmem_app_t *appi;
    PMIX_LIST_FOREACH (appi, apps, pmix_gds_shmem_app_t) {
        pmix_data_array_t *darray = NULL;
        size_t i = 0;

        pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            return PMIX_ERR_NOMEM;
        }
        kv->key = strdup(PMIX_APP_INFO_ARRAY);
        kv->value = (pmix_value_t *)malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }
        // TODO(skg) Why +1?
        const size_t nds = pmix_list_get_size(&appi->appinfo) + 1;
        PMIX_DATA_ARRAY_CREATE(darray, nds, PMIX_INFO);
        if (NULL == darray) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }

        pmix_info_t *info = (pmix_info_t *)darray->array;
        // Put in the appnum.
        PMIX_INFO_LOAD(&info[i++], PMIX_APPNUM, &appi->appnum, PMIX_UINT32);
        // Transfer the app infos.
        pmix_kval_t *kvi;
        PMIX_LIST_FOREACH (kvi, &appi->appinfo, pmix_kval_t) {
            PMIX_LOAD_KEY(info[i].key, kvi->key);
            rc = PMIx_Value_xfer(&info[i].value, kvi->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_ARRAY_FREE(darray);
                PMIX_RELEASE(kv);
                return rc;
            }
            i++;
        }
        kv->value->data.darray = darray;
        kv->value->type = PMIX_DATA_ARRAY;
        pmix_list_append(kvs, &kv->super);
    }
    return rc;
}

pmix_status_t
pmix_gds_shmem_fetch_appinfo(
    const char *key,
    pmix_gds_shmem_job_t *job,
    pmix_list_t *target,
    pmix_info_t *info,
    size_t ninfo,
    pmix_list_t *kvs
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_app_t *app = NULL;
    uint32_t appnum = 0;
    bool found = false;

    PMIX_GDS_SHMEM_VOUT(
        "%s FETCHING APP INFO WITH NAPPS=%zd",
        PMIX_NAME_PRINT(&pmix_globals.myid),
        pmix_list_get_size(target)
    );
    // Scan for the appnum to identify which app they are asking about.
    for (size_t n = 0; n < ninfo; n++) {
        if (!PMIX_CHECK_KEY(&info[n], PMIX_APPNUM)) {
            continue;
        }
        PMIX_VALUE_GET_NUMBER(rc, &info[n].value, appnum, uint32_t);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        found = true;
        break;
    }
    if (!found) {
        /* if the key is NULL, then they want all the info from
         * all apps */
        if (NULL == key) {
            rc = fetch_all_app_info(target, kvs);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            return rc;
        }
        // Else assume they are asking for our app.
        appnum = pmix_globals.appnum;
    }
    // Scan the list of apps to find the matching entry.
    pmix_gds_shmem_app_t *appi;
    PMIX_LIST_FOREACH (appi, target, pmix_gds_shmem_app_t) {
        if (appnum == appi->appnum) {
            app = appi;
            break;
        }
    }
    if (NULL == app) {
        return PMIX_ERR_NOT_FOUND;
    }
    // See if they wanted to know something about
    // a node that is associated with this app.
    rc = pmix_gds_shmem_fetch_nodeinfo(
        key, job, &app->nodeinfo, info, ninfo, kvs
    );
    if (PMIX_ERR_DATA_VALUE_NOT_FOUND != rc) {
        return rc;
    }
    // Scan the info list of this app to generate the results.
    rc = PMIX_ERR_NOT_FOUND;
    pmix_kval_t *kvi;
    PMIX_LIST_FOREACH (kvi, &app->appinfo, pmix_kval_t) {
        if (NULL == key || PMIX_CHECK_KEY(kvi, key)) {
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(kvi->key);
            kv->value = NULL;
            PMIX_VALUE_XFER(rc, kv->value, kvi->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                return rc;
            }
            pmix_list_append(kvs, &kv->super);
            rc = PMIX_SUCCESS;
            if (NULL != key) {
                break;
            }
        }
    }
    return rc;
}

/**
 * Fetches session information.
 */
static pmix_status_t
fetch_session_info(
    pmix_gds_shmem_job_t *job,
    const char *key,
    pmix_info_t qualifiers[],
    size_t nqual,
    pmix_list_t *kvs
) {
    PMIX_GDS_SHMEM_VOUT(
        "%s FETCHING PMIX_SESSION_INFO key=%s",
        PMIX_NAME_PRINT(&pmix_globals.myid),
        (NULL == key) ? "NULL" : key
    );

    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_component_t *component = &pmix_mca_gds_shmem_component;
    // They must have provided a session ID.
    for (size_t m = 0; m < nqual; m++) {
        if (!PMIX_CHECK_KEY(&qualifiers[m], PMIX_SESSION_ID)) {
            continue;
        }
        uint32_t sid;
        // See if we have this session.
        PMIX_VALUE_GET_NUMBER(rc, &qualifiers[m].value, sid, uint32_t);
        if (PMIX_SUCCESS != rc) {
            // Didn't provide a correct value.
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        pmix_gds_shmem_session_t *si;
        PMIX_LIST_FOREACH (si, &component->sessions, pmix_gds_shmem_session_t) {
            if (si->id != sid) {
                continue;
            }
            // See if they want info for a specific node.
            rc = pmix_gds_shmem_fetch_nodeinfo(
                key, job, &si->nodeinfo, qualifiers, nqual, kvs
            );
            // If they did, then we are done.
            if (PMIX_ERR_DATA_VALUE_NOT_FOUND != rc) {
                return rc;
            }
            // Check the session info.
            pmix_kval_t *kvi;
            PMIX_LIST_FOREACH (kvi, &si->sessioninfo, pmix_kval_t) {
                if (NULL == key || PMIX_CHECK_KEY(kvi, key)) {
                    pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
                    kv->key = strdup(kvi->key);
                    kv->value = NULL;
                    PMIX_VALUE_XFER(rc, kv->value, kvi->value);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_RELEASE(kv);
                        return rc;
                    }
                    pmix_list_append(kvs, &kv->super);
                    if (NULL != key) {
                        // We are done.
                        return PMIX_SUCCESS;
                    }
                }
            }
        }
    }
    // If we get here, then the session wasn't found.
    return PMIX_ERR_NOT_FOUND;
}

static pmix_status_t
fetch_job_level_info_for_namespace(
    pmix_gds_shmem_job_t *job,
    pmix_info_t qualifiers[],
    size_t nqual,
    pmix_list_t *kvs
) {
    // Fetch all values from the hash table tied to rank=wildcard.
    pmix_status_t rc = pmix_hash_fetch(
        &job->hashtab_internal, PMIX_RANK_WILDCARD, NULL, NULL, 0, kvs
    );
    if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
        return rc;
    }
    // Also need to add any job-level info.
    pmix_kval_t *kvi;
    PMIX_LIST_FOREACH (kvi, &job->jobinfo, pmix_kval_t) {
        pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup(kvi->key);
        kv->value = (pmix_value_t *)malloc(sizeof(pmix_value_t));
        PMIX_VALUE_XFER(rc, kv->value, kvi->value);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(kv);
            return rc;
        }
        pmix_list_append(kvs, &kv->super);
    }
    // Collect the relevant node-level info.
    rc = pmix_gds_shmem_fetch_nodeinfo(
        NULL, job, &job->nodeinfo, qualifiers, nqual, kvs
    );
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    // Collect the relevant app-level info.
    rc = pmix_gds_shmem_fetch_appinfo(
        NULL, job, &job->apps, qualifiers, nqual, kvs
    );
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    // Finally, we need the job-level info for each rank in the job.
    for (pmix_rank_t rank = 0; rank < job->nspace->nprocs; rank++) {
        pmix_list_t rkvs;
        PMIX_CONSTRUCT(&rkvs, pmix_list_t);
        rc = pmix_hash_fetch(
            &job->hashtab_internal, rank, NULL, NULL, 0, &rkvs
        );
        if (PMIX_ERR_NOMEM == rc) {
            PMIX_LIST_DESTRUCT(&rkvs);
            return rc;
        }
        if (0 == pmix_list_get_size(&rkvs)) {
            PMIX_DESTRUCT(&rkvs);
            continue;
        }
        const size_t ninfo = pmix_list_get_size(&rkvs);
        // Setup to return the result.
        pmix_kval_t *kv;
        PMIX_KVAL_NEW(kv, PMIX_PROC_DATA);
        kv->value->type = PMIX_DATA_ARRAY;
        const size_t niptr = ninfo + 1; // Need space for the rank.
        PMIX_DATA_ARRAY_CREATE(kv->value->data.darray, niptr, PMIX_INFO);
        pmix_info_t *iptr = (pmix_info_t *)kv->value->data.darray->array;
        /* start with the rank */
        PMIX_INFO_LOAD(&iptr[0], PMIX_RANK, &rank, PMIX_PROC_RANK);
        // Now transfer rest of data across.
        size_t n = 1;
        pmix_kval_t *kvptr;
        PMIX_LIST_FOREACH(kvptr, &rkvs, pmix_kval_t) {
            PMIX_LOAD_KEY(&iptr[n].key, kvptr->key);
            PMIx_Value_xfer(&iptr[n].value, kvptr->value);
            ++n;
        }
        // Add to the results.
        pmix_list_append(kvs, &kv->super);
        // Release the search result.
        PMIX_LIST_DESTRUCT(&rkvs);
    }
    return rc;
}

pmix_status_t
pmix_gds_shmem_fetch(
    const pmix_proc_t *proc,
    pmix_scope_t scope,
    bool copy,
    const char *key,
    pmix_info_t qualifiers[],
    size_t nqual,
    pmix_list_t *kvs
) {
    PMIX_HIDE_UNUSED_PARAMS(copy);

    pmix_status_t rc = PMIX_SUCCESS;
    pmix_hash_table_t *ht = NULL;
    bool nodeinfo = false;
    bool appinfo = false;
    bool nigiven = false;
    bool apigiven = false;

    PMIX_GDS_SHMEM_VOUT(
        "%s:%s key=%s for proc=%s on scope=%s",
        "fetch", PMIX_NAME_PRINT(&pmix_globals.myid),
        (NULL == key) ? "NULL" : key, PMIX_NAME_PRINT(proc),
        PMIx_Scope_string(scope)
    );
    // Get the tracker for this job. We should have already created one, so
    // that's why we pass false in pmix_gds_shmem_get_job_tracker().
    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(proc->nspace, false, &job);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // If the rank is wildcard and the key is NULL, then the caller is asking
    // for a complete copy of the job-level info for this nspace---retrieve it.
    if (NULL == key && PMIX_RANK_WILDCARD == proc->rank) {
        rc = fetch_job_level_info_for_namespace(job, qualifiers, nqual, kvs);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        return rc;
    }
    // See if they are asking for session, node, or app-level info.
    for (size_t n = 0; n < nqual; n++) {
        if (PMIX_CHECK_KEY(&qualifiers[n], PMIX_SESSION_INFO)) {
            rc = fetch_session_info(job, key, qualifiers, nqual, kvs);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            return rc;
        }
        else if (PMIX_CHECK_KEY(&qualifiers[n], PMIX_NODE_INFO)) {
            nodeinfo = PMIX_INFO_TRUE(&qualifiers[n]);
            nigiven = true;
        }
        else if (PMIX_CHECK_KEY(&qualifiers[n], PMIX_APP_INFO)) {
            appinfo = PMIX_INFO_TRUE(&qualifiers[n]);
            apigiven = true;
        }
    }
    // Check for node/app keys in the absence of corresponding qualifier.
    if (NULL != key && !nigiven && !apigiven) {
        if (pmix_check_node_info(key)) {
            nodeinfo = true;
        }
        else if (pmix_check_app_info(key)) {
            appinfo = true;
        }
    }
    if (!PMIX_RANK_IS_VALID(proc->rank)) {
        if (nodeinfo) {
            rc = pmix_gds_shmem_fetch_nodeinfo(
                key, job, &job->nodeinfo, qualifiers, nqual, kvs
            );
            if (PMIX_SUCCESS != rc && PMIX_RANK_WILDCARD == proc->rank) {
                // Need to check internal as we might have an older peer.
                ht = &job->hashtab_internal;
                goto doover;
            }
            return rc;
        }
        else if (appinfo) {
            rc = pmix_gds_shmem_fetch_appinfo(
                key, job, &job->apps, qualifiers, nqual, kvs
            );
            if (PMIX_SUCCESS != rc && PMIX_RANK_WILDCARD == proc->rank) {
                // Need to check internal as we might have an older peer.
                ht = &job->hashtab_internal;
                goto doover;
            }
            return rc;
        }
    }
    // Fetch from the corresponding hash table. Note that we always provide a
    // copy as we don't support shared-memory.
    // TODO(skg) Support shared-memory.
    if (PMIX_INTERNAL == scope ||
        PMIX_SCOPE_UNDEF == scope ||
        PMIX_GLOBAL == scope ||
        PMIX_RANK_WILDCARD == proc->rank) {
        ht = &job->hashtab_internal;
    }
    else if (PMIX_LOCAL == scope || PMIX_GLOBAL == scope) {
        ht = &job->hashtab_local;
    }
    else if (PMIX_REMOTE == scope) {
        ht = &job->hashtab_remote;
    }
    else {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // TODO(skg) Consider moving away from this doover methodology.
doover:
    // If rank=PMIX_RANK_UNDEF, then we need to search all known ranks for this
    // nspace as any one of them could be the source.
    if (PMIX_RANK_UNDEF == proc->rank) {
        for (pmix_rank_t rnk = 0; rnk < job->nspace->nprocs; rnk++) {
            rc = pmix_hash_fetch(ht, rnk, key, qualifiers, nqual, kvs);
            if (PMIX_ERR_NOMEM == rc) {
                return rc;
            }
            if (PMIX_SUCCESS == rc && NULL != key) {
                return rc;
            }
        }
        // Also need to check any job-level info.
        pmix_kval_t *kvi;
        PMIX_LIST_FOREACH (kvi, &job->jobinfo, pmix_kval_t) {
            if (NULL == key || PMIX_CHECK_KEY(kvi, key)) {
                pmix_kval_t* kv = PMIX_NEW(pmix_kval_t);
                kv->key = strdup(kvi->key);
                kv->value = NULL;
                PMIX_VALUE_XFER(rc, kv->value, kvi->value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_RELEASE(kv);
                    return rc;
                }
                pmix_list_append(kvs, &kv->super);
                if (NULL != key) {
                    break;
                }
            }
        }
        if (NULL == key) {
            // And need to add all job info just in case
            // that was passed via a different GDS component.
            rc = pmix_hash_fetch(
                &job->hashtab_internal, PMIX_RANK_WILDCARD, NULL, NULL, 0, kvs
            );
        }
        else {
            rc = PMIX_ERR_NOT_FOUND;
        }
    }
    else {
        rc = pmix_hash_fetch(ht, proc->rank, key, qualifiers, nqual, kvs);
    }
    if (PMIX_SUCCESS == rc) {
        if (PMIX_GLOBAL == scope) {
            if (ht == &job->hashtab_local) {
                // Need to do this again for the remote data.
                ht = &job->hashtab_remote;
                goto doover;
            }
            else if (ht == &job->hashtab_internal) {
                // Check local.
                ht = &job->hashtab_local;
                goto doover;
            }
        }
    }
    else {
        if (PMIX_GLOBAL == scope || PMIX_SCOPE_UNDEF == scope) {
            if (ht == &job->hashtab_internal) {
                // Need to also try the local data.
                ht = &job->hashtab_local;
                goto doover;
            }
            else if (ht == &job->hashtab_local) {
                // Need to also try the remote data.
                ht = &job->hashtab_remote;
                goto doover;
            }
        }
    }
    if (0 == pmix_list_get_size(kvs)) {
        // If we didn't find it and the rank was valid, then check to see if the
        // data exists in a different scope.  This is done to avoid having the
        // process go into a timeout wait when the data will never appear within
        // the specified scope.
        if (PMIX_RANK_IS_VALID(proc->rank)) {
            if (PMIX_LOCAL == scope) {
                // Check the remote scope.
                rc = pmix_hash_fetch(
                    &job->hashtab_remote, proc->rank,
                    key, qualifiers, nqual, kvs
                );
                if (PMIX_SUCCESS == rc || pmix_list_get_size(kvs) > 0) {
                    pmix_kval_t *kv;
                    while (NULL != (kv = (pmix_kval_t *)pmix_list_remove_first(kvs))) {
                        PMIX_RELEASE(kv);
                    }
                    rc = PMIX_ERR_EXISTS_OUTSIDE_SCOPE;
                }
                else {
                    rc = PMIX_ERR_NOT_FOUND;
                }
            }
            else if (PMIX_REMOTE == scope) {
                // Check the local scope.
                rc = pmix_hash_fetch(
                    &job->hashtab_local, proc->rank,
                    key, qualifiers, nqual, kvs
                );
                if (PMIX_SUCCESS == rc || 0 < pmix_list_get_size(kvs)) {
                    pmix_kval_t *kv;
                    while (NULL != (kv = (pmix_kval_t *)pmix_list_remove_first(kvs))) {
                        PMIX_RELEASE(kv);
                    }
                    rc = PMIX_ERR_EXISTS_OUTSIDE_SCOPE;
                }
                else {
                    rc = PMIX_ERR_NOT_FOUND;
                }
            }
        }
        else {
            rc = PMIX_ERR_NOT_FOUND;
        }
    }
    return rc;
}

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
