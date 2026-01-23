#include <linux/cleanup.h>

#include <net/devlink.h>

#include "resource.h"
#include "ice_adapter.h"
#include "ice.h"
#include "ice_lib.h"

#define ICE_NUM_GLOBAL_LUTS	16
#define ICE_ANY_SLOT	-1

DEFINE_GUARD(ice_adapter_devl, struct ice_adapter *,
	     devl_lock(priv_to_devlink(_T)), devl_unlock(priv_to_devlink(_T)));

static u32 ice_devl_res_cnt(const struct ice_adapter *adapter,
			    enum ice_devl_resource_id res_id)
{
	const struct ice_devl_resource *res = &adapter->resources[res_id];
	u32 sum = 0;

	for (int i = 0; i < res->max_size; i++)
		sum += res->owner[i] != NULL;

	return sum;
}

static int ice_devl_res_take(struct ice_pf *pf,
			     enum ice_devl_resource_id res_id, int slot,
			     void *owner)
{
	struct ice_devl_resource *res = &pf->adapter->resources[res_id];
	int end = slot == ICE_ANY_SLOT ? res->max_size : slot + 1;
	int beg = slot == ICE_ANY_SLOT ? 0 : slot;
	int err, new_id = ICE_ANY_SLOT;

	for (int id = beg; id < end; id++) {
		if (!res->owner[id]) {
			new_id = id;
			break;
		}
	}
	if (new_id == ICE_ANY_SLOT)
		return -ENOSPC;

	switch (res_id) {
	case ICE_RSS_LUT_GLOBAL: {
		u16 lut_id;

		err = ice_alloc_rss_global_lut(&pf->hw, &lut_id);
		if (err)
			return err;
		if (lut_id != new_id)
			return -ENOANO;
		break;
	}
	default:
		break;
	}

	res->owner[new_id] = owner;
	return new_id;
}

static int ice_devl_res_free(struct ice_pf *pf,
			     enum ice_devl_resource_id res_id, void *owner)
{
	struct ice_devl_resource *res = &pf->adapter->resources[res_id];
	int err = 0, id_to_free = ICE_ANY_SLOT;

	for (int i = 0; i < res->max_size; i++) {
		if (res->owner[i] == owner) {
			id_to_free = i;
			break;
		}
	}
	if (id_to_free == ICE_ANY_SLOT)
		return 0;

	switch (res_id) {
	case ICE_RSS_LUT_GLOBAL:
		err = ice_free_rss_global_lut(&pf->hw, id_to_free);
		break;
	default:
		break;
	}

	res->owner[id_to_free] = NULL;
	return err;
}

void ice_free_rss_lut_all(struct ice_vf *vf)
{
	struct ice_pf *pf = vf->pf;

	scoped_guard(ice_adapter_devl, pf->adapter) {
		ice_devl_res_free(pf, ICE_RSS_LUT_GLOBAL, vf);
		ice_devl_res_free(pf, ICE_RSS_LUT_PF, vf);
	}
}

static int ice_devl_res_owned_idx(struct ice_adapter *adapter,
				  enum ice_devl_resource_id res_id, void *owner)
{
	const struct ice_devl_resource *res = &adapter->resources[res_id];

	for (int i = 0; i < res->max_size; i++) {
		if (res->owner[i] == owner)
			return i;
	}
	return -ENXIO;
}

static bool ice_is_devl_res_owned_by(struct ice_adapter *adapter,
				     enum ice_devl_resource_id res_id,
				     void *owner)
{
	return ice_devl_res_owned_idx(adapter, res_id, owner) >= 0;
}

static u64 ice_rss_lut_whole_dev_occ_get_global(void *priv)
{
	struct ice_adapter *adapter = priv;

	return ice_devl_res_cnt(adapter, ICE_RSS_LUT_GLOBAL);
}

static u64 ice_rss_lut_whole_dev_occ_get_pf(void *priv)
{
	struct ice_adapter *adapter = priv;

	return ice_devl_res_cnt(adapter, ICE_RSS_LUT_PF);
}

static u64 ice_rss_lut_whole_dev_occ_get_both(void *priv)
{
	return ice_rss_lut_whole_dev_occ_get_global(priv) +
	       ice_rss_lut_whole_dev_occ_get_pf(priv);
}

