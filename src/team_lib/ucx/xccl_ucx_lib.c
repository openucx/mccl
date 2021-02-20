/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2021.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "config.h"
#include "xccl_ucx_lib.h"
#include "xccl_ucx_team.h"
#include "xccl_ucx_context.h"
#include "allgather/allgather.h"
#include "allreduce/allreduce.h"
#include "alltoall/alltoall.h"
#include "alltoallv/alltoallv.h"
#include "reduce/reduce.h"
#include "fanout/fanout.h"
#include "fanin/fanin.h"
#include "bcast/bcast.h"
#include "barrier/barrier.h"
#include "utils/mem_component.h"
#include <ucs/memory/memory_type.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>

static ucs_config_field_t xccl_team_lib_ucx_config_table[] = {
    {"", "", NULL,
        ucs_offsetof(xccl_team_lib_ucx_config_t, super),
        UCS_CONFIG_TYPE_TABLE(xccl_team_lib_config_table)
    },

    {NULL}
};

static ucs_config_field_t xccl_tl_ucx_context_config_table[] = {
    {"", "", NULL,
        ucs_offsetof(xccl_tl_ucx_context_config_t, super),
        UCS_CONFIG_TYPE_TABLE(xccl_tl_context_config_table)
    },

    {"BLOCK_STREAM", "no",
     "Block stream while collective is in progress",
     ucs_offsetof(xccl_tl_ucx_context_config_t, block_stream),
     UCS_CONFIG_TYPE_BOOL
    },

    {"NET_DEVICES", "all",
     "Specifies which network device(s) to use",
     ucs_offsetof(xccl_tl_ucx_context_config_t, devices),
     UCS_CONFIG_TYPE_STRING_ARRAY
    },

    {"BARRIER_KN_RADIX", "4",
     "Radix value for knomial barrier algorithm",
     ucs_offsetof(xccl_tl_ucx_context_config_t, barrier_kn_radix),
     UCS_CONFIG_TYPE_UINT
    },

    {"BCAST_KN_RADIX", "4",
     "Radix value for knomial bcast algorithm",
     ucs_offsetof(xccl_tl_ucx_context_config_t, bcast_kn_radix),
     UCS_CONFIG_TYPE_UINT
    },

    {"REDUCE_KN_RADIX", "4",
     "Radix value for knomial reduce algorithm",
     ucs_offsetof(xccl_tl_ucx_context_config_t, reduce_kn_radix),
     UCS_CONFIG_TYPE_UINT
    },

    {"ALLREDUCE_ALG", "auto",
     "Allreduce algorithm: knomial, sra, auto",
     ucs_offsetof(xccl_tl_ucx_context_config_t, allreduce_alg_id),
     UCS_CONFIG_TYPE_ENUM(xccl_allreduce_alg_names)},

    {"ALLREDUCE_KN_RADIX", "4",
     "Radix value for knomial allreduce algorithm",
     ucs_offsetof(xccl_tl_ucx_context_config_t, allreduce_kn_radix),
     UCS_CONFIG_TYPE_UINT
    },

    {"NUM_TO_PROBE", "10",
     "Number of p2p request tests in a single poll loop",
     ucs_offsetof(xccl_tl_ucx_context_config_t, num_to_probe),
     UCS_CONFIG_TYPE_UINT
    },

    {"ALLTOALL_PAIRWISE_CHUNK", "1",
     "Number of peers processed concurrently in the pairwise exchange alltoall algorithm. "
     "0 - means chunk equals to team size",
     ucs_offsetof(xccl_tl_ucx_context_config_t, alltoall_pairwise_chunk),
     UCS_CONFIG_TYPE_UINT
    },

    {"ALLTOALL_PAIRWISE_REVERSE", "0",
     "Enables reverse ordering in the pairwise exchange alltoall algorithm",
     ucs_offsetof(xccl_tl_ucx_context_config_t, alltoall_pairwise_reverse),
     UCS_CONFIG_TYPE_BOOL
    },

    {"ALLTOALL_PAIRWISE_BARRIER", "0",
     "Number of rounds in alltoall after which barrier posted",
     ucs_offsetof(xccl_tl_ucx_context_config_t, alltoall_pairwise_barrier),
     UCS_CONFIG_TYPE_UINT
    },

    {"PRE_MEM_MAP", "0",
     "register communication buffer with UCX to populate registration cache."
     " 0 - No pre-registtation."
     " 1 - Registration during collective Init "
     " 2 - Registration during the cudaMalloc "
     " 3 - Registration during both collective Init and cudaMalloc ",
     ucs_offsetof(xccl_tl_ucx_context_config_t, pre_mem_map),
     UCS_CONFIG_TYPE_UINT
    },

    {"PPN", "32",
     "Estimated number of processes per node",
     ucs_offsetof(xccl_tl_ucx_context_config_t, ppn),
     UCS_CONFIG_TYPE_UINT
    },

    {NULL}
};

