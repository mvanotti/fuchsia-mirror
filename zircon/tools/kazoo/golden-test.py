#!/usr/bin/env python3.8

# Copyright 2021 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""
Compares previous output of all kazoo generation with current output.

The current golden state (vs. real syscalls) is saved in golden.txt.

A `cp` command will be printed to update golden.txt if the current
pipeline generates different output than previously checked in.
"""

import argparse
import datetime
import os
import shutil
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(__file__)
GOLDEN = os.path.join(os.path.dirname(__file__), 'golden.txt')


def parse_args():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        '--fidlc', help='rspfile containing fidlc binary', required=True)
    parser.add_argument(
        '--kazoo', help='rspfile containing kazoo binary', required=True)
    parser.add_argument(
        '--syscalls',
        help='path to syscalls',
        default=os.path.normpath(
            os.path.join(SCRIPT_DIR, os.pardir, os.pardir, 'vdso')))
    parser.add_argument(
        '--p4merge',
        action='store_true',
        help='open diffs in p4merge if changes')
    parser.add_argument(
        '--output-touch', help='file to touch on success', required=True)
    parser.add_argument(
        '--tmp-base', help='location to use for temporary files', required=True)
    parser.add_argument(
        '--new-golden',
        help='path to output the new golden file',
        required=True)
    parser.add_argument(
        '--depfile',
        help='The path to write a depfile, see depfile from GN.',
        required=True)
    return parser.parse_args()


def generate_fidlc_json(fidlc_binary, syscall_dir, output_fidlc_json_path):
    fidl_files = []
    with os.scandir(syscall_dir) as it:
        for entry in it:
            if (not entry.name.startswith('.') and entry.is_file() and
                    entry.name.endswith('.fidl')):
                fidl_files.append(os.path.join(syscall_dir, entry.name))

    subprocess.check_call(
        [
            fidlc_binary, '--experimental', 'new_syntax_only', '--json',
            output_fidlc_json_path, '--files'
        ] + sorted(fidl_files))


def build_golden(input_files, output_combined):
    with open(output_combined, 'wb') as outf:
        for filename in input_files:
            outf.write(
                ('----- %s START -----\n' %
                 os.path.basename(filename)).encode('utf-8'))
            outf.write(open(filename, 'rb').read())
            outf.write(
                ('----- %s END -----\n\n\n' %
                 os.path.basename(filename)).encode('utf-8'))


def generate_kazoo_outputs(
        kazoo_binary, fidlc_json_path, all_output_styles, output_dir):

    def guess_ext(style):
        if style in ('go-vdso-arm64-calls', 'go-vdso-x86-calls'):
            return 's'
        if style.startswith('go'):
            return 'go'
        if style == 'rust':
            return 'rs'
        if style == 'json':
            return 'json'
        return 'h'

    style_args = []
    files = []
    for style in all_output_styles:
        filename = '%s.%s' % (os.path.join(output_dir, style), guess_ext(style))
        files.append(filename)
        style_args.append('--%s=%s' % (style, filename))
    subprocess.check_call([kazoo_binary] + style_args + [fidlc_json_path])
    return files


def read_rsp(path):
    with open(path, 'r') as f:
        return f.read().strip()


def main():
    args = parse_args()

    tmp_json = os.path.join(args.tmp_base, 'syscalls.json')
    fidlc = read_rsp(args.fidlc)
    generate_fidlc_json(fidlc, args.syscalls, tmp_json)

    tmp_kazoo_dir = os.path.join(args.tmp_base, 'kazoo-outputs')
    try:
        shutil.rmtree(tmp_kazoo_dir)
    except:
        pass
    os.makedirs(tmp_kazoo_dir)

    all_styles = (
        'c-ulib-header',
        'category',
        'go-syscall-arm64-asm',
        'go-syscall-stubs',
        'go-syscall-x86-asm',
        'go-vdso-arm64-calls',
        'go-vdso-keys',
        'go-vdso-x86-calls',
        'json',
        'kernel-header',
        'kernel-wrappers',
        'private-header',
        'public-header',
        'rust',
        'syscall-numbers',
        'testonly-public-header',
    )
    kazoo = read_rsp(args.kazoo)
    files = generate_kazoo_outputs(kazoo, tmp_json, all_styles, tmp_kazoo_dir)

    build_golden(files, args.new_golden)
    rc = subprocess.call(['diff', '-u', GOLDEN, args.new_golden])
    if rc != 0:
        print('CHANGES DETECTED IN SYSCALL OUTPUT')
        print()
        print('Please run:')
        print(
            '  cp "$(fx get-build-dir)/%s" "$(fx get-build-dir)/%s"' %
            (args.new_golden, GOLDEN))
        print('to acknowledge these changes.')
        print()
        print(
            'Files can be found locally in %s for inspection.' % tmp_kazoo_dir)
        if args.p4merge:
            subprocess.call(['p4merge', GOLDEN, args.new_golden])
    else:
        with open(args.output_touch, 'w') as f:
            f.write(str(datetime.datetime.utcnow()))
        with open(args.depfile, 'w') as f:
            f.write(f'{args.output_touch} {args.new_golden}: {fidlc} {kazoo}\n')

    shutil.rmtree(tmp_kazoo_dir)

    return rc


if __name__ == '__main__':
    sys.exit(main())
