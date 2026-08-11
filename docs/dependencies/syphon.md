# Syphon runtime integration

Sync discovers `Syphon.framework` dynamically at runtime on macOS. The native
code does not import Syphon headers or link `Syphon.framework` at build time.
The installable preview builds official Syphon revision
`71351d4b484cd2d1917867f7846a5cdca724552d`, embeds the resulting framework in
`Sync.app/Contents/Frameworks`, and preserves its complete redistribution
license in `packaging/macos/Third-Party-Notices.txt` and in every app and DMG.
Source-only daemon builds may continue to supply another compatible framework
at runtime.

The runtime boundary is based on the documented `SyphonMetalServer` selectors in the official
[`SyphonMetalServer.h`](https://github.com/Syphon/Syphon-Framework/blob/main/SyphonMetalServer.h).
Its copyright and redistribution notice is preserved below:

> SyphonMetalServer.h
>
> Syphon
>
> Copyright 2020-2023 Maxime Touroute & Philippe Chaurand (www.millumin.com),
> bangnoise (Tom Butterworth) & vade (Anton Marini). All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> * Redistributions of source code must retain the above copyright
> notice, this list of conditions and the following disclaimer.
>
> * Redistributions in binary form must reproduce the above copyright
> notice, this list of conditions and the following disclaimer in the
> documentation and/or other materials provided with the distribution.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
> ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
> WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
> DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
> DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
> (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
> LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
> ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
> (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
> SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
