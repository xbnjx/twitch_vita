# Avisos de Terceros y Atribución de Licencias (Third Party Notices)

Este proyecto, **Twitch Vita**, desarrollado por **Benjamin Duarte** (<https://benjaminduarte.lat/>), es un software libre y de código abierto distribuido bajo los términos de la **GNU General Public License v3.0 (GPL-3.0)**.

Para su funcionamiento en la consola PlayStation Vita / PlayStation TV, incorpora, enlaza estáticamente o utiliza componentes, librerías, tipografías y recursos de terceros creados por la comunidad de código abierto. A continuación se detallan las licencias, autores y términos de atribución correspondientes.

---

## 1. Componentes Principales del Proyecto

### Twitch Vita
- **Autor / Creador:** Benjamin Duarte (<https://benjaminduarte.lat/>)
- **Licencia:** GNU General Public License v3.0 (GPL-3.0)
- **Repositorio:** <https://github.com/xbnjx/twitch_vita>

### vita-hw-decoder
- **Descripción:** Módulo de decodificación y presentación multimedia acelerada por hardware nativo para PlayStation Vita (`SceVideodec` / `SceAvcdec`) con soporte de HLS, audio elemental AAC y contenedores MP4/MPEG-TS.
- **Licencia:** GNU General Public License v3.0 (GPL-3.0)

### vita-https
- **Descripción:** Capa de red segura para PlayStation Vita construida sobre `libcurl` y `Mbed TLS`, con paquete de certificados raíz CA integrados para conexiones HTTPS con TLS 1.2 y TLS 1.3.
- **Licencia:** GNU General Public License v3.0 (GPL-3.0)

---

## 2. Bibliotecas y Herramientas de Terceros

### FFmpeg (libavformat, libavcodec, libavutil)
- **Sitio web:** <https://ffmpeg.org/>
- **Licencia:** GNU Lesser General Public License v2.1 o posterior (LGPL-2.1-or-later)
- **Parches y controladores Vita:** Incluye el backend `h264_vita` derivado del proyecto [wiliwili](https://github.com/xfangfang/wiliwili), distribuido bajo la **GNU General Public License v3.0 (GPL-3.0)**.
- **Copia de licencias:** Consulte `licenses/FFmpeg-LGPL-2.1.txt` y `licenses/wiliwili-GPL-3.0.txt` en el paquete.

### libcurl
- **Sitio web:** <https://curl.se/>
- **Copyright:** (c) 1996 - 2024, Daniel Stenberg, <daniel@haxx.se>, y colaboradores.
- **Licencia:** Licencia curl (derivada de MIT/X11):
```text
COPYRIGHT AND PERMISSION NOTICE

Copyright (c) 1996 - 2024, Daniel Stenberg, <daniel@haxx.se>, and many contributors.
All rights reserved.

Permission to use, copy, modify, and distribute this software for any purpose
with or without fee is hereby granted, provided that the above copyright
notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS. IN
NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
OR OTHER DEALINGS IN THE SOFTWARE.
```

### Mbed TLS
- **Sitio web:** <https://www.trustedfirmware.org/projects/mbed-tls/>
- **Copyright:** (c) 2006-2024, Arm Limited.
- **Licencia:** Apache License, Version 2.0:
```text
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

### Certifi (Mozilla Root CA Bundle)
- **Sitio web:** <https://curl.se/docs/caextract.html>
- **Licencia:** Mozilla Public License 2.0 (MPL-2.0).
- **Copia de licencia:** Consulte `licenses/certifi-MPL-2.0.txt`.

### Jansson
- **Sitio web:** <https://digip.org/jansson/>
- **Copyright:** (c) 2009-2020 Petri Lehtinen <petri@digip.org>
- **Licencia:** MIT License:
```text
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### libwebp
- **Sitio web:** <https://chromium.googlesource.com/webm/libwebp>
- **Copyright:** (c) 2010, Google Inc. All rights reserved.
- **Licencia:** BSD 3-Clause License:
```text
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### QR Code Generator (Nayuki)
- **Sitio web:** <https://www.nayuki.io/page/qr-code-generator-library>
- **Copyright:** (c) Project Nayuki.
- **Licencia:** MIT License.

### vita2dlib
- **Sitio web:** <https://github.com/xerpi/libvita2d>
- **Copyright:** (c) 2015 Sergi Granell (xerpi)
- **Licencia:** BSD 2-Clause License:
```text
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## 3. Tipografías y Recursos Gráficos

### Tipografía Principal: Bitstream Vera / DejaVu Sans (`font.ttf`)
- **Copyright:** (c) 2003 by Bitstream, Inc. Derechos reservados. Modificaciones DejaVu en dominio público. Glifos adicionales (c) Tavmjong Bah.
- **Licencia:** Bitstream Vera License (consulte `licenses/FONT-LICENSE.txt`).

### Tipografía de Emojis: Google Noto Color Emoji / Twemoji (`emoji.ttf`)
- **Copyright:** (c) 2013 Google LLC / Twitter, Inc.
- **Licencia:** SIL Open Font License, Version 1.1 / Creative Commons CC-BY 4.0 (consulte `licenses/EMOJI-LICENSE.txt`).

---

## 4. Aviso Legal y Marcas Registradas (Disclaimer)

- **Twitch** y el logotipo de Twitch son marcas registradas de **Twitch Interactive, Inc.** o **Amazon.com, Inc.** en los Estados Unidos y otros países.
- **PlayStation**, **PlayStation Vita**, **PS Vita** y **PS TV** son marcas comerciales o registradas de **Sony Interactive Entertainment Inc.**
- Este software es un desarrollo independiente y no oficial de la comunidad homebrew. **No está afiliado, respaldado, autorizado ni certificado por Twitch Interactive, Inc., Amazon.com, Inc., ni por Sony Interactive Entertainment Inc.**
