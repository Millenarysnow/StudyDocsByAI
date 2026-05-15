// sample_xml/my_widget.h
// 这是方案 A (XML) 风格: C++ 头文件是"纯净"的，没有任何绑定标注。
// 绑定信息在单独的 my_widget.meta 文件里。

#pragma once
#include <string>

class MyWidget {
public:
    MyWidget() : m_width(100), m_height(50) {}

    void setText(const char* s) { m_text = s; }
    const char* getText() const { return m_text.c_str(); }

    void setSize(int w, int h) { m_width = w; m_height = h; }
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

    // 这个不应该被导出 (XML 里没声明)
    void internalDebug() {}

    int m_width;
    int m_height;
    std::string m_text;

    // 这个也不应该被导出
    int m_internal_counter;
};
