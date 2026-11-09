# Rules for AI agents working in this fork

Read this before changing anything. It applies to every agent (Codex, Claude Code, others)
and to every branch.

## The device: one mistake can be permanent

- The owner's E87 badge (JieLi BR35 / AC707N, 4 MB flash) has **no wired recovery**:
  no USB data path and no ISP access. The only way to change its firmware is BLE OTA from
  **Settings → Update**.
- A firmware image that does not boot to the menu, open the Update screen and accept the
  next OTA is a **permanent brick**. Every change is judged against that first.
- **Agents never flash, OTA, raw-write, upload resources to or reboot the badge
  themselves.** Produce images, scripts and exact commands; the owner runs them.

## Do not change without the owner's explicit approval

- **OTA path:** `qix_ota_server.c` staging/apply/verify, UFW packaging and the OEM uboot,
  `QIX_STAGE_*`, the raw-flash protection ranges, the Update screen and its battery gate.
- **Flash layout:** CODE `[0, 0x17E000)`, OTA staging `0x17E000..0x2A0000`, resources
  `0x2A0000..0x33E000` (see `docs/resources.md`), system data and the MAC from `0x3BE000`.
- **Power:** PMU/LDO/DCDC selection and `TCFG_LOWPOWER_*` (low-power mode 2 and DCDC15
  hung these boards at boot), soft power-off (nothing turns the badge back on), CPU clock
  outside the SDK clock manager, charge configuration (`TCFG_CHARGE_POWERON_ENABLE` must stay 1).
- **LCD panel:** init command lists, SWRESET, sleep/wake sequencing. A panel pushed into
  Sleep/Display Off by bad commands may not be recoverable even by stock firmware.
- **Safety net:** `firmware/display/badge_safety.h` (TE guard, UI watchdog) stays in every
  image; do not disable `BADGE_SAFETY_WATCHDOG` in a build meant for the badge.

If a task seems to need one of these, stop and describe the change and its risk instead.

## How to change firmware

- Branch from `dev`: `feat/<topic>` or `fix/<topic>`. One topic per branch, one logical
  change per commit, so any single change can be reverted.
- The **Badge Display** workflow must build the branch. Read the build log for new warnings
  in the files you touched; a green job is not enough.
- Say in the commit/PR text whether the change was built with the vendor toolchain and
  whether it was tested on hardware. Never claim either if it was not.
- Hardware check order for the owner: flash the image, immediately flash the same image again
  from the new firmware's Update screen (proves OTA still works), only then test features.

## Branches

- `main` mirrors upstream ElectronicCats. Do not commit to it.
- `fix/windows-ota-chunk-size` is the head of upstream PR #11. Never push to it.
- `dev` is the working branch; finished, hardware-checked branches are merged into it.
- Never rewrite published history and never force-push.

## Personal data never enters the repository

This repository and its CI logs are public. Do not put any of these in files, commit
messages, branch names, issues, PR text or workflow output:

- the badge's MAC address or custom BLE name, the owner's name, e-mail or local paths;
- wallpapers, photos, identity records and other blobs made for the owner's badge;
- flash dumps (they contain the badge's `key_mac`), BLE captures and logs with addresses.

Keep such files outside the repository and use placeholders in docs and examples
(`ADDRESS`, `YOUR_NAME`, `/private/location/...`). Firmware stays generic: personal values
are provisioned over BLE, never compiled in or read by CI (see `docs/badge-system.md`).
Before committing, check the diff for anything that identifies the owner or the device.

## Commits

- Do not add a `Claude-Session:` line. `Co-Authored-By:` is fine.

## Evidence

- A hypothesis is not a cause. Label it as a hypothesis until a measurement confirms it.
- Record hypotheses and their outcome in `docs/hypotheses.md`, including rejected ones,
  so the same idea is not chased twice.
- Measure instead of estimating: `python -m qix_ble.safety ADDRESS` and
  `python -m qix_ble.power ADDRESS` read diagnostics without writing anything.

## Local layout

- The vendor SDK may live outside the repository; pass `--sdk PATH` to
  `scripts/prepare-display.py` and `scripts/prepare-badge-system.py`. Those scripts modify
  the SDK in place; keep a pristine commit in the SDK's own git repository.
- Python tools are installed from `tools/qix-ble` in editable mode, so they follow the
  checked-out branch.
