# Third-Party Notices and Source Availability

MiyooFin is licensed under GPL-3.0-only. See `LICENSE` for the complete
GNU General Public License version 3 text.

This document records third-party material that is included in the MiyooFin
source tree or in prebuilt `MiyooFin.zip` releases. It does not replace or
change the license terms supplied by those projects.

## MiyooFin corresponding source

For a prebuilt `MiyooFin.zip` published on the GitHub Releases page, the
corresponding MiyooFin source is the GitHub source archive for the same release
tag. The repository contains the build scripts and packaging instructions used
to produce the application binaries.

Repository:
https://github.com/S3ggie/MiyooFin

## SDL2 for Miyoo Mini (bundled shared library)

Binary release paths include:

- `lib/libSDL2-2.0.so.0`
- `lib/libSDL2-2.0.so.0.18.2`
- `lib/libSDL2.so`

MiyooFin builds these files from the `vanilla` line of `XK9274/sdl2_miyoo`,
pinned to commit:

`478ddde6a415d48b4c497ce3a2679c08afd23f40`

Pinned source:
https://github.com/XK9274/sdl2_miyoo/tree/478ddde6a415d48b4c497ce3a2679c08afd23f40

The pinned repository carries a GPL-3.0 license. MiyooFin applies a small set
of build-time source edits while constructing the cross-toolchain. Those edits
are stated explicitly and reproducibly in `Dockerfile.onionos`; the pinned
upstream source together with that Dockerfile describes the source used for
the distributed SDL2 library. The GPLv3 text is included in MiyooFin's
`LICENSE` file and is also copied into prebuilt packages.

## GNU C++ and GCC runtime libraries (bundled shared libraries)

Binary release paths include:

- `lib/libstdc++.so.6`
- `lib/libstdc++.so.6.0.25`
- `lib/libgcc_s.so.1`

The cross-toolchain is based on Debian Buster. These runtime files correspond
to Debian's GCC 8 source package, version `8.3.0-6`.

Corresponding source:
https://sources.debian.org/src/gcc-8/8.3.0-6/

Debian archive:
https://archive.debian.org/debian/pool/main/g/gcc-8/

The GCC runtime libraries are covered by GPLv3 with the GCC Runtime Library
Exception, version 3.1. The GPLv3 text is in `LICENSE`; the complete Runtime
Library Exception is reproduced at the end of this document.

Official GCC license information:
https://gcc.gnu.org/onlinedocs/gcc-8.3.0/libstdc++/manual/manual/license.html

## Mozilla CA certificate bundle (vendored and bundled)

File:

- `cacert.pem`

The bundle is downloaded from curl's official CA Extract service by
`tools/update-ca-bundle.sh`. It is a converted form of Mozilla's CA certificate
store and is licensed under the Mozilla Public License 2.0 (MPL-2.0).

Source/bundle:
https://curl.se/ca/cacert.pem

License information:
https://curl.se/docs/caextract.html
https://www.mozilla.org/MPL/2.0/

This Source Code Form is subject to the terms of the Mozilla Public License,
v. 2.0. The URLs above identify both the source form and a copy of that
license.

## stb_image (vendored source)

File:

- `third_party/stb_image.h`

`stb_image` v2.30 is offered by its authors under an MIT-style permission
notice or, alternatively, a public-domain dedication. The vendored file keeps
those notices intact. MiyooFin relies on the public-domain alternative for
this embedded image-loader source.

Upstream project:
https://github.com/nothings/stb

## Libraries used from the device but not redistributed by MiyooFin

MiyooFin links to or uses additional libraries supplied by OnionOS/the Miyoo
runtime. In particular, the six `vendor/miyoo/lib` libraries imported by
`make import-miyoo-libs` are build inputs only. They are ignored by Git and
are not copied into `MiyooFin.zip` by the package recipe. The application also
uses the device's libcurl at runtime rather than bundling a libcurl shared
library.

---

# GCC Runtime Library Exception 3.1

GCC RUNTIME LIBRARY EXCEPTION

Version 3.1, 31 March 2009

Copyright (C) 2009 Free Software Foundation, Inc. <http://fsf.org/>

Everyone is permitted to copy and distribute verbatim copies of this
license document, but changing it is not allowed.

This GCC Runtime Library Exception ("Exception") is an additional
permission under section 7 of the GNU General Public License, version
3 ("GPLv3"). It applies to a given file (the "Runtime Library") that
bears a notice placed by the copyright holder of the file stating that
the file is governed by GPLv3 along with this Exception.

When you use GCC to compile a program, GCC may combine portions of
certain GCC header files and runtime libraries with the compiled
program. The purpose of this Exception is to allow compilation of
non-GPL (including proprietary) programs to use, in this way, the
header files and runtime libraries covered by this Exception.

0. Definitions.

A file is an "Independent Module" if it either requires the Runtime
Library for execution after a Compilation Process, or makes use of an
interface provided by the Runtime Library, but is not otherwise based
on the Runtime Library.

"GCC" means a version of the GNU Compiler Collection, with or without
modifications, governed by version 3 (or a specified later version) of
the GNU General Public License (GPL) with the option of using any
subsequent versions published by the FSF.

"GPL-compatible Software" is software whose conditions of propagation,
modification and use would permit combination with GCC in accord with
the license of GCC.

"Target Code" refers to output from any compiler for a real or virtual
target processor architecture, in executable form or suitable for
input to an assembler, loader, linker and/or execution
phase. Notwithstanding that, Target Code does not include data in any
format that is used as a compiler intermediate representation, or used
for producing a compiler intermediate representation.

The "Compilation Process" transforms code entirely represented in
non-intermediate languages designed for human-written code, and/or in
Java Virtual Machine byte code, into Target Code. Thus, for example,
use of source code generators and preprocessors need not be considered
part of the Compilation Process, since the Compilation Process can be
understood as starting with the output of the generators or
preprocessors.

A Compilation Process is "Eligible" if it is done using GCC, alone or
with other GPL-compatible software, or if it is done without using any
work based on GCC. For example, using non-GPL-compatible Software to
optimize any GCC intermediate representations would not qualify as an
Eligible Compilation Process.

1. Grant of Additional Permission.

You have permission to propagate a work of Target Code formed by
combining the Runtime Library with Independent Modules, even if such
propagation would otherwise violate the terms of GPLv3, provided that
all Target Code was generated by Eligible Compilation Processes. You
may then convey such a combination under terms of your choice,
consistent with the licensing of the Independent Modules.

2. No Weakening of GCC Copyleft.

The availability of this Exception does not imply any general
presumption that third-party software is unaffected by the copyleft
requirements of the license of GCC.
