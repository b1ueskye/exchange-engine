# 学习进度与验收记录

每次练习结束后，把测试命令和实际结果记到自己的 `exchange-lab`。下面的表用于追踪预期成果。Day 01–32 可跟随[教程首页](../README.md)的八个阶段完成。每行描述相应检查点的能力与验收。

| 检查点 | 新模块与路径 | 公开能力 | 过关证据 | 下一步 |
| --- | --- | --- | --- | --- |
| 01 | `include/exchange/notional.hpp`、`src/notional.cpp`、`app/main.cpp`、`tests/notional_test.cpp`、`CMakeLists.txt` | `quote_notional` 和三个 CMake 目标 | 输出 3060，CTest 运行 10 个金额场景 | 值类型与构造 |
| 05 | `tests/`、Debug/Release 构建 | 测试辅助函数、调试与 sanitizer 流程 | 四项 CTest 在 Debug、Release 和 ASan/UBSan 通过。故意改错预期可定位到场景 | 领域模型 |
| 08 | `units.hpp`、`model.hpp`、`tests/trading_fixtures.hpp` 及对应实现和测试 | 单位、命令校验、值回执与手算夹具 | 7 项 CTest 通过。手算结果覆盖价限、FIFO、总预算与改价末态 | 价格档 |
| 11 | `order_book.hpp`、`src/order_book.cpp`、订单簿测试 | 价档、FIFO、ID 索引、`validate`、`snapshot`、`resting_orders` | 10 项 CTest。索引回滚、L2 深度和旧快照独立 | 撮合 |
| 16 | `src/order_book.cpp`、`tests/reference_book.hpp`、撮合及差分测试 | GTC、IOC、FOK_BUDGET、撤减改 | 15 项 CTest，3600 条命令逐步比较，失败种子可复现 | 多品种与账户 |
| 20 | `engine.hpp`、`ledger.hpp` 及对应实现 | `Engine::execute`、资金、结算和查询 | 19 项 CTest，双品种现货端到端及逐币种守恒 | 基准与布局 |
| 24 | `bench/`、池和替代簿实现 | `Book` 策略，两个实现同契约 | 23 项 CTest，两种布局各核对 3600 条命令，四类负载逐轮比对 | 异步 |
| 27 | `include/exchange/async_engine.hpp` | `submit`、`close` | 满载、竞态关停、顺序重放 | 恢复 |
| 30 | `include/exchange/persistence.hpp` | 日志、快照、索引重建 | 29 项 CTest，六种独立进程恢复场景及尾段一致性 | 期货 |
| 31 | `positions.hpp`、`java_position_order.hpp`、`futures_risk.hpp` 与实现、Java/C++ 对照测试 | 同一 Engine 的现货和期货风险、共享账户与盘口估值 | 固定风险测试、408 行实际 Java 对照；恢复在下一课升级 | 版本与完整集成 |
| 32 | v2 编解码、恢复测试、集成矩阵 | v1 迁移、持仓与缓存恢复、两产品全链路 | 34 项 CTest；24 组合 × 3 种子；八种独立进程场景 | 按决策记录扩展 |

可复制到自己的学习日志中填写。

```text
课次与代码版本
本课新增目录及接口
能独立解释的 C++ 机制
本课固定的业务不变量
实际运行命令和结果
遇到的首个错误及原因
下一课依赖的当前能力
```

[逐课任务](../00-overview/learning-roadmap.md) · [架构决策](architecture-decisions.md)
