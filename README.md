# mxlight

**Keep your Logitech MX Keys backlight on.**

Logi Options+ lets you set the backlight brightness and an auto-off timeout, but
there's no "always on" option. After about 5 seconds of not typing, the backlight
fades out.

`mxlight` fixes that. It's a tiny (~150 KB) Windows app that lives in your system
tray and keeps the light on. Click the icon to pause or resume whenever you want.

- **Lives in the system tray.** No console window, no clutter.
- **No install, no dependencies.** A single native `.exe`. No .NET, no Python, no
  drivers. Just the HID APIs built into Windows.
- **Barely there.** It sleeps between short pulses, so CPU use is near zero.

> ⚠️ **One honest caveat:** on the firmware I tested, the MX Keys only relights
> its backlight on a real power off→on transition. So `mxlight` pulses it
> off-then-on a few times a minute, which leaves a very brief (~5-10 ms) blink you
> can occasionally catch. That's the minimum physically possible. See
> [How it works](#how-it-works).

---

## Quick start

1. Download **`mxlight.exe`** from the
   [latest release](https://github.com/rompom777/MXLights/releases/latest).
2. Run it. An icon appears in your system tray and the backlight starts staying on.
3. *(Optional)* Right-click the icon and pick **Start with Windows** to launch it
   automatically at sign-in.

That's the whole setup. No toolchain, no clone, no admin rights.

> On first run, Windows SmartScreen may pop up a blue "Windows protected your PC"
> box, because the exe isn't code-signed (certificates cost money). Click
> **More info**, then **Run anyway**.
>
> Don't want to take my word that it's safe? You can
> [prove the download was built from this source](#verifying-the-download), or
> [build it yourself](#build-it-yourself).

---

## Using it

- **Left-click** the icon to pause or resume.
- **Right-click** for a menu: toggle **Enabled**, toggle **Start with Windows**,
  or **Quit**.

The icon color tells you what's happening:

- 🟢 **Green:** keeping the backlight on.
- 🟡 **Amber:** running, but looking for the keyboard (asleep, or switched to
  another device). It resumes on its own once the keyboard is back.
- ⚪ **Grey:** paused.

> **Tips**
> - Make sure the MX Keys is switched to this PC and awake (tap a key) the first
>   time you run it.
> - Don't see the icon? Check the tray's hidden-icons overflow (the `^` chevron),
>   then drag it onto the taskbar to keep it visible.

---

## Where it installs

`mxlight` writes nothing until you ask it to. The only things it ever touches:

- **Start with Windows** copies the exe to `%LOCALAPPDATA%\mxlight\mxlight.exe`
  and adds one registry value under `HKCU\...\CurrentVersion\Run`. Copying to a
  fixed spot is what lets autostart keep working after you delete the file you
  first ran, so you can grab it from Downloads and forget about it.
- Turning off **Start with Windows** removes that registry value.

To remove the app completely: **Quit** it, then delete the `%LOCALAPPDATA%\mxlight`
folder.

---

## Verifying the download

You don't have to trust that the published `mxlight.exe` matches this source. You
can prove it.

Every release is built on GitHub's own servers straight from the tagged commit, so
the whole build is public on the
[Actions tab](https://github.com/rompom777/MXLights/actions). Each binary also
ships with a signed
[build-provenance attestation](https://docs.github.com/actions/security-guides/using-artifact-attestations-to-establish-provenance-for-builds).

To check it, install the [GitHub CLI](https://cli.github.com/) and run:

```bat
gh attestation verify mxlight.exe --repo rompom777/MXLights
```

A ✓ means the exact file you downloaded was built by this repo from a specific
commit, and never touched afterward.

Just want a plain integrity check? Every release lists a **SHA-256** (and an
`mxlight.exe.sha256` file). Compare it against:

```powershell
Get-FileHash mxlight.exe -Algorithm SHA256
```

---

## Build it yourself

Rather compile from source? You only need **Visual Studio 2022** (or the
standalone Build Tools) with the *Desktop development with C++* workload. Nothing
else.

```bat
git clone https://github.com/rompom777/MXLights.git
cd MXLights
build.bat
```

`build.bat` finds the MSVC toolchain on its own and produces `build\mxlight.exe`.
It's the exact command GitHub Actions runs to build the official release.

---

## How it works

The MX Keys backlight is run entirely by the keyboard's own firmware, which fades
the LEDs after a few seconds of inactivity. Faking keystrokes doesn't help:
synthetic input never reaches the hardware, so the fade timer keeps ticking.

The only way to affect the backlight is to speak Logitech's **HID++ 2.0** protocol
to the keyboard (through the receiver), the same channel Logi Options+ uses. So
`mxlight`:

1. Enumerates Logitech HID++ interfaces (vendor `0x046D`, usage page `0xFF00`)
   using only Windows' built-in HID APIs.
2. Finds the keyboard by pinging each receiver slot and checking for the
   `BACKLIGHT2` feature (`0x1982`).
3. Sends a keepalive: a backlight **OFF** immediately followed by an **ON**,
   repeated a little faster than the ~5-second fade.

On this firmware (BACKLIGHT2 **version 1**), the documented "set permanent mode"
command is silently ignored, and the timer resets *only* on an off→on transition.
So the off→on pulse is the one thing that works, and the brief blink can't be
avoided. The ON pulse uses a *static* effect so the LEDs snap back instantly with
no fade-in, keeping the blink as short as possible.

Credit where it's due: the HID++ protocol details came from the
[Solaar](https://github.com/pwr-Solaar/Solaar) project, and the keepalive approach
from [MXLightkeeper](https://github.com/CasualDeveloper/MXLightkeeper). Thanks to
both.

---

## License

[MIT](LICENSE) © 2026 Roman Zrajevsky
