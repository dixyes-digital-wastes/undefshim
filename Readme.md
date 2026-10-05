# undefshim

Shim for running Windows (arm64) on hardware that does not support the instructions it needs.

## Background

Windows requires lrcpc and lse since 24h2 (26xxx), while my machine (Hi1620) does not support lrcpc (it is almost an a76 without lrcpc). So I need to shim the unsupported instructions.

## Usage

### Build

```bash
# needs clang and lld-link
make
```

The driver is `build/undefshim_driver.efi`. That is all a machine needs: put it on a volume the firmware can read, and load it before Windows starts.

### Run

The driver has to be loaded before the Windows boot manager runs, which is what a UEFI shell is for. With the driver's volume current:

```
# at UEFI shell
fsX:
load -nc \undefshim_driver.efi
fsY:
\EFI\Microsoft\Boot\bootmgfw.efi
```

It reads `us.toml` from the volume it was loaded from, and carries on with its defaults when there is none -- the serial port it would report on is one of the things that file specifies, so without it nothing is printed and what it is doing is on the screen instead.

## How it works

See docs.

## TODOs

- docs
- (maybe) Support LSE for a72
- Heterogeneous clusters (big.LITTLE)

## Acknowledgements

- This project is inspired by [EfiGuard](https://github.com/mattiwatti/efiguard).
- This project uses a modified [posix-uefi](https://gitlab.com/bztsrc/posix-uefi) as its UEFI library.
- This project uses a modified [toml-c](https://github.com/arp242/toml-c) as its config parser.
- This project borrows some code from [musl](https://git.musl-libc.org/git/musl), through the UEFI library's formatted output.
- This project uses [atarist-font](https://github.com/ntwk/atarist-font), under its own license, as its font.
- 感谢[蓝色大肥鱼](https://deepseek.com/)，基本上所有代码都是大肥鱼写的

## License

The project is licensed under the GNU Affero General Public License v3.0 or later. See the LICENSE file for details.

```text
undefshim
Copyright (C) 2026 Yun Dou <dixyes@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
```
