using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

internal static class RecipeRunner
{
    private static string ErrorText(byte[] error)
    {
        var end = Array.IndexOf(error, (byte)0);
        return Encoding.UTF8.GetString(error, 0, end < 0 ? error.Length : end);
    }

    internal static int Run(string[] args)
    {
        if (args.Length != 4 || args[0] != "--recipe")
        {
            Console.Error.WriteLine("Usage: IRSdkSample --recipe <station.json> <recipe.json> <new-output-directory>");
            return 2;
        }

        try
        {
            var libraryDirectory = Environment.GetEnvironmentVariable("IR_SDK_LIB_DIR");
            if (string.IsNullOrWhiteSpace(libraryDirectory))
            {
                Console.Error.WriteLine("Set IR_SDK_LIB_DIR to the platform library directory.");
                return 2;
            }
            IRStation.ConfigureNativeLibraries(Path.GetFullPath(libraryDirectory));

            var stationPath = Path.GetFullPath(args[1]);
            var recipePath = Path.GetFullPath(args[2]);
            var station = JsonNode.Parse(File.ReadAllText(stationPath))?.AsObject()
                ?? throw new InvalidOperationException("Station JSON must contain an object.");
            var acquisition = station["acquisition"]?.AsObject()
                ?? throw new InvalidOperationException("station.acquisition is missing.");
            if (acquisition["backend"]?.GetValue<string>() == "raw_replay")
            {
                var source = acquisition["source"]?.GetValue<string>()
                    ?? throw new InvalidOperationException("station.acquisition.source is missing.");
                acquisition["source"] = Path.GetFullPath(source, Path.GetDirectoryName(stationPath)!);
            }

            var stationJson = station.ToJsonString();
            var recipeJson = File.ReadAllText(recipePath);
            var outputDirectory = Path.GetFullPath(args[3], Path.GetDirectoryName(recipePath)!);
            if (Directory.Exists(outputDirectory) || File.Exists(outputDirectory))
                throw new InvalidOperationException("Output directory already exists.");

            var error = new byte[2048];
            var status = IRStation.CheckRecipeJson(stationJson, recipeJson, error, error.Length);
            if (status != 0)
            {
                Console.Error.WriteLine($"IR_CheckRecipeJson status={status}: {ErrorText(error)}");
                return status;
            }

            status = IRStation.RunRecipeJson(stationJson, recipeJson, outputDirectory, out var run, error, error.Length);
            if (status != 0)
            {
                Console.Error.WriteLine($"IR_RunRecipeJson status={status} stage={run.FailedStage}" +
                    $" step_index={run.FailedStepIndex} completed_frames={run.CompletedFrames}" +
                    $" cleanup_status={run.CleanupStatus} {ErrorText(error)}");
                return status;
            }

            using var result = JsonDocument.Parse(File.ReadAllText(Path.Combine(outputDirectory, "result.json")));
            var runStatus = result.RootElement.GetProperty("run_status").GetString();
            Console.WriteLine($"run_status={runStatus}");
            Console.WriteLine($"completed_frames={result.RootElement.GetProperty("completed_frames").GetInt32()}");
            if (runStatus == "NG") return 20;
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error.Message);
            return 1;
        }
    }
}
