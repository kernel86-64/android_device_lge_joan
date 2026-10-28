#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

def FullOTA_InstallEnd(info):
    info.script.AppendExtra('assert(run_program("/tmp/install/bin/device_check.sh") == 0);')
    # Keep the automatic-sideload recovery in sync with the installed ROM.
    info.script.AppendExtra(
        'assert(package_extract_file("recovery.img", "/dev/block/by-name/recovery"));'
    )
