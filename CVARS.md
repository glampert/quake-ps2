# PS2 backend cvars

Every cvar registered by the PS2 backend under [src/ps2/](src/ps2/), with its default in the
debug (`make`) and release (`make release`) builds. See the [README](README.md) for the
bigger picture. QuakeSpasm's own cvars are not listed here.

- **Arch.** marks `CVAR_ARCHIVE` cvars: they are written to `config.cfg` and read back on
  the next boot, so the value in your config, not the default, is what a run starts with.
- **—** in the release column means the cvar is not registered at all there: the code that
  reads it is compiled out with `PS2_QUAKE_DEBUG` / `PS2_QUAKE_PROFILE`. Archived cvars never
  get a **—**. Every build registers them, even where nothing reads them, because the quit
  drops an archived cvar the build never registered from `config.cfg`.

None are registered yet: the backend's cvars come back as each subsystem is ported.

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
