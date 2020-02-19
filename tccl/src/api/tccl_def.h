/*
* Copyright (C) Mellanox Technologies Ltd. 2001-2020.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifndef TCCL_DEF_H_
#define TCCL_DEF_H_
#include <stddef.h>
#include <stdint.h>

/**
 * @ingroup TCCL_TEAM_LIB
 * @brief TCCL library handle
 *
 * @todo add description here
 */
typedef struct tccl_team_lib* tccl_team_lib_h;
typedef struct tccl_lib* tccl_lib_h;

/**
 * @ingroup TCCL_TEAM
 * @brief TCCL team handle
 *
 * @todo add description here
 */
typedef struct tccl_team* tccl_team_h;

/**
 * @ingroup TCCL_TEAM_CONTEXT
 * @brief TCCL team context handle
 *
 * @todo add description here
 */
typedef struct tccl_context* tccl_context_h;

/**
 * @ingroup TCCL_TEAM_CONTEXT
 * @brief TCCL team context config handle
 *
 * @todo add description here
 */
typedef struct tccl_context_config* tccl_context_config_h;

/**
 * @ingroup TCCL_TEAM
 * @brief TCCL team config handle
 *
 * @todo add description here
 */
typedef struct tccl_team_config* tccl_team_config_h;

/**
 * @ingroup TCCL_COLL
 * @brief TCCL collective requst handle
 *
 * @todo add description here
 */

typedef struct tccl_coll_req* tccl_coll_req_h;

#define TCCL_BIT(i)               (1ul << (i))
#define TCCL_MASK(i)              (TCCL_BIT(i) - 1)
#define TCCL_PP_QUOTE(x)                 # x
#endif
