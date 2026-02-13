/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026, Intel Corporation. */

struct devlink;
struct ice_hw;
struct ice_pf;

void ice_devl_pf_resources_register(struct ice_pf *pf);
void ice_devl_whole_dev_resources_register(const struct ice_hw *hw,
					   struct devlink *devlink);

int ice_take_rss_lut_pf(struct ice_pf *pf, void *owner);
int ice_take_rss_lut_global(struct ice_pf *pf, void *owner);
