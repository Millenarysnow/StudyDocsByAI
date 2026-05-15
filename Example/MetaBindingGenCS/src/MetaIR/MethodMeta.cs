// ============================================================================
// MetaIR/MethodMeta.cs — 一个可导出的 C++ 方法的描述
// ============================================================================
namespace MetaBindingGen.MetaIR;

public sealed record MethodMeta(
    string LuaName,         // Lua 侧暴露的名字 (比如 "setValue")
    string CppName,         // C++ 侧方法名 (比如 "setValue")
    bool   IsConst = false,
    bool   IsStatic = false
)
{
    public override string ToString()
        => $"MethodMeta(LuaName={LuaName}, CppName={CppName}, Const={IsConst}, Static={IsStatic})";
}
