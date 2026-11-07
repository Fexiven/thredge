# SPDX-License-Identifier: Apache-2.0
# No REQUIRES: the main component then depends on every component in the
# build, which is what the border-router headers included by main.c need.
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS ".")
