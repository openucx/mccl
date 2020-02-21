#include "config.h"
#include "tccl_ucx_lib.h"
#include "bcast.h"
#include "tccl_ucx_sendrecv.h"
#include <stdlib.h>
#include <string.h>

#define CALC_DIST(_size, _radix, _dist) do{     \
        _dist = 1;                              \
        while (_dist*_radix < _size) {          \
            _dist*=_radix;                      \
        }                                       \
    }while(0)

tccl_status_t tccl_ucx_bcast_knomial_progress(tccl_ucx_collreq_t *req)
{
    tccl_team_h team   = req->team;
    void *data_buffer = req->args.buffer_info.dst_buffer;
    size_t data_size  = req->args.buffer_info.len;
    int group_rank    = team->oob.rank;
    int group_size    = team->oob.size;
    int root          = req->args.root;
    int radix         = req->bcast_kn.radix;
    tccl_ucx_request_t **reqs = req->bcast_kn.reqs;
    int vrank = (group_rank - root + group_size) % group_size;
    int dist  = req->bcast_kn.dist;
    int i, vpeer, peer, vroot_at_level, root_at_level, pos;

    if (req->bcast_kn.active_reqs) {
        if (TCCL_OK == tccl_ucx_testall((tccl_ucx_team_t *)team, reqs,
                                           req->bcast_kn.active_reqs)) {
            req->bcast_kn.active_reqs = 0;
        } else {
            return TCCL_OK;
        }
    }

    while (dist >= 1) {
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
                    tccl_ucx_send_nb(data_buffer, data_size, peer,
                                    (tccl_ucx_team_t*)team, req->tag,
                                    &reqs[req->bcast_kn.active_reqs++]);
                }
            }
        } else if (pos > 0) {
            vroot_at_level = vrank - pos*dist;
            root_at_level  = (vroot_at_level + root) % group_size;
            tccl_ucx_recv_nb(data_buffer, data_size, root_at_level,
                            (tccl_ucx_team_t*)team, req->tag, &reqs[req->bcast_kn.active_reqs++]);
            assert(req->bcast_kn.active_reqs == 1);
        }
        dist /= radix;

        if (req->bcast_kn.active_reqs) {
            if (TCCL_OK == tccl_ucx_testall((tccl_ucx_team_t *)team, reqs,
                                               req->bcast_kn.active_reqs)) {
                req->bcast_kn.active_reqs = 0;
            } else {
                req->bcast_kn.dist = dist;
                return TCCL_OK;
            }
        }
    }
    req->complete = TCCL_OK;
    return TCCL_OK;
}

tccl_status_t tccl_ucx_bcast_knomial_start(tccl_ucx_collreq_t *req)
{
    size_t data_size = req->args.buffer_info.len;
    int group_rank   = req->team->oob.rank;
    int group_size   = req->team->oob.size;
    memset(req->bcast_kn.reqs, 0, sizeof(req->bcast_kn.reqs));
    req->bcast_kn.radix   = 4;//TODO
    if (req->bcast_kn.radix > req->team->oob.size) {
        req->bcast_kn.radix = req->team->oob.size;
    }

    req->bcast_kn.active_reqs = 0;
    CALC_DIST(group_size, req->bcast_kn.radix, req->bcast_kn.dist);
    if (req->args.root == group_rank) {
        if (req->args.buffer_info.src_buffer !=
            req->args.buffer_info.dst_buffer) {
            memcpy(req->args.buffer_info.dst_buffer,
                   req->args.buffer_info.src_buffer, data_size);
        }
    }
    req->progress = tccl_ucx_bcast_knomial_progress;
    return tccl_ucx_bcast_knomial_progress(req);
}
