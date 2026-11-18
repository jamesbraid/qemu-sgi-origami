# SPDX-License-Identifier: BSD-3-Clause
ia64_ss = ss.source_set()
ia64_ss.add(when: 'CONFIG_SGI_SHUB', if_true: files('sgi-shub.c'))
hw_arch += {'ia64': ia64_ss}
