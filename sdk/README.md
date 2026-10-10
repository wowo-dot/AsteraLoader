# Astera Loader plugin kit

Everything needed to write a plugin for Astera Loader, the plugin loader
for Monster Hunter: World on PC.

- `include/al/al_plugin.h`: the plugin API, one C header. It is the
  reference: every type, call and rule is described there.
- `docs/plugin-guide.md`: how the parts fit together. Start here.
- `examples/hello/hello.c`: the smallest working plugin, in C.
- `CMakeLists.txt`: builds the example.

## Build the example

Windows with Visual Studio (C/C++ for x64) and CMake 3.25 or newer:

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

The plugin is `build\Release\hello.dll`. Put it in
`<game>\AsteraLoader\plugins\hello\hello.dll` (under Mod Organizer 2: a mod
of its own with that layout) and start the game through Astera Loader. Its
log is `AsteraLoader\logs\plugins\hello.log`.

## Your own plugin

Copy the example, rename the folder, the DLL and the name in
`al_plugin_info_t` (all three must match), and build it the same way.
Plugins are plain DLLs with a C interface: any language that can export C
functions from a 64-bit Windows DLL works.

## Licence

MIT (`LICENSE`).
