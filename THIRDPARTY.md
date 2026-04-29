# Third-party sources

This firmware vendors a small number of files from upstream projects.
Each is preserved with its original copyright notice in the file
header (`SPDX-FileCopyrightText`) and the upstream's full LICENSE text
is reproduced verbatim in `licenses/`.

| Vendored under | Upstream | Files | License |
|---|---|---|---|
| `src/gud/gud.c`, `src/gud/gud.h` | [notro/gud-pico](https://github.com/notro/gud-pico) @ `984a171` (`libraries/gud_pico/`) | verbatim | MIT — see [`licenses/gud-pico-LICENSE.md`](licenses/gud-pico-LICENSE.md) |
| `src/gud/gud_usb.c`, `src/gud/gud_usb.h` | [notro/gud-pico](https://github.com/notro/gud-pico) @ `984a171` (`libraries/gud_pico/driver.{c,h}`) | adapted for ESP-IDF / TinyUSB | MIT — see [`licenses/gud-pico-LICENSE.md`](licenses/gud-pico-LICENSE.md) |
| `src/gud/esp_lcd_st7735.c`, `src/gud/esp_lcd_st7735.h` | [Xinyuan-LilyGO/T-Dongle-S3](https://github.com/Xinyuan-LilyGO/T-Dongle-S3) @ `fda1b85` (`examples/lcd/bsp_lcd/`) | verbatim, include path adjusted | MIT — see [`licenses/T-Dongle-S3-LICENSE`](licenses/T-Dongle-S3-LICENSE) |

The remaining files in `src/`, `tools/`, `udev/`, and `docs/` are
original to this project and are released under the
[MIT License](LICENSE) (matches the SPDX line in `src/main.c`).

## Adding new vendored code

When importing more upstream code in the future:

1. Pin to a specific commit in the file header (lifted-verbatim or
   adapted-from line).
2. Add `SPDX-License-Identifier:` and `SPDX-FileCopyrightText:` lines
   to every imported file, preserving upstream's copyright holder(s).
3. Drop the upstream LICENSE text into `licenses/<project>-LICENSE`
   verbatim.
4. Add a row to the table above.
