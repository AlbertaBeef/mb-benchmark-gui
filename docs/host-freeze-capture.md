# Capturing the host freeze (Axelera + another card)

Five failures so far. The trigger, as of 2026-10-31 12:13, is **a device init
while another card is active, in either direction** — not Axelera's own load,
and not load/power/duration: that run had Axelera at 1627 fps and 3.140 V for
four seconds, and it died in the same second as *DeepX's* `make_runner()`. The
outcome is sometimes a wedged card (host survives, kernel logs `Link down` +
MSI timeout) and sometimes a dead host, which leaves **nothing** behind: no
kernel oops, no systemd shutdown, no BMC event at the moment itself.

**Step 3 below is now the highest-value test**, because the trigger being
device *initialisation* — DMA mapping setup — is what points at the IOMMU, and
Axelera's own init logs two `IO_PAGE_FAULT`s where one per boot is normal. The machine currently has
no capture facility at all, so each occurrence costs a power cycle and yields
one data point: "it stopped".

Checked 2026-10-31, before any of the below was applied:

| | state |
| --- | --- |
| kdump / `crashkernel=` | **absent** |
| `console=` on the kernel cmdline | **absent** (so IPMI SOL shows nothing) |
| `kernel.hardlockup_panic` | **0** — a detected lockup just sits there |
| `kernel.panic_on_oops` | **0** |
| `kernel.panic` | **0** — no auto-reboot |
| `nmi_watchdog` | 1 (detector on, but its finding is not fatal) |
| IOMMU | default, i.e. on |

Apply 1 and 2 together; they need one reboot between them and the next test.

## 1. Serial console + fatal lockups (one GRUB edit)

This is the only method that reports through a total freeze: the trace goes out
the serial port, which the BMC exposes over the network, so nothing has to be
written to disk.

```bash
sudo cp /etc/default/grub /etc/default/grub.bak
sudo sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT=.*/GRUB_CMDLINE_LINUX_DEFAULT="quiet splash console=tty0 console=ttyS0,115200n8 crashkernel=512M-:256M"/' /etc/default/grub
grep GRUB_CMDLINE /etc/default/grub          # read it back before committing
sudo update-grub
```

`console=tty0` first and `ttyS0` last is deliberate: the **last** `console=`
becomes /dev/console, and that is the one a panic prints to.

```bash
sudo tee /etc/sysctl.d/99-lockup-capture.conf >/dev/null <<'SYSCTL'
# Make a lockup fatal and recordable rather than a silent hang. Added
# 2026-10-31 for the Axelera+other-card host freeze: four occurrences, no
# evidence from any of them.
kernel.hardlockup_panic = 1
kernel.softlockup_panic = 1
kernel.panic_on_oops = 1
kernel.panic = 20
SYSCTL
sudo sysctl --system | grep -E "hardlockup|softlockup|panic"
```

`kernel.panic = 20` reboots 20 s after a panic, so a remote operator gets the
machine back without a power cycle — and the trace is already on the wire.

**Watch it from another machine** (not this host — its console dies with it):

```bash
ipmitool -I lanplus -H <bmc-ip> -U <user> -P <pass> sol activate | tee freeze-$(date +%F-%H%M).log
```

Leave that running for the whole test. `~.` detaches. If SOL is in use,
`ipmitool ... sol deactivate` first. The BMC is on its own NIC; this host's OS
address (10.0.0.123 on enp66s0f1) is not it.

## 2. kdump, so a panic writes a vmcore

```bash
sudo apt-get install -y kdump-tools        # asks to enable; say yes
sudo systemctl enable --now kdump-tools
kdump-config show                          # expect "current state: ready to kdump"
```

Needs the `crashkernel=` from step 1, so reboot first. A vmcore lands in
`/var/crash/`. Only a *panic* produces one — which is why step 1's
`hardlockup_panic` matters: without it a hard lockup never becomes a panic.

## 3. The hypothesis test: IOMMU off

Two cards DMA-ing concurrently through the AMD IOMMU is the one mechanism that
fits both facts — it only ever happens with Axelera **plus** another card, and
nothing survives it. `axl`'s single `IO_PAGE_FAULT` per boot is documented as
routine (it fires when the runtime loads firmware), but routine with one active
card is not evidence about two.

Run this **after** steps 1-2, so that if it freezes anyway there is a trace:

```bash
# temporary, for one boot — add at the GRUB menu's `e` prompt rather than on disk
amd_iommu=off
```

Then repeat the failing case: DeepX + Axelera, staggered 5 s.

- **No freeze with the IOMMU off** → that is the mechanism, and the next step is
  `iommu=pt` (passthrough for the identity-mapped case) rather than off, plus
  an AMD/Axelera bug report with the SOL trace.
- **Freezes anyway** → the IOMMU is out, and the SOL trace from step 1 should
  finally name what is hanging.

Do not leave `amd_iommu=off` on permanently: it removes DMA isolation for every
device on the box.

## What NOT to spend another power cycle on

- **Double buffering.** It was **off** in both 2026-10-31 failures. Ruled out.
- **The 3.3 V supply.** The PMD2 shows the PSU rail at 3.291 V idle and
  3.278 V at 17.6 W — 13 mV of droop — while the card's own INA228 rail moves
  115 mV over the same range. The supply is stiff and far from its limit; the
  droop is all downstream in the board's M.2 distribution.
- **Total power.** The 10:58 freeze was two cards at ~33 W, well under the
  four-card ~45 W that has run for minutes at a time.
- **A THERMTRIP in the SEL** right after a freeze. It is the hard power-off
  that follows, not the cause — see CLAUDE.md.
