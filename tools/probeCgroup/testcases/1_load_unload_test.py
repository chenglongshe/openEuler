#!/usr/bin/env python
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>

import os
import subprocess
import time

def test_module_load_unload():
    try:
        subprocess.check_call(['insmod', '../probeCgroup.ko'])
        time.sleep(1)
        print('loading module successfully!')
        subprocess.check_call(['rmmod', 'probeCgroup'])
        output = subprocess.check_output(['lsmod'])
        assert b'probeCgroup' not in output
        print('unloading module successfully!')
    except subprocess.CalledProcessError as e:
        print('Load unload test failed. Insmod failed.')
    except AssertionError as e:
        print('Load unload test failed. Cannot remove module.')

if __name__ == '__main__':
    test_module_load_unload()