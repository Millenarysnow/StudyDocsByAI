// ============================================================================
// Backends/LuaBackend.cs — 生成 C++ Lua 绑定代码
//
// 输入: ClassMeta
// 输出: register_xxx_to_lua.h 文件
//
// 生成的代码假设你已经有一套 C++/Lua 绑定运行时
// (beginRegisterClass / REGISTER_MEMBER / REGISTER_MEMBER_VAR 等模板)。
// ============================================================================
using System.Collections.Generic;
using System.IO;
using System.Text;
using MetaBindingGen.MetaIR;

namespace MetaBindingGen.Backends;

public sealed class LuaBackend : IBackend
{
    public string Name => "lua";

    public void Generate(IReadOnlyList<ClassMeta> metas, string outputDir, string sourceDesc)
    {
        Directory.CreateDirectory(outputDir);

        foreach (var meta in metas)
        {
            var code = GenerateCode(meta, sourceDesc);
            var outFile = Path.Combine(outputDir, $"register_{ToSnakeCase(meta.CppClassName)}_to_lua.h");
            File.WriteAllText(outFile, code);
            System.Console.WriteLine($"  wrote: {outFile}");
        }
    }

    private static string GenerateCode(ClassMeta meta, string sourceDesc)
    {
        var sb = new StringBuilder();

        sb.AppendLine("/*//----------------------------------------------------------------------------------");
        sb.AppendLine("  WARNING: THIS FILE IS AUTO-GENERATED. DO NOT EDIT BY HAND!");
        sb.AppendLine($"  Source: {sourceDesc}");
        sb.AppendLine("*///----------------------------------------------------------------------------------");
        sb.AppendLine("#pragma once");
        sb.AppendLine();
        sb.AppendLine($"#include \"{meta.HeaderInclude}\"");
        sb.AppendLine();
        sb.AppendLine("template<class T> class RegisterCPPClassToLua;");
        sb.AppendLine();
        sb.AppendLine("template<>");
        sb.AppendLine($"class RegisterCPPClassToLua<{meta.CppClassName}> {{");
        sb.AppendLine("public:");
        sb.AppendLine("    static void registerClass(lua_State* L) {");
        sb.AppendLine($"        beginRegisterClass<{meta.CppClassName}>(L, \"{meta.LuaClassName}\");");
        sb.AppendLine();

        // 方法注册
        if (meta.Methods.Count == 0)
        {
            sb.AppendLine("        // (no methods)");
        }
        else
        {
            foreach (var m in meta.Methods)
            {
                sb.AppendLine($"        REGISTER_MEMBER(L, \"{m.LuaName}\", &{meta.CppClassName}::{m.CppName});");
            }
        }

        // 成员变量注册
        if (meta.Members.Count == 0)
        {
            sb.AppendLine("        // (no members)");
        }
        else
        {
            foreach (var m in meta.Members)
            {
                sb.AppendLine($"        REGISTER_MEMBER_VAR(L, \"{m.LuaName}\", &{meta.CppClassName}::{m.CppName});");
            }
        }

        sb.AppendLine($"        endRegisterClass<{meta.CppClassName}>(L, \"{meta.LuaClassName}\");");
        sb.AppendLine("    }");
        sb.AppendLine("};");

        return sb.ToString();
    }

    /// <summary>把 CamelCase 转成 snake_case。比如 "MyWidget" → "my_widget"。</summary>
    private static string ToSnakeCase(string name)
    {
        var sb = new StringBuilder(name.Length + 4);
        for (int i = 0; i < name.Length; i++)
        {
            char c = name[i];
            if (char.IsUpper(c))
            {
                if (i > 0) sb.Append('_');
                sb.Append(char.ToLower(c));
            }
            else
            {
                sb.Append(c);
            }
        }
        return sb.ToString();
    }
}
