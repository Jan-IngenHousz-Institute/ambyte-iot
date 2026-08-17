# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Generate the Windows PE version resource for the packaged executable.

A PyInstaller build with no CompanyName/ProductName/FileDescription produces a
blank PE, and Defender's ML models score that as more suspicious than an
otherwise identical binary carrying real publisher metadata. This is not a
substitute for an Authenticode signature, only the part of the gap that is free.

Usage: python -m flash_gui.build_version_info <output-path>
"""

import sys

from flash_gui import __version__

COMPANY = "Jan Ingenhousz Institute"
PRODUCT = "Ambyte Flash GUI"
DESCRIPTION = "Flash and provision ambyte sensor boards"
COPYRIGHT = "Jan Ingenhousz Institute, GPL-3.0"
FILENAME = "ambyte-flash-gui.exe"

TEMPLATE = """VSVersionInfo(
  ffi=FixedFileInfo(
    filevers={vers},
    prodvers={vers},
    mask=0x3f,
    flags=0x0,
    OS=0x40004,
    fileType=0x1,
    subtype=0x0,
    date=(0, 0)
  ),
  kids=[
    StringFileInfo([
      StringTable('040904B0', [
        StringStruct('CompanyName', {company!r}),
        StringStruct('FileDescription', {description!r}),
        StringStruct('FileVersion', {dotted!r}),
        StringStruct('InternalName', 'ambyte-flash-gui'),
        StringStruct('LegalCopyright', {copyright!r}),
        StringStruct('OriginalFilename', {filename!r}),
        StringStruct('ProductName', {product!r}),
        StringStruct('ProductVersion', {dotted!r}),
      ])
    ]),
    VarFileInfo([VarStruct('Translation', [1033, 1200])])
  ]
)
"""


def version_tuple(version):
    """`"0.2.1"` -> `(0, 2, 1, 0)`. Windows requires exactly four integers."""
    numeric = version.split("+", 1)[0].split("-", 1)[0]
    parts = [int(part) for part in numeric.split(".")[:4]]
    return tuple(parts + [0] * (4 - len(parts)))


def render(version=__version__):
    vers = version_tuple(version)
    return TEMPLATE.format(
        vers=vers,
        dotted=".".join(str(part) for part in vers),
        company=COMPANY,
        product=PRODUCT,
        description=DESCRIPTION,
        copyright=COPYRIGHT,
        filename=FILENAME,
    )


def main(argv):
    if len(argv) != 2:
        raise SystemExit("usage: python -m flash_gui.build_version_info <path>")
    with open(argv[1], "w", encoding="utf-8") as handle:
        handle.write(render())
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
