#!/usr/bin/env python
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>

import os
import subprocess
import sys

def run_tests(directory):
    """Run all Python scripts in the given directory."""
    python_files = [f for f in os.listdir(directory) if f.endswith('test.py')]
    python_files.sort()

    for filename in python_files:
        try:
            filepath = os.path.join(directory, filename)

            subprocess.check_call([sys.executable, filepath])
        except subprocess.CalledProcessError as e:
            print(f"Error executing {filename}:")
            return
        except Exception as e:
            print(f"Error executing {filename}:")
            print(e)
            return

if __name__ == '__main__':
    tests_directory = '.'
    subprocess.check_call(['gcc', 'mem-allocate.c', '-o', 'mem-allocate'])
    subprocess.check_call(['gcc', 'simple-mem-allocate.c', '-o', 'simple-mem-allocate'])
    subprocess.check_call(['gcc', 'multiple-thread-mem-allocate.c', '-o', 'multiple-thread-mem-allocate'])
    run_tests(tests_directory)