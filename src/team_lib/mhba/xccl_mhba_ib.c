/*
 * Copyright (C) Mellanox Technologies Ltd. 2020.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include "xccl_mhba_ib.h"
#include "utils/utils.h"

xccl_status_t xccl_mhba_create_ibv_ctx(char *ib_devname,
                                       struct ibv_context **ctx)
{
    struct ibv_device        **dev_list = ibv_get_device_list(NULL);
    struct mlx5dv_context_attr attr     = {};
    struct ibv_device         *ib_dev;
    if (!ib_devname) {
        /* If no device was specified by name, use by default the first
           available device. */
        ib_dev = *dev_list;
        if (!ib_dev) {
            xccl_mhba_error("No IB devices found");
            return XCCL_ERR_NO_MESSAGE;
        }
    } else {
        int i;
        for (i = 0; dev_list[i]; ++i)
            if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
                break;
        ib_dev = dev_list[i];
        if (!ib_dev) {
            xccl_mhba_error("IB device %s not found", ib_devname);
            return XCCL_ERR_NO_MESSAGE;
        }
    }

    /* Need to open the device with `MLX5DV_CONTEXT_FLAGS_DEVX` flag, as it is
       needed for mlx5dv_create_mkey() (See man pages of mlx5dv_create_mkey()). */

    attr.flags = MLX5DV_CONTEXT_FLAGS_DEVX;
    *ctx       = mlx5dv_open_device(ib_dev, &attr);
    return XCCL_OK;
}

int xccl_mhba_check_port_active(struct ibv_context *ctx, int port_num)
{
    struct ibv_port_attr port_attr;
    ibv_query_port(ctx, port_num, &port_attr);
    if (port_attr.state == IBV_PORT_ACTIVE &&
        port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
        return 1;
    }
    return 0;
}

int xccl_mhba_get_active_port(struct ibv_context *ctx)
{
    struct ibv_device_attr device_attr;
    int                    i;
    ibv_query_device(ctx, &device_attr);
    for (i = 1; i <= device_attr.phys_port_cnt; i++) {
        if (xccl_mhba_check_port_active(ctx, i)) {
            return i;
        }
    }
    return -1;
}

