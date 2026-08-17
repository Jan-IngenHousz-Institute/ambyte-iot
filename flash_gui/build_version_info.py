# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Generate the Windows PE version resource for the packaged executable.

A PyInstaller build with no CompanyName/ProductName/FileDescription produces a
blank PE, and Defender's ML models score that as more suspicious than an
otherwise identical binary carrying real publisher metadata. This is not a
substitute for an Authenticode signature, only the part of the gap that is free.

Deliberately carries no version number. The release job publishes the artifact
promoted from main rather than rebuilding at tag time, so a version compiled in
here could not be corrected to match the `flash-gui-v*` tag, and a hand-bumped
constant just drifts (it sat at 0.1.0 across two releases). The tag and the zip
filename identify the build; FixedFileInfo needs the numeric fields to exist, so
they are zeroed rather than filled with something false.

Usage: python -m flash_gui.build_version_info <output-path>
"""

import sys

COMPANY = "Jan Ingenhousz Institute"
PRODUCT = "Ambyte Flash GUI"
DESCRIPTION = "Flash and provision ambyte sensor boards"
COPYRIGHT = "Jan Ingenhousz Institute, GPL-3.0"
FILENAME = "ambyte-flash-gui.exe"

TEMPLATE = """VSVersionInfo(
  ffi=FixedFileInfo(
    filevers=(0, 0, 0, 0),
    prodvers=(0, 0, 0, 0),
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
        StringStruct('InternalName', 'ambyte-flash-gui'),
        StringStruct('LegalCopyright', {copyright!r}),
        StringStruct('OriginalFilename', {filename!r}),
        StringStruct('ProductName', {product!r}),
      ])
    ]),
    VarFileInfo([VarStruct('Translation', [1033, 1200])])
  ]
)
"""


def render():
    return TEMPLATE.format(
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
