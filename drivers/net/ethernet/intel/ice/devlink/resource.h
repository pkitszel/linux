struct ice_pf;

void ice_devlink_whole_dev_resources_register(struct device *dev,
					      struct devlink *devlink);
void ice_devl_pf_resources_register(struct ice_pf *pf);

int ice_take_rss_lut_pf(struct ice_pf *pf, void *owner);
int ice_take_rss_lut_global(struct ice_pf *pf, void *owner);
