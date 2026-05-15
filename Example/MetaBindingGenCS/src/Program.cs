// ============================================================================
// Program.cs — 命令行入口
//
// 用法:
//   MetaBindingGen.exe --frontend xml --input sample_xml --output generated_xml
//   MetaBindingGen.exe --frontend cpp --input sample_cpp --output generated_cpp
// ============================================================================
using System;
using System.Collections.Generic;
using System.IO;
using MetaBindingGen.Frontends;
using MetaBindingGen.Backends;

namespace MetaBindingGen;

public static class Program
{
    // ========================================================================
    // 注册表: 加新前端/后端时, 在这里登记即可
    // ========================================================================
    private static readonly Dictionary<string, IFrontend> s_frontends = new()
    {
        { "xml", new XmlFrontend() },
        { "cpp", new CppFrontend() },
        // 以后加: { "json", new JsonFrontend() },
    };

    private static readonly Dictionary<string, IBackend> s_backends = new()
    {
        { "lua", new LuaBackend() },
        // 以后加: { "python", new PyBindBackend() },
    };

    public static int Main(string[] args)
    {
        // ------ 解析命令行 ------
        var cli = ParseArgs(args);
        if (cli == null)
        {
            PrintUsage();
            return 1;
        }

        // ------ 验证前端 ------
        if (!s_frontends.TryGetValue(cli.Frontend, out var frontend))
        {
            Console.Error.WriteLine($"ERROR: 未知前端 '{cli.Frontend}'。可用: {string.Join(", ", s_frontends.Keys)}");
            return 1;
        }

        // ------ 验证后端 ------
        if (!s_backends.TryGetValue(cli.Backend, out var backend))
        {
            Console.Error.WriteLine($"ERROR: 未知后端 '{cli.Backend}'。可用: {string.Join(", ", s_backends.Keys)}");
            return 1;
        }

        // ------ 验证输入目录 ------
        if (!Directory.Exists(cli.Input))
        {
            Console.Error.WriteLine($"ERROR: 输入目录不存在: {cli.Input}");
            return 1;
        }

        // ============ 前端: 扫描输入 ============
        Console.WriteLine($"=== Using frontend: {frontend.Name} ===");
        var metas = frontend.ScanDirectory(cli.Input);

        if (metas.Count == 0)
        {
            Console.Error.WriteLine("No classes found!");
            return 1;
        }

        // ============ 打印 IR (调试用) ============
        Console.WriteLine();
        Console.WriteLine("=== Intermediate Representation (IR) ===");
        foreach (var meta in metas)
        {
            Console.WriteLine(meta);
            Console.WriteLine();
        }

        // ============ 后端: 生成代码 ============
        Console.WriteLine($"=== Using backend: {backend.Name}, output: {cli.Output} ===");
        var sourceDesc = $"frontend={frontend.Name}, input={cli.Input}";
        backend.Generate(metas, cli.Output, sourceDesc);

        Console.WriteLine();
        Console.WriteLine($"Done! {metas.Count} class(es) processed.");
        return 0;
    }

    // ------------------------------------------------------------------------
    // 简单的命令行参数解析, 不依赖第三方库
    // ------------------------------------------------------------------------
    private sealed class CliArgs
    {
        public string Frontend { get; set; } = "";
        public string Backend  { get; set; } = "lua";  // 默认 lua
        public string Input    { get; set; } = "";
        public string Output   { get; set; } = "";
    }

    private static CliArgs? ParseArgs(string[] args)
    {
        var cli = new CliArgs();
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--frontend" when i + 1 < args.Length:
                    cli.Frontend = args[++i];
                    break;
                case "--backend" when i + 1 < args.Length:
                    cli.Backend = args[++i];
                    break;
                case "--input" when i + 1 < args.Length:
                    cli.Input = args[++i];
                    break;
                case "--output" when i + 1 < args.Length:
                    cli.Output = args[++i];
                    break;
                case "-h":
                case "--help":
                    return null;
                default:
                    Console.Error.WriteLine($"Unknown arg: {args[i]}");
                    return null;
            }
        }

        if (string.IsNullOrEmpty(cli.Frontend) ||
            string.IsNullOrEmpty(cli.Input) ||
            string.IsNullOrEmpty(cli.Output))
        {
            return null;
        }

        return cli;
    }

    private static void PrintUsage()
    {
        Console.WriteLine("Usage:");
        Console.WriteLine("  MetaBindingGen --frontend <name> --input <dir> --output <dir> [--backend <name>]");
        Console.WriteLine();
        Console.WriteLine("Options:");
        Console.WriteLine("  --frontend   Frontend name. Available: " + string.Join(", ", s_frontends.Keys));
        Console.WriteLine("  --backend    Backend name (default: lua). Available: " + string.Join(", ", s_backends.Keys));
        Console.WriteLine("  --input      Input directory");
        Console.WriteLine("  --output     Output directory");
        Console.WriteLine();
        Console.WriteLine("Examples:");
        Console.WriteLine("  MetaBindingGen --frontend xml --input sample_xml --output generated_xml");
        Console.WriteLine("  MetaBindingGen --frontend cpp --input sample_cpp --output generated_cpp");
    }
}
