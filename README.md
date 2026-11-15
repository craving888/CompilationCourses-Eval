# ToyC

从 C++17 / Flex 词法分析，经递归下降、AST、静态语义检查和三地址 IR，
生成 RV64GC 汇编。详见 [IR 设计](DESIGN.md)和[课程规范](https://lightsmilew.github.io/compiler-principles/)。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ln -sfn build/compiler compiler
./compiler -token < input.c
./compiler -ast < input.c
./compiler -ir < input.c
./compiler -asm < input.c > input.s
riscv64-unknown-elf-gcc -march=rv64gc -mabi=lp64d -nostdlib -static \
  input.s third_party/toyc/libtoyc.a -o input
qemu-riscv64 ./input < input.in
```

Flex 使用最长匹配，非法字符诊断后恢复。语法分析对各优先级用循环进行左结合，
一元运算递归，else 匹配最近 if；错误恢复在边界重新同步。语义检查使用作用域栈、
按声明顺序登记函数，并组合语句的正常、返回、跳出及继续出口。

| IR | RV64GC |
|---|---|
| 常量、载入、写入 | li、lw、sw |
| 32 位加减乘除取余 | addw、subw、mulw、divw、remw |
| 比较和逻辑取反 | slt / xor / seqz / snez / xori |
| 条件与循环 | 条件跳转与基本块标签 |
| 调用、返回 | call、统一尾声、ret |

栈帧从 sp 起依次为外传实参、局部 i32 槽、基线虚拟值槽、对齐区和 ra；
总大小按 16 字节对齐。前 8 个参数由 a0–a7 传递，余者占 8 字节栈槽，
被调用者从原 sp 处读取；返回值用 a0。每次调用前活跃值都在固定槽中。
没有 phi 指令：IR 在两条短路结果边写同一槽，合流块读取，因此无需并行复制。

第五部分在 `-ir -opt` 与 `-asm -opt` 时启用 IR 优化：
前向常量格在所有前驱合并槽状态；编译期整数运算使用宽类型检查，避免折叠除零与溢出。
折叠分支后用 CFG 剪除不可达块。局部公共子表达式复用同块中支配使用点的值，
变量槽用反向活跃分析删除死 store；无用的纯计算递归消除，保留所有仍然可达的 call。
同一块内从最近的写入转发局部值。短小的单块函数可内联，保留函数调用的副作用；
直接自尾调用在实参全部求值后更新参数槽，再跳回函数体。
对具有独立入口的简单循环，外提不依赖循环写入的纯计算；
仅当计数器初始化、边界和递增保证终止，且结束值未被读取时，去除空计数循环。
各变换迭代至不动点。此实现没有 mem2reg 或全局值编号。

第六部分在 `-asm -opt` 启用目标代码优化。分配前复用同一基本块中、没有槽写入间隔的
重复读取。根据定义及最后使用位置对块内值做线性扫描，分配 t2–t5；
跨块或活过 call 的临时值放在栈中。寄存器用完时将结束最晚的区间放到栈，
定义处写入，使用前加载。静态读写次数较高的最多十一个私有局部变量使用 s1–s11，
序言保存、尾声恢复。指令直接使用分配到的寄存器；常量在使用处作为立即数或寄存器值，
比较只有分支这一处使用时直接生成条件跳转。叶函数省去 ra 的保存和恢复。
未实现图着色、区间拆分或基于运行数据的溢出代价估计。
