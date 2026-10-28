# mxlight

**Keep your Logitech MX Keys backlight on.**

Logi Options+ lets you set the backlight brightness and an auto-off _timeout_,
but there's no "always on" option — after about 5 seconds of not typing, the
backlight fades out. `mxlight` is a tiny (~150 KB), dependency-free Windows
program that keeps it lit: it runs quietly in your **system tray**, and you click
its icon to pause or resume whenever you like.

- **Lives in the system tray.** No console window. **Left-click** the icon to
  pause/resume; **right-click** for a menu (Enabled, Start with Windows, Quit).
  The icon is colour-coded: green = keeping the backlight on, amber = searching
  for the keyboard, grey = paused.
- **No install, no runtime, no dependencies.** A single native `.exe`. No .NET,
  no Python, no drivers — just the HID APIs built into Windows.
- **Tiny footprint.** It sleeps between short keepalive pulses; near-zero CPU.

> ⚠️ **One honest caveat:** on the MX Keys firmware I tested, the keyboard only
> relights its backlight on a real power **off→on** transition, so `mxlight` has
> to pulse it off-then-on a few times a minute. That leaves a _very_ brief
> (~5-10 ms) blink that you can occasionally catch if you look closely. It's the
> minimum physically possible — see [How it works](#how-it-works).

---

## Quick start (download)

1. Download **`mxlight.exe`** from the
   [**latest release**](https://github.com/rompom777/MXLights/releases/latest).
2. Run it. An icon appears in the system tray and the backlight starts staying on.
3. _(Optional)_ Right-click the icon → **Start with Windows** to launch it
   automatically at sign-in. This installs a copy to a permanent location for you
   (see [Where it installs](#where-it-installs)), so you can delete the download.

That's the whole setup — no toolchain, no clone, no admin rights.

> On first run Windows SmartScreen may show a blue _"Windows protected your PC"_
> box because the exe isn't code-signed (signing certificates cost money). Click
> **More info → Run anyway**. If you'd rather not take my word that the download
> is safe, see [Verifying the download](#verifying-the-download) — you can
> cryptographically prove it was built from this source — or
> [build it yourself](#build-it-yourself).

### Using it

- **Left-click** the tray icon to pause/resume.
- **Right-click** for a menu: toggle **Enabled**, toggle **Start with Windows**,
  or **Quit**.
- The icon is colour-coded: 🟢 green = keeping the backlight on, 🟡 amber =
  running but searching for the keyboard, ⚪ grey = paused.

If the keyboard is asleep or switched to another device (Easy-Switch), the icon
turns amber and it resumes keeping the light on as soon as the keyboard is back.

> Tip: make sure the MX Keys is switched to this PC and awake (tap a key) the
> first time you run it. If you don't see the icon, check the tray's hidden-icons
> overflow (the `^` chevron) and drag it onto the taskbar to keep it visible.

### Where it installs

`mxlight` needs no installer and writes nothing until you ask it to. The only two
things it ever touches:

- **"Start with Windows"** copies the exe to
  `%LOCALAPPDATA%\mxlight\mxlight.exe` and adds one registry value under
  `HKCU\…\CurrentVersion\Run` pointing at that copy. Copying to a fixed location
  is what lets autostart keep working after you delete the file you first ran.
- Un-checking "Start with Windows" removes that registry value. To fully remove
  the app, **Quit** it and delete the `%LOCALAPPDATA%\mxlight` folder.

---

## Verifying the download

You don't have to trust that the published `mxlight.exe` matches this source —
you can **prove** it. Every release is built on GitHub's servers straight from the
tagged commit (the full build is public on the [Actions](https://github.com/rompom777/MXLights/actions)
tab), and each binary ships with a signed [build-provenance attestation](https://docs.github.com/actions/security-guides/using-artifact-attestations-to-establish-provenance-for-builds).

With the [GitHub CLI](https://cli.github.com/) installed:

```bat
gh attestation verify mxlight.exe --repo rompom777/MXLights
```

A ✓ confirms the exact file you downloaded was built by this repo's workflow from
a specific commit — it was never hand-uploaded or modified. Each release also
lists a **SHA-256** (and an `mxlight.exe.sha256` file) if you just want a plain
integrity check:

```powershell
Get-FileHash mxlight.exe -Algorithm SHA256
```

---

## Build it yourself

Prefer to compile from source instead of downloading? You only need **Visual
Studio 2022** (or the standalone Build Tools) with the _Desktop development with
C++_ workload — no other dependencies.

```bat
git clone https://github.com/rompom777/MXLights.git
cd MXLights
build.bat
```

`build.bat` finds the MSVC toolchain automatically and produces
`build\mxlight.exe`. (This is the exact command GitHub Actions runs to produce
the official release binary.)

---

## How it works

The MX Keys backlight is controlled entirely by the keyboard's own firmware,
which fades the LEDs after a few seconds of inactivity. Faking keystrokes from
software does **not** help — synthetic input never reaches the keyboard hardware,
so the firmware's fade timer keeps running.

The only way to affect the backlight is to speak Logitech's **HID++ 2.0**
protocol directly to the keyboard (through the receiver), the same channel Logi
Options+ uses. `mxlight`:

1. Enumerates Logitech HID++ interfaces (vendor `0x046D`, usage page `0xFF00`)
   using only Windows' built-in HID APIs.
2. Finds the keyboard by pinging each receiver slot and checking for the
   `BACKLIGHT2` feature (`0x1982`).
3. Sends a keepalive: a backlight **OFF** immediately followed by an **ON**,
   repeated a little faster than the ~5-second fade.

On this firmware (BACKLIGHT2 **version 1**) the documented "set permanent mode"
command is silently ignored, and the backlight timer resets _only_ on an off→on
transition — so the off→on pulse is the only thing that works, and the brief
blink is unavoidable. The ON pulse uses a _static_ effect so the LEDs snap back
instantly (no fade-in), which makes the blink as short as possible.

The HID++ protocol details were derived from the
[**Solaar**](https://github.com/pwr-Solaar/Solaar) project, and the keepalive
approach from [**MXLightkeeper**](https://github.com/CasualDeveloper/MXLightkeeper).
Many thanks to both. See [`progress_log.txt`](progress_log.txt) for the full
reverse-engineering story.

---

## License

[MIT](LICENSE) © 2026 Roman Zrajevsky
