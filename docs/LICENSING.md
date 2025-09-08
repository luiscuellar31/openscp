# OpenSCP – Licensing Information

Copyright (C) 2025  Luis Cuellar

OpenSCP is distributed under a dual-licensing model:

## 1) GPLv3 (GNU General Public License v3.0)
OpenSCP is free software. You can use it, modify it, and redistribute it under the terms of the GNU GPL version 3 **only**, as published by the Free Software Foundation.

See the `LICENSE` file in this repository for the full text of the GNU GPL v3.0.

## 2) Commercial License
For companies or organizations that wish to integrate OpenSCP into proprietary, closed-source, or commercial products **without** being subject to GPL copyleft obligations, commercial licenses are available.

**Scope:** The commercial license covers **only** OpenSCP’s own code. Third-party libraries remain under their original licenses and must be complied with when redistributing binaries:
- Qt 6 (LGPL-3.0) – dynamic linking recommended
- libssh2 (BSD-3-Clause)
- OpenSSL 3 (Apache-2.0) (+ `NOTICE` if applicable)
- zlib (zlib license)

If you are interested in the commercial option, please contact: **luiscuellar31@proton.me**.

---

## Third-party components and notices
When distributing binaries, include the license texts for the third-party libraries you ship (e.g. inside `Resources/licenses/` in the app bundle or `licenses/` in releases):

- `qt-LGPL-3.0.txt` (Qt)
- `libssh2-BSD-3-Clause.txt` (libssh2)
- `openssl-Apache-2.0.txt` + `openssl-NOTICE.txt` if provided (OpenSSL 3)
- `zlib-license.txt` (zlib)

System frameworks on macOS (AppKit, Security.framework, etc.) do **not** require inclusion here.

---

## LGPL source offer (Qt)
This product includes Qt libraries licensed under the GNU LGPL v3. In compliance with the license, we offer access to the corresponding source code of the LGPL-covered libraries used to build this product.

You can obtain the exact Qt source code from:
- `<URL estable al tarball exacto de Qt 6.x que usas>`

Alternatively, you may request a copy by email within 3 years from this release:
- **luiscuellar31@proton.me**

_No charge applies other than reasonable media/shipping costs (if applicable)._

---

## No warranty
OpenSCP under GPLv3 is provided **“as is”**, without any warranty (see GPLv3 §§15–16). Commercial licensing may include support, SLAs, and/or warranty terms as agreed in the contract.

---

## (Optional) Contributions and licensing
By contributing to OpenSCP, you agree to license your contributions under GPLv3 and grant the project maintainers the right to relicense them under the commercial license for OpenSCP.  
If you prefer another contribution model (e.g., CLA), please contact **luiscuellar31@proton.me**.