static size_t xccl_ucx_get_buffer_len(xccl_coll_op_args_t *coll_args,
                                      int team_size, int is_src)
{
    size_t length = 0;
    int i;
    uint32_t *counts;
    int dt_len;

    if (is_src) {
        counts = coll_args->buffer_info.src_counts;
        dt_len = xccl_dt_size(coll_args->buffer_info.src_datatype);
    } else {
        counts = coll_args->buffer_info.dst_counts;
        dt_len = xccl_dt_size(coll_args->buffer_info.dst_datatype);
    }

    switch(coll_args->coll_type) {
        case XCCL_ALLTOALLV:
            for (i = 0; i < team_size; i++) {
                length += counts[i];
            }
            length *=  dt_len;
            break;
        case XCCL_ALLTOALL:
            length = coll_args->buffer_info.len * team_size;
            break;
        defaut:
            xccl_ucx_error("not implemented");
    }

    return length;
}

static inline xccl_status_t
xccl_ucx_coll_base_init(xccl_coll_op_args_t *coll_args, xccl_tl_team_t *team,
                        xccl_ucx_collreq_t **request)
{
    xccl_team_lib_ucx_context_t *ctx = ucs_derived_of(team->ctx, xccl_team_lib_ucx_context_t);
    xccl_status_t     status;
    ucs_memory_type_t src_mem_type, dst_mem_type;
    size_t            length;

    /* TODO: where to handle MPI_INPLACE? */
    if ((ptrdiff_t)coll_args->buffer_info.src_buffer == 0x1) {
        coll_args->buffer_info.src_buffer = coll_args->buffer_info.dst_buffer;
    }

    if ((coll_args->coll_type == XCCL_BCAST) ||
        (coll_args->coll_type == XCCL_ALLREDUCE) ||
        (coll_args->coll_type == XCCL_REDUCE) ||
        (coll_args->coll_type == XCCL_ALLTOALL) ||
        (coll_args->coll_type == XCCL_ALLTOALLV) ||
        (coll_args->coll_type == XCCL_ALLGATHER)) {
        status = xccl_mem_component_type(coll_args->buffer_info.src_buffer,
                                         &src_mem_type);
        if (status != XCCL_OK) {
            xccl_ucx_error("memtype detection error");
            return XCCL_ERR_INVALID_PARAM;
        }
        if (coll_args->buffer_info.src_buffer != coll_args->buffer_info.dst_buffer) {
            status = xccl_mem_component_type(coll_args->buffer_info.dst_buffer,
                                             &dst_mem_type);
            if (status != XCCL_OK) {
                xccl_ucx_error("memtype detection error");
                return XCCL_ERR_INVALID_PARAM;
            }
        } else {
            dst_mem_type = src_mem_type;
        }
    }
    xccl_ucx_trace_req("memory types: src %s, dst %s",
                       ucs_memory_type_names[src_mem_type],
                       ucs_memory_type_names[dst_mem_type]);
    //todo malloc ->mpool
    xccl_ucx_collreq_t *req = (xccl_ucx_collreq_t *)malloc(sizeof(*req));
    memcpy(&req->args, coll_args, sizeof(*coll_args));
    req->complete     = XCCL_INPROGRESS;
    req->team         = team;
    req->super.lib    = &xccl_team_lib_ucx.super;
    req->tag          = ((xccl_ucx_team_t*)team)->seq_num++;
    req->src_mem_type = src_mem_type;
    req->dst_mem_type = dst_mem_type;
    req->stream_req   = NULL;

    // for testing
    if (ctx->pre_mem_map == XCCL_TEAM_UCX_COLL_INIT_PRE_MAP ||
        ctx->pre_mem_map == XCCL_TEAM_UCX_COLL_INIT_AND_ALLOC_PRE_MAP) {
        if (coll_args->coll_type == XCCL_ALLTOALLV || coll_args->coll_type == XCCL_ALLTOALL) {
            length = xccl_ucx_get_buffer_len(coll_args, team->params.oob.size, 1);
            xccl_ucx_mem_map(coll_args->buffer_info.src_buffer, length, src_mem_type, ctx);
            if (coll_args->buffer_info.src_buffer != coll_args->buffer_info.dst_buffer) {
                length = xccl_ucx_get_buffer_len(coll_args, team->params.oob.size, 0);
                xccl_ucx_mem_map(coll_args->buffer_info.dst_buffer, length, dst_mem_type, ctx);
            }
        }
    }

    (*request)     = req;
    return XCCL_OK;
}

