/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/base.h"

#include "src/util/show_help.h"

#include "rmaps_ppr.h"
#include "src/mca/rmaps/base/base.h"

/*
 * Local functions
 */

static int prte_rmaps_ppr_open(void);
static int prte_rmaps_ppr_close(void);
static int prte_rmaps_ppr_query(prte_mca_base_module_t **module, int *priority);
static int prte_rmaps_ppr_register(void);

prte_rmaps_base_component_t prte_rmaps_ppr_component = {
    .base_version = {
        PRTE_RMAPS_BASE_VERSION_2_0_0,

        .mca_component_name = "ppr",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                    PMIX_RELEASE_VERSION),
        .mca_open_component = prte_rmaps_ppr_open,
        .mca_close_component = prte_rmaps_ppr_close,
        .mca_query_component = prte_rmaps_ppr_query,
        .mca_register_component_params = prte_rmaps_ppr_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int my_priority;

static int prte_rmaps_ppr_open(void)
{
    return PRTE_SUCCESS;
}

static int prte_rmaps_ppr_query(prte_mca_base_module_t **module, int *priority)
{
    *priority = my_priority;
    *module = (prte_mca_base_module_t *) &prte_rmaps_ppr_module;
    return PRTE_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int prte_rmaps_ppr_close(void)
{
    return PRTE_SUCCESS;
}

static int prte_rmaps_ppr_register(void)
{
    my_priority = 90;
    (void) prte_mca_base_component_var_register(&prte_rmaps_ppr_component.base_version, "priority",
                                                "Priority of the ppr rmaps component",
                                                PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);

    return PRTE_SUCCESS;
}
