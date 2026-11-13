# QNX image: automatic start and security policy

`scripts/image_build.sh` builds the QEMU x86_64 image of the target with the ventilator inside. `scripts/vm_run.sh`
boots it and measures the time from QEMU launch to the first breath. The files in this folder are
[mkqnximage](https://www.qnx.com/developers/docs/8.0/) snippets; the build script copies them into
`~/qnxvm/local/snippets/`.

| File | What it does |
|---|---|
| `system_files.custom.in` | Puts the binaries in `/system/bg` (read-only system partition), plus `bg_stop.sh` for the tests |
| `startup_postfs.custom` | Starts plant, `safety_mon`, `power_mgr` and `ventd` right after the filesystems mount, **before** the console, network and `sshd`. `START_P(type)` becomes `on -T type` |
| `data_files.custom` | `secpolgenerate` configuration: the rest of the system unrestricted (development image), our six types **restricted** to the policy |
| `policy.txt` | The security policy, compiled at build time into `/data/var/secpol/secpol.bin` |

Image options used: `--secpol=open --policy=image/policy.txt --usb=no --ip=172.31.1.2 --data-size=200
--data-inodes=6000`.

`--usb=no`: the VM has no USB devices and the ventilator does not use USB. With USB enabled (the default),
`io-sock -m usb` (uid 33) retried every 2 s to open `/dev/usb/io-usb-otg` (owned by uid 44, mode 0644), and
`secpolgenerate -v` printed each failure on the console. That is a POSIX permission problem, not a missing policy
rule: "allowing" it would mean giving the network stack abilities to override file permissions. Removing the
unused service is the right fix, and it also shrinks the attack surface. Two `dumper` lines (`/dev/slog2/stderr`,
mode `--w--w--w-`) still appear once at boot; they come from the QNX image template and do not repeat.
With a static IP the VM needs no DHCP, so `vm_run.sh` sets `OPT_NETWORK=bridge:br0`.

## Types

A QNX security type is a label the kernel attaches to a process. The policy says what each type may do. Abilities
that are not listed are denied, even to root.

| Process | Type | Who gives it the type |
|---|---|---|
| `pneumatic_sim` (plant) | `bg_plant_t` | boot script, `on -T` |
| `safety_mon` | `bg_safety_t` | boot script |
| `power_mgr` | `bg_power_t` | boot script |
| `ventd` | `bg_ventd_t` | boot script |
| `breath_ctrl` | `bg_safety_t__ctrl` | `safety_mon` at spawn: `secpol_posix_spawnattr_settypeid(..., "ctrl")` |
| `hmi` | `bg_safety_t__hmi` | `safety_mon` at spawn: `secpol_posix_spawnattr_settypeid(..., "hmi")` |

The last two are *derived types* (`derive_type bg_safety_t ctrl bg_safety_t__ctrl`). `safety_mon` may only give
its children those two types (`settypeid:bg_safety_t__ctrl,bg_safety_t__hmi`), so it cannot raise a child above
them.

## Who may connect to whom

A channel gets the type of the process that created it. `channel_connect:<types>` lists the channels a type may
open. This is the core of layer 2:

| Type | May connect to | Notably NOT to |
|---|---|---|
| `bg_safety_t__ctrl` (controller) | plant, `safety_mon` | |
| `bg_safety_t` (`safety_mon`) | plant | |
| `bg_power_t` | plant, `safety_mon` | |
| `bg_ventd_t` | `safety_mon`, `power_mgr` | plant |
| `bg_safety_t__hmi` | `safety_mon`, `power_mgr`, network stack | plant, `ventd` |

The other abilities are what the processes need and nothing more. `mem_lock` is needed for `mlockall()`, and
`public_channel` for `name_attach()`. Only `safety_mon` has `spawn` (to relaunch children). Only `ventd` has
`pathspace` (to own `/dev/ventilator/*`). Only the HMI has `network/jailroot` (its socket).

## How the policy was made

1. Boot with `--secpol=open`. All types are unrestricted, and `secpolgenerate` records every ability each type
   uses.
2. Run everything the system does: boot, ventilation, operator commands through the HMI and through
   `/dev/ventilator`, killing the HMI and the controller (restarts).
3. Copy `/dev/secpolgenerate/policy` to `policy.txt` and rebuild with our types restricted.
4. Check that `/dev/secpolgenerate/errors` shows no denial for a legitimate process. The only remaining denial in
   the image is the system's own (`dumper` at boot, unrelated to the ventilator).

Learning a policy from observation has a known weakness: it allows what was observed, including anything a bug did
during the learning run. We read every `bg_` rule (above); none of them is surprising.

## Test

`scripts/b2_run.sh` starts the ventilator exactly as the boot does and checks:

- an attacker with the HMI type (`evil_hmi`) and one with the `ventd` type: `name_open()` of the plant fails with
  `EPERM`. The plant never receives a message (its denial counter stays at 0).
- a shell with the HMI type cannot open `/dev/ventilator/settings`. Root's shell, with an unrestricted type, can.
- the kernel logs the three attempts. No legitimate process is denied. Ventilation continues
  (`results/sec/log_b2.txt`).

## Limits (development image)

- The rest of the system (console, `sshd`, root shell) is unrestricted, so root can still do anything, including
  `on -T` with any type. A production image would use `--secpol=secure`, restrict every type and drop root for our
  processes. That is not done here.
- The types guard the channels. Inside an allowed channel, layer 1 (the plant checks the sender pid of each actuator
  command) still applies.
