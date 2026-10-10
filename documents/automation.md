# Automated test sessions

Run timed controller commands with offscreen rendering and periodic screenshots.
Use a separate copy of your user data for every experiment.

Set `SHAD_AUTOMATION_USER_DIR` to an absolute, existing directory containing a
`.shadps4-test-user` marker file. The emulator uses that directory for its user
files and forces saves into its `home` subdirectory, regardless of configured
home paths. An invalid test directory stops startup instead of falling back to
normal user data.

Set `SHAD_AUTOMATION_SCRIPT` to a text file containing timed commands:

```text
12 press cross
12.2 release cross
40 axis rightx 180
42 recenter
43 screenshot
50 quit
```

Times are seconds since the input loop starts. Supported commands are `press`,
`release`, `axis`, `recenter`, `screenshot`, and `quit`. Axis names are `leftx`,
`lefty`, `rightx`, `righty`, `l2`, and `r2`. Stick axes use byte values from 0 to
255, with 128 centered. Scripted input bypasses response shaping and smoothing.

Automation uses a virtual controller and does not open physical gamepads unless
`SHAD_AUTOMATION_PHYSICAL_PAD` is set. The
window remains offscreen and windowed. Screenshots are captured every five
seconds in the test user's `screenshots` directory. Vulkan rendering still uses
the GPU; concurrent game sessions can affect performance measurements.

Build on this Windows setup with `scripts\build-windows.cmd`, which initializes
the Visual Studio Build Tools environment and builds with Clang and Ninja.