xccl_status_t xccl_mhba_qp_connect(struct ibv_qp *qp, uint32_t qp_num,
                                   uint16_t lid, int port)
{
    int                ret;
    struct ibv_qp_attr qp_attr;

    xccl_mhba_debug("modify QP to INIT");
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state        = IBV_QPS_INIT;
    qp_attr.pkey_index      = 0;
    qp_attr.port_num        = port;
    qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
                              IBV_ACCESS_REMOTE_ATOMIC | IBV_ACCESS_LOCAL_WRITE;
    if (ibv_modify_qp(qp, &qp_attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS) != 0) {
        xccl_mhba_error("QP RESET->INIT failed");
        return XCCL_ERR_NO_MESSAGE;
    }

    xccl_mhba_debug("modify QP to RTR");

    memset((void *)&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state              = IBV_QPS_RTR;
    qp_attr.path_mtu              = IBV_MTU_4096;
    qp_attr.dest_qp_num           = qp_num;
    qp_attr.rq_psn                = 0x123;
    qp_attr.min_rnr_timer         = 20;
    qp_attr.max_dest_rd_atomic    = 1;
    qp_attr.ah_attr.dlid          = lid;
    qp_attr.ah_attr.sl            = 0;
    qp_attr.ah_attr.src_path_bits = 0;
    qp_attr.ah_attr.port_num      = port;
    
    ret = ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_AV |
                        IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (ret != 0) {
        xccl_mhba_error("QP INIT->RTR failed (error %d)", ret);
        return XCCL_ERR_NO_MESSAGE;
    }

    // Modify QP to RTS
    xccl_mhba_debug("modify QP to RTS");
    qp_attr.qp_state      = IBV_QPS_RTS;
    qp_attr.timeout       = 10;
    qp_attr.retry_cnt     = 7;
    qp_attr.rnr_retry     = 7;
    qp_attr.sq_psn        = 0x123;
    qp_attr.max_rd_atomic = 1;
    
    ret = ibv_modify_qp(qp, &qp_attr,  IBV_QP_STATE | IBV_QP_TIMEOUT |
                        IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                        IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret != 0) {
        xccl_mhba_error("QP RTR->RTS failed");
        return XCCL_ERR_NO_MESSAGE;
    }
    return XCCL_OK;
}

xccl_status_t xccl_mhba_init_dc_qps_and_connect(xccl_mhba_team_t *mhba_team, uint32_t *local_data, uint8_t port_num){
    int i;
    struct ibv_qp_init_attr_ex attr_ex;
    struct mlx5dv_qp_init_attr attr_dv;
    struct ibv_qp_attr qp_attr_to_init;
    struct ibv_qp_attr qp_attr_to_rtr;
    struct ibv_qp_attr qp_attr_to_rts;
    memset(&attr_ex, 0, sizeof(attr_ex));
    memset(&attr_dv, 0, sizeof(attr_dv));
    memset(&qp_attr_to_init, 0, sizeof(qp_attr_to_init));
    memset(&qp_attr_to_rtr, 0, sizeof(qp_attr_to_rtr));
    memset(&qp_attr_to_rts, 0, sizeof(qp_attr_to_rts));

    attr_ex.qp_type = IBV_QPT_DRIVER;
    attr_ex.send_cq = mhba_team->net.cq;
    attr_ex.recv_cq = mhba_team->net.cq;
    attr_ex.pd = mhba_team->node.shared_pd;
    attr_ex.cap.max_send_wr = (SQUARED(mhba_team->node.sbgp->group_size / 2) + 1) * MAX_OUTSTANDING_OPS *
                              xccl_round_up(mhba_team->net.net_size, mhba_team->num_dci_qps);
    attr_ex.cap.max_send_sge = 1;
    attr_ex.comp_mask |= IBV_QP_INIT_ATTR_SEND_OPS_FLAGS | IBV_QP_INIT_ATTR_PD;
    attr_ex.send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE | IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM |
                             IBV_QP_EX_WITH_ATOMIC_FETCH_AND_ADD;
    attr_dv.comp_mask |= MLX5DV_QP_INIT_ATTR_MASK_DC | MLX5DV_QP_INIT_ATTR_MASK_QP_CREATE_FLAGS;
    attr_dv.dc_init_attr.dc_type = MLX5DV_DCTYPE_DCI;
    attr_dv.create_flags |= MLX5DV_QP_CREATE_DISABLE_SCATTER_TO_CQE;

    qp_attr_to_init.qp_state = IBV_QPS_INIT;
    qp_attr_to_init.pkey_index = 0;
    qp_attr_to_init.port_num = port_num;
    qp_attr_to_init.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                                      IBV_ACCESS_REMOTE_READ |
                                      IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

    qp_attr_to_rtr.qp_state = IBV_QPS_RTR;
    qp_attr_to_rtr.path_mtu = IBV_MTU_4096;
    qp_attr_to_rtr.min_rnr_timer = 20;
    qp_attr_to_rtr.ah_attr.port_num = port_num;
    qp_attr_to_rtr.ah_attr.is_global = 0;

    qp_attr_to_rts.qp_state = IBV_QPS_RTS;
    qp_attr_to_rts.timeout = 10; //todo - what value?
    qp_attr_to_rts.retry_cnt = 7;
    qp_attr_to_rts.rnr_retry = 7;
    qp_attr_to_rts.sq_psn = 0x123;
    qp_attr_to_rts.max_rd_atomic = 1;

    //create DCIs
    for (i =0; i<mhba_team->num_dci_qps ;i++) {
        mhba_team->net.dcis[i].dci_qp = mlx5dv_create_qp(mhba_team->node.shared_ctx, &attr_ex, &attr_dv);
        if (!mhba_team->net.dcis[i].dci_qp) {
            xccl_mhba_error("Couldn't create DCI QP");
            goto fail;
        }
        // Turn DCI ibv_qp to ibv_qpex and ibv_mqpex
        mhba_team->net.dcis[i].dc_qpex = ibv_qp_to_qp_ex(mhba_team->net.dcis[i].dci_qp);
        if (!mhba_team->net.dcis[i].dc_qpex) {
            xccl_mhba_error("Failed turn ibv_qp to ibv_qp_ex, error: %d", errno);
            goto fail;
        }
        mhba_team->net.dcis[i].dc_mqpex = mlx5dv_qp_ex_from_ibv_qp_ex(mhba_team->net.dcis[i].dc_qpex);
        if (!mhba_team->net.dcis[i].dc_mqpex) {
            xccl_mhba_error("Failed turn ibv_qp_ex to mlx5dv_qp_ex, error: %d", errno);
            goto fail;
        }

        if (ibv_modify_qp(mhba_team->net.dcis[i].dci_qp, &qp_attr_to_init,
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT) != 0) {
            xccl_mhba_error("Failed to modify init qp");
            goto fail;
        }

        if (ibv_modify_qp(mhba_team->net.dcis[i].dci_qp, &qp_attr_to_rtr,
                          IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_AV) != 0) {
            xccl_mhba_error("Failed to modify qp to rtr");
            goto fail;
        }

        if (ibv_modify_qp(mhba_team->net.dcis[i].dci_qp, &qp_attr_to_rts,
                          IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY
                          | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) != 0) {
            xccl_mhba_error("Failed to modify qp to rts");
            goto fail;
        }
    }

    //create DCT
    memset(&attr_ex, 0, sizeof(struct ibv_qp_init_attr_ex));
    memset(&attr_dv, 0, sizeof(struct mlx5dv_qp_init_attr));

    attr_ex.qp_type = IBV_QPT_DRIVER;
    attr_ex.send_cq = mhba_team->net.cq;
    attr_ex.recv_cq = mhba_team->net.cq;
    attr_ex.comp_mask |= IBV_QP_INIT_ATTR_PD;
    attr_ex.pd = mhba_team->node.shared_pd;
    struct ibv_srq_init_attr srq_attr;
    memset(&srq_attr, 0, sizeof(struct ibv_srq_init_attr));
    srq_attr.attr.max_wr = 1;
    srq_attr.attr.max_sge = 1;
    // SRQ isn't really needed since we don't use SEND and RDMA WRITE with IMM, but needed because it's DCT
    mhba_team->net.srq = ibv_create_srq(mhba_team->node.shared_pd, &srq_attr);
    if (mhba_team->net.srq  == NULL) {
        xccl_mhba_error("Failed to create Shared Receive Queue (SRQ)");
        goto fail;
    }
    attr_ex.srq = mhba_team->net.srq ;

    attr_dv.comp_mask |= MLX5DV_QP_INIT_ATTR_MASK_DC;
    attr_dv.dc_init_attr.dc_type = MLX5DV_DCTYPE_DCT;
    attr_dv.dc_init_attr.dct_access_key = DC_KEY;

    mhba_team->net.dct_qp = mlx5dv_create_qp(mhba_team->node.shared_ctx, &attr_ex, &attr_dv);
    if (mhba_team->net.dct_qp == NULL) {
        xccl_mhba_error("Couldn't create DCT QP errno=%d",errno);
        goto srq_fail;
    }

    if (ibv_modify_qp(mhba_team->net.dct_qp, &qp_attr_to_init,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) {
        xccl_mhba_error("Failed to modify init qp");
        goto dct_fail;
    }

    if (ibv_modify_qp(mhba_team->net.dct_qp, &qp_attr_to_rtr,
                      IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_AV| IBV_QP_MIN_RNR_TIMER) != 0) {
        xccl_mhba_error("Failed to modify init qp");
        goto dct_fail;
    }

    local_data[0] = mhba_team->net.dct_qp->qp_num;
    return XCCL_OK;

dct_fail:
    if(ibv_destroy_qp(mhba_team->net.dct_qp)) {
        xccl_mhba_error("Couldn't destroy QP");
    }
srq_fail:
    if(ibv_destroy_srq(mhba_team->net.srq)) {
        xccl_mhba_error("Couldn't destroy SRQ");
    }
fail:
    for (i=i-1; i>= 0;i--) {
        if(ibv_destroy_qp(mhba_team->net.dcis[i].dci_qp)) {
            xccl_mhba_error("Couldn't destroy QP");
        }
    }
    return XCCL_ERR_NO_MESSAGE;
}

xccl_status_t xccl_mhba_create_rc_qps(xccl_mhba_team_t *mhba_team, uint32_t *local_data){
    struct ibv_qp_init_attr qp_init_attr;
    int i;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    //todo change in case of non-homogenous ppn
    qp_init_attr.send_cq = mhba_team->net.cq;
    qp_init_attr.recv_cq = mhba_team->net.cq;
    qp_init_attr.cap.max_send_wr =
            (SQUARED(mhba_team->node.sbgp->group_size / 2) + 1) * MAX_OUTSTANDING_OPS; // TODO switch back to fixed tx/rx
    qp_init_attr.cap.max_recv_wr = 0;
    qp_init_attr.cap.max_send_sge    = 1;
    qp_init_attr.cap.max_recv_sge    = 0;
    qp_init_attr.cap.max_inline_data = 0;
    qp_init_attr.qp_type             = IBV_QPT_RC;

    mhba_team->net.rc_qps = malloc(sizeof(struct ibv_qp *) * mhba_team->net.net_size);
    if (!mhba_team->net.rc_qps) {
        xccl_mhba_error("failed to allocate asr qps array");
        goto fail_after_malloc;
    }
    for (i = 0; i < mhba_team->net.net_size; i++) {
        mhba_team->net.rc_qps[i] =
                ibv_create_qp(mhba_team->node.shared_pd, &qp_init_attr);
        if (!mhba_team->net.rc_qps[i]) {
            xccl_mhba_error("failed to create qp for dest %d, errno %d", i,
                            errno);
            goto qp_creation_failure;
        }
        local_data[i] = mhba_team->net.rc_qps[i]->qp_num;
    }
    return XCCL_OK;

qp_creation_failure:
    for (i=i-1; i >= 0; i--) {
        if(ibv_destroy_qp(mhba_team->net.rc_qps[i])) {
            xccl_mhba_error("Couldn't destroy QP");
        }
    }
    free(mhba_team->net.rc_qps);
fail_after_malloc:
    return XCCL_ERR_NO_MESSAGE;
}

xccl_status_t xccl_mhba_create_ah(struct ibv_ah **ah_ptr, uint16_t lid, uint8_t port_num,
        xccl_mhba_team_t *mhba_team){
    struct ibv_ah_attr ah_attr;
    memset(&ah_attr, 0, sizeof(struct ibv_ah_attr));

    ah_attr.dlid           = lid;
    ah_attr.port_num       = port_num;
    ah_attr.is_global     = 0;
    ah_attr.grh.hop_limit  = 0;

    *ah_ptr = ibv_create_ah(mhba_team->node.shared_pd, &ah_attr);
    if (!(*ah_ptr)) {
        xccl_mhba_error("Failed to create ah");
        return XCCL_ERR_NO_MESSAGE;
    }
    return XCCL_OK;
}