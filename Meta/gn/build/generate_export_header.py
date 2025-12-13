#!/usr/bin/env python3

# Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse

TEMPLATE = """
#ifndef {EXPORT_MACRO_NAME}_H
#define {EXPORT_MACRO_NAME}_H

#ifdef {PREFIX}_STATIC_DEFINE
#  define {EXPORT_MACRO_NAME}
#  define {PREFIX}_NO_EXPORT
#else
#  ifndef {EXPORT_MACRO_NAME}
#    ifdef {PREFIX}_EXPORTS
        /* We are building this library */
#      define {EXPORT_MACRO_NAME} {ATTRIBUTE_EXPORT}
#    else
        /* We are using this library */
#      define {EXPORT_MACRO_NAME} {ATTRIBUTE_IMPORT}
#    endif
#  endif

#  ifndef {PREFIX}_NO_EXPORT
#    define {PREFIX}_NO_EXPORT {ATTRIBUTE_NO_EXPORT}
#  endif
#endif

#ifndef {PREFIX}_DEPRECATED
#  define {PREFIX}_DEPRECATED {ATTRIBUTE_DEPRECATED}
#endif

#ifndef {PREFIX}_DEPRECATED_EXPORT
#  define {PREFIX}_DEPRECATED_EXPORT {EXPORT_MACRO_NAME} {PREFIX}_DEPRECATED
#endif

#ifndef {PREFIX}_DEPRECATED_NO_EXPORT
#  define {PREFIX}_DEPRECATED_NO_EXPORT {PREFIX}_NO_EXPORT {PREFIX}_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef {PREFIX}_NO_DEPRECATED
#    define {PREFIX}_NO_DEPRECATED
#  endif
#endif

#endif /* {EXPORT_MACRO_NAME}_H */
"""

def generate_export_header(prefix: str, export_macro_name: str, platform: str) -> str:
    format_dict = {}
    format_dict["PREFIX"] = prefix
    format_dict["EXPORT_MACRO_NAME"] = export_macro_name
    if platform == "win":
        format_dict["ATTRIBUTE_EXPORT"] = "__declspec(dllexport)"
        format_dict["ATTRIBUTE_IMPORT"] = "__declspec(dllimport)"
        format_dict["ATTRIBUTE_NO_EXPORT"] = ""
        format_dict["ATTRIBUTE_DEPRECATED"] = "__declspec(deprecated)"
    else:
        format_dict["ATTRIBUTE_EXPORT"] = '__attribute__((visibility("default")))'
        format_dict["ATTRIBUTE_IMPORT"] = format_dict["ATTRIBUTE_EXPORT"]
        format_dict["ATTRIBUTE_NO_EXPORT"] = '__attribute__((visibility("hidden")))'
        format_dict["ATTRIBUTE_DEPRECATED"] = '__attribute__ ((__deprecated__))'

    return TEMPLATE.format(**format_dict)


def main():
    parser = argparse.ArgumentParser(description="Generate export header", add_help=False)
    parser.add_argument("-H", action="help", help="Show this help message and exit")
    parser.add_argument("-n", "--prefix-name", required=True, help="The prefix for the macros")
    parser.add_argument("-e", "--export-macro-name", help="The macro name to mark exports with, replacing the default of [prefix]_EXPORT")
    parser.add_argument("-p", "--platform", required=True, help="The macro name to mark exports with")
    parser.add_argument("-o", "--output", required=True, help="The file path to output to")

    args = parser.parse_args()

    prefix = args.prefix_name.upper()

    export_macro_name = f"{prefix}_EXPORT"
    if args.export_macro_name is not None:
        export_macro_name = args.export_macro_name.upper()

    platform = args.platform

    content = generate_export_header(prefix, export_macro_name, platform)

    with open(args.output, "w") as output_file:
        output_file.write(content)


if __name__ == "__main__":
    main()
