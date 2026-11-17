# Credential storage and network boundaries

PBS-Cord uses Discord's QR approval flow. It does not collect or store your Discord password. The account credential retained for a linked session is a Discord session token, held decrypted in memory while in use.

## On-console encryption

Remembering an account writes `/data/pbs-discord/account-auto.vault` using OpenSSL **AES-256-GCM** authenticated encryption. Each save uses a random 16-byte salt, 12-byte nonce and 16-byte authentication tag. **scrypt** derives a 32-byte encryption key with `N=32768`, `r=8`, `p=1`, and a 64 MiB maximum-memory setting. The JSON vault contains a version and Base64-encoded salt, nonce, tag and ciphertext.

Automatic sign-in generates a random 32-byte device secret, stored as 64 hexadecimal characters in the adjacent `account-auto.key`. That string is the scrypt input. The account directory is set to mode `0700`, and newly written vault/key files to `0600`. Because the key and vault are on the same device, a reader with access to both can decrypt the session. This is not hardware-backed storage and does not protect against full console filesystem access.

The separate legacy/manual `account.vault` format uses the same cipher and KDF with a user-provided local unlock passphrase (10–1024 bytes). That passphrase is separate from your Discord password. Forgetting a saved account removes its local vault/key files; it does not promise secure erasure of storage or remote revocation of the Discord session.

## Network behavior

Current account-token call sites send the token only to Discord: REST requests use the `Authorization` header, and Discord Gateway identification sends it over WSS. The token is not returned in dashboard status responses or attached to artwork requests. Connections require TLS 1.2 or newer, validate certificates using the bundled CA trust set, and verify the destination hostname. This describes the current call sites; the generic HTTPS helper is not a universal Discord-only destination filter.

If Discord requests hCaptcha, the dashboard loads JavaScript from `js.hcaptcha.com` and passes the provider its challenge sitekey and `rqdata`. Verification therefore involves an external provider and its browser requests. The account token is not supplied to that widget. The returned verification solution and private challenge headers are sent to Discord. Optional game artwork resolves public PlayStation Store pages and image CDN URLs without the Discord account token; browsers can fetch the resulting public images.

## Dashboard access

The PBS dashboard listens on the LAN over **unauthenticated HTTP**. Treat anyone who can reach it as able to operate its controls and read returned account metadata, server/channel lists, QR links and verification challenge data. Status responses do not include the Discord session token. Local vault passphrases submitted to unlock/manual-save routes, and verification solutions, cross that HTTP connection without TLS protection.

Use a trusted LAN and keep the dashboard off the public internet. Encryption of saved tokens does not secure the dashboard connection. Review reports before sharing them and never publish account files, keys, private diagnostics or session tokens.
