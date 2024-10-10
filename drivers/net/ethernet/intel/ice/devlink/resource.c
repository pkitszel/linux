#include <linux/cleanup.h>
#include <net/devlink.h>

#include "resource.h"
#include "ice_adapter.h"
#include "ice.h"

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

static int ice_devl_res_take(struct ice_adapter *adapter,
			     enum ice_devl_resource_id res_id, int slot,
			     void *owner)
{
	struct ice_devl_resource *res = &adapter->resources[res_id];
	int end = slot == ICE_ANY_SLOT ? res->max_size : slot + 1;
	int beg = slot == ICE_ANY_SLOT ? 0 : slot;

	for (int id = beg; id < end; id++) {
		if (!res->owner[id]) {
			res->owner[id] = owner;
			return id;
		}
	}

	return -ENOSPC;
}

static void ice_devl_res_free(struct ice_adapter *adapter,
			      enum ice_devl_resource_id res_id, void *owner)
{
	struct ice_devl_resource *res = &adapter->resources[res_id];

	for (int i = 0; i < res->max_size; i++) {
		if (res->owner[i] == owner) {
			res->owner[i] = NULL;
			break;
		}
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

static int ice_devl_res_change(bool take, struct ice_adapter *adapter,
			       enum ice_devl_resource_id res_id, int slot,
			       void *owner)
{
	if (!take) {
		ice_devl_res_free(adapter, res_id, owner);
		return 0;
	}

	return ice_devl_res_take(adapter, res_id, slot, owner) < 0 ?
	       -ENOSPC : 0;
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
 * compute the current state of the resource the @owner have
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

static int ice_maybe_change_rss_lut(struct ice_pf *pf,
				    enum ice_rss_lut_resource_state old,
				    enum ice_rss_lut_resource_state new,
				    struct netlink_ext_ack *extack)
{
	enum ice_rss_lut_resource_state change_to = new & ~old;
	enum ice_rss_lut_resource_state change_from = old & ~new;
	struct ice_aq_get_set_rss_lut_params params = {};
	struct ice_vsi *vsi = ice_get_main_vsi(pf);
	u8 *lut __free(kfree) = NULL;
	enum ice_lut_type lut_type;
	int err, lut_size;

	if (change_to & ICE_HAS_PF_LUT) {
		NL_SET_ERR_MSG_MOD(extack, "change -> PF");
		lut_type = ICE_LUT_PF;
	} else if (change_from & ICE_HAS_PF_LUT) {
		NL_SET_ERR_MSG_MOD(extack, "change -> GLOBAL");
		lut_type = ICE_LUT_GLOBAL;
		params.global_lut_id = ice_devl_res_owned_idx(pf->adapter,
							      ICE_RSS_LUT_GLOBAL,
							      pf);
	} else {
		NL_SET_ERR_MSG_MOD(extack, "no change");
		return 0;
	}

	lut_size = ice_lut_type_to_size(lut_type);
	lut = kmalloc(lut_size, GFP_KERNEL);
	if (!lut)
		return -ENOMEM;

	ice_fill_rss_lut(lut, lut_size, vsi->rss_size);
	params.lut = lut;
	params.lut_size = lut_size;
	params.lut_type = lut_type;
	params.vsi_handle = vsi->idx;
	err = ice_aq_set_rss_lut(&pf->hw, &params);
	if (err)
		return err;

	vsi->rss_table_size = lut_size;
	return 0;
}

static int ice_rss_lut_pf_occ_set_validate(u64 size,
					   struct netlink_ext_ack *extack,
					   struct ice_pf *pf)
{
	/* devlink core performs basic val first, here we only forbid setting
	 * both PF and GLOBAL LUT counts of given PF VSI to 0. */

	if (size)
		return 0; /* fine to get more */

	if (ice_rss_lut_resource_state(pf->adapter, pf) == ICE_HAS_BOTH_LUTS)
		return 0; /* fine to give up one if you have both */

	NL_SET_ERR_MSG_MOD(extack,
		"at least one of 512+ sized LUTs must be assigned to PF device at all times");
	return -EDOM;
}

static int ice_rss_lut_pf_occ_set_pf(u64 size, struct netlink_ext_ack *extack,
				     void *priv)
{
	enum ice_rss_lut_resource_state old, new;
	struct ice_adapter *adapter;
	struct ice_pf *pf = priv;
	int pf_id = pf->hw.pf_id;
	int err;

	adapter = pf->adapter;
	scoped_guard(ice_adapter_devl, adapter) {
		old = ice_rss_lut_resource_state(adapter, pf);
		err = ice_rss_lut_pf_occ_set_validate(size, extack, pf);
		if (err)
			return err;

		err = ice_devl_res_change(size, pf->adapter, ICE_RSS_LUT_PF,
					  pf_id, pf);
		if (err)
			return err;

		new = ice_rss_lut_resource_state(adapter, pf);
		return ice_maybe_change_rss_lut(pf, old, new, extack);
	}
}

static int ice_rss_lut_pf_occ_set_global(u64 size,
					 struct netlink_ext_ack *extack,
					 void *priv)
{
	enum ice_rss_lut_resource_state old, new;
	struct ice_adapter *adapter;
	struct ice_pf *pf = priv;
	int err;

	adapter = pf->adapter;
	scoped_guard(ice_adapter_devl, adapter) {
		old = ice_rss_lut_resource_state(adapter, pf);
		err = ice_rss_lut_pf_occ_set_validate(size, extack, pf);
		if (err)
			return err;

		err = ice_devl_res_change(size, adapter, ICE_RSS_LUT_GLOBAL,
					  ICE_ANY_SLOT, pf);
		if (err)
			return err;

		new = ice_rss_lut_resource_state(adapter, pf);
		return ice_maybe_change_rss_lut(pf, old, new, extack);
	}
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
	struct ice_adapter *adapter = pf->adapter;
	int pf_id = pf->hw.pf_id;

	scoped_guard(ice_adapter_devl, adapter)
		return ice_devl_res_take(adapter, ICE_RSS_LUT_PF, pf_id, owner);
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
	struct ice_adapter *adapter = pf->adapter;

	scoped_guard(ice_adapter_devl, adapter)
		return ice_devl_res_take(adapter, ICE_RSS_LUT_GLOBAL,
					 ICE_ANY_SLOT, owner);
}

static void ice_devl_res_register(struct devlink *devlink,
				  struct ice_devl_resource *resources)
{
	struct devlink_resource_size_params size_params;

	devlink_resource_size_params_init(&size_params, 0, 0, 1,
					  DEVLINK_RESOURCE_UNIT_ENTRY);
	for (int i = 0; i < ICE_DEVL_RESOURCES_COUNT; i++) {
		struct ice_devl_resource *res = &resources[i];
		int err, resource_id = i;

		if (!res->max_size)
			continue;

		size_params.size_max = res->max_size;
		err = devl_resource_register(devlink, res->name,
					     res->start_size, resource_id,
					     res->parent_id, &size_params);
		if (WARN_ONCE(err, "not all resource handlers registered, err: %d, resname: %s\n",
			      err, res->name))
			break;

		devl_resource_occ_set_get_register(devlink, resource_id,
						   res->set, res->get,
						   devlink_priv(devlink));
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

	ice_devl_res_register(devlink, adapter->resources);
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
	ice_devl_res_register(devlink, pf_resources);
}
