// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, Intel Corporation. */

#include "resource.h"
#include "ice_adapter.h"
#include "ice.h"
#include "ice_lib.h"

#define ICE_NUM_GLOBAL_LUTS	16
#define ICE_ANY_SLOT		-1

/* each NAC numbers its PFs from 0, secondary NAC ones go after primary ones */
static int ice_devl_pf_id(struct ice_pf *pf)
{
	struct ice_hw *hw = &pf->hw;

	if (ice_is_primary(hw))
		return hw->logical_pf_id;

	return hw->logical_pf_id + ICE_PORTS_PER_QUAD;
}

/* each NAC has its own global LUTs, with IDs given by its FW */
static struct ice_devl_resource *
ice_devl_res_tbl(struct ice_pf *pf, enum ice_devl_resource_id res_id)
{
	if (res_id == ICE_RSS_LUT_GLOBAL)
		return &pf->adapter->global_luts[!ice_is_primary(&pf->hw)];

	return &pf->adapter->pf_luts;
}

static u32 ice_devl_res_cnt(const struct ice_devl_resource *res)
{
	u32 sum = 0;

	for (int i = 0; i < ARRAY_SIZE(res->owner); i++)
		sum += res->owner[i] != NULL;

	return sum;
}

static int ice_devl_res_take(struct ice_pf *pf,
			     enum ice_devl_resource_id res_id, int slot,
			     void *owner)
{
	struct ice_devl_resource *res = ice_devl_res_tbl(pf, res_id);
	int beg, end, new_id = ICE_ANY_SLOT;
	u16 lut_id;

	if (res_id == ICE_RSS_LUT_GLOBAL) {
		int err;

		WARN_ON_ONCE(slot != ICE_ANY_SLOT);

		err = ice_alloc_rss_global_lut(&pf->hw, &lut_id);
		if (err)
			return err;
		if (lut_id >= ARRAY_SIZE(res->owner)) {
			ice_free_rss_global_lut(&pf->hw, lut_id);
			return -EIO;
		}

		slot = lut_id;
	}

	end = slot == ICE_ANY_SLOT ? ARRAY_SIZE(res->owner) : slot + 1;
	beg = slot == ICE_ANY_SLOT ? 0 : slot;
	for (int id = beg; id < end; id++) {
		if (!res->owner[id]) {
			new_id = id;
			break;
		}
	}
	if (new_id == ICE_ANY_SLOT) {
		if (WARN_ON(res_id == ICE_RSS_LUT_GLOBAL))
			ice_free_rss_global_lut(&pf->hw, lut_id);

		return -ENOSPC;
	}

	res->owner[new_id] = owner;
	res->pf_id[new_id] = ice_devl_pf_id(pf);
	return new_id;
}

static int ice_devl_res_free(struct ice_pf *pf,
			     enum ice_devl_resource_id res_id, void *owner)
{
	struct ice_devl_resource *res = ice_devl_res_tbl(pf, res_id);
	int err = 0, id_to_free = ICE_ANY_SLOT;

	for (int i = 0; i < ARRAY_SIZE(res->owner); i++) {
		if (res->owner[i] == owner) {
			id_to_free = i;
			break;
		}
	}
	if (id_to_free == ICE_ANY_SLOT)
		return 0;

	if (res_id == ICE_RSS_LUT_GLOBAL) {
		err = ice_free_rss_global_lut(&pf->hw, id_to_free);
		if (err)
			dev_warn(ice_pf_to_dev(pf), "failed to free global RSS LUT %d, leaked until PF reset: %d\n",
				 id_to_free, err);
	}

	res->owner[id_to_free] = NULL;
	return err;
}

static void ice_free_rss_lut_flr_locked(struct ice_pf *pf)
{
	int pf_id = ice_devl_pf_id(pf);
	struct ice_devl_resource *res;

	devl_assert_locked(pf->adapter->devlink);
	pf->adapter->pf_luts.owner[pf_id] = NULL;

	res = ice_devl_res_tbl(pf, ICE_RSS_LUT_GLOBAL);
	for (int i = 0; i < ARRAY_SIZE(res->owner); i++) {
		/* On FLR/PFR resources assigned to PF are cleared by FW,
		 * reflect that in the SW table.
		 * VFs on given PF must be de-programmed too.
		 */
		if (res->pf_id[i] == pf_id)
			res->owner[i] = NULL;
	}
}

