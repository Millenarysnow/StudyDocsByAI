// src/binding_runtime.h — 最小的 C++ 绑定运行时
//
// 实际引擎里这些会连接到 Lua 虚拟机 (参考 MiniLuaBind demo 里的 MemberFunctionCaller)。
// 这里为了演示 CMake 构建流程, 只打印信息, 不真的调 Lua。
#pragma once
#include <iostream>
#include <string>

struct lua_State;  // 占位

// "注册一个类到 Lua" 的开端: 实际会创建 metatable
template<class T>
void beginRegisterClass(lua_State* /*L*/, const char* class_name) {
    std::cout << "[binding] begin class \"" << class_name << "\"" << std::endl;
}

// "注册一个类到 Lua" 的结束
template<class T>
void endRegisterClass(lua_State* /*L*/, const char* class_name) {
    std::cout << "[binding] end class \"" << class_name << "\"" << std::endl;
}

// 注册成员函数: 实际会把函数指针塞进 metatable
#define REGISTER_MEMBER(L, name, method) \
    do { \
        std::cout << "[binding]   + method: " << (name) << std::endl; \
        (void)(L); \
        (void)(method); \
    } while (0)

// 注册成员变量: 实际会生成 getter/setter
#define REGISTER_MEMBER_VAR(L, name, member) \
    do { \
        std::cout << "[binding]   + member: " << (name) << std::endl; \
        (void)(L); \
        (void)(member); \
    } while (0)

// 供生成代码特化的模板
template<class T> class RegisterCPPClassToLua;
