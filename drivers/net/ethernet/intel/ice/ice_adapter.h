/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Red Hat */

#ifndef _ICE_ADAPTER_H_
#define _ICE_ADAPTER_H_

#include <linux/cleanup.h>
#include <linux/types.h>
#include <linux/spinlock_types.h>

#include <net/devlink.h>

struct pci_dev;
struct ice_pf;

enum ice_devl_resource_id {
	/* keep parent IDs prior to children, because we register in order */
	ICE_TOP_RESOURCE = DEVLINK_RESOURCE_ID_PARENT_TOP,
	ICE_RSS_LUT_BOTH,
	ICE_RSS_LUT_GLOBAL,
	ICE_RSS_LUT_PF,
	ICE_DEVL_RESOURCES_COUNT
};

#define ICE_MAX_DEVL_RESOURCE_UNITS 16

/**
 * struct ice_devl_resource - driver data for devlink resource, config & runtime
 *
 * @owner: entity that owns given resource (like ptr to ice_pf or ice_vf)
 * @name: name of the resource to register it with
 * @max_size: max size of the resource, to present in the uAPI/validate against
 * @start_size: starting size of the resource
 * @parent_id: ID of the parent resource
 * @get: occ getter callback
 * @set: occ setter callback
 */
struct ice_devl_resource {
	void *owner[ICE_MAX_DEVL_RESOURCE_UNITS];
	const char *name;
	devlink_resource_occ_get_t *get;
	devlink_resource_occ_set_t *set;
	u32 max_size;
	u32 parent_id;
	u32 start_size;
};

/**
 * struct ice_port_list - data used to store the list of adapter ports
 *
 * This structure contains data used to maintain a list of adapter ports
 *
 * @ports: list of ports
 * @lock: protect access to the ports list
 */
struct ice_port_list {
	struct list_head ports;
	/* To synchronize the ports list operations */
	struct mutex lock;
};

/**
 * struct ice_adapter - PCI adapter resources shared across PFs
 * @__init_done: priv - tells if spinlocks are initialized
 * @devlink: ice adapter's devlink (whole dev devlink)
 * @ptp_gltsyn_time_lock: Spinlock protecting access to the GLTSYN_TIME
 *                        register of the PTP clock.
 * @txq_ctx_lock: Spinlock protecting access to the GLCOMM_QTX_CNTX_CTL register
 * @ctrl_pf: Control PF of the adapter
 * @ports: Ports list
 * @resources: array of ice's data for devlink resources
 */
struct ice_adapter {
	struct devlink *devlink;
	bool __init_done;

	/* For access to the GLTSYN_TIME register */
	spinlock_t ptp_gltsyn_time_lock;
	/* For access to GLCOMM_QTX_CNTX_CTL register */
	spinlock_t txq_ctx_lock;

	struct ice_pf *ctrl_pf;
	struct ice_port_list ports;

	struct ice_devl_resource resources[ICE_DEVL_RESOURCES_COUNT];
};

struct ice_adapter *ice_adapter_get(struct pci_dev *pdev);
void ice_adapter_put(struct ice_adapter *adapter);

DEFINE_GUARD(ice_adapter_devl, struct ice_adapter *,
	     devl_lock((_T)->devlink), devl_unlock((_T)->devlink))

#endif /* _ICE_ADAPTER_H */
