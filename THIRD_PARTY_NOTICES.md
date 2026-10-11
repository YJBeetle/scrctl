# Third-party notices

This file records the licenses of the dependencies added during the application and transport refactor. It accompanies binary installs under share/doc/scrctl.
Existing SDL2, OpenSSL and optional FFmpeg dependencies retain their own licenses
and distribution requirements.

## libcurl (curl license)

Source: https://curl.se/libcurl/

Native DDI downloads and Apple TSS requests link libcurl. macOS uses the SDK system library; Linux
and Windows use the configured system or toolchain package. HTTPS peer and host
verification remain enabled. Windows DDI downloads use the native certificate
store. Non-Apple TSS requests use the dedicated public Apple root below, so an
installed scrctl package does not depend on the build machine's CA bundle path.
Bundled libcurl and its runtime dependencies are recorded by the existing package
provenance tools and retain their applicable license files.

License text: https://curl.se/docs/copyright.html

```text
COPYRIGHT AND PERMISSION NOTICE

Copyright (c) 1996 - 2026, Daniel Stenberg, daniel@haxx.se, and many
contributors, see the THANKS file.

All rights reserved.

Permission to use, copy, modify, and distribute this software for any purpose
with or without fee is hereby granted, provided that the above copyright
notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall not
be used in advertising or otherwise to promote the sale, use or other dealings
in this Software without prior written authorization of the copyright holder.
```

DDI payloads are downloaded separately by the user and are not part of the scrctl
binary package. The fixed third-party download source and verification scope are
documented in docs/DDI.md.

## Apple Root CA (public certificate)

Non-Apple platforms include the public Apple Root CA exclusively for TLS
verification of the fixed `https://gs.apple.com` TSS endpoint. The certificate is
not installed into the system trust store and is not used for DDI asset downloads.
macOS continues to use its system libcurl trust store for TSS.

Official publication: https://www.apple.com/certificateauthority/
and https://www.apple.com/appleca/AppleIncRootCertificate.cer

DER SHA256: `b0b1730ecbc7ff4505142c49f1295e6eda6bcaed7e2c68c5be91b5a11001f024`.
The PEM in `src/ddi/AppleTssTrust.h` is an unmodified encoding of that DER.
The certificate remains Apple's property; it is not covered by scrctl's license.
Apple Certificate Policy describes publication and distribution of public root
certificates: https://images.apple.com/certificateauthority/pdf/Apple_Certificate_Policy_v7.0.pdf

## nlohmann/json 3.12.0 (MIT)

Source: https://github.com/nlohmann/json/tree/v3.12.0

```text
MIT License

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## CLI11 2.5.0 (BSD-3-Clause)

Source: https://github.com/CLIUtils/CLI11/tree/v2.5.0

```text
CLI11 2.2 Copyright (c) 2017-2025 University of Cincinnati, developed by Henry
Schreiner under NSF AWARD 1414736. All rights reserved.

Redistribution and use in source and binary forms of CLI11, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## pugixml 1.16 (MIT)

Source: https://github.com/zeux/pugixml/tree/v1.16

```text
MIT License

Copyright (c) 2006-2026 Arseny Kapoulkine

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
```

## mdns 1.4.3 (Unlicense / public domain)

Source: https://github.com/mjansson/mdns/tree/1.4.3

scrctl uses this single-header library for DNS message encoding and parsing.
The upstream example programs are not built or distributed.

```text
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org>
```

## nghttp2 (MIT; optional compatibility probe only)

Source: https://github.com/nghttp2/nghttp2/tree/v1.70.0

The production scrctl target does not link this library. The optional probe was
validated with 1.70.0 and uses the system package.

```text
nghttp2 - HTTP/2 C Library

Copyright (c) 2013, 2014 Tatsuhiro Tsujikawa

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## lwIP 2.2.1 (BSD-3-Clause)

Source: https://github.com/lwip-tcpip/lwip/tree/STABLE-2_2_1_RELEASE

The production scrctl target and optional SCRCTL_LWIP_PROBE link lwIP built from
a fixed, SHA256-verified source archive. Individual source files retain their
additional copyright notices.

```text
Copyright (c) 2001, 2002 Swedish Institute of Computer Science.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
```


## GNU gettext / libintl

Source: https://www.gnu.org/software/gettext/

Message catalogs are compiled with GNU gettext tools. On systems with built-in
gettext support (such as glibc), the runtime uses that implementation. Elsewhere,
it links the installed libintl library; this macOS build uses shared libintl.
GNU libintl is distributed under the GNU Lesser General Public License.
The build tools and runtime have distinct licenses; the gettext tools are build
requirements and are not bundled in scrctl. A binary package that also bundles
libintl must include its applicable license and meet its distribution requirements.
Project translation files retain the scrctl project license.


## Windows ARM64 development artifacts

The MSYS2 CLANGARM64 FFmpeg package used by the current development build enables
GPL and version3 components (GPL-3.0-or-later). It is not an LGPL-only FFmpeg build.
The CI artifact records the installed package versions and FFmpeg configuration
and includes the toolchain's dependency license files. Its runtime-origins.tsv and
package-metadata.json map each installed DLL to a same-name toolchain file with an
identical SHA256, its local pacman owner and exact installed package version. The
metadata preserves the original package information and license declarations;
unknown owners or mismatched DLL bytes fail this MSYS2 packaging step. These are
build provenance records. The separate source collection step downloads the exact
MSYS2 source packages, verifies their signatures and declared input checksums, and
delivers PKGBUILD, upstream sources and patches in the companion source artifact.
See docs/SOURCE_MATERIALS.md for the binding, rebuild instructions and scope;
provenance alone does not replace corresponding source and redistribution requirements.
See docs/WINDOWS.md for the current verification and release-packaging scope.

Package definitions and source retrieval instructions:
https://github.com/msys2/MINGW-packages

FFmpeg licensing and distribution guidance:
https://ffmpeg.org/legal.html
