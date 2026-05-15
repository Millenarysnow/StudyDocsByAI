// sample_cpp/my_widget.h
// 这是方案 B (Unreal 风格): 绑定信息直接用宏标注在 C++ 源码里。
// 没有单独的元数据文件。

#pragma once
#include <string>

// 这些宏在正常编译时展开为空，只是给源码扫描工具看的"标记"
#ifndef BIND_LUA_CLASS
#define BIND_LUA_CLASS()
#define BIND_LUA_METHOD()
#define BIND_LUA_MEMBER()
#endif


BIND_LUA_CLASS()
class MyWidget {
public:
    MyWidget() : m_width(100), m_height(50) {}

    BIND_LUA_METHOD()
    void setText(const char* s) { m_text = s; }

    BIND_LUA_METHOD()
    const char* getText() const { return m_text.c_str(); }

    BIND_LUA_METHOD()
    void setSize(int w, int h) { m_width = w; m_height = h; }

    BIND_LUA_METHOD()
    int getWidth() const { return m_width; }

    BIND_LUA_METHOD()
    int getHeight() const { return m_height; }

    // 这个没标注, 不导出
    void internalDebug() {}

    BIND_LUA_MEMBER()
    int m_width;

    BIND_LUA_MEMBER()
    int m_height;

    std::string m_text;

    // 这个没标注, 不导出
    int m_internal_counter;
};
