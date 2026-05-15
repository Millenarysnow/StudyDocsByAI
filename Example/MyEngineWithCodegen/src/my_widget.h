// src/my_widget.h — 带 BIND_LUA_* 标注的 C++ 类
#pragma once
#include <string>
#include <iostream>

// 这些宏在正常 C++ 编译时展开为空，只是给 MetaBindingGen 扫描工具看的"标记"
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
    void setText(const char* s) {
        m_text = s;
        std::cout << "[C++] MyWidget::setText(\"" << s << "\")" << std::endl;
    }

    BIND_LUA_METHOD()
    const char* getText() const { return m_text.c_str(); }

    BIND_LUA_METHOD()
    void setSize(int w, int h) {
        m_width = w;
        m_height = h;
        std::cout << "[C++] MyWidget::setSize(" << w << ", " << h << ")" << std::endl;
    }

    BIND_LUA_METHOD()
    int getWidth() const { return m_width; }

    BIND_LUA_METHOD()
    int getHeight() const { return m_height; }

    BIND_LUA_METHOD()
    int getArea() const { return m_width * m_height; }

    BIND_LUA_MEMBER()
    int m_width;

    BIND_LUA_MEMBER()
    int m_height;

    std::string m_text;
};
