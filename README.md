# Dictophone

**English** | [Русский](https://github.com/Frish07/dictophone/blob/main/README.ru.md)

A compact audio recorder for **Windows 10/11 x64**, written in **C++17**. It has a Russian-language interface and a 360 × 300 window. Run it without installation and record audio locally.

Current version: **1.4.0**. A portable package with FFmpeg is available in [Releases](https://github.com/Frish07/dictophone/releases). Extract the entire archive and run `Dictophone.exe`.

GitHub Actions checks that the app starts from the published ZIP and that all five audio formats work with the bundled FFmpeg without installing packages. [Portable release checks](https://github.com/Frish07/dictophone/actions/workflows/portable-release.yml)

## Getting started

If you are running the app from the repository, open **[`app/Dictophone.exe`](app/Dictophone.exe)**. WAV recording works out of the box.

To enable **MP3, OGG, FLAC, and M4A**, run this command once from the project root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/setup-ffmpeg.ps1
```

The script downloads a Windows build of FFmpeg from Gyan, checks its SHA256, and places the encoder next to the app. Gyan is listed on the [official FFmpeg download page](https://ffmpeg.org/download.html). The app itself works offline. The downloaded encoder is accompanied by its source information and license. Its size keeps `ffmpeg.exe` out of the Git repository.

## Controls

The app's button and menu labels are in Russian.

- **▶ Запись** (Record) starts recording and becomes **■ Стоп** (Stop). Stop finishes the recording and opens the save dialog.
- **Ⅱ** pauses recording. **▶** on the same button resumes the current recording.
- While paused, the main button becomes **Удалить** (Delete), shown as a trash icon in the compact window. It opens a **Да / Нет** (Yes / No) confirmation. Yes deletes the current unsaved draft and resets the timer. No keeps the recording paused. Resuming restores the Stop button.
- **Сохранить** (Save) or **Ctrl+S** opens the Save As dialog in the selected folder. Enter a name or choose another folder. The selected audio format's extension is added automatically. Canceling keeps the recording paused.
- **◇ / ◆** toggles always on top. Hover over buttons to see tooltips.
- **⋯** opens the mode, format, and output folder menu. The default output folder is `Documents\Dictophone`.
- **Ctrl+O** opens the output folder. Drag the window by its header, or use its minimize and close buttons.

### Compact window

Select **⋯ → Уменьшенное окно** (Compact window) or press **Ctrl+M** to switch to a **306 × 64** panel with three icon buttons for recording/stopping, pausing/resuming, and saving, plus a timer on the right. Tooltips and screen reader labels remain available. Both recording modes and always-on-top behavior work in the compact window.

Drag the panel by its timer. **Right-click** the panel to open the same menu, including the size and always-on-top options. Return to the full window with **Ctrl+M** or a double-click on the timer. The selected window size is remembered after closing the app.

### Push-to-talk mode

Select **⋯ → Рация** (Push-to-talk). **Hold Space** to record and release it to pause. Repeated presses append to the same file, omitting the gaps between presses. You can also hold the **● Держать** (Hold) button with the mouse when starting a new recording. Press **Сохранить** (Save) to finish.

After releasing Space, the main button lets you delete the draft with confirmation. Hold **Space** again to resume a paused push-to-talk recording. The **● Держать** button is also available for the mouse at the start of a new recording.

By default, Space works while the recorder has focus. **⋯ → Пробел в других приложениях** (Space in other apps) enables global push-to-talk, so you can keep the recorder above a game or another app while working there. In push-to-talk mode with this option enabled, the recorder captures Space keystrokes. Turn off the option or switch to regular recording mode to type spaces in other apps. Global controls may be unavailable on secure desktops and in some elevated applications.

The mode and format are locked during an unfinished recording. If export fails, you can choose another format, such as WAV, and retry. You can change the output folder before saving.

## Audio and recovery

The recorder uses the default Windows microphone through WASAPI. When the driver supports RAW, it requests capture with Windows voice effects disabled. Otherwise, it uses standard capture. WAV and FLAC preserve the capture channels and sample rate in 24-bit PCM. Bluetooth headset quality depends on the input mode Windows provides.

Audio is written to disk as it is recorded. WAV has a 4 GB limit, approximately four hours at 48 kHz with two channels. At the limit, the app stops recording and lets you save it. The level meter shows the loudest channel, and the timer counts recorded audio only.

Settings are stored in `%LOCALAPPDATA%\Dictophone\settings.ini`. Temporary WAV files are in `%LOCALAPPDATA%\Dictophone\Recovery`, accessible from the menu. The WAV header is updated every second. A temporary WAV remains available after an export error or closing without saving. After a crash, the final incomplete segment may need manual recovery. Successful export removes the temporary WAV. Existing output files are protected from overwriting.

If the microphone cannot be opened, check the default input device and **Windows Settings → Privacy & security → Microphone → Let desktop apps access your microphone**.

## Building

Option 1 uses [MSYS2](https://www.msys2.org/) with the `mingw-w64-ucrt-x86_64-gcc` package. The build script also looks for the compiler in the standard MSYS2 directories.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -WithFfmpeg -Test
```

The output is `app/Dictophone.exe`, with the GCC runtime linked into the executable. To run checks with a real microphone and create local test recordings:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Microphone
```

Option 2 uses Visual Studio 2022 with the Desktop development with C++ workload and CMake.

```powershell
cmake -S . -B build/cmake -A x64
cmake --build build/cmake --config Release
ctest --test-dir build/cmake -C Release --output-on-failure
```

For compressed formats, place `ffmpeg.exe` next to the resulting `Dictophone.exe`. The entire `app` folder forms the portable package. Tests cover PCM/float conversion, WAV headers, checkpoints, Unicode paths, overwrite protection, source preservation on errors, and all formats when FFmpeg is available. The microphone test is run separately from CI.

## License

The app's source code is distributed under the MIT license. FFmpeg has its own license, and the Gyan essentials build includes GPL components. See the `ffmpeg-license.txt` file created by the setup script and the [FFmpeg licensing information](https://ffmpeg.org/legal.html).
