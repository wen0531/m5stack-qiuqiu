# 文档索引

| 文档 | 内容 | 适合谁 |
|---|---|---|
| [01-quickstart.md](01-quickstart.md) | 从零跑通：刷固件、装 App、跑桥、接 ASR | 第一次使用 |
| [02-protocol.md](02-protocol.md) | BLE / WebSocket / 音频包协议细节 | 想改通信、写自己的客户端 |
| [03-custom-avatar.md](03-custom-avatar.md) | 用 C++ 开发自己的角色形象 | 想换形象/做二次创作 |
| [04-config.md](04-config.md) | 所有配置项：agents.json、环境变量、地址规则 | 部署与调参 |
| [05-pitfalls.md](05-pitfalls.md) | 17 个真实踩坑与修复（含根因） | 排错时先看这个 |
| [06-power.md](06-power.md) | 功耗实测与省电方案（含 light sleep 结论） | 关心续航 |
| [07-ab-slots.md](07-ab-slots.md) | A/B 双固件槽：切换、克隆、自动回滚 | 想安全升级 |
| [08-i18n.md](08-i18n.md) | 多语言实现与添加新语言的方法 | 想加自己的语言 |

## 排错顺序

1. 先跑 `python tools/selftest.py`
2. 看 [05-pitfalls.md](05-pitfalls.md) 有没有同款现象
3. 再查 [04-config.md](04-config.md) 的配置项
