/*
 * Copyright (c) 2002-2017 Balabit
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#ifndef __PE_VERSIONING_H__
#define __PE_VERSIONING_H__

#include "syslog-ng-config.h"

/* version references for major syslog-ng OSE versions. All error messages
 * should reference the syslog-ng version number through these macros, in order
 * to make it relatively simple to explain PE/OSE version numbers to users. */

#if (SYSLOG_NG_USE_PE_VERSION_MAPPING)
#if defined(__VERSIONING_H__)
#undef VERSION_3_0
#undef VERSION_3_1
#undef VERSION_3_2
#undef VERSION_3_3
#undef VERSION_3_4
#undef VERSION_3_5
#undef VERSION_3_6
#undef VERSION_3_7
#undef VERSION_3_8
#undef VERSION_3_9
#undef VERSION_3_10

#undef VERSION_VALUE_3_10
#undef VERSION_VALUE
#undef VERSION_CURRENT
#undef VERSION_CURRENT_VER_ONLY
#endif


#define VERSION_3_0 "syslog-ng PE 3.0"
#define VERSION_3_1 "syslog-ng PE 4.0"
#define VERSION_3_2 "syslog-ng PE 4.0"
#define VERSION_3_3 "syslog-ng PE 4.1"
#define VERSION_3_4 "syslog-ng 3.4"
#define VERSION_3_5 "syslog-ng 3.5"
#define VERSION_3_6 "syslog-ng 3.6"
#define VERSION_3_7 "syslog-ng 3.7"
#define VERSION_3_8 "syslog-ng PE 7.0"
#define VERSION_3_9 "syslog-ng PE 7.0"
#define VERSION_3_10 "syslog-ng PE 7.0"

#define VERSION_VALUE_3_10 0x0700
#define VERSION_VALUE   0x0700
#define VERSION_CURRENT VERSION_3_10
#define VERSION_CURRENT_VER_ONLY "7.0"

#endif
#endif
