// src/main.cpp — 引擎入口
#include "binding_runtime.h"
#include "my_widget.h"

// 注意这个 include: 它是 CMake 构建时自动生成的文件, 不在 src/ 下!
// include 路径由 CMake 的 target_include_directories 指定。
#include "register_my_widget_to_lua.h"

int main() {
    std::cout << "=== Engine Startup ===" << std::endl;

    // 调用代码生成器产出的 registerClass, 把 MyWidget 注册给 Lua
    RegisterCPPClassToLua<MyWidget>::registerClass(nullptr);

    std::cout << std::endl << "=== Using MyWidget ===" << std::endl;

    // 正常使用 C++ 类 (这部分就是你的游戏业务逻辑)
    MyWidget w;
    w.setText("Hello from C++");
    w.setSize(800, 600);
    std::cout << "[main] ---- modified! ---- w.getText() = " << w.getText() << std::endl;
    std::cout << "[main] w.getWidth() = " << w.getWidth() << std::endl;

    std::cout << std::endl << "=== Done ===" << std::endl;
    return 0;
}
