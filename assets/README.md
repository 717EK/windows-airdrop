# PulseIt visual assets

- `brand/pulseit.svg`: editable vector mark; the waveform is original to this project.
- `brand/pulseit.png`: transparent 1024 px mark.
- `brand/PulseIt.ico`: Windows icon with 16, 24, 32, 48, 64, 128 and 256 px frames.
- `installer/`: high-DPI welcome and small wizard bitmaps.
- `readme/`: public vector diagrams and screenshots rendered with synthetic demo data.

Run `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/render-brand.ps1` on Windows to regenerate the PNG, ICO and setup bitmaps with System.Drawing. The SVG and in-app vector share the same normalized waveform coordinates. Asset changes must also be included by `tools/installer-source.py` so executable icons can be rebuilt from distributed source.

Screenshots are illustrative; they are not evidence of physical device discovery or file delivery. These project-created assets follow the repository's default license.