void ice_free_rss_lut_flr(struct ice_pf *pf)
{
	scoped_guard(ice_adapter_devl, pf->adapter)
		ice_free_rss_lut_flr_locked(pf);
}

void ice_free_rss_lut_vf(struct ice_vf *vf)
{
	struct ice_pf *pf = vf->pf;

	scoped_guard(ice_adapter_devl, pf->adapter) {
		ice_devl_res_free(pf, ICE_RSS_LUT_GLOBAL, vf);
		ice_devl_res_free(pf, ICE_RSS_LUT_PF, vf);
	}
}

static int ice_devl_res_owned_idx(struct ice_pf *pf,
				  enum ice_devl_resource_id res_id, void *owner)
{
	const struct ice_devl_resource *res = ice_devl_res_tbl(pf, res_id);

	for (int i = 0; i < ARRAY_SIZE(res->owner); i++) {
		if (res->owner[i] == owner)
			return i;
	}

	return -ENXIO;
}

static bool ice_is_devl_res_owned_by(struct ice_pf *pf,
				     enum ice_devl_resource_id res_id,
				     void *owner)
{
	return ice_devl_res_owned_idx(pf, res_id, owner) >= 0;
}

static u64 ice_rss_lut_whole_dev_occ_get_global(void *priv)
{
	struct ice_adapter *adapter = priv;

	return ice_devl_res_cnt(&adapter->global_luts[0]) +
	       ice_devl_res_cnt(&adapter->global_luts[1]);
}

static u64 ice_rss_lut_whole_dev_occ_get_pf(void *priv)
{
	struct ice_adapter *adapter = priv;

	return ice_devl_res_cnt(&adapter->pf_luts);
}

static enum ice_lut_type ice_rss_lut_type(struct ice_pf *pf, void *owner)
{
	if (ice_is_devl_res_owned_by(pf, ICE_RSS_LUT_PF, owner))
		return ICE_LUT_PF;
	if (ice_is_devl_res_owned_by(pf, ICE_RSS_LUT_GLOBAL, owner))
		return ICE_LUT_GLOBAL;

	return ICE_LUT_VSI;
}

static u64 ice_rss_lut_pf_size_get(void *priv)
{
	struct ice_pf *pf = priv;

	scoped_guard(ice_adapter_devl, pf->adapter) {
		enum ice_lut_type type = ice_rss_lut_type(pf, pf);

		/* reset in progress, it gives the PF its PF LUT back */
		if (type == ICE_LUT_VSI)
			type = ICE_LUT_PF;
		return ice_lut_type_to_size(type);
	}
}

static u64 ice_rss_lut_vf_size_get(void *priv)
{
	struct ice_vf *vf = priv;
	struct ice_pf *pf;

	pf = vf->pf;
	scoped_guard(ice_adapter_devl, pf->adapter)
		return ice_lut_type_to_size(ice_rss_lut_type(pf, vf));
}

static int ice_rss_lut_user_resize(struct ice_vsi *vsi,
				   enum ice_lut_type lut_type, u8 **user_lut,
				   struct netlink_ext_ack *extack)
{
	int qs_num = ice_lut_type_to_qs_num(lut_type);
	int size = ice_lut_type_to_size(lut_type);
	struct net_device *netdev = vsi->netdev;
	int old_size = vsi->rss_table_size;
	u32 max_q = 0;
	int err = 0;
	u32 *indir;
	u8 *lut;

	if (!netdev || !vsi->rss_lut_user || !netif_is_rxfh_configured(netdev))
		return 0;

	indir = kmalloc_objs(*indir, max(old_size, size));
	if (!indir)
		return -ENOMEM;

	for (int i = 0; i < old_size; i++) {
		indir[i] = vsi->rss_lut_user[i];
		max_q = max(max_q, indir[i]);
	}
	if (!ethtool_rxfh_indir_can_resize(netdev, indir, old_size, size)) {
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "can't fold the RSS indirection table to %d entries, reset it first",
				       size);
		err = -EINVAL;
		goto out;
	}
	if (max_q >= qs_num) {
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "RSS indirection table uses queue %u, the new LUT is limited to %d queues",
				       max_q, qs_num);
		err = -EINVAL;
		goto out;
	}

	lut = devm_kmalloc(ice_pf_to_dev(vsi->back), size, GFP_KERNEL);
	if (!lut) {
		err = -ENOMEM;
		goto out;
	}

	ethtool_rxfh_indir_resize(netdev, indir, old_size, size);
	for (int i = 0; i < size; i++)
		lut[i] = indir[i];
	*user_lut = lut;
