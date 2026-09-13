# PS5 FW 12.40 — exploit chain facts (research 2026-09)

## What works on 12.40

Per GBAtemp PS5 Exploit Guide (community-maintained):

- **Kernel exploit (KEX):** P2JB covers 9.00–12.70 → 12.40 is inside the
  window. Note: P2JB can take up to an hour to fire on FW up to 12.70.
- **Hypervisor (HV):** NO — highest HV exploit is 7.61. No Linux on 12.40.
- **Userland entry points available:** BD-JB (to 13.42), Y2JB (to 13.40,
  YouTube app + backup restore), NFNH (Netflix app, to ~12.x), YARPE
  (Ren'Py), LuaC0re (to 12.70 via PS4-game save), WebKit/SlopKit
  (7.00–13.60).
- **No PSN** while exploited. Ever.
- PS5 FPKG installs don't work (A53 hack not public). "Backups" = dump
  loading via kstuff/etaHEN/ItemzFlow. PS4 backported FPKGs DO work.

## Recommended console setup (do BEFORE first payload)

1. Router: block `*.ps5.update.playstation.net` + `sgst.prod.dl.playstation.net`
   + `gs2.ww.prod.dl.playstation.net` (dus01/fus01 for US) — prevents
   accidental updates and update nags. DNS-block alone is NOT reliable.
2. Console backup: Settings > System > System Software > Back Up and
   Restore > Back Up Your PS5.
3. Manual updates only, ever: USB PUP from darthsternie.net, never the
   RECOVERY PUP (factory wipe).

## Payload loader

elfldr (ps5-payload-dev/elfldr) — accepts ELFs on port 9021. Autoload via
`autoload.txt` if set up with etaHEN. Debug: gdbsrv (port 2159), shsrv
(port 2323, telnet-like shell), klogsrv (port 3232, kernel log).

## NFS feasibility (open question)

- PS5 kernel is FreeBSD-derived; FreeBSD mounts NFS client-side via
  mount(2) + nfsclient module. Unknown whether Sony kept NFS in their
  kernel build.
- Test plan: shsrv → try `mount -t nfs host:/export /mnt/nfs` → document.
- Fallback if kernel NFS is absent: Proxmox-side daemon that NFS-mounts
  and re-serves over plain HTTP to the payload's downloader.
