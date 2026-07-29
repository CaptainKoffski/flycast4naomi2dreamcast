# Patches

`flycast-syphon-build-fix.diff` — macOS build fix for the `core/deps/Syphon`
**submodule** (a fork can't carry commits for it, so it ships as a patch).
Apply it *inside the submodule*, after `git submodule update --init --recursive`:

```sh
git -C core/deps/Syphon apply "$PWD/patches/flycast-syphon-build-fix.diff"
```

It changes Syphon's `target_precompile_headers` from `PUBLIC` to `PRIVATE` so
the ObjC prefix-header PCH doesn't propagate to the Flycast target and break
its OBJC++ sources ("Objective-C was disabled in PCH").

See `INSTRUMENTATION.md` at the repo root for the full story of this fork.