static u64 ice_rss_lut_pf_occ_get_global(void *priv)
{
	struct ice_adapter *adapter;
	struct ice_pf *pf = priv;

	adapter = pf->adapter;
	scoped_guard(ice_adapter_devl, adapter)
		return ice_is_devl_res_owned_by(adapter, ICE_RSS_LUT_GLOBAL, pf);
}

static u64 ice_rss_lut_pf_occ_get_pf(void *priv)
{
	struct ice_adapter *adapter;
	struct ice_pf *pf = priv;

	adapter = pf->adapter;
	scoped_guard(ice_adapter_devl, adapter)
		return ice_is_devl_res_owned_by(adapter, ICE_RSS_LUT_PF, pf);
}


static u64 ice_rss_lut_pf_occ_get_both(void *priv)
{
	struct ice_adapter *adapter;
	struct ice_pf *pf = priv;

	adapter = pf->adapter;
	scoped_guard(ice_adapter_devl, adapter)
		return ice_is_devl_res_owned_by(adapter, ICE_RSS_LUT_PF, pf) +
		       ice_is_devl_res_owned_by(adapter, ICE_RSS_LUT_GLOBAL, pf);
}


static u64 ice_rss_lut_vf_occ_get_global(void *priv)
{
	struct ice_adapter *adapter;
	struct ice_vf *vf = priv;

	if (!priv)
		return 100;
	if (!vf->pf)
		return 101;
	if (!vf->pf->adapter)
		return 102;

	adapter = vf->pf->adapter;
	scoped_guard(ice_adapter_devl, adapter)
		return ice_is_devl_res_owned_by(adapter, ICE_RSS_LUT_GLOBAL, vf);
}

static u64 ice_rss_lut_vf_occ_get_pf(void *priv)
{
	struct ice_adapter *adapter;
	struct ice_vf *vf = priv;

	if (!priv)
		return 100;
	if (!vf->pf)
		return 101;
	if (!vf->pf->adapter)
		return 102;

	adapter = vf->pf->adapter;
	scoped_guard(ice_adapter_devl, adapter)
		return ice_is_devl_res_owned_by(adapter, ICE_RSS_LUT_PF, vf);
}

static u64 ice_rss_lut_vf_occ_get_both(void *priv)
{
	struct ice_adapter *adapter;
	struct ice_vf *vf = priv;

	if (!priv)
		return 100;
	if (!vf->pf)
		return 101;
	if (!vf->pf->adapter)
		return 102;

	adapter = vf->pf->adapter;
	scoped_guard(ice_adapter_devl, adapter)
		return ice_is_devl_res_owned_by(adapter, ICE_RSS_LUT_PF, vf) +
		       ice_is_devl_res_owned_by(adapter, ICE_RSS_LUT_GLOBAL, vf);
}

static int ice_devl_resource_deny_occ_set(u64 size,
					  struct netlink_ext_ack *extack,
					  void *priv)
{
	NL_SET_ERR_MSG_MOD(extack,
		"can not change directly, parent/aggregate resource just adds up children data");
	return -EPERM;
}

enum ice_rss_lut_resource_state {
	ICE_HAS_NO_LUT = 0,
	ICE_HAS_GLOBAL_LUT = BIT(ICE_RSS_LUT_GLOBAL),
	ICE_HAS_PF_LUT = BIT(ICE_RSS_LUT_PF),
	ICE_HAS_BOTH_LUTS = ICE_HAS_GLOBAL_LUT | ICE_HAS_PF_LUT,
};

enum ice_lut_size ice_lut_type_to_size(enum ice_lut_type type);

/** ice_rss_lut_resource_state - compute opaque resource state for given owner
 * @adapter: the adapter the @owner is on
 * @owner: the entity to compute state of resources for
 *
 * compute the current state of the resource the @owner has
 */
static enum ice_rss_lut_resource_state
ice_rss_lut_resource_state(struct ice_adapter *adapter, void *owner)
{
	enum ice_rss_lut_resource_state ret = ICE_HAS_NO_LUT;

	if (ice_is_devl_res_owned_by(adapter, ICE_RSS_LUT_GLOBAL, owner))
		ret |= ICE_HAS_GLOBAL_LUT;
	if (ice_is_devl_res_owned_by(adapter, ICE_RSS_LUT_PF, owner))
		ret |= ICE_HAS_PF_LUT;

	return ret;
}

