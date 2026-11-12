# SPDX-License-Identifier: BSD-3-Clause
# A library for device models; a board that embeds it selects DSP56300_CORE.
system_ss.add(when: 'CONFIG_DSP56300_CORE', if_true: files(
  'dsp56300.c',
  'dsp56300-alu.c',
  'dsp56300-ops.c',
))
