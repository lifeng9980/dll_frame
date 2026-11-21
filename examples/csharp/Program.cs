using System.Runtime.InteropServices;
using System.Text;

internal static class IRStation
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct Geometry
    {
        public int Width;
        public int Height;
        public int ValidBits;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NoiseRecipe
    {
        public IntPtr RecipeId;
        public ulong Detector;
        public ulong Blackbody;
        public int IntegrationUs;
        public int FrameHz;
        public int EmptyUs;
        public double TemperatureC;
        public double ToleranceC;
        public int StableWindowMs;
        public int StableTimeoutMs;
        public int RequireDeviceStable;
        public int Frames;
        public int FrameTimeoutMs;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NoiseResult
    {
        public int Completed;
        public double MeanDn;
        public double TemporalNoiseDn;
        public int FailedStep;
        public int CleanupStatus;
    }

    [DllImport("IR_station", EntryPoint = "IR_OpenFrames", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OpenFrames(int backend, byte[] source, int validBits,
        out Geometry geometry, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_RunNoise", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RunNoise(ref NoiseRecipe recipe, IntPtr outputDirectory,
        out NoiseResult result, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_CloseFrames", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int CloseFrames(byte[] error, int errorBytes);

    internal static void ConfigureNativeLibraries(string directory)
    {
        NativeLibrary.SetDllImportResolver(typeof(IRStation).Assembly, (name, _, _) =>
        {
            if (name is not ("IR_station" or "IR_analysis"))
                return IntPtr.Zero;

            var fileName = OperatingSystem.IsWindows() ? $"{name}.dll" : $"lib{name}.so";
            return NativeLibrary.Load(Path.Combine(directory, fileName));
        });
    }
}

internal static class Program
{
    private static byte[] Utf8Z(string value) => Encoding.UTF8.GetBytes(value + '\0');

    private static string ErrorText(byte[] error)
    {
        var end = Array.IndexOf(error, (byte)0);
        return Encoding.UTF8.GetString(error, 0, end < 0 ? error.Length : end);
    }

    private static int Main(string[] args)
    {
        if (args.Length is < 1 or > 2)
        {
            Console.Error.WriteLine("Usage: IRSdkSample <raw16-directory> [new-output-directory]");
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
            if (Marshal.SizeOf<IRStation.NoiseRecipe>() != 80 ||
                Marshal.SizeOf<IRStation.NoiseResult>() != 32)
                throw new InvalidOperationException("The C# structures do not match the IR SDK x64 C ABI.");
            IRStation.ConfigureNativeLibraries(Path.GetFullPath(libraryDirectory));

            var source = Utf8Z(Path.GetFullPath(args[0]));
            var outputText = args.Length == 2 ? Path.GetFullPath(args[1]) : null;
            var output = outputText is null ? IntPtr.Zero :
                Marshal.StringToCoTaskMemUTF8(outputText);
            var error = new byte[1024];
            var geometry = new IRStation.Geometry();
            var status = IRStation.OpenFrames(2, source, 0, out geometry, error, error.Length);
            if (status != 0)
            {
                Console.Error.WriteLine($"IR_OpenFrames status={status}: {ErrorText(error)}");
                if (output != IntPtr.Zero) Marshal.FreeCoTaskMem(output);
                return status;
            }

            var recipeId = Marshal.StringToCoTaskMemUTF8("csharp-offline-noise-example-1");
            try
            {
                var recipe = new IRStation.NoiseRecipe
                {
                    RecipeId = recipeId,
                    Frames = 2,
                    FrameTimeoutMs = 0
                };
                status = IRStation.RunNoise(ref recipe, output, out var result, error, error.Length);
                Console.WriteLine($"width={geometry.Width} height={geometry.Height} valid_bits={geometry.ValidBits}");
                Console.WriteLine($"status={status} completed={result.Completed} failed_step={result.FailedStep} cleanup_status={result.CleanupStatus}");
                if (status == 0)
                    Console.WriteLine($"mean_dn={result.MeanDn:R}\ntemporal_noise_dn={result.TemporalNoiseDn:R}");
                else
                    Console.Error.WriteLine($"IR_RunNoise: {ErrorText(error)}");
            }
            finally
            {
                Marshal.FreeCoTaskMem(recipeId);
                if (output != IntPtr.Zero) Marshal.FreeCoTaskMem(output);
                var closeStatus = IRStation.CloseFrames(error, error.Length);
                if (status == 0 && closeStatus != 0)
                {
                    Console.Error.WriteLine($"IR_CloseFrames status={closeStatus}: {ErrorText(error)}");
                    status = closeStatus;
                }
            }
            return status;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error.Message);
            return 1;
        }
    }
}
