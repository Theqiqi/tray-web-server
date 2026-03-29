[简体中文](doc/README.zh-CN.md)

# tray-web-server

`tray-web-server` is the standalone Linux C++ HTTP server used by `tray-web`.

It replaces the old Python backend and provides:

- static file hosting for the panel UI
- login and session handling
- settings persistence
- bundled core process management
- subscription and user APIs

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

```bash
./build/tray_web_server --app-root /path/to/tray-web
```

## Related Projects

- [tray-web](https://github.com/Theqiqi/tray-web): the full web panel that embeds this server
- [tray-core](https://github.com/Theqiqi/tray-core): the Linux C++ proxy runtime managed through the panel

## License

This project is free for non-commercial use, but attribution is required.

Commercial use requires a paid license. See [LICENSE.md](LICENSE.md).
