# agata-ps5

Open-source PS5 homebrew payload for jailbroken consoles — a web-app-driven
file manager, downloader, and (eventually) network-mount manager. Think
"pegasus-dl, but open source" with our own roadmap.

**Target console:** PS5 on firmware 12.40 (jailbroken, P2JB chain, elfldr)
**Stack:** C (ps5-payload-sdk) + raw BSD sockets — no external libraries
**Built by:** Chris Jazinski + Agata (Hermes Agent)

## Status

- Toolchain working on the build box (LLVM 19 + ps5-payload-sdk)
- `agata-websrv` v0: minimal HTTP server payload — serves a UI page and a
  `/status` JSON endpoint on port 6971. Builds clean, not yet deployed.

## What we're building

A single payload ELF that, once sent to the console via elfldr (port 9021),
serves a local web UI on the LAN with:

1. **Status** — console info, uptime, mount table
2. **File manager** — browse PS5 storage, upload/download/rename/delete
3. **Downloads** — queue direct-URL package downloads to console storage
4. **Mounts** — NFS/SMB mount management (experimental; see issues)

## Build

```console
export PS5_PAYLOAD_SDK=<path to ps5-payload-sdk>
export PATH=<llvm-19-bindir>:$PATH
make -C agata-websrv
```

## Deploy

```console
nc -q0 <ps5-ip> 9021 < agata-websrv/agata_ps5_websrv.elf
# then browse http://<ps5-ip>:6971/
```

## Repository layout

- `agata-websrv/` — the payload (C)
- `docs/` — research notes, ecosystem maps, firmware findings

## Legal / scope

Personal-use homebrew for a console we own. No PSN access, no piracy
tooling, no license circumvention. Use with content you have rights to.

## License

GPL-3.0 (matching ps5-payload-sdk upstream)
