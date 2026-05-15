// ============================================================================
// Frontends/IFrontend.cs — 前端抽象接口
//
// 所有前端都要实现这个接口。加新前端 = 实现一个新类, 注册到 Program.cs。
// ============================================================================
using System.Collections.Generic;
using MetaBindingGen.MetaIR;

namespace MetaBindingGen.Frontends;

public interface IFrontend
{
    /// <summary>
    /// 前端的唯一标识, 比如 "xml" / "cpp" / "json"。
    /// 用户用 --frontend xml 指定。
    /// </summary>
    string Name { get; }

    /// <summary>
    /// 扫描一个目录, 把所有能解析的输入文件都转成 ClassMeta。
    /// </summary>
    /// <param name="inputDir">输入目录路径</param>
    /// <returns>解析得到的 ClassMeta 列表</returns>
    IReadOnlyList<ClassMeta> ScanDirectory(string inputDir);
}
