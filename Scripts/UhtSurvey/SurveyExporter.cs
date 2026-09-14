using System.Text;
using System.Text.Json;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Types;
using EpicGames.UHT.Utils;

namespace LhatSurvey;

// Loaded only by the copied survey manifest, not by normal project builds.
[UnrealHeaderTool]
public static class SurveyExporter
{
    [UhtExporter(Name = "LhatSurvey", Description = "Flat reflection inventory for API size measurement", Options = UhtExporterOptions.None, ModuleName = "Lhat")]
    public static void Export(IUhtExportFactory factory)
    {
        foreach (UhtModule module in factory.Session.Modules)
        {
            var types = new List<object>();
            foreach (UhtPackage package in module.Packages)
                foreach (UhtType type in package.Children)
                    Visit(type, types);
            var document = new { module = module.ShortName, module_type = module.Module.ModuleType.ToString(), types };
            factory.CommitOutput(factory.MakePath(module, ".json"), JsonSerializer.Serialize(document));
        }
    }

    private static Dictionary<string, string> Metadata(UhtType type)
    {
        var values = new Dictionary<string, string>();
        if (type.MetaData.Dictionary != null)
            foreach (var pair in type.MetaData.Dictionary)
                values[type.MetaData.GetKeyString(pair.Key)] = pair.Value;
        return values;
    }

    private static void Visit(UhtType type, List<object> types)
    {
        if (type is UhtClass cls && cls.ClassType != UhtClassType.NativeInterface)
        {
            types.Add(new {
                kind = "class", name = cls.EngineName, cpp_name = cls.SourceName,
                header = Header(cls),
                path = cls.PathName, base_path = cls.SuperClass?.PathName,
                flags = cls.ClassFlags.ToString(), define_scope = cls.DefineScope.ToString(),
                metadata = Metadata(cls),
                functions = cls.Functions.Select(Function).ToArray(),
                properties = cls.Children.OfType<UhtProperty>().Select(Property).ToArray()
            });
        }
        else if (type is UhtScriptStruct str)
        {
            types.Add(new {
                kind = "struct", name = str.EngineName, cpp_name = str.SourceName,
                header = Header(str),
                path = str.PathName, base_path = str.Super?.PathName,
                metadata = Metadata(str), define_scope = str.DefineScope.ToString(),
                properties = str.Children.OfType<UhtProperty>().Select(Property).ToArray()
            });
        }
        else if (type is UhtEnum en)
        {
            types.Add(new {
                kind = "enum", name = en.EngineName, cpp_name = en.FullyQualifiedCppType,
                header = Header(en),
                path = en.PathName, metadata = Metadata(en), define_scope = en.DefineScope.ToString(),
                values = en.EnumValues.Select(v => new { name = v.Name, value = v.Value }).ToArray()
            });
        }
        // Include nested enums/structs without following any back-reference.
        if (type is not UhtFunction && type is not UhtProperty)
            foreach (UhtType child in type.Children)
                Visit(child, types);
    }

    private static object Function(UhtFunction fn) => new {
        name = fn.EngineName, path = fn.PathName, flags = fn.FunctionFlags.ToString(),
        export_flags = fn.FunctionExportFlags.ToString(),
        define_scope = fn.DefineScope.ToString(), metadata = Metadata(fn),
        parameters = fn.Children.OfType<UhtProperty>().Select(Property).ToArray()
    };

    private static object Header(UhtType type) => new {
        file = type.HeaderFile.FilePath,
        include = type.HeaderFile.IncludeFilePath,
        relative = type.HeaderFile.ModuleRelativeFilePath
    };

    private static object Property(UhtProperty prop) => new {
        name = prop.EngineName, cpp_type = prop.AppendText(new StringBuilder(), UhtPropertyTextType.Generic).ToString(),
        property_class = prop.EngineClassName, flags = prop.PropertyFlags.ToString(),
        define_scope = prop.DefineScope.ToString(), array_dimension = prop.ArrayDimensions,
        references = prop.EnumerateReferencedTypes().Select(t => t.PathName).Distinct().ToArray(),
        metadata = Metadata(prop)
    };
}
