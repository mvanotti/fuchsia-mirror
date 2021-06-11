#!/usr/bin/env python3.8
# Copyright 2021 The Fuchsia Authors
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT

import argparse
import json
import os
import string

from datetime import datetime

HEADER = """// Copyright %s The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

// GENERATED BY //zircon/kernel/lib/code-patching/hermetic-embedding.py.
// DO NOT EDIT.
""" % datetime.today().year

ASM_FILE = string.Template(
    HEADER + """
#include <lib/arch/asm.h>

.text

$body

""")

CPP_FILE = string.Template(
    HEADER + """
#include <ktl/byte.h>
#include <ktl/span.h>
#include <ktl/string_view.h>

$externs

// Looks a code patching alternative up by name, returning its raw, binary
// contents if known or else returning an empty span.
ktl::span<const ktl::byte> GetPatchAlternative(ktl::string_view name) {
$function_body
}

""")


def main():
    parser = argparse.ArgumentParser(
        description=
        "Creates an object file defining a function for looking up hermetic alternative blobs by name"
    )
    parser.add_argument("--name", help="The name of the stub function")
    # Metadata contents is expected to be in the following format:
    #
    # [
    #   { entry: "memset_fast", path: "build/dir/relative/path/to/x.bin" },
    #   { entry: "memset_slow", path: "build/dir/relative/path/to/y.bin" }
    # ]
    parser.add_argument(
        "--metadata-file",
        metavar="FILE",
        help="a JSON file of hermetic patch alternative metadata")
    parser.add_argument(
        "--depfile", metavar="FILE", help="A dependencies file to write to")
    parser.add_argument(
        "--asm-outfile",
        metavar="FILE",
        help=
        ".S file that defines symbols giving hermetic patch alternative content."
    )
    parser.add_argument(
        "--cpp-outfile",
        metavar="FILE",
        help=
        ".cc file that defines a function for looking up hermetic patch alternative content by name"
    )
    args = parser.parse_args()

    with open(args.metadata_file, "r") as metadata_file:
        metadata = json.load(metadata_file)

    deps = []
    asm_body = []
    cpp_externs = []
    cpp_function_body = []
    for entry in metadata:
        name = entry["name"]
        path = entry["path"]
        alternative_start = "HERMETIC_ALTERNATIVE_START_%s" % name
        alternative_stop = "HERMETIC_ALTERNATIVE_STOP_%s" % name

        deps.append(path)

        # Defines the symbols for the beginnings and ends of each alternative.
        asm_body.extend(
            [
                ".object %s, data, global" % alternative_start,
                ".incbin \"%s\"" % path,
                ".label %s, global" % alternative_stop,
                ".end_object",
                "",
            ])
        # The .cc file will declare the beginnings and ends as extern - and
        # further define a function for looking up an alternative by name.
        cpp_externs.extend(
            [
                "extern \"C\" const ktl::byte %s[];" % alternative_start,
                "extern \"C\" const ktl::byte %s[];" % alternative_stop,
            ])
        cpp_function_body.extend(
            [
                "   if (name == \"%s\"sv) {" % name,
                "       return {%s, static_cast<size_t>(%s - %s)};" %
                (alternative_start, alternative_stop, alternative_start),
                "   }",
            ])

    # Return an empty span if we were unable to find a match.
    cpp_function_body.append("   return {};")

    with open(args.depfile, "w") as depfile:
        dep_list = " ".join(sorted(deps))
        depfile.write("%s: %s\n" % (args.asm_outfile, dep_list))
        depfile.write("%s: %s\n" % (args.cpp_outfile, dep_list))

    with open(args.asm_outfile, "w") as outfile:
        outfile.write(ASM_FILE.substitute(body="\n".join(asm_body)))

    with open(args.cpp_outfile, "w") as outfile:
        outfile.write(
            CPP_FILE.substitute(
                externs="\n".join(cpp_externs),
                function_body="\n".join(cpp_function_body)))


if __name__ == "__main__":
    main()
