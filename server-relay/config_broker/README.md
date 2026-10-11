# 本地配置 broker

这是 root-only 秘密文件 helper，无 TCP、供应商调用、任意路径、shell 或动态模块接口。生产入口 `python -m config_broker` 只接受部署指定的目录和固定 relay UID；Unix socket 用 Linux peer credential 校验，relay 客户端也核验 broker UID 0。Root 目录 0700，状态 / 快照文件 0600；测试 ownership 替身只由合成测试直接构造，生产入口没有绕过参数。

当前 RPC 和持久格式统一见 [broker 契约](../../docs/contracts/config-broker-v1.json)；允许方法和字段源定义由该契约路由。当前格式为新安装版本，不读取作者旧分步保存状态。

配置由 `save_activate` 一次原子发布，不再保存未启用候选。中断后读到整份旧或新配置；读取当前 revision 可重试丢失的响应。管理视图只返回非秘密字段和 configured 布尔值；实际 worker 读取快照须有持久 pin。

快照仅保留当前配置与真实在途任务需要的版本，状态大小有界。任务结束 / 停止失败后解除 pin 并回收旧秘密文件；恢复时自动从当前版本任务 ledger 对齐，不要求用户清点历史或手工确认迁移。实际部署和容器权限验证在相应 Docker 步骤完成。

回归入口为项目根 `./scripts/test_relay.sh test_config_broker`，包含一操作保存、在途快照、回收、RPC / 文件权限和持久写入边界故障测试。
