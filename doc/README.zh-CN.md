# tray-web-server

`tray-web-server` 是 `tray-web` 使用的独立 Linux C++ HTTP 服务。

它替代了旧的 Python 后端，负责：

- 面板静态资源服务
- 登录与会话管理
- 设置持久化
- 核心进程管理
- 用户订阅与导出接口

## 构建

```bash
cmake -S . -B build
cmake --build build -j
```

## 运行

```bash
./build/tray_web_server --app-root /path/to/tray-web
```

## 相关项目

- [tray-web](https://github.com/Theqiqi/tray-web)：集成本服务的完整 Web 面板
- [tray-core](https://github.com/Theqiqi/tray-core)：通过面板进行管理的 Linux C++ 代理内核

## 许可证

本项目非商用免费，但必须保留署名。

商用需要购买付费许可证。详见 [LICENSE.md](../LICENSE.md)。
