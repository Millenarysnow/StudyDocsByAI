/*//----------------------------------------------------------------------------------
  WARNING: THIS FILE IS AUTO-GENERATED. DO NOT EDIT BY HAND!
  Source: frontend=cpp, input=sample_cpp
*///----------------------------------------------------------------------------------
#pragma once

#include "my_widget.h"

template<class T> class RegisterCPPClassToLua;

template<>
class RegisterCPPClassToLua<MyWidget> {
public:
    static void registerClass(lua_State* L) {
        beginRegisterClass<MyWidget>(L, "MyWidget");

        REGISTER_MEMBER(L, "setText", &MyWidget::setText);
        REGISTER_MEMBER(L, "getText", &MyWidget::getText);
        REGISTER_MEMBER(L, "setSize", &MyWidget::setSize);
        REGISTER_MEMBER(L, "getWidth", &MyWidget::getWidth);
        REGISTER_MEMBER(L, "getHeight", &MyWidget::getHeight);
        REGISTER_MEMBER_VAR(L, "width", &MyWidget::m_width);
        REGISTER_MEMBER_VAR(L, "height", &MyWidget::m_height);
        endRegisterClass<MyWidget>(L, "MyWidget");
    }
};