static int ice_maybe_change_rss_lut(struct ice_pf *pf, void *owner,
				    enum ice_rss_lut_resource_state old,
				    enum ice_rss_lut_resource_state new,
				    struct netlink_ext_ack *extack)
{
	struct ice_aq_get_set_rss_lut_params params = {};
	struct ice_adapter *adapter = pf->adapter;
	u8 *lut __free(kfree) = NULL;
	struct ice_hw *hw = &pf->hw;
	enum ice_lut_type lut_type;
	int err, lut_size, lut_id;
	struct ice_vf *vf = NULL;
	struct ice_vsi *vsi;

	if (old & new & ICE_HAS_PF_LUT) {
		NL_SET_ERR_MSG_MOD(extack, "stay PF high");
		return 0;
	}

	if (new & ICE_HAS_PF_LUT) {
		lut_type = ICE_LUT_PF;
		NL_SET_ERR_MSG_FMT(extack, "change X -> PF");
	} else if (new & ICE_HAS_GLOBAL_LUT) {
		lut_id = ice_devl_res_owned_idx(adapter, ICE_RSS_LUT_GLOBAL, owner);
		if (lut_id < 0)
			return lut_id;

		lut_type = ICE_LUT_GLOBAL;
		params.global_lut_id = lut_id;
	} else {
		lut_type = ICE_LUT_VSI;
		if (owner == pf) {
			NL_SET_ERR_MSG_FMT(extack, "cannot change PF VSI LUT to 0");
			return -EXDEV;
		}
	}
	lut_size = ice_lut_type_to_size(lut_type);
	lut = kmalloc(lut_size, GFP_KERNEL);
	if (!lut)
		return -ENOMEM;

	if (pf == owner) {
		vsi = ice_get_main_vsi(pf);
	} else {
		vf = owner;
		vsi = ice_get_vf_vsi(vf);
	}
	ice_fill_rss_lut(lut, lut_size, vsi->rss_size);
	params.lut = lut;
	params.lut_size = lut_size;
	params.lut_type = lut_type;
	params.vsi_handle = vsi->idx;
	err = ice_aq_set_rss_lut(hw, &params);
	if (err) {
		NL_SET_ERR_MSG_FMT(extack, "AQ failed: %s", libie_aq_str(hw->adminq.sq_last_status));
		return err;
	}

	vsi->rss_table_size = lut_size;
	vsi->wanted.rss_lut_type = lut_type;
	if (vf) {
		if (lut_type == ICE_LUT_GLOBAL)
			vsi->rss_size = 64;
		vsi->flags |= ICE_VSI_FLAG_RELOAD;
		NL_SET_ERR_MSG_FMT(extack, "VF reset Requested");
		ice_reset_vf(vf, ICE_VF_RESET_NOTIFY | ICE_VF_RESET_LOCK);
	} else {
		vsi->rss_lut_type = lut_type;
	}
	return 0;
}

static int ice_devl_res_change(bool take, enum ice_devl_resource_id res_id,
			       struct ice_pf *pf, void *owner, int slot,
			       struct netlink_ext_ack *extack)
{
	enum ice_rss_lut_resource_state old, new, change;
	struct ice_adapter *adapter = pf->adapter;
	int err;

	change = BIT(res_id);
	old = ice_rss_lut_resource_state(adapter, owner);

	new = old;
	if (take)
		new |= change;
	else
		new &= ~change;


	if (new == old) {
		NL_SET_ERR_MSG_MOD(extack, "new == old");
		return 0;
	}

	if (pf == owner && !take && old != ICE_HAS_BOTH_LUTS) {
		NL_SET_ERR_MSG_MOD(extack,
			"at least one of 512+ sized LUTs must be assigned to PF device at all times");
		return -EDOM;
	}


	if (take) {
		int slot_id;

		slot_id = ice_devl_res_take(pf, res_id, slot, owner);
		if (slot_id < 0)
			return slot_id;
	}