static inline xccl_status_t
xccl_ucx_allreduce_init(xccl_coll_op_args_t *coll_args,
                        xccl_tl_coll_req_t **request, xccl_tl_team_t *team)
{
    //TODO alg selection for allreduce should happen here
    xccl_ucx_collreq_t          *req;
    xccl_status_t               status;
    xccl_team_lib_ucx_context_t *ctx;
    int                         alg_id;

    status = xccl_ucx_coll_base_init(coll_args, team, &req);
    if (status != XCCL_OK) {
        return status;
    }
    if (req->src_mem_type != req->dst_mem_type) {
        xccl_ucx_error("Different mem types are not supported in allreduce"
                       "(%s, %s)",
                        ucs_memory_type_names[req->src_mem_type],
                        ucs_memory_type_names[req->dst_mem_type]);
        free(req);
        return XCCL_ERR_INVALID_PARAM;
    }
    ctx = ucs_derived_of(team->ctx, xccl_team_lib_ucx_context_t);

    if (!coll_args->alg.set_by_user) {
        if (ctx->allreduce_alg_id == XCCL_UCX_ALLREDUCE_ALG_AUTO) {
            if (coll_args->buffer_info.len < 4096) {
                alg_id = XCCL_UCX_ALLREDUCE_ALG_KNOMIAL;
            } else {
                alg_id = XCCL_UCX_ALLREDUCE_ALG_SRA;
            }
        } else {
            alg_id = ctx->allreduce_alg_id;
        }
    } else {
        alg_id = coll_args->alg.id;
    }

    if (alg_id > XCCL_UCX_ALLREDUCE_ALG_LAST) {
        xccl_ucx_error("wrong allreduce alg (%d)", alg_id);
        free(req);
        return XCCL_ERR_INVALID_PARAM;
    }

    req->start = xccl_ucx_allreduce_start[alg_id];
    (*request) = (xccl_tl_coll_req_t*)&req->super;

    return status;
}