out:
	kfree(indir);
	return err;
}

static int ice_change_rss_lut(struct ice_pf *pf, void *owner,
			      enum ice_lut_type lut_type,
			      struct netlink_ext_ack *extack)
{
	struct ice_aq_get_set_rss_lut_params params = {};
	struct ice_hw *hw = &pf->hw;
	int err, lut_size, lut_id;
	struct ice_vf *vf = NULL;
	struct ice_vsi *vsi;
	u8 *user_lut = NULL;
	u16 rss_size;
	u8 *lut;

	if (lut_type == ICE_LUT_GLOBAL) {
		lut_id = ice_devl_res_owned_idx(pf, ICE_RSS_LUT_GLOBAL, owner);
		if (lut_id < 0)
			return lut_id;

		params.global_lut_id = lut_id;
	}

	if (pf == owner) {
		vsi = ice_get_main_vsi(pf);
	} else {
		vf = owner;
		vsi = ice_get_vf_vsi(vf);
	}

	lut_size = ice_lut_type_to_size(lut_type);
	lut = kzalloc(lut_size, GFP_KERNEL);
	if (!lut)
		return -ENOMEM;
	params.lut = lut;
	params.lut_size = lut_size;
	params.lut_type = lut_type;
	params.vsi_handle = vsi->idx;
	mutex_lock(&pf->rss_lut_lock);
	rss_size = min(vsi->num_rxq, ice_lut_type_to_qs_num(lut_type));
	err = ice_rss_lut_user_resize(vsi, lut_type, &user_lut, extack);
	if (err)
		goto out;
	/* PF with RXHASH disabled keeps its LUT zeroed */
	if (vf || !vsi->netdev || vsi->netdev->features & NETIF_F_RXHASH) {
		if (user_lut)
			memcpy(lut, user_lut, lut_size);
		else
			ice_fill_rss_lut(lut, lut_size, rss_size);
	}
	err = ice_aq_set_rss_lut(hw, &params);
	if (err) {
		NL_SET_ERR_MSG_FMT(extack, "AQ failed: %s", libie_aq_str(hw->adminq.sq_last_status));
		goto out;
	}

	err = ice_vsi_update_rss_lut(vsi, lut_type, params.global_lut_id);
	if (err)
		goto out;

	devm_kfree(ice_pf_to_dev(pf), vsi->rss_lut_user);
	vsi->rss_lut_user = user_lut;
	user_lut = NULL;
	if (lut_type == ICE_LUT_GLOBAL)
		vsi->global_lut_id = params.global_lut_id;
	vsi->rss_table_size = lut_size;
	vsi->rss_lut_type = lut_type;
	vsi->rss_size = rss_size;
	if (vf) {
		u16 max_qs = ice_lut_type_to_qs_num(lut_type);

		/* don't keep queues that the new LUT can't spread traffic to */
		if ((vf->num_req_qs ?: vf->num_vf_qs) > max_qs)
			vf->num_req_qs = max_qs;
		ice_schedule_vf_reset(vf);
	}
out:
	mutex_unlock(&pf->rss_lut_lock);
	devm_kfree(ice_pf_to_dev(pf), user_lut);
	kfree(lut);
	return err;
}

static enum ice_devl_resource_id ice_rss_lut_res_id(enum ice_lut_type type)
{
	return type == ICE_LUT_PF ? ICE_RSS_LUT_PF : ICE_RSS_LUT_GLOBAL;
}

static int ice_rss_lut_set(struct ice_pf *pf, void *owner, u64 size,
			   struct netlink_ext_ack *extack)
{
	enum ice_lut_type old_type, new_type;
	int err, slot;

	if (!test_bit(ICE_FLAG_RSS_ENA, pf->flags)) {
		NL_SET_ERR_MSG_MOD(extack, "RSS is not supported");
		return -EOPNOTSUPP;
	}

	switch (size) {
	case ICE_LUT_VSI_SIZE:
		new_type = ICE_LUT_VSI;
		break;
	case ICE_LUT_GLOBAL_SIZE:
		new_type = ICE_LUT_GLOBAL;
		break;
	case ICE_LUT_PF_SIZE:
		new_type = ICE_LUT_PF;
		break;
	default:
		if (owner == pf)
			NL_SET_ERR_MSG_MOD(extack,
					   "RSS LUT size must be 512 or 2048");
		else
			NL_SET_ERR_MSG_MOD(extack,
					   "RSS LUT size must be 64, 512 or 2048");
		return -EINVAL;
	}

