# Controller tuning

This fork uses right-stick sensitivity `1.55`, response-curve exponent `1.1`, and
axis smoothing disabled by default. The curve applies to each right-stick axis
after its deadzone. Left-stick response and button bindings keep their existing settings.

Set `SHAD_CAM_TUNE_FILE` before starting BB_Launcher to enable live camera tuning.
Use an absolute path to a text file containing:

```text
sens=1.55
curve=1.1
```

The file is checked at most every 500 ms when right-stick output is processed.
Changes apply without restarting the game. Missing files and invalid values keep
the previous settings. Blank lines and lines beginning with `#` are accepted.

- `sens` must be a finite positive number. Higher values reach maximum output with less stick movement.
- `curve` must be finite and at least `0.05`. Values above `1` give finer control near the center; values below `1` increase the response near the center.
- Set both values to `1` to use the original linear response.

Set `SHAD_AXIS_SMOOTH_MS` to a nonnegative integer before launching the emulator.
Use `0` to disable smoothing or `33` to restore upstream's smoothing window. Changes require an emulator
restart. Invalid or overflowing values disable smoothing and produce a log warning.

For example, start BB_Launcher from the same PowerShell session:

```powershell
$env:SHAD_CAM_TUNE_FILE = 'C:\Users\daves\Desktop\Bloodborne\camera_tune.txt'
$env:SHAD_AXIS_SMOOTH_MS = '0'
& 'C:\Users\daves\Desktop\Bloodborne\BB_Launcher.exe'
```

Select the build compiled from this branch in BB_Launcher. An already-running
launcher must be closed before starting it with these environment variables.
