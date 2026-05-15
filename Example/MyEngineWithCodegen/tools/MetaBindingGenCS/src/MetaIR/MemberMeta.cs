// ============================================================================
// MetaIR/MemberMeta.cs — 一个可导出的 C++ 成员变量的描述
// ============================================================================
namespace MetaBindingGen.MetaIR;

public sealed record MemberMeta(
    string LuaName,         // Lua 侧暴露的名字 (比如 "value")
    string CppName,         // C++ 侧字段名 (比如 "m_value")
    string TypeName         // C++ 类型 (比如 "int", "float", "std::string")
)
{
    public override string ToString()
        => $"MemberMeta(LuaName={LuaName}, CppName={CppName}, Type={TypeName})";
}