	old_type = ice_rss_lut_type(pf, owner);
	if (new_type == old_type)
		return 0;

	/* reset rebuilds the table and the VSIs, see ice_rebuild_rss_luts(),
	 * pairs with adapter devl taken in ice_prepare_rss_luts_for_reset()
	 */
	if (ice_is_reset_in_progress(pf->state)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "reset in progress, try again later");
		return -EBUSY;
	}

	/* pairs with set_bit() under adapter devl in ice_setup_tc_mqprio_qdisc() */
	if (test_bit(ICE_FLAG_TC_MQPRIO, pf->flags)) {
		NL_SET_ERR_MSG_MOD(extack, "ADQ configured, can't change RSS LUTs");
		return -EOPNOTSUPP;
	}

	if (new_type != ICE_LUT_VSI) {
		slot = ICE_ANY_SLOT;
		if (new_type == ICE_LUT_PF)
			slot = ice_devl_pf_id(pf);
		err = ice_devl_res_take(pf, ice_rss_lut_res_id(new_type), slot,
					owner);
		if (err < 0) {
			if (new_type == ICE_LUT_PF)
				NL_SET_ERR_MSG_MOD(extack,
						   "PF LUT is used by another function");
			else
				NL_SET_ERR_MSG_MOD(extack,
						   "failed to get a global RSS LUT");
			return err;
		}
	}

	err = ice_change_rss_lut(pf, owner, new_type, extack);
	if (err) {
		NL_SET_ERR_MSG_WEAK_MOD(extack, "failed to change RSS LUT");
		if (new_type != ICE_LUT_VSI)
			ice_devl_res_free(pf, ice_rss_lut_res_id(new_type),
					  owner);
		return err;
	}

	if (old_type != ICE_LUT_VSI)
		ice_devl_res_free(pf, ice_rss_lut_res_id(old_type), owner);

	return 0;
}

/* ethtool core sizes RSS buffers by rss_table_size under rtnl, and the new
 * LUT is filled based on NETIF_F_RXHASH, which is also changed under rtnl
 */
static int ice_rss_lut_pf_size_set(u64 size, struct netlink_ext_ack *extack,
				   void *priv)
{
	struct ice_pf *pf = priv;
	int err;

	rtnl_lock();
	scoped_guard(ice_adapter_devl, pf->adapter)
		err = ice_rss_lut_set(pf, pf, size, extack);
	rtnl_unlock();

	return err;
}

static int ice_rss_lut_vf_size_set(u64 size, struct netlink_ext_ack *extack,
				   void *priv)
{
	struct ice_vf *vf = priv;
	struct ice_pf *pf;

	pf = vf->pf;
	scoped_guard(mutex, &vf->cfg_lock) {
		if (!ice_vf_is_ready(vf))
			return -EBUSY;

		scoped_guard(ice_adapter_devl, pf->adapter)
			return ice_rss_lut_set(pf, vf, size, extack);
	}
}

/**
 * ice_rss_lut_is_reassigned - check if RSS LUTs of PF are in non-default state
 * @pf: the PF to check
 *
 * Return: true if @pf lost its PF LUT, or @pf or any of its VFs owns a
 * global LUT. Caller must hold the adapter devlink lock.
 */
bool ice_rss_lut_is_reassigned(struct ice_pf *pf)
{
	int pf_id = ice_devl_pf_id(pf);
	struct ice_devl_resource *res;

	devl_assert_locked(pf->adapter->devlink);
	if (pf->adapter->pf_luts.owner[pf_id] != pf)
		return true;

	res = ice_devl_res_tbl(pf, ICE_RSS_LUT_GLOBAL);
	for (int i = 0; i < ARRAY_SIZE(res->owner); i++)
		if (res->owner[i] && res->pf_id[i] == pf_id)
			return true;

	return false;
}

/**
 * ice_take_rss_lut_pf - allocate PF RSS LUT for PF
 * @pf: the PF device that PF LUT is physically on, and to allocate it for
 *
 * Acquire PF RSS LUT for the caller.
 *
 * Return: 0 on success, negative on error.
 */
int ice_take_rss_lut_pf(struct ice_pf *pf)
{
	int ret, pf_id = ice_devl_pf_id(pf);

	scoped_guard(ice_adapter_devl, pf->adapter) {
		ret = ice_devl_res_take(pf, ICE_RSS_LUT_PF, pf_id, pf);
		if (ret >= 0)
			return 0;

		return ret;
	}
}

