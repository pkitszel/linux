struct devlink;
struct ice_hw;
struct ice_pf;
struct ice_vf;

void ice_devl_pf_resources_register(struct ice_pf *pf);
void ice_devl_whole_dev_resources_register(const struct ice_hw *hw,
					   struct devlink *devlink);
void ice_devlink_vf_resources_register(struct ice_vf *vf);

int ice_take_rss_lut_pf(struct ice_pf *pf, void *owner);
int ice_take_rss_lut_global(struct ice_pf *pf, void *owner);

void ice_free_rss_lut_all(struct ice_vf *vf);