static inline xccl_status_t
xccl_ucx_alltoall_init(xccl_coll_op_args_t *coll_args,
                       xccl_tl_coll_req_t **request, xccl_tl_team_t *team)
{
    xccl_ucx_collreq_t *req;
    xccl_status_t      status = XCCL_OK;

    xccl_ucx_coll_base_init(coll_args, team, &req);
    if (!coll_args->alg.set_by_user) {
        //TODO alg selection for alltoall should happen here
        if (coll_args->buffer_info.src_buffer == coll_args->buffer_info.dst_buffer) {
            req->start = xccl_ucx_alltoall_linear_shift_start;
        } else {
            req->start = xccl_ucx_alltoall_pairwise_start;
        }
    } else {
        switch (coll_args->alg.id) {
            case 0:
                req->start = xccl_ucx_alltoall_pairwise_start;
                break;
            case 1:
                req->start = xccl_ucx_alltoall_linear_shift_start;
                break;
            default:
                free(req);
                req = NULL;
                status = XCCL_ERR_INVALID_PARAM;
        }
    }
    (*request) = (xccl_tl_coll_req_t*)&req->super;
    return status;
}

static inline xccl_status_t
xccl_ucx_alltoallv_init(xccl_coll_op_args_t *coll_args,
                        xccl_tl_coll_req_t **request, xccl_tl_team_t *team)
{
    xccl_ucx_collreq_t *req;
    xccl_status_t      status = XCCL_OK;

    xccl_ucx_coll_base_init(coll_args, team, &req);
    if (!coll_args->alg.set_by_user) {
        //TODO alg selection for alltoallv should happen here
        if (coll_args->buffer_info.src_buffer == coll_args->buffer_info.dst_buffer) {
            xccl_ucx_warn("Inplace alltoallv isn't supported");
            free(req);
            req = NULL;
            status = XCCL_ERR_UNSUPPORTED;
        } else {
            req->start = xccl_ucx_alltoallv_pairwise_start;
        }
    } else {
        switch (coll_args->alg.id) {
            case 0:
                req->start = xccl_ucx_alltoallv_pairwise_start;
                break;
            default:
                free(req);
                req = NULL;
                status = XCCL_ERR_INVALID_PARAM;
        }
    }
    (*request) = (xccl_tl_coll_req_t*)&req->super;
    return status;
}

static inline xccl_status_t
xccl_ucx_allgather_init(xccl_coll_op_args_t *coll_args,
                        xccl_tl_coll_req_t **request, xccl_tl_team_t *team)
{
    xccl_ucx_collreq_t *req;
    xccl_status_t      status = XCCL_OK;

    xccl_ucx_coll_base_init(coll_args, team, &req);
    if (!coll_args->alg.set_by_user) {
        //TODO alg selection for allgather should happen here
        req->start = xccl_ucx_allgather_ring_start;
    } else {
        switch (coll_args->alg.id) {
            case 0:
                req->start = xccl_ucx_allgather_ring_start;
                break;
            default:
                free(req);
                req = NULL;
                status = XCCL_ERR_INVALID_PARAM;
        }
    }
    (*request) = (xccl_tl_coll_req_t*)&req->super;
    return status;
}

static inline xccl_status_t
xccl_ucx_reduce_init(xccl_coll_op_args_t *coll_args,
                     xccl_tl_coll_req_t **request, xccl_tl_team_t *team)
{
    //TODO alg selection for allreduce should happen here
    xccl_ucx_collreq_t *req;
    xccl_status_t status = XCCL_OK;

    status = xccl_ucx_coll_base_init(coll_args, team, &req);
    if (status != XCCL_OK) {
        return status;
    }

    if (req->src_mem_type != req->dst_mem_type) {
        xccl_ucx_error("Different mem types are not supported in reduce"
                       "(%s, %s)",
                        ucs_memory_type_names[req->src_mem_type],
                        ucs_memory_type_names[req->dst_mem_type]);
        free(req);
        return XCCL_ERR_INVALID_PARAM;
    }

    if (!coll_args->alg.set_by_user) {
        /* Automatic algorithm selection - take knomial */
        req->start = xccl_ucx_reduce_knomial_start;
    } else {
        switch (coll_args->alg.id) {
        case 0:
            req->start = xccl_ucx_reduce_linear_start;
            break;
        case 1:
            req->start = xccl_ucx_reduce_knomial_start;
            break;
        default:
            free(req);
            req = NULL;
            status = XCCL_ERR_INVALID_PARAM;
        }
    }
    (*request) = &req->super;
    return XCCL_OK;
}

