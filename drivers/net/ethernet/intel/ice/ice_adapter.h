/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Red Hat */

#ifndef _ICE_ADAPTER_H_
#define _ICE_ADAPTER_H_

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/spinlock_types.h>

#include <net/devlink.h>

#include "ice_type.h"

struct pci_dev;
struct ice_pf;

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

#define ICE_ADAPTER_PBA_LEN	128

/**
 * struct ice_adapter_info - device wide data read from the creating PF
 *
 * Describes the device as a whole, not any of its functions. Read once, from
 * the HW of the PF which created the adapter, as none of it is expected to
 * change while the device is bound to the driver.
 *
 * @nvm: NVM (PSID) version and EETRACK identifier of the active NVM bank
 * @orom: Option ROM version of the active OROM bank
 * @netlist: netlist version of the active netlist bank
 * @pkg_ver: version of the DDP package the device runs
 * @pba: Product Board Assembly identifier, empty string if not readable
 * @pkg_name: name of the DDP package the device runs
 * @dsn: PCI Device Serial Number, zero if the PFs of this adapter do not
 *       share a single DSN (multi-NAC E825C)
 * @pkg_track_id: track identifier of the DDP package the device runs
 * @fw_build: build identifier of the management (EMP) firmware
 * @cgu_id: identifier of the Clock Generation Unit
 * @cgu_cfg_ver: configuration version of the Clock Generation Unit
 * @cgu_fw_ver: firmware version of the Clock Generation Unit
 * @device_id: PCI device identifier
 * @fw_maj_ver: major version of the management firmware
 * @fw_min_ver: minor version of the management firmware
 * @fw_patch: patch version of the management firmware
 * @api_maj_ver: major version of the management firmware API
 * @api_min_ver: minor version of the management firmware API
 * @api_patch: patch version of the management firmware API
 * @revision_id: PCI revision identifier
 * @cgu_part_number: part number of the Clock Generation Unit, valid only
 *                   when @has_cgu is set
 * @has_cgu: device is equipped with a Clock Generation Unit
 * @has_cgu_fw: firmware version of the Clock Generation Unit was read
 */
struct ice_adapter_info {
	struct ice_nvm_info nvm;
	struct ice_orom_info orom;
	struct ice_netlist_info netlist;
	struct ice_pkg_ver pkg_ver;
	char pba[ICE_ADAPTER_PBA_LEN];
	char pkg_name[ICE_PKG_NAME_SIZE];
	u64 dsn;
	u32 pkg_track_id;
	u32 fw_build;
	u32 cgu_id;
	u32 cgu_cfg_ver;
	u32 cgu_fw_ver;
	u16 device_id;
	u8 fw_maj_ver;
	u8 fw_min_ver;
	u8 fw_patch;
	u8 api_maj_ver;
	u8 api_min_ver;
	u8 api_patch;
	u8 revision_id;
	u8 cgu_part_number;
	bool has_cgu;
	bool has_cgu_fw;
};

/**
 * struct ice_adapter - PCI adapter resources shared across PFs
 * @devlink: ice adapter's devlink (whole dev devlink)
 * @info: device wide information reported by devlink dev info
 * @ptp_gltsyn_time_lock: Spinlock protecting access to the GLTSYN_TIME
 *                        register of the PTP clock.
 * @txq_ctx_lock: Spinlock protecting access to the GLCOMM_QTX_CNTX_CTL register
 * @cpi_phy_lock: Per-PHY mutex serializing CPI REQ/ACK transactions.
 *               Index 0 = PHY0, index 1 = PHY1. Used on E825C devices.
 * @ctrl_pf: Control PF of the adapter
 * @ports: Ports list
 */
struct ice_adapter {
	struct devlink *devlink;
	struct ice_adapter_info info;

	/* For access to the GLTSYN_TIME register */
	spinlock_t ptp_gltsyn_time_lock;
	/* For access to GLCOMM_QTX_CNTX_CTL register */
	spinlock_t txq_ctx_lock;
	/* Serialize CPI REQ/ACK transactions per PHY (E825C only) */
	struct mutex cpi_phy_lock[ICE_E825_MAX_PHYS];

	struct ice_pf *ctrl_pf;
	struct ice_port_list ports;
};

struct ice_adapter *ice_adapter_get(struct ice_pf *pf);
void ice_adapter_put(struct ice_adapter *adapter);

#endif /* _ICE_ADAPTER_H */
