# PBS Cord: streaming-only build

Download `pbs-discord.elf` and load it using your PS5 payload loader. Open the dashboard on the console's LAN address, port 9999, then use Livestream to connect Discord and start streaming.

This build includes the compact/enlargeable livestream preview, restored charcoal dashboard colors, Discord-style named server and channel selection, resolution/frame-rate controls, saved-account auto sign-in, and optional PS5 game activity. Two-way voice chat and the experimental companion are excluded. Discord video and game-audio streaming remain included.

Saved account tokens are encrypted on-device with AES-256-GCM and scrypt. Account tokens are sent only to Discord over certificate-validated HTTPS/WSS. Automatic sign-in stores its key beside the encrypted vault; see [SECURITY.md](https://github.com/tanner02/PBS-Cord/blob/5ec0c9a3bedb99578c58a08fbdfd74ab5c928616/SECURITY.md) for the precise storage and network boundaries.

Matching source: [5ec0c9a](https://github.com/tanner02/PBS-Cord/tree/5ec0c9a3bedb99578c58a08fbdfd74ab5c928616), including the pinned dependencies, [build instructions](https://github.com/tanner02/PBS-Cord/blob/5ec0c9a3bedb99578c58a08fbdfd74ab5c928616/BUILDING.md), and [component licenses](https://github.com/tanner02/PBS-Cord/blob/5ec0c9a3bedb99578c58a08fbdfd74ab5c928616/THIRD_PARTY.md).

Validation: PS5 cross-build; 24 host tests; 49 dashboard tests; 5 asset-embedding tests; dashboard player/audio recovery checks; matching-source GitHub CI. This exact ELF was not reloaded on the console during publication. Tested project scope is PS5 firmware 12.00; HEVC, 4K Discord streaming, and two-way voice are not verified features.

SHA-256 (`pbs-discord.elf`):

```text
60e8f0dc305d8bfd71175f2badf17ab3690273f4dc3b4b5bfb13defdf01e67a2
```
