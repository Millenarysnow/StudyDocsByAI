"""
backend_lua.py — 后端: 生成 C++ Lua 绑定代码

输入: ClassMeta (中间表示)
输出: register_xxx_to_lua.h 文件内容

生成的代码假设你已经有了一套 C++/Lua 绑定模板 (如原理 4 讲过的
beginRegisterClass / REGISTER_MEMBER 宏)。本生成器产出的代码就是
调用这些模板, 和手写绑定代码等价。
"""

from pathlib import Path
from meta_ir import ClassMeta


CODE_TEMPLATE = """\
/*//----------------------------------------------------------------------------------
  WARNING: THIS FILE IS AUTO-GENERATED. DO NOT EDIT BY HAND!
  Source: {source_desc}
*///----------------------------------------------------------------------------------
#pragma once

#include "{header_include}"

template<class T> class RegisterCPPClassToLua;

template<>
class RegisterCPPClassToLua<{cpp_class_name}> {{
public:
    static void registerClass(lua_State* L) {{
        beginRegisterClass<{cpp_class_name}>(L, "{lua_class_name}");

{method_registrations}
{member_registrations}
        endRegisterClass<{cpp_class_name}>(L, "{lua_class_name}");
    }}
}};
"""


def generate_code(meta: ClassMeta, source_desc: str = "<unknown>") -> str:
    """根据 ClassMeta 生成一份完整的绑定代码 (字符串)"""

    # 生成方法注册行
    method_lines = []
    for m in meta.methods:
        line = (f'        REGISTER_MEMBER(L, "{m.lua_name}", '
                f'&{meta.cpp_class_name}::{m.cpp_name});')
        method_lines.append(line)
    method_block = "\n".join(method_lines) if method_lines else "        // (no methods)"

    # 生成成员变量注册行
    member_lines = []
    for m in meta.members:
        line = (f'        REGISTER_MEMBER_VAR(L, "{m.lua_name}", '
                f'&{meta.cpp_class_name}::{m.cpp_name});')
        member_lines.append(line)
    member_block = "\n".join(member_lines) if member_lines else "        // (no members)"

    return CODE_TEMPLATE.format(
        source_desc=source_desc,
        cpp_class_name=meta.cpp_class_name,
        lua_class_name=meta.lua_class_name,
        header_include=meta.header_include,
        method_registrations=method_block,
        member_registrations=member_block,
    )


def write_to_file(meta: ClassMeta, output_dir: Path, source_desc: str = "") -> Path:
    """生成代码并写入文件。返回写入的文件路径。"""
    output_dir.mkdir(parents=True, exist_ok=True)

    # 文件名: register_my_widget_to_lua.h
    # 把 CamelCase 转成 snake_case
    snake_name = "".join(
        "_" + c.lower() if c.isupper() else c
        for c in meta.cpp_class_name
    ).lstrip("_")
    out_file = output_dir / f"register_{snake_name}_to_lua.h"

    code = generate_code(meta, source_desc)
    out_file.write_text(code, encoding="utf-8")
    return out_file
