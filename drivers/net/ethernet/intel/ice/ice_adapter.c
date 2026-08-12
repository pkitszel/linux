// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: Copyright Red Hat

#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "ice_adapter.h"
#include "ice.h"
#include "ice_lib.h"
#include "devlink/devlink.h"

#define ICE_ADAPTER_FIXED_INDEX	BIT_ULL(63)

#define ICE_ADAPTER_INDEX_E825C	\
	(ICE_DEV_ID_E825C_BACKPLANE | ICE_ADAPTER_FIXED_INDEX)

static u64 ice_adapter_index(struct pci_dev *pdev)
{
	switch (pdev->device) {
	case ICE_DEV_ID_E825C_BACKPLANE:
	case ICE_DEV_ID_E825C_QSFP:
	case ICE_DEV_ID_E825C_SFP:
	case ICE_DEV_ID_E825C_SGMII:
		/* E825C devices have multiple NACs which are connected to the
		 * same clock source, and which must share the same
		 * ice_adapter structure. We can't use the serial number since
		 * each NAC has its own NVM generated with its own unique
		 * Device Serial Number. Instead, rely on the embedded nature
		 * of the E825C devices, and use a fixed index. This relies on
		 * the fact that all E825C physical functions in a given
		 * system are part of the same overall device.
		 */
		return ICE_ADAPTER_INDEX_E825C;
	default:
		return pci_get_dsn(pdev) & ~ICE_ADAPTER_FIXED_INDEX;
	}
}

/**
 * ice_adapter_read_info - Read device wide information from the creating PF
 * @adapter: adapter being created
 * @pf: PF which created @adapter, with HW already initialized
 *
 * Read the information describing the device as a whole. It is read once,
 * because it is the same for every PF of the device and does not change while
 * the device is bound to the driver. Whatever is not readable is left zeroed
 * and is not reported by devlink.
 */
static void ice_adapter_read_info(struct ice_adapter *adapter,
				  struct ice_pf *pf)
{
	struct ice_adapter_info *info = &adapter->info;
	struct ice_hw *hw = &pf->hw;

	info->nvm = hw->flash.nvm;
	info->orom = hw->flash.orom;
	info->netlist = hw->flash.netlist;

	info->fw_maj_ver = hw->fw_maj_ver;
	info->fw_min_ver = hw->fw_min_ver;
	info->fw_patch = hw->fw_patch;
	info->fw_build = hw->fw_build;
	info->api_maj_ver = hw->api_maj_ver;
	info->api_min_ver = hw->api_min_ver;
	info->api_patch = hw->api_patch;

	info->pkg_ver = hw->active_pkg_ver;
	info->pkg_track_id = hw->active_track_id;
	strscpy(info->pkg_name, (const char *)hw->active_pkg_name);

	info->device_id = hw->device_id;
	info->revision_id = hw->revision_id;

	/* Each E825C NAC has its own DSN, so no DSN identifies such device */
	if (!(ice_adapter_index(pf->pdev) & ICE_ADAPTER_FIXED_INDEX))
		info->dsn = pci_get_dsn(pf->pdev);

	info->has_cgu = ice_is_feature_supported(pf, ICE_F_CGU);
	if (info->has_cgu) {
		info->cgu_part_number = hw->cgu_part_number;
		info->has_cgu_fw = !ice_aq_get_cgu_info(hw, &info->cgu_id,
							&info->cgu_cfg_ver,
							&info->cgu_fw_ver);
	}

	if (ice_read_pba_string(hw, (u8 *)info->pba, sizeof(info->pba)))
		info->pba[0] = '\0';
}

static int ice_adapter_init(void *priv, void *init_param)
{
	struct ice_adapter *adapter = priv;
	struct ice_pf *pf = init_param;
	struct devlink *devlink;

	devlink = shd_priv_to_devlink(adapter);
	adapter->devlink = devlink;
	ice_adapter_read_info(adapter, pf);

	spin_lock_init(&adapter->ptp_gltsyn_time_lock);
	spin_lock_init(&adapter->txq_ctx_lock);
	for (int i = 0; i < ARRAY_SIZE(adapter->cpi_phy_lock); i++)
		mutex_init(&adapter->cpi_phy_lock[i]);

	mutex_init(&adapter->ports.lock);
	INIT_LIST_HEAD(&adapter->ports.ports);

	return 0;
}

static void ice_adapter_fini(void *priv)
{
	struct ice_adapter *adapter = priv;

	WARN_ON(!list_empty(&adapter->ports.ports));
	for (int i = 0; i < ARRAY_SIZE(adapter->cpi_phy_lock); i++)
		mutex_destroy(&adapter->cpi_phy_lock[i]);
	mutex_destroy(&adapter->ports.lock);
}

static const struct devlink_ops ice_adapter_devlink_ops = {
	.shd_init = ice_adapter_init,
	.shd_fini = ice_adapter_fini,
	.info_get = ice_devlink_adapter_info_get,
};

/**
 * ice_adapter_get - Get a shared ice_adapter structure.
 * @pf: Pointer to the PF getting the ice_adapter.
 *
 * Gets a pointer to a shared ice_adapter structure. Physical functions (PFs)
 * of the same multi-function PCI device share one ice_adapter structure.
 * The ice_adapter is reference-counted. The PF driver must use ice_adapter_put
 * to release its reference.
 *
 * Context: Process, may sleep.
 * Return:  Pointer to ice_adapter on success.
 *          ERR_PTR() on error.
 */
struct ice_adapter *ice_adapter_get(struct ice_pf *pf)
{
	struct pci_dev *pdev = pf->pdev;
	struct ice_adapter *adapter;
	struct devlink *devlink;
	char devlink_id[32];
	u64 index;

	index = ice_adapter_index(pdev);
	snprintf(devlink_id, sizeof(devlink_id), "%llx", index);
	devlink = devlink_shd_get(devlink_id, &ice_adapter_devlink_ops,
				  sizeof(*adapter), pf, pdev->dev.driver);
	if (IS_ERR(devlink))
		return ERR_CAST(devlink);

	adapter = devlink_shd_get_priv(devlink);

	return adapter;
}

/**
 * ice_adapter_put - Release a reference to the shared ice_adapter structure.
 * @adapter: the ice_adapter to release reference to
 *
 * Releases the reference to ice_adapter previously obtained with
 * ice_adapter_get.
 *
 * Context: Process, may sleep.
 */
void ice_adapter_put(struct ice_adapter *adapter)
{
	devlink_shd_put(adapter->devlink);
}