static inline xccl_status_t
xccl_ucx_fanin_init(xccl_coll_op_args_t *coll_args,
                    xccl_tl_coll_req_t **request, xccl_tl_team_t *team)
{
    //TODO alg selection for allreduce should happen here
    xccl_ucx_collreq_t *req;
    xccl_ucx_coll_base_init(coll_args, team, &req);
    req->start = xccl_ucx_fanin_linear_start;
    (*request) = (xccl_tl_coll_req_t*)&req->super;
    return XCCL_OK;
}

static inline xccl_status_t
xccl_ucx_fanout_init(xccl_coll_op_args_t *coll_args,
                     xccl_tl_coll_req_t **request, xccl_tl_team_t *team)
{
    //TODO alg selection for allreduce should happen here
    xccl_ucx_collreq_t *req;
    xccl_ucx_coll_base_init(coll_args, team, &req);
    req->start = xccl_ucx_fanout_linear_start;
    (*request) = (xccl_tl_coll_req_t*)&req->super;
    return XCCL_OK;
}

static inline xccl_status_t
xccl_ucx_bcast_init(xccl_coll_op_args_t *coll_args,
                    xccl_tl_coll_req_t **request, xccl_tl_team_t *team)
{
    //TODO alg selection for allreduce should happen here
    xccl_ucx_collreq_t *req;
    xccl_status_t status = XCCL_OK;
    xccl_ucx_coll_base_init(coll_args, team, &req);
    if (!coll_args->alg.set_by_user) {
        /* Automatic algorithm selection - take knomial */
        req->start = xccl_ucx_bcast_knomial_start;
    } else {
        switch (coll_args->alg.id) {
        case 0:
            req->start = xccl_ucx_bcast_linear_start;
            break;
        case 1:
            req->start = xccl_ucx_bcast_knomial_start;
            break;
        default:
            free(req);
            req = NULL;
            status = XCCL_ERR_INVALID_PARAM;
        }
    }
    (*request) = (xccl_tl_coll_req_t*)&req->super;
    return status;
}

static inline xccl_status_t
xccl_ucx_barrier_init(xccl_coll_op_args_t *coll_args,
                      xccl_tl_coll_req_t **request, xccl_tl_team_t *team)
{
    //TODO alg selection for allreduce should happen here
    xccl_ucx_collreq_t *req;
    xccl_ucx_coll_base_init(coll_args, team, &req);
    req->start = xccl_ucx_barrier_knomial_start;
    (*request) = (xccl_tl_coll_req_t*)&req->super;
    return XCCL_OK;
}

static xccl_status_t
xccl_ucx_collective_init(xccl_coll_op_args_t *coll_args,
                         xccl_tl_coll_req_t **request, xccl_tl_team_t *team)
{
    switch (coll_args->coll_type) {
    case XCCL_ALLGATHER:
        return xccl_ucx_allgather_init(coll_args, request, team);
    case XCCL_ALLREDUCE:
        return xccl_ucx_allreduce_init(coll_args, request, team);
    case XCCL_ALLTOALL:
        return xccl_ucx_alltoall_init(coll_args, request, team);
    case XCCL_ALLTOALLV:
        return xccl_ucx_alltoallv_init(coll_args, request, team);
    case XCCL_BARRIER:
        return xccl_ucx_barrier_init(coll_args, request, team);
    case XCCL_REDUCE:
        return xccl_ucx_reduce_init(coll_args, request, team);
    case XCCL_BCAST:
        return xccl_ucx_bcast_init(coll_args, request, team);
    case XCCL_FANIN:
        return xccl_ucx_fanin_init(coll_args, request, team);
    case XCCL_FANOUT:
        return xccl_ucx_fanout_init(coll_args, request, team);
    }
    return XCCL_ERR_INVALID_PARAM;
}