/**
 * ice_release_rss_lut_pf - release PF RSS LUT of PF
 * @pf: the PF device that PF LUT is physically on, and to release it from
 *
 * Release PF RSS LUT for the caller.
 *
 * No need to call this function in normal operation, provided for error path.
 */
void ice_release_rss_lut_pf(struct ice_pf *pf)
{
	scoped_guard(ice_adapter_devl, pf->adapter)
		ice_devl_res_free(pf, ICE_RSS_LUT_PF, pf);
}

/**
 * ice_prepare_rss_luts_for_reset - drop global RSS LUTs of PF from the table
 * @pf: the PF that is about to be reset
 *
 * FW frees global RSS LUTs of the PF and its VFs on reset, and other PFs may
 * get them from FW before ice_rebuild_rss_luts() of @pf, so drop them from
 * the table now. Taking the lock also waits for RSS LUT changes that checked
 * for reset just before it was requested.
 */
void ice_prepare_rss_luts_for_reset(struct ice_pf *pf)
{
	int pf_id = ice_devl_pf_id(pf);
	struct ice_devl_resource *res;

	scoped_guard(ice_adapter_devl, pf->adapter) {
		if (ice_rss_lut_is_reassigned(pf))
			dev_info(ice_pf_to_dev(pf),
				 "reset restores the default RSS LUT assignment\n");

		res = ice_devl_res_tbl(pf, ICE_RSS_LUT_GLOBAL);
		for (int i = 0; i < ARRAY_SIZE(res->owner); i++)
			if (res->pf_id[i] == pf_id)
				res->owner[i] = NULL;
	}
}

/**
 * ice_rebuild_rss_lut_pf - restore default RSS LUT assignment after reset
 * @pf: the PF that was reset
 *
 * FW drops RSS LUT assignments of the PF on reset, so give the PF LUT back to
 * the PF, and make the PF VSI use it.
 * Must be called before the VSIs are rebuilt.
 *
 * Return: 0 on success, negative on error.
 */
static int ice_rebuild_rss_lut_pf(struct ice_pf *pf)
{
	struct ice_vsi *vsi = ice_get_main_vsi(pf);
	struct ice_adapter *adapter = pf->adapter;
	u8 *user_lut = NULL;
	int err;

	devl_lock(adapter->devlink);
	ice_free_rss_lut_flr_locked(pf);
	err = ice_devl_res_take(pf, ICE_RSS_LUT_PF, ice_devl_pf_id(pf), pf);
	if (err < 0)
		goto unlock;

	mutex_lock(&pf->rss_lut_lock);
	if (vsi && vsi->rss_lut_type != ICE_LUT_PF) {
		/* growing fails only on ENOMEM, losing the user RSS LUT */
		ice_rss_lut_user_resize(vsi, ICE_LUT_PF, &user_lut, NULL);
		devm_kfree(ice_pf_to_dev(pf), vsi->rss_lut_user);
		vsi->rss_lut_user = user_lut;
		vsi->rss_table_size = ice_lut_type_to_size(ICE_LUT_PF);
		vsi->rss_lut_type = ICE_LUT_PF;
	}
	mutex_unlock(&pf->rss_lut_lock);
	err = 0;
unlock:
	devl_unlock(adapter->devlink);

	return err;
}

/**
 * ice_rebuild_rss_luts - restore default RSS LUT assignment after reset
 * @pf: the PF that was reset
 *
 * Wrapper of ice_rebuild_rss_lut_pf() that also tells the ethtool core when
 * the user RSS LUT was dropped, and makes the VF VSIs use their default LUTs.
 * Must be called without rtnl held.
 *
 * Return: 0 on success, negative on error.
 */
