"""
meta_ir.py — 中间表示 (Intermediate Representation)

这是整个系统的核心：一个和输入格式、输出格式都无关的数据结构。
所有前端都产出这个结构，所有后端都消费这个结构。

设计原则:
- 字段要足够描述"一个可绑定的 C++ 类"
- 字段名要清晰、语义无歧义
- 不含任何"输入语法"或"输出语法"的残留
"""

from dataclasses import dataclass, field
from typing import List


@dataclass
class MethodMeta:
    """一个可导出的 C++ 方法"""
    lua_name: str        # Lua 侧名字 (比如 "addItem")
    cpp_name: str        # C++ 侧方法名 (比如 "addItem")
    is_const: bool = False
    is_static: bool = False


@dataclass
class MemberMeta:
    """一个可导出的 C++ 成员变量"""
    lua_name: str        # Lua 侧名字 (比如 "value")
    cpp_name: str        # C++ 侧字段名 (比如 "m_value")
    type_name: str       # C++ 类型 (比如 "int", "float", "std::string")


@dataclass
class ClassMeta:
    """一个完整的 C++ 可绑定类的元数据"""
    cpp_class_name: str                    # C++ 类名 (比如 "MyWidget")
    lua_class_name: str                    # Lua 侧暴露的名字 (通常相同)
    header_include: str                    # 需要 #include 的头文件路径
    methods: List[MethodMeta] = field(default_factory=list)
    members: List[MemberMeta] = field(default_factory=list)

    def __str__(self):
        lines = [f"ClassMeta(cpp_class_name={self.cpp_class_name!r},"]
        lines.append(f"          header_include={self.header_include!r},")
        lines.append(f"          methods=[")
        for m in self.methods:
            lines.append(f"            {m},")
        lines.append(f"          ],")
        lines.append(f"          members=[")
        for m in self.members:
            lines.append(f"            {m},")
        lines.append(f"          ])")
        return "\n".join(lines)
