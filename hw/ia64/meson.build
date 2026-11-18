# SPDX-License-Identifier: BSD-3-Clause
ia64_ss = ss.source_set()
ia64_ss.add(when: 'CONFIG_SGI_SHUB', if_true: files('sgi-shub.c', 'sgi-shub-uart.c'))
# The x-shub1-test machine is a qtest fixture, so it is built with them only.
ia64_ss.add(when: 'CONFIG_ALTIX350_TEST_FIXTURES',
            if_true: files('sgi-shub-test.c'))
hw_arch += {'ia64': ia64_ss}
