# 撮合引擎 HTTP API

`exchange_api` 是本地 HTTP/JSON 服务。它监听 `127.0.0.1`，由同一个事件循环处理请求并调用撮合引擎。账户、余额和订单保存在进程内，重启会清空状态。请求中的 `user_id` 是业务字段；服务只面向本机可信客户端，没有客户端身份认证，也不提供跨域访问。

## 构建并启动

API 支持 macOS 和 Linux。依赖 C++20、CMake、pkg-config、libevent ≥ 2.1.12 和 json-c ≥ 0.16。macOS 可用 `brew install pkgconf libevent json-c`；Debian 系发行版可安装 `pkg-config libevent-dev libjson-c-dev`，具体版本以包管理器为准。依赖缺失时 CMake 会跳过 API 目标，撮合引擎仍可单独构建。

在仓库根目录执行：

```sh
cmake -S exchange-lab -B build -DEXCHANGE_BUILD_API=ON
cmake --build build --target exchange_api
./build/exchange_api 8080
```

省略端口时使用 8080，传入 `0` 时由系统分配可用端口。进程启动后打印实际地址。

## 路由

| 方法和路径 | 用途 | 参数 |
| --- | --- | --- |
| `GET /health` | 健康检查 | — |
| `POST /v1/symbols` | 注册交易品种 | `symbol_id`、`base`、`quote`、`base_scale`、`quote_scale`、`maker_fee_per_lot`、`taker_fee_per_lot` |
| `POST /v1/users` | 开户 | `user_id` |
| `POST /v1/balances/adjust` | 入金或出金 | `user_id`、`currency`、`units`、`funding_id` |
| `POST /v1/orders` | 下单 | `symbol_id`、`user_id`、`order_id`、`side`、`type`、`quantity`，以及对应订单类型的价格或预算 |
| `POST /v1/orders/cancel` | 撤销剩余挂单 | `symbol_id`、`user_id`、`order_id` |
| `POST /v1/orders/reduce` | 减量 | 撤单字段加 `quantity`，表示减掉的数量 |
| `POST /v1/orders/move` | 改价 | 撤单字段加 `new_price` |
| `GET /v1/users/{user_id}` | 查余额、冻结资金和挂单 | 用户 ID |
| `GET /v1/books/{symbol_id}` | 查买卖盘，各最多 20 档 | 品种 ID |
| `GET /v1/totals` | 查各币种的余额、冻结、费用和资金守恒 | — |

`side` 为 `bid` 或 `ask`。`type` 为 `gtc`、`ioc` 或 `budget`。GTC 和 IOC 需要 `price`，买单可额外传 `reserve_bid_price`。预算单需要 `budget_total`，单位为价格档乘手数，与引擎中的 `NotionalSteps` 一致。下单可选 `timestamp_ns`，缺省为零。入金填正的 `units`，出金填负数，`funding_id` 按用户递增。

POST 发送 `Content-Type: application/json`，请求体最多 16 KiB。ID、价格、数量、金额和序号在响应中使用十进制字符串，避免 JavaScript 损失整数精度。请求也可传安全范围内的整数 JSON 数字（绝对值小于 2⁵³）。金额、价格和数量按引擎的整数单位传输。

## 从开户到成交

```sh
api=http://127.0.0.1:8080
post() {
  curl -sS -H 'Content-Type: application/json' -d "$2" "$api$1"
  printf '\n'
}

post /v1/symbols '{"symbol_id":"1","base":"10","quote":"20","base_scale":"1","quote_scale":"1","maker_fee_per_lot":"1","taker_fee_per_lot":"2"}'
post /v1/users '{"user_id":"1"}'
post /v1/users '{"user_id":"2"}'
post /v1/balances/adjust '{"user_id":"1","currency":"20","units":"10000","funding_id":"1"}'
post /v1/balances/adjust '{"user_id":"2","currency":"10","units":"100","funding_id":"1"}'
post /v1/orders '{"symbol_id":"1","user_id":"2","order_id":"10","side":"ask","type":"gtc","price":"100","quantity":"2"}'
post /v1/orders '{"symbol_id":"1","user_id":"1","order_id":"11","side":"bid","type":"ioc","price":"101","quantity":"3"}'
curl -sS "$api/v1/users/1"
```

买单响应中的 `events` 先返回数量为 2 的 `trade`，再返回数量为 1 的 `reject_remainder`，原因为 `ioc_remainder`。这条指令已处理，HTTP 状态为 200。

引擎响应包含 `status`、`seq` 和 `events`，用户与资金汇总查询还返回 `report`。健康检查和盘口查询不分配新序号，盘口响应的 `seq` 是读取时的引擎序号。常见拒绝包括 400（参数无效）、404（品种、用户或订单不存在）、409（重复挂单 ID 或资金流水冲突）、422（余额不足、超出保留价或结算失败）、503（订单池容量不足）。请求在进入引擎前被拒绝时，响应以 `status` 和 `message` 报错，不含 `seq`。HTTP 解析器也会直接拒绝超出请求大小限制的消息。

## 验证

```sh
ctest --test-dir build --output-on-failure -R '^exchange_http_api$'
```

配置时检测到 Python 3.9 或以上才注册该集成测试，它会启动临时本地服务并通过 HTTP 验证完整订单流程。