int ice_rebuild_rss_luts(struct ice_pf *pf)
{
	u16 max_qs = ice_lut_type_to_qs_num(ICE_LUT_VSI);
	struct ice_vsi *vsi = ice_get_main_vsi(pf);
	struct ice_vf *vf;
	unsigned int bkt;
	int err;

	/* ethtool core sizes RSS buffers by rss_table_size under rtnl */
	rtnl_lock();
	err = ice_rebuild_rss_lut_pf(pf);
	if (vsi && vsi->netdev && !vsi->rss_lut_user &&
	    netif_is_rxfh_configured(vsi->netdev))
		ethtool_rxfh_indir_lost(vsi->netdev);
	rtnl_unlock();
	if (err)
		return err;

	/* VF creation takes rtnl under table_lock, so don't nest them here */
	mutex_lock(&pf->vfs.table_lock);
	mutex_lock(&pf->rss_lut_lock);
	ice_for_each_vf(pf, bkt, vf) {
		vsi = ice_get_vf_vsi(vf);
		if (!vsi || vsi->rss_lut_type == ICE_LUT_VSI)
			continue;

		vsi->rss_table_size = ice_lut_type_to_size(ICE_LUT_VSI);
		vsi->rss_lut_type = ICE_LUT_VSI;
		/* don't keep queues that the VSI LUT can't spread traffic to */
		if ((vf->num_req_qs ?: vf->num_vf_qs) > max_qs)
			vf->num_req_qs = max_qs;
	}
	mutex_unlock(&pf->rss_lut_lock);
	mutex_unlock(&pf->vfs.table_lock);

	return 0;
}

static void ice_devl_hw_res_register(struct devlink *devlink, const char *name,
				     enum ice_devl_resource_id res_id, u64 cnt,
				     devlink_resource_occ_get_t *occ_get,
				     struct ice_adapter *adapter)
{
	struct devlink_resource_size_params size_params;
	int err;

	devlink_resource_size_params_init(&size_params, cnt, cnt, 1,
					  DEVLINK_RESOURCE_UNIT_ENTRY);
	err = devl_resource_register(devlink, name, cnt, res_id,
				     ICE_TOP_RESOURCE, &size_params);
	if (WARN_ONCE(err, "failed to register devlink resource %s: %d\n",
		      name, err))
		return;

	devl_resource_occ_get_register(devlink, res_id, occ_get, adapter);
}

static void ice_devl_rss_lut_register(struct devlink *devlink, u64 size_min,
				      devlink_resource_size_get_t *size_get,
				      devlink_resource_size_set_t *size_set,
				      void *priv)
{
	struct devlink_resource_size_params size_params;
	int err;

	devlink_resource_size_params_init(&size_params, size_min,
					  ICE_LUT_PF_SIZE, size_min,
					  DEVLINK_RESOURCE_UNIT_ENTRY);
	err = devl_resource_register(devlink, "rss_lut", size_get(priv),
				     ICE_RSS_LUT, ICE_TOP_RESOURCE,
				     &size_params);
	if (WARN_ONCE(err, "failed to register devlink resource rss_lut: %d\n",
		      err))
		return;

	devl_resource_runtime_register(devlink, ICE_RSS_LUT, size_get, size_set,
				       priv);
}

void ice_devl_whole_dev_resources_register(const struct ice_hw *hw,
					   struct ice_adapter *adapter)
{
	struct devlink *devlink = adapter->devlink;
	int global_lut_cnt = ICE_NUM_GLOBAL_LUTS;
	int pf_lut_cnt = hw->dev_caps.num_funcs;

	devl_assert_locked(devlink);
	/* see ice_devl_pf_id() and ice_devl_res_tbl() */
	if (ice_is_dual(hw)) {
		global_lut_cnt = 2 * ICE_NUM_GLOBAL_LUTS;
		pf_lut_cnt = 2 * ICE_PORTS_PER_QUAD;
	}

	ice_devl_hw_res_register(devlink, "rss_lut_512", ICE_RSS_LUT_GLOBAL,
				 global_lut_cnt,
				 ice_rss_lut_whole_dev_occ_get_global, adapter);
	ice_devl_hw_res_register(devlink, "rss_lut_2048", ICE_RSS_LUT_PF,
				 pf_lut_cnt, ice_rss_lut_whole_dev_occ_get_pf,
				 adapter);
}

void ice_devl_pf_resources_register(struct ice_pf *pf)
{
	struct devlink *devlink = priv_to_devlink(pf);

	devl_assert_locked(devlink);
	/* PF can't use a VSI LUT */
	ice_devl_rss_lut_register(devlink, ICE_LUT_GLOBAL_SIZE,
				  ice_rss_lut_pf_size_get,
				  ice_rss_lut_pf_size_set, pf);
}

void ice_devlink_vf_resources_register(struct ice_vf *vf)
{
	struct devlink *devlink = vf->devlink;

	devl_assert_locked(devlink);
	ice_devl_rss_lut_register(devlink, ICE_LUT_VSI_SIZE,
				  ice_rss_lut_vf_size_get,
				  ice_rss_lut_vf_size_set, vf);
}
