#include "config.h"
#include "xccl_ucx_lib.h"
#include "reduce.h"
#include "xccl_ucx_sendrecv.h"
#include "utils/mem_component.h"
#include <stdlib.h>
#include <string.h>

#define CALC_DIST(_size, _radix, _dist) do{     \
        _dist = 1;                              \
        while (_dist*_radix < _size) {          \
            _dist*=_radix;                      \
        }                                       \
    }while(0)

#define REQ_PROCESSED ((void*)0x1)

xccl_status_t xccl_ucx_reduce_knomial_progress(xccl_ucx_collreq_t *req)
{
    xccl_ucx_request_t **reqs  = req->reduce_kn.reqs;
    xccl_ucx_team_t *team      = ucs_derived_of(req->team, xccl_ucx_team_t);
    size_t         data_size   = req->args.buffer_info.len;
    int            group_rank  = team->super.params.oob.rank;
    int            group_size  = team->super.params.oob.size;
    int            root        = req->args.root;
    int            radix       = req->reduce_kn.radix;
    int vrank = (group_rank - root + group_size) % group_size;
    int dist  = req->reduce_kn.dist;
    void *dst_buffer, *src_buffer, *scratch;
    int i, vpeer, peer, vroot_at_level, root_at_level, pos;

    scratch    = req->reduce_kn.scratch;
    src_buffer = req->reduce_kn.data_buf;
    dst_buffer = root == group_rank ? req->args.buffer_info.dst_buffer :
        (void*)((ptrdiff_t)scratch + (radix-1)*data_size);

    if (req->reduce_kn.phase > 0) {
        goto poll;
    }

    while (dist <= req->reduce_kn.max_dist) {
        if (vrank % dist == 0) {
            pos = (vrank/dist) % radix;
        } else {
            pos = -1;
        }
        if (pos == 0) {
            for (i=radix-1; i>=1; i--) {
                vpeer = vrank + i*dist;
                if (vpeer < group_size) {
                    peer = (vpeer + root) % group_size;
                    xccl_ucx_recv_nb((void*)((ptrdiff_t)scratch + req->reduce_kn.active_reqs*data_size),
                                     data_size, peer, team, req->tag,
                                     &reqs[req->reduce_kn.active_reqs]);
                    req->reduce_kn.active_reqs++;
                    req->reduce_kn.phase = 1;
                }
            }
        } else if (pos > 0) {
            vroot_at_level = vrank - pos*dist;
            root_at_level  = (vroot_at_level + root) % group_size;
            xccl_ucx_send_nb(src_buffer, data_size, root_at_level,
                            team, req->tag, &reqs[req->reduce_kn.active_reqs++]);
            req->reduce_kn.phase = 2;
            assert(req->reduce_kn.active_reqs == 1);
        }
        dist *= radix;
    poll:
        if (req->reduce_kn.active_reqs) {
            int n_completed;
            int n_polls = 0;
            int max_polls =  TEAM_UCX_CTX(team)->num_to_probe;
            while (n_polls++ < max_polls) {
                n_completed = 0;
                xccl_ucx_progress(team);
                for (i=0; i<req->reduce_kn.active_reqs; i++) {
                    if (REQ_PROCESSED == reqs[i]) {
                        n_completed++;
                        continue;
                    }
                    if (NULL == reqs[i] || reqs[i]->status == XCCL_UCX_REQUEST_DONE) {
                        if (req->reduce_kn.phase == 1) {
                            xccl_mem_component_reduce((void*)((ptrdiff_t)scratch + i*data_size),
                                                      src_buffer, dst_buffer,
                                                      req->args.reduce_info.count,
                                                      req->args.reduce_info.dt,
                                                      req->args.reduce_info.op,
                                                      req->mem_type);
                            req->reduce_kn.data_buf = dst_buffer;
                            src_buffer = dst_buffer;
                        }
                        n_completed++;
                        if (reqs[i]) {
                            xccl_ucx_req_free(reqs[i]);
                        }
                        reqs[i] = REQ_PROCESSED;
                    }
                }
                if (n_completed == req->reduce_kn.active_reqs) {
                    req->reduce_kn.active_reqs = 0;
                    req->reduce_kn.phase       = 0;
                    memset(reqs, 0, req->reduce_kn.active_reqs*sizeof(*reqs));
                    break;
                }
            }
            if (req->reduce_kn.active_reqs) {
                req->reduce_kn.dist = dist;
                return XCCL_OK;
            }
        }
    }
    xccl_mem_component_free(req->reduce_kn.scratch, req->mem_type);
    req->complete = XCCL_OK;
    return XCCL_OK;
}

xccl_status_t xccl_ucx_reduce_knomial_start(xccl_ucx_collreq_t *req)
{
    size_t data_size = req->args.buffer_info.len;
    int group_rank   = req->team->params.oob.rank;
    int group_size   = req->team->params.oob.size;

    xccl_ucx_trace("knomial reduce start");
    memset(req->reduce_kn.reqs, 0, sizeof(req->reduce_kn.reqs));
    req->reduce_kn.radix   = 4;//TODO
    if (req->reduce_kn.radix > group_size) {
        req->reduce_kn.radix = group_size;
    }

    req->reduce_kn.active_reqs = 0;
    req->reduce_kn.phase       = 0;
    req->reduce_kn.dist        = 1;
    req->reduce_kn.data_buf    = req->args.buffer_info.src_buffer;
    CALC_DIST(group_size, req->reduce_kn.radix, req->reduce_kn.max_dist);

    xccl_mem_component_alloc(&req->reduce_kn.scratch,
                             req->reduce_kn.radix*data_size, req->mem_type);
    req->progress = xccl_ucx_reduce_knomial_progress;
    return xccl_ucx_reduce_knomial_progress(req);
}
