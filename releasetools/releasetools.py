#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

def FullOTA_InstallEnd(info):
    info.script.AppendExtra('assert(run_program("/tmp/install/bin/device_check.sh") == 0);')