	err = ice_maybe_change_rss_lut(pf, owner, old, new, extack);
	if (err)
		return err;

	if (!take) {
		err = ice_devl_res_free(pf, res_id, owner);
		if (err) {
			NL_SET_ERR_MSG_FMT(extack, "could not free global lut, err: %d", err);
			return err;
		}
	}

	return 0;
}

static int ice_rss_lut_pf_occ_set_pf(u64 size, struct netlink_ext_ack *extack,
				     void *priv)
{
	struct ice_pf *pf = priv;
	int pf_id = pf->hw.pf_id;

	scoped_guard(ice_adapter_devl, pf->adapter)
		return ice_devl_res_change(size, ICE_RSS_LUT_PF, pf, pf, pf_id,
					   extack);
}

static int ice_rss_lut_pf_occ_set_global(u64 size,
					 struct netlink_ext_ack *extack,
					 void *priv)
{
	struct ice_pf *pf = priv;

	scoped_guard(ice_adapter_devl, pf->adapter)
		return ice_devl_res_change(size, ICE_RSS_LUT_GLOBAL, pf, pf,
					   ICE_ANY_SLOT, extack);
}

static int ice_rss_lut_vf_occ_set_pf(u64 size, struct netlink_ext_ack *extack,
				     void *occ_priv)
{
	struct ice_vf *vf = occ_priv;
	struct ice_pf *pf = vf->pf;
	int pf_id = pf->hw.pf_id;

	scoped_guard(ice_adapter_devl, pf->adapter)
		return ice_devl_res_change(size, ICE_RSS_LUT_PF, pf, vf, pf_id,
					   extack);
}

static int ice_rss_lut_vf_occ_set_global(u64 size,
					 struct netlink_ext_ack *extack,
					 void *occ_priv)
{
	struct ice_vf *vf = occ_priv;
	struct ice_pf *pf = vf->pf;

	scoped_guard(ice_adapter_devl, pf->adapter)
		return ice_devl_res_change(size, ICE_RSS_LUT_GLOBAL, pf, vf,
					   ICE_ANY_SLOT, extack);
}

/**
 * ice_take_rss_lut_pf - allocate PF RSS LUT
 * @pf: the PF device that PF LUT is physically on
 *
 * Attempt to acquire PF RSS LUT for the caller.
 *
 * Return: nonnegative on success, -ENOSPC if PF LUT was already taken.
 */
int ice_take_rss_lut_pf(struct ice_pf *pf, void *owner)
{
	int pf_id = pf->hw.pf_id;

	scoped_guard(ice_adapter_devl, pf->adapter)
		return ice_devl_res_take(pf, ICE_RSS_LUT_PF, pf_id, owner);
}

/**
 * ice_take_rss_lut_global - allocate GLOBAL RSS LUT
 * @pf: the PF device that PF LUT is physically on
 *
 * Attempt to acquire GLOBAL RSS LUT for the caller.
 *
 * Return: GLOBAL LUT ID on success,
 * -EIO on AQ error, -ENOSPC if there are no free PF LUTs.
 */
int ice_take_rss_lut_global(struct ice_pf *pf, void *owner)
{
	scoped_guard(ice_adapter_devl, pf->adapter)
		return ice_devl_res_take(pf, ICE_RSS_LUT_GLOBAL,
					 ICE_ANY_SLOT, owner);
}

static void ice_devl_res_register(struct devlink *devlink,
				  struct ice_devl_resource *resources,
				  void *occ_priv)
{
	struct devlink_resource_size_params size_params;

	devlink_resource_size_params_init(&size_params, 0, 0, 1,
					  DEVLINK_RESOURCE_UNIT_ENTRY);
	for (int i = 0; i < ICE_DEVL_RESOURCES_COUNT; i++) {
		struct ice_devl_resource *res = &resources[i];
		int err, resource_id = i;

		if (!res->name)
			continue; /* skip empty entries in config table */

		size_params.size_max = res->max_size;
		err = devl_resource_register(devlink, res->name,
					     res->start_size, resource_id,
					     res->parent_id, &size_params);
		if (WARN_ONCE(err, "not all resource handlers registered, err: %d, resname: %s\n",
			      err, res->name))
			break;

		devl_resource_occ_set_get_register(devlink, resource_id,
						   res->set, res->get, occ_priv);
	}
}