static xccl_status_t xccl_ucx_collective_post(xccl_tl_coll_req_t *request)
{
    xccl_ucx_collreq_t *req = ucs_derived_of(request, xccl_ucx_collreq_t);
    xccl_status_t st;

    req->stream_req = NULL;
    if (req->args.field_mask & XCCL_COLL_OP_ARGS_FIELD_STREAM) {
        if (TEAM_UCX_CTX_REQ(req)->block_stream) {
            st = xccl_mem_component_start_acitivity(&req->args.stream,
                                                    &req->stream_req);
            if (st != XCCL_OK) {
                return st;
            }
            st = xccl_mem_component_query_activity(req->stream_req);
            if (st == XCCL_INITIALIZED) {
                /* collective is not ready to start, start it later*/
                /* assign dummy value to ready_to_start*/
                req->ready_to_start = (void*)0x1;
                return XCCL_OK;
            }
            if (st != XCCL_INPROGRESS) {
                return st;
            }
        } else {
            st = xccl_mc_event_record(&req->args.stream, &req->ready_to_start);
            if (st != XCCL_OK) {
                return st;
            }
            st = xccl_mc_event_query(req->ready_to_start);
            if (st == XCCL_INPROGRESS) {
                /* collective is not ready to start, start it later*/
                return XCCL_OK;
            }
            if (st != XCCL_OK) {
                return st;
            }
            xccl_mc_event_free(req->ready_to_start);
        }
    }
    req->ready_to_start = NULL;
    return req->start(req);
}

static xccl_status_t xccl_ucx_collective_test(xccl_tl_coll_req_t *request)
{
    xccl_ucx_collreq_t *req = ucs_derived_of(request, xccl_ucx_collreq_t);
    xccl_status_t status;

    if (req->ready_to_start != NULL) {
        if (TEAM_UCX_CTX_REQ(req)->block_stream) {
            status = xccl_mem_component_query_activity(req->stream_req);
            /* status can't be XCCL_OK since collective wasn't started*/
            assert(status != XCCL_OK);
            if (status == XCCL_INITIALIZED) {
                return XCCL_INPROGRESS;
            } else if (status != XCCL_INPROGRESS) {
                /* error */
                return status;
            }
        } else {
            status = xccl_mc_event_query(req->ready_to_start);
            if (status != XCCL_OK) {
                return status;
            }
            xccl_mc_event_free(req->ready_to_start);
        }
        req->ready_to_start = NULL;
        req->start(req);
    }
    if (XCCL_INPROGRESS == req->complete) {
        if (XCCL_OK != (status = req->progress(req))) {
            return status;
        };
    }
    if ((XCCL_INPROGRESS != req->complete) &&
        (req->stream_req != NULL)) {
        xccl_mem_component_finish_acitivity(req->stream_req);
        req->stream_req = NULL;
    }

    return req->complete;
}

static xccl_status_t xccl_ucx_collective_wait(xccl_tl_coll_req_t *request)
{
    xccl_ucx_collreq_t *req = ucs_derived_of(request, xccl_ucx_collreq_t);
    xccl_status_t status;

    do {
        status = xccl_ucx_collective_test(request);
    } while (status == XCCL_INPROGRESS);

    return status;
}

static xccl_status_t xccl_ucx_collective_finalize(xccl_tl_coll_req_t *request)
{
    free(request);
    return XCCL_OK;
}

static xccl_status_t xccl_ucx_lib_open(xccl_team_lib_h self,
                                       xccl_team_lib_config_t *config) {
    xccl_team_lib_ucx_t        *tl  = ucs_derived_of(self, xccl_team_lib_ucx_t);
    xccl_team_lib_ucx_config_t *cfg = ucs_derived_of(config, xccl_team_lib_ucx_config_t);

    tl->log_component.log_level = cfg->super.log_component.log_level;
    sprintf(tl->log_component.name, "%s", "TEAM_UCX");
    xccl_ucx_debug("Team UCX opened");
    if (cfg->super.priority == -1) {
        tl->super.priority = 10;
    } else {
        tl->super.priority = cfg->super.priority;
    }

    return XCCL_OK;
}

