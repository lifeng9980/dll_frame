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

    [StructLayout(LayoutKind.Sequential)]
    internal struct RecipeResult
    {
        public int CompletedFrames;
        public int FailedStage;
        public int FailedStepIndex;
        public int CleanupStatus;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct BlackbodyReading
    {
        public double ActualC;
        public double SetpointC;
        public int Stable;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct FrameStamp
    {
        public ulong Sequence;
        public ulong DriverSequence;
        public long TimestampNs;
        public uint LostFrames;
        public uint Fields;
        public int TimeOrigin;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct CaptureStats
    {
        public ulong FramesReceived;
        public ulong FramesDelivered;
        public ulong FramesDropped;
        public ulong PreviewFrames;
        public int QueueCapacity;
        public int QueueDepth;
        public int Width;
        public int Height;
        public int Running;
        public int Reserved;
        public double EstimatedFps;
    }

    [DllImport("IR_station", EntryPoint = "IR_StationVersion", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr StationVersionPtr();

    internal static string StationVersion() => Marshal.PtrToStringUTF8(StationVersionPtr()) ?? string.Empty;

    [DllImport("IR_station", EntryPoint = "IR_OpenFrames", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OpenFrames(int backend, byte[] source, int validBits,
        out Geometry geometry, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_FlushFrames", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FlushFrames(int discardFrames, int timeoutMs,
        out int discarded, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_SetFrameQueueCapacity", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetFrameQueueCapacity(int capacity, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_TryReadLatestFrame", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TryReadLatestFrame(
        ushort[]? pixels, long pixelCapacity, out Geometry geometry, out FrameStamp stamp,
        out int hasFrame, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_GetCaptureStats", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GetCaptureStats(out CaptureStats stats, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_WaitBlackbodyStable", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int WaitBlackbodyStable(ulong handle, double targetC, double toleranceC,
        int stableWindowMs, int timeoutMs, int requireDeviceStable,
        out BlackbodyReading reading, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_RunNoise", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RunNoise(ref NoiseRecipe recipe, IntPtr outputDirectory,
        out NoiseResult result, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_CloseFrames", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int CloseFrames(byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_CheckRecipeJson", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int CheckRecipeJson(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string stationJson,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string recipeJson,
        byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_RunRecipeJson", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RunRecipeJson(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string stationJson,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string recipeJson,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string outputDirectory,
        out RecipeResult result, byte[] error, int errorBytes);

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

    private static int RunAnalysisSample()
    {
        try
        {
            var libraryDirectory = Environment.GetEnvironmentVariable("IR_SDK_LIB_DIR");
            if (string.IsNullOrWhiteSpace(libraryDirectory))
            {
                Console.Error.WriteLine("Set IR_SDK_LIB_DIR to the platform library directory.");
                return 2;
            }
            IRAnalysis.VerifyAbiSizes();
            IRStation.ConfigureNativeLibraries(Path.GetFullPath(libraryDirectory));
            var version = IRAnalysis.AnalysisVersion();
            if (string.IsNullOrEmpty(version))
                throw new InvalidOperationException("IR_AnalysisVersion returned an empty string.");

            var err = new byte[1024];
            var t1Frames = new ushort[] { 30196, 30196, 30196, 30196, 30204, 30204, 30204, 30204 };
            var v1Map = new double[4];
            var vnMap = new double[4];
            var statSummary = new double[2];
            var status = IRAnalysis.FrameStatisticsU16(t1Frames, 8, 2, 2, 2, 0,
                v1Map, vnMap, 4, statSummary, 2, err, err.Length);
            if (status != 0)
                throw new InvalidOperationException($"IR_FrameStatisticsU16 status={status}: {ErrorText(err)}");

            var v2Map = new[] { 30000.0, 30000.0, 30000.0, 30000.0 };
            var vmMap = new[] { 30100.0, 30100.0, 30100.0, 30100.0 };
            var calcParams = new IRAnalysis.CalcParameters
            {
                T0C = 20.0,
                T1C = 30.0,
                PixelPitchUm = 15.0,
                Emissivity = 1.0,
                SystemGain = 1.0,
                AdcSwingV = 2.0,
                FNumber = 1.0,
                VmaxPerCount = 2.0,
                IntegrationTimeUs = 1000,
                InvalidIntegrationUs = 0,
                DigitalFull = 65535
            };
            var gbCrit = new IRAnalysis.GbCriteria
            {
                ResponseUpper = 1.5,
                ResponseLower = 0.5,
                NoiseUpper = 1.5,
                NetdUpper = 2.0,
                ShortBadThreshold = 0.0,
                OpenBadThreshold = 0.0,
                CycleUpper = 3
            };
            var gjbCrit = new IRAnalysis.GjbCriteria
            {
                K1 = 1.0,
                K2 = 1.0,
                K3 = 0.5,
                K4 = 0.5,
                CycleUpper = 3
            };

            status = IRAnalysis.GbEvaluateMaps(v1Map, v2Map, vnMap, 2, 2, 4,
                ref calcParams, ref gbCrit, out var gb, IntPtr.Zero, err, err.Length);
            if (status != 0)
                throw new InvalidOperationException($"IR_GbEvaluateMaps status={status}: {ErrorText(err)}");

            status = IRAnalysis.GjbEvaluateMaps(v1Map, v2Map, vnMap, vmMap, 2, 2, 4,
                ref calcParams, ref gjbCrit, out var gjb, IntPtr.Zero, err, err.Length);
            if (status != 0)
                throw new InvalidOperationException($"IR_GjbEvaluateMaps status={status}: {ErrorText(err)}");

            status = IRAnalysis.MapSummaryF64(v1Map, null, 2, 2, 4, 0, 0, 0, 0, 5.0, 95.0,
                out var mapSum, err, err.Length);
            if (status != 0 || mapSum.ValidCount != 4)
                throw new InvalidOperationException($"IR_MapSummaryF64 status={status}: {ErrorText(err)}");

            var sig = new double[8];
            for (var i = 0; i < sig.Length; ++i)
                sig[i] = 2.0 + 3.0 * Math.Cos(2.0 * Math.PI * i / sig.Length);
            var freqs = new double[5];
            var amps = new double[5];
            status = IRAnalysis.ComputeRfft1D(sig, sig.Length, 80.0, IRAnalysis.WindowRectangular, 1,
                freqs, amps, null, freqs.Length, err, err.Length);
            if (status != 0 || Math.Abs(amps[1] - 3.0) > 1e-9)
                throw new InvalidOperationException($"IR_ComputeRfft1D status={status}: {ErrorText(err)}");

            Console.WriteLine($"analysis_version={version}");
            Console.WriteLine($"gb_grade={gb.BadPixels.ChipGrade}");
            Console.WriteLine($"gjb_bad_count={gjb.BadPixels.TotalBadCount}");
            Console.WriteLine("fft_ok=1");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 1;
        }
    }

    private static int Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--recipe")
            return RecipeRunner.Run(args);
        if (args.Length > 0 && args[0] == "--analysis")
            return RunAnalysisSample();

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
                Marshal.SizeOf<IRStation.NoiseResult>() != 32 ||
                Marshal.SizeOf<IRStation.BlackbodyReading>() != 24 ||
                Marshal.SizeOf<IRStation.FrameStamp>() != 40 ||
                Marshal.SizeOf<IRStation.CaptureStats>() != 64)
                throw new InvalidOperationException("The C# structures do not match the IR SDK x64 C ABI.");
            IRStation.ConfigureNativeLibraries(Path.GetFullPath(libraryDirectory));
            if (string.IsNullOrEmpty(IRStation.StationVersion()))
                throw new InvalidOperationException("IR_StationVersion returned an empty string.");

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