void ice_devl_whole_dev_resources_register(const struct ice_hw *hw,
					   struct devlink *devlink)
{
	struct ice_adapter *adapter = devlink_priv(devlink);
	int pf_lut_cnt = hw->dev_caps.num_funcs;

	devl_assert_locked(devlink);

	adapter->resources[ICE_RSS_LUT_GLOBAL] = (struct ice_devl_resource) {
		.name = "lut_512",
		.parent_id = ICE_RSS_LUT_BOTH,
		.max_size = ICE_NUM_GLOBAL_LUTS,
		.get = ice_rss_lut_whole_dev_occ_get_global,
		.set = ice_devl_resource_deny_occ_set,
	};
	adapter->resources[ICE_RSS_LUT_PF] = (struct ice_devl_resource) {
		.name = "lut_2048",
		.parent_id = ICE_RSS_LUT_BOTH,
		.max_size = pf_lut_cnt,
		.get = ice_rss_lut_whole_dev_occ_get_pf,
		.set = ice_devl_resource_deny_occ_set,
	};
	adapter->resources[ICE_RSS_LUT_BOTH] = (struct ice_devl_resource) {
		.name = "rss",
		.parent_id = ICE_TOP_RESOURCE,
		.max_size = pf_lut_cnt + ICE_NUM_GLOBAL_LUTS,
		.get = ice_rss_lut_whole_dev_occ_get_both,
		.set = ice_devl_resource_deny_occ_set,
	};

	ice_devl_res_register(devlink, adapter->resources, adapter);
}

void ice_devl_pf_resources_register(struct ice_pf *pf)
{
	struct ice_devl_resource pf_resources[ICE_DEVL_RESOURCES_COUNT] = {
		[ICE_RSS_LUT_GLOBAL] = {
			.name = "lut_512",
			.parent_id = ICE_RSS_LUT_BOTH,
			.max_size = 1,
			.get = ice_rss_lut_pf_occ_get_global,
			.set = ice_rss_lut_pf_occ_set_global,
		},
		[ICE_RSS_LUT_PF] = {
			.name = "lut_2048",
			.parent_id = ICE_RSS_LUT_BOTH,
			.max_size = 1,
			.get = ice_rss_lut_pf_occ_get_pf,
			.set = ice_rss_lut_pf_occ_set_pf,
			.start_size = 1,
		},
		[ICE_RSS_LUT_BOTH] = {
			.name = "rss",
			.parent_id = ICE_TOP_RESOURCE,
			.max_size = 1,
			.get = ice_rss_lut_pf_occ_get_both,
			.set = ice_devl_resource_deny_occ_set,
		},
	};
	struct devlink *devlink = priv_to_devlink(pf);

	devl_assert_locked(devlink);
	ice_devl_res_register(devlink, pf_resources, pf);
}

void ice_devlink_vf_resources_register(struct ice_vf *vf)
{
	struct ice_devl_resource vf_resources[ICE_DEVL_RESOURCES_COUNT] = {
		[ICE_RSS_LUT_GLOBAL] = {
			.name = "lut_512",
			.parent_id = ICE_RSS_LUT_BOTH,
			.max_size = 1,
			.get = ice_rss_lut_vf_occ_get_global,
			.set = ice_rss_lut_vf_occ_set_global,
		},
		[ICE_RSS_LUT_PF] = {
			.name = "lut_2048",
			.parent_id = ICE_RSS_LUT_BOTH,
			.max_size = 1,
			.get = ice_rss_lut_vf_occ_get_pf,
			.set = ice_rss_lut_vf_occ_set_pf,
		},
		[ICE_RSS_LUT_BOTH] = {
			.name = "rss",
			.parent_id = ICE_TOP_RESOURCE,
			.max_size = 1,
			.get = ice_rss_lut_vf_occ_get_both,
			.set = ice_devl_resource_deny_occ_set,
		},
	};
	struct devlink *devlink = vf->devlink;

	scoped_guard(devl, devlink)
		ice_devl_res_register(devlink, vf_resources, vf);
}