static xccl_status_t xccl_ucx_lib_query(xccl_team_lib_h lib, xccl_tl_attr_t *tl_attr) {
    int    num_ibv_devices;
    struct ibv_device **device_list;
    char   (*devices)[16];
    int    i, rc, p;

    if (tl_attr->field_mask & XCCL_TL_ATTR_FIELD_CONTEXT_CREATE_MODE) {
        tl_attr->context_create_mode = XCCL_TEAM_LIB_CONTEXT_CREATE_MODE_LOCAL;
    }

    return XCCL_OK;
}

xccl_team_lib_ucx_t xccl_team_lib_ucx = {
    .super.name                  = "ucx",
    .super.id                    = XCCL_TL_UCX,
    .super.priority              = 10,
    .super.team_lib_config       = {
        .name                    = "UCX team library",
        .prefix                  = "TEAM_UCX_",
        .table                   = xccl_team_lib_ucx_config_table,
        .size                    = sizeof(xccl_team_lib_ucx_config_t),
    },
    .super.tl_context_config     = {
        .name                    = "UCX tl context",
        .prefix                  = "TEAM_UCX_",
        .table                   = xccl_tl_ucx_context_config_table,
        .size                    = sizeof(xccl_tl_ucx_context_config_t),
    },
    .super.team_lib_open         = xccl_ucx_lib_open,
    .super.team_lib_query        = xccl_ucx_lib_query,
    .super.params.reproducible   = XCCL_REPRODUCIBILITY_MODE_NON_REPRODUCIBLE,
    .super.params.thread_mode    = XCCL_THREAD_MODE_SINGLE |
                                   XCCL_THREAD_MODE_MULTIPLE,
    .super.params.team_usage     = XCCL_LIB_PARAMS_TEAM_USAGE_SW_COLLECTIVES,
    .super.params.coll_types     = XCCL_COLL_CAP_BARRIER | XCCL_COLL_CAP_FANIN |
                                   XCCL_COLL_CAP_FANOUT | XCCL_COLL_CAP_REDUCE |
                                   XCCL_COLL_CAP_BCAST | XCCL_COLL_CAP_ALLREDUCE |
                                   XCCL_COLL_CAP_ALLTOALL | XCCL_COLL_CAP_ALLTOALLV |
                                   XCCL_COLL_CAP_ALLGATHER,
    .super.mem_types             = UCS_BIT(UCS_MEMORY_TYPE_HOST) |
                                   UCS_BIT(UCS_MEMORY_TYPE_CUDA),
    .super.ctx_create_mode       = XCCL_TEAM_LIB_CONTEXT_CREATE_MODE_LOCAL,
    .super.team_context_create   = xccl_ucx_create_context,
    .super.team_context_progress = NULL,
    .super.team_context_destroy  = xccl_ucx_destroy_context,
    .super.team_create_post      = xccl_ucx_team_create_post,
    .super.team_create_test      = xccl_ucx_team_create_test,
    .super.team_destroy          = xccl_ucx_team_destroy,
    .super.collective_init       = xccl_ucx_collective_init,
    .super.collective_post       = xccl_ucx_collective_post,
    .super.collective_wait       = xccl_ucx_collective_wait,
    .super.collective_test       = xccl_ucx_collective_test,
    .super.collective_finalize   = xccl_ucx_collective_finalize,
    .super.global_mem_map_start  = NULL,
    .super.global_mem_map_test   = NULL,
    .super.global_mem_unmap      = NULL,
};

void xccl_ucx_send_completion_cb(void *request, ucs_status_t status,
                                 void *user_data)
{
    xccl_ucx_request_t *req = (xccl_ucx_request_t*)request;
    req->status = XCCL_UCX_REQUEST_DONE;
}

void xccl_ucx_recv_completion_cb(void* request, ucs_status_t status,
                                 const ucp_tag_recv_info_t *info,
                                 void *user_data)
{
    xccl_ucx_request_t *req = (xccl_ucx_request_t*)request;
    req->status = XCCL_UCX_REQUEST_DONE;
}
