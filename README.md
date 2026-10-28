# mxlight

**Keep your Logitech MX Keys backlight on.**

Logi Options+ lets you set the backlight brightness and an auto-off _timeout_,
but there's no "always on" option — after about 5 seconds of not typing, the
backlight fades out. `mxlight` is a tiny (~150 KB), dependency-free Windows
program that keeps it lit: run it in a terminal, and the backlight stays on until
you stop it with **Ctrl+C**.

- **No install, no runtime, no dependencies.** A single native `.exe`. No .NET,
  no Python, no drivers — just the HID APIs built into Windows.
- **Tiny footprint.** It sleeps between short keepalive pulses; near-zero CPU.

> ⚠️ **One honest caveat:** on the MX Keys firmware I tested, the keyboard only
> relights its backlight on a real power **off→on** transition, so `mxlight` has
> to pulse it off-then-on a few times a minute. That leaves a _very_ brief
> (~5-10 ms) blink that you can occasionally catch if you look closely. It's the
> minimum physically possible — see [How it works](#how-it-works).

---

## Build

Requires **Visual Studio 2022** (or the bare Build Tools) with the _Desktop
development with C++_ workload. No other dependencies.

```bat
build.bat
```

This finds the MSVC toolchain automatically and produces `build\mxlight.exe`.

## Use

```bat
build\mxlight.exe
```

That's it. Leave it running and your backlight stays on; press **Ctrl+C** to
stop. If the keyboard is asleep or switched to another device (Easy-Switch), it
will wait and start keeping the light on as soon as the keyboard comes back.

> Tip: make sure the MX Keys is switched to this PC and awake (tap a key) the
> first time you run it.

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
