# Day 02 · 用值类型描述一笔委托

## 1. 本期内容与学习目标

沿用 Day 01 的 `exchange-lab`。你需要知道函数、头文件、`optional` 和 CMake 目标，预计用时 1–2 小时。本课给委托草稿加上方向、ID、价格和数量，并用一个实验分清 C++ 中的「复制订单」和「给同一订单起别名」。本课的 `OrderDraft` 是练习类型，Day 06–07 再建立具有明确单位的正式委托。

## 2. 业务背景与原理

交易入口收到的是一条指令。交易员可能在发送前复制草稿，将数量从 3 手改为 5 手做比较。撮合模块以后还会维护挂单的剩余量。两种操作的存活时间不同，草稿的字段也不能拿来表示已经成交多少。下面的草稿和副本都使用 ID 7，复制出的草稿只是同一委托的备选输入。


| 动作                  |   原始草稿 |  新草稿或别名 |
| --------------------- | ---------: | ------------: |
| 创建买入草稿          | ID 7，3 手 |            空 |
| 复制到`copy` 并改数量 |       3 手 |          5 手 |
| 通过别名修改原草稿    |       4 手 | 副本仍为 5 手 |

## 3. Java 原始项目的实现分析

`ApiPlaceOrder` 的字段是 `final`，API 转换器将字段逐个写入预分配的 `OrderCommand`。撮合可用入站命令立即成交，GTC 留有余量时，Naive 簿另建 `Order` 保存活动记录。`OrderAction` 是 Java 枚举，定义 `BID` 和 `ASK`。Java 引用赋值 `draftB = draftA` 共享同一个对象，原 API DTO 的 `final` 字段限制的是赋值，实际的活动记录字段可变。这个区分可以在 C++ 用两个独立类型表达。 [ApiPlaceOrder](https://github.com/exchange-core/exchange-core/blob/2f8548749839e9095c8dc597e4b61521d259fa5d/src/main/java/exchange/core2/core/common/api/ApiPlaceOrder.java#L28-L38) · [命令转换](https://github.com/exchange-core/exchange-core/blob/2f8548749839e9095c8dc597e4b61521d259fa5d/src/main/java/exchange/core2/core/ExchangeApi.java#L427-L440) · [GTC 剩余入簿](https://github.com/exchange-core/exchange-core/blob/2f8548749839e9095c8dc597e4b61521d259fa5d/src/main/java/exchange/core2/core/orderbook/OrderBookNaiveImpl.java#L104-L140) · [方向枚举](https://github.com/exchange-core/exchange-core/blob/2f8548749839e9095c8dc597e4b61521d259fa5d/src/main/java/exchange/core2/core/common/OrderAction.java#L21)

## 4. 本期 C++ 知识

### 类、构造与访问权限

`struct` 和 `class` 都可以有构造函数、方法和数据成员。默认访问权限分别为 `public` 和 `private`。对于没有字段约束的测试夹具，公开字段写起来短。草稿的价格和数量必须为正，适合把字段设为 `private`，把建立对象的入口集中在 `create`。

构造函数的名字与类相同，没有返回类型。冒号后的初始化列表直接初始化成员。成员按类中声明的顺序初始化，写在列表前面也不会改变这一点。含对象成员时，先默认构造再在函数体赋值可能引入额外工作，有些成员还没有默认构造函数。把声明和初始化列表写成同一顺序，有利于看出一个初始化表达式是否依赖尚未初始化的成员。 [C++ 成员初始化](https://eel.is/c++draft/class.base.init)

### 方向、值与别名

`enum class Side { bid, ask };` 定义有作用域的枚举。用 `Side::bid` 取值，整数不能隐式用作 `Side`。外部协议以后要把数字转成枚举，转换时逐个核对编码。即使使用了强枚举，显式转换仍可能生成没有对应枚举项的值。

`auto copy = *draft;` 复制 `optional` 里的草稿。隐式生成的复制构造函数按成员复制，此处每个成员都是小值。`auto& alias = *draft;` 绑定已有草稿，`&` 表示引用。对 `alias` 调方法与对草稿本身调方法作用相同。通过 `const OrderDraft&` 借用时，只能调用签名末尾标有 `const` 的成员函数。这类成员函数不能用普通方式改写该对象的非 mutable 成员。 [C++ 隐式复制构造](https://eel.is/c++draft/class.copy.ctor)

`std::int64_t` 仍用于三个数字字段，所以交换 ID 与价格可能通过编译。草稿类收拢字段与规则。Day 06 将继续用不同的类型拦住单位混用。

## 5. 架构设计与技术选型


| 方案               | 写法及适用范围                                                                             |
| ------------------ | ------------------------------------------------------------------------------------------ |
| 公开字段聚合       | 适合允许任意初值的测试数据。调用者须负责保持合法，任意赋值都能绕开检查                     |
| 私有构造与可空工厂 | 把非法草稿限制在创建入口。修改数量走有返回值的方法，失败时保留原值                         |
| 公开构造并抛异常   | 能守住不变量。预期会反复遇到的用户输入错误需要异常处理。本课用可空工厂把失败放在函数签名里 |

`OrderDraft::create` 的成本是固定次数的比较及小值构造。`set_quantity` 返回是否接受新值，成功时只改数量。它不会改方向、价格和 ID。草稿及副本由各自的局部变量拥有，借用活到被借用对象销毁或被重建为止。

## 6. 从零逐步实现

### 6.1 定义草稿及更新规则

创建 `include/exchange/order_draft.hpp`。先敲成员和私有构造函数，再敲 `create`。类体内定义的简单函数适合放进头文件，允许相同定义随头文件进入多个翻译单元。`static` 工厂可用类名直接调用。

```cpp
#ifndef EXCHANGE_ORDER_DRAFT_HPP
#define EXCHANGE_ORDER_DRAFT_HPP

#include <cstdint>
#include <optional>

namespace exchange {

enum class Side { bid, ask };

class OrderDraft {
public:
    static std::optional<OrderDraft> create(std::int64_t id, Side side,
                                            std::int64_t price, std::int64_t quantity) {
        if (id <= 0 || price <= 0 || quantity <= 0 ||
            (side != Side::bid && side != Side::ask)) {
            return std::nullopt;
        }
        return OrderDraft{id, side, price, quantity};
    }

    std::int64_t id() const { return id_; }
    Side side() const { return side_; }
    std::int64_t price() const { return price_; }
    std::int64_t quantity() const { return quantity_; }

    bool set_quantity(std::int64_t quantity) {
        if (quantity <= 0) {
            return false;
        }
        quantity_ = quantity;
        return true;
    }

private:
    OrderDraft(std::int64_t id, Side side, std::int64_t price, std::int64_t quantity)
        : id_{id}, side_{side}, price_{price}, quantity_{quantity} {}

    std::int64_t id_;
    Side side_;
    std::int64_t price_;
    std::int64_t quantity_;
};

}

#endif
```

访问器按值返回小整数和枚举。调用者可以把返回值赋给自己的变量，无法通过它绕过 `set_quantity`。花括号中的 `OrderDraft{id, side, price, quantity}` 调用私有构造函数，之后结果转换成有值的 `optional<OrderDraft>`。本课选择用头文件实现，`exchange_core` 通过公开 include 路径提供该头。

### 6.2 接上 Day 01 的测试工程

第 7 节写好测试文件后，在 `CMakeLists.txt` 末尾追加以下目标。文件只有一个 `main`，独立测试目标可单独运行。

```cmake
add_executable(order_draft_tests tests/order_draft_test.cpp)
target_link_libraries(order_draft_tests PRIVATE exchange_core)
add_test(NAME order_draft COMMAND order_draft_tests)
```

## 7. 测试、调试与验证

创建 `tests/order_draft_test.cpp`。`->` 在这里通过 optional 访问其内部对象的成员，效果相当于对 `(*original)` 取成员。测试已经在第一处条件确认它有值。`static_assert` 在编译时检查条件，`std::is_convertible_v` 查询一种类型能否隐式转成另一种类型。

```cpp
#include "exchange/order_draft.hpp"

#include <iostream>
#include <type_traits>

int main() {
    using exchange::OrderDraft;
    using exchange::Side;
    static_assert(!std::is_convertible_v<int, Side>);

    auto original = OrderDraft::create(7, Side::bid, 102, 3);
    if (!original || original->id() != 7 || original->side() != Side::bid ||
        original->price() != 102 || original->quantity() != 3) {
        std::cerr << "FAIL: initial draft\n";
        return 1;
    }

    auto copy = *original;
    auto& alias = *original;
    if (!copy.set_quantity(5) || original->quantity() != 3 ||
        !alias.set_quantity(4) || original->quantity() != 4 || copy.quantity() != 5) {
        std::cerr << "FAIL: copy or alias\n";
        return 1;
    }

    if (alias.set_quantity(0) || alias.quantity() != 4 ||
        OrderDraft::create(0, Side::bid, 102, 1) ||
        OrderDraft::create(8, Side::ask, -1, 1) ||
        OrderDraft::create(8, Side::ask, 102, 0)) {
        std::cerr << "FAIL: invalid input must leave a valid draft\n";
        return 1;
    }

    std::cout << "original: " << original->quantity()
              << ", copy: " << copy.quantity() << '\n';
    return 0;
}
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build -V
```

多配置生成器运行 `cmake --build build --config Debug` 和 `ctest --test-dir build -C Debug -V`。两项测试应通过，草稿测试输出 `original: 4, copy: 5`。把 `auto copy` 改为 `auto& copy` 并重测，预期收到 `FAIL: copy or alias`。若编译器抱怨调用私有构造函数，回到 `OrderDraft::create` 创建对象。若 `Side::bid` 找不到，检查是否在 `exchange` 作用域。

## 8. Java 与 C++ 实现深度对比

Java API 把新单字段封装在 DTO 中，用转换器向环槽写值。C++ 草稿也集中封装输入，拷贝时会为所有成员复制出一组独立值。C++ 可以借用引用消除一次大对象复制，也要保证对象一直存活。本课对象只有三个整数和一个枚举，按值复制直接表达「备选草稿」。性能上的取舍待订单和容器实际出现后再测。

## 9. 课后练习

1. **基础。** 建一条卖单并通过 `const OrderDraft&` 读方向。试写一次 `set_quantity` 并保存编译诊断。
2. **工程。** 为改价草稿补充 `set_price`，用负价和正价各写一个用例。通过的更新只改价格，失败保留旧价。
3. **思考。** 解释为何复制一个草稿再改量不能自动获得新的订单身份。给出一个应由后续订单簿拒绝的输入序列。

## 10. 总结与下一期

草稿的值语义已由测试覆盖。Day 03 会给这段测试画出对象寿命，进一步看拥有资源的局部对象怎样在三种退出路径释放资源。

[Day 01](day-01.md) · [课程目录](../00-overview/learning-roadmap.md) · [Day 03](day-03.md)

<details>
<summary>练习参考答案</summary>

1. `const OrderDraft& view = *original;` 可读 `view.side()`。`view.set_quantity(2)` 因其成员函数签名缺少末尾 `const` 而被拒绝。
2. 与 `set_quantity` 使用相同检查，成功路径写 `price_`。测试负价时先记住旧价，之后核对返回 `false` 和旧价。合法价时同时核对 ID、方向和数量。
3. 复制会复制 `id_`。原单用 ID 7 在某品种挂簿后，副本再次以该品种和 ID 7 下单，课程会在成交前拒绝。

</details>
