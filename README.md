# PBS-Cord

A PBS fork with Discord streaming support.

PBS-Cord adds a Discord Livestream tab to the PBS dashboard, branded **PBS Cord**. Link your account by scanning a QR code with the official Discord app, choose a voice channel or DM call, and stream the console's capture with game audio. Approving the link signs you in; streaming starts only when you press Start. Optional game activity can show the running PS5 game on Discord.

## Use

The latest streaming-only payload is available from [GitHub Releases](https://github.com/tanner02/PBS-Cord/releases/latest), with its SHA-256 checksum and matching source revision.

Build or obtain `pbs-discord.elf`, then load it using your PS5 payload loader. Open the PBS dashboard on the console's LAN address and port (9999 by default), or use its console tile. In Livestream, link Discord, select a destination and start. Capture quality controls select resolution and frame rate. The default requested encoder bitrate is **4 Mbit/s**; the default capture profile is 720p. See [BUILDING.md](BUILDING.md) for build commands and loader arguments.

Remembering an account saves its Discord session token encrypted on the PS5 with **AES-256-GCM**, using **scrypt** key derivation. PBS-Cord does not request or store your Discord password. Current account-token network calls go only to Discord over certificate-validated HTTPS/WSS. Automatic sign-in keeps its device key beside the encrypted vault; protect access to the console's files. The dashboard uses unauthenticated HTTP: keep it on a trusted LAN. hCaptcha verification and public artwork can contact external services. See [SECURITY.md](SECURITY.md) for the exact boundaries and parameters.

## Tested scope and limits

Discord integration has been tested on **PS5 firmware 12.00 only**, including 1080p60 streaming and capture fallback. A capture fallback preserves the requested bitrate rather than automatically reducing it. Higher-resolution requests may fall back according to capture availability; HEVC and 4K Discord streaming are not verified features.

The inherited PBS source contains other firmware profiles and a PS4 backend. Their presence does not establish Discord support on those systems. See the [upstream PBS documentation](https://github.com/seregonwar/pbs-payload) for those backends and the archived [upstream README](docs/UPSTREAM-README.md) for historical context.

Two-way voice chat is not included. Streaming video and game audio uses Discord voice signaling.

## Source and credits

PBS-Cord is based on [PBS Payload](https://github.com/seregonwar/pbs-payload). Its upstream license, authorship and component notices are retained. See [THIRD_PARTY.md](THIRD_PARTY.md), [SOURCE-MANIFEST.txt](SOURCE-MANIFEST.txt) and [LICENSE](LICENSE).
