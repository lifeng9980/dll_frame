using System.Runtime.InteropServices;
using System.Text;

internal static class IRStation
{
    internal enum DeviceKind
    {
        Pcn7Lt = 2,
        Dcn1000 = 3,
        It6332a = 4,
        OvenGwsSten = 5,
        Dmm6500 = 6,
        DetectorSerial = 7,
        CoolerModbus = 8
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Geometry
    {
        public int Width;
        public int Height;
        public int ValidBits;
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

    [StructLayout(LayoutKind.Sequential)]
    internal struct OvenReading
    {
        public double ActualC;
        public double SetpointC;
        public int Mode;
        public int AlarmActive;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DmmReading
    {
        public double Value;
        public int Mode;
        public int Valid;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PowerReading
    {
        public double SetpointV;
        public double CurrentLimitA;
        public double MeasuredV;
        public double MeasuredA;
        public int OutputEnabled;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PowerCurrentStableSpec
    {
        public double ExpectedVoltageV;
        public double VoltageToleranceV;
        public double MaxCurrentA;
        public double StartupThresholdA;
        public double StableThresholdA;
        public double MaxVariationA;
        public int StartupRequiredSamples;
        public int StartupTimeoutMs;
        public int StableRequiredSamples;
        public int SampleIntervalMs;
        public int TimeoutMs;
        public int Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct DetectorCurrentReading
    {
        public uint RawCode;
        public uint Reserved;
        public double SenseVoltageV;
        public double CurrentMa;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct CoolerReading
    {
        public uint RawValue;
        public int Valid;
        public double VoltageMv;
    }

    [DllImport("IR_station", EntryPoint = "IR_StationVersion", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr StationVersionPtr();

    internal static string StationVersion() => Marshal.PtrToStringUTF8(StationVersionPtr()) ?? string.Empty;

    [DllImport("IR_station", EntryPoint = "IR_OpenFrames", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OpenFrames(int backend, byte[] source, int validBits,
        out Geometry geometry, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_StartFrames", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int StartFrames(byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_ReadFrames", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ReadFrames(int frames, int timeoutMs,
        ushort[] pixels, long pixelCapacity, [Out] FrameStamp[] stamps, int stampCapacity,
        out int completed, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_StopFrames", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int StopFrames(byte[] error, int errorBytes);

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

    [DllImport("IR_station", EntryPoint = "IR_CloseFrames", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int CloseFrames(byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_ListSerialPorts", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ListSerialPorts(byte[]? jsonOut, int capacity,
        out int requiredBytes, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_ListCaptureDevices", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ListCaptureDevices(int backend, byte[]? jsonOut, int capacity,
        out int requiredBytes, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_SetOven", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetOven(ulong handle, double targetC, out double readbackC,
        byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_ReadOven", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ReadOven(ulong handle, out OvenReading reading,
        byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_StopOven", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int StopOven(ulong handle, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_WaitOvenStable", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int WaitOvenStable(ulong handle, double targetC, double toleranceC,
        int stableWindowMs, int timeoutMs, int requireNoAlarm,
        out OvenReading reading, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_SetDmmMode", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetDmmMode(ulong handle, int mode, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_ReadDmm", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ReadDmm(ulong handle, int expectedMode,
        out DmmReading reading, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_SetDetectorTiming", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetDetectorTiming(ulong handle, int integrationUs,
        int frameHz, int emptyUs, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_SetDetectorGpol", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetDetectorGpol(ulong handle, double gpolV,
        byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_SetDetectorRegister", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetDetectorRegister(ulong handle, byte commandCode,
        byte[] payload, int payloadBytes, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_OpenDevice", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OpenDevice(int kind,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string configJson,
        out ulong handle, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_CloseDevice", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int CloseDevice(ulong handle, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_SetBlackbody", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetBlackbody(ulong handle, double targetC, out double readbackC,
        byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_ReadBlackbody", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ReadBlackbody(ulong handle, out BlackbodyReading reading,
        byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_SetPowerChannel", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetPowerChannel(ulong handle, int channel,
        double voltageV, double currentLimitA, out PowerReading readback,
        byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_SetPowerOutput", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SetPowerOutput(ulong handle, int channel, int enabled,
        out PowerReading readback, byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_ReadPower", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ReadPower(ulong handle, int channel, out PowerReading reading,
        byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_WaitPowerCurrentStable", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int WaitPowerCurrentStable(ulong handle, int channel,
        ref PowerCurrentStableSpec spec, out PowerReading reading,
        byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_MeasureDetectorCurrent", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int MeasureDetectorCurrent(ulong handle, int gainCode,
        ushort channelCode, out DetectorCurrentReading reading,
        byte[] error, int errorBytes);

    [DllImport("IR_station", EntryPoint = "IR_ReadCooler", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ReadCooler(ulong handle, out CoolerReading reading,
        byte[] error, int errorBytes);

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
        IntPtr options, out RecipeResult result, byte[] error, int errorBytes);

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

            var gaussIn = new[] { 10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0 };
            var gaussLow = new double[9];
            var gaussHigh = new double[9];
            status = IRAnalysis.SpatialLowHighPassF64(gaussIn, null, 3, 3, 9,
                IRAnalysis.SpatialFilterGaussian, 3, 3, gaussLow, gaussHigh, err, err.Length);
            if (status != 0 || Math.Abs(gaussLow[4] - 50.0) > 1e-12 || Math.Abs(gaussHigh[4]) > 1e-12)
                throw new InvalidOperationException($"IR_SpatialLowHighPassF64(Gaussian) status={status}: {ErrorText(err)}");

            var flickFrames = new ushort[]
            {
                1000, 1000, 1000, 1020,
                1000, 1000, 1000, 1020,
                1000, 1000, 1000, 1020,
                1000, 1000, 1000, 1000
            };
            var flickMask = new byte[4];
            status = IRAnalysis.FlickerDetectU16(flickFrames, 16, 4, 1, 4, null, null, null,
                1.5, 2, out var flickSum, null, flickMask, 4, null, null, 0, err, err.Length);
            if (status != 0 || flickSum.FlickerCount != 1 || flickMask[3] != 1)
                throw new InvalidOperationException($"IR_FlickerDetectU16 status={status}: {ErrorText(err)}");

            var fft2dIn = new double[16];
            for (var r = 0; r < 4; ++r)
            {
                for (var col = 0; col < 4; ++col)
                    fft2dIn[r * 4 + col] = 10.0 + Math.Cos(2.0 * Math.PI * col / 4.0);
            }
            var fft2dMag = new double[16];
            status = IRAnalysis.Fft2dMagnitudeF64(fft2dIn, null, 4, 4, 16,
                IRAnalysis.WindowRectangular, 0, 1, out var fft2dSum, fft2dMag, null, err, err.Length);
            if (status != 0 || Math.Abs(fft2dSum.DcMagnitude - 10.0) > 1e-9 ||
                Math.Abs(fft2dSum.PeakAcMagnitude - 0.5) > 1e-9 ||
                Math.Abs(fft2dSum.TotalAcPower - 0.5) > 1e-9 ||
                Math.Abs(fft2dMag[2 * 4 + 2] - 10.0) > 1e-9)
                throw new InvalidOperationException($"IR_Fft2dMagnitudeF64 status={status}: {ErrorText(err)}");

            Console.WriteLine($"analysis_version={version}");
            Console.WriteLine($"gb_grade={gb.BadPixels.ChipGrade}");
            Console.WriteLine($"gjb_bad_count={gjb.BadPixels.TotalBadCount}");
            Console.WriteLine("fft_ok=1");
            Console.WriteLine("gaussian_ok=1");
            Console.WriteLine("flicker_ok=1");
            Console.WriteLine("fft2d_ok=1");
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
            if (Marshal.SizeOf<IRStation.BlackbodyReading>() != 24 ||
                Marshal.SizeOf<IRStation.OvenReading>() != 24 ||
                Marshal.SizeOf<IRStation.DmmReading>() != 16 ||
                Marshal.SizeOf<IRStation.PowerReading>() != 40 ||
                Marshal.SizeOf<IRStation.PowerCurrentStableSpec>() != 72 ||
                Marshal.SizeOf<IRStation.DetectorCurrentReading>() != 24 ||
                Marshal.SizeOf<IRStation.CoolerReading>() != 16 ||
                Marshal.SizeOf<IRStation.FrameStamp>() != 40 ||
                Marshal.SizeOf<IRStation.CaptureStats>() != 64)
                throw new InvalidOperationException("The C# structures do not match the IR SDK x64 C ABI.");
            IRStation.ConfigureNativeLibraries(Path.GetFullPath(libraryDirectory));
            if (string.IsNullOrEmpty(IRStation.StationVersion()))
                throw new InvalidOperationException("IR_StationVersion returned an empty string.");

            var discErr = new byte[512];
            var discSt = IRStation.ListSerialPorts(null, 0, out var reqSerialBytes, discErr, discErr.Length);
            if (discSt != 3 /* IR_BUFFER_TOO_SMALL */ || reqSerialBytes < 3)
                throw new InvalidOperationException($"IR_ListSerialPorts size query failed status={discSt}: {ErrorText(discErr)}");
            var serialBuf = new byte[reqSerialBytes];
            if (IRStation.ListSerialPorts(serialBuf, serialBuf.Length, out _, discErr, discErr.Length) != 0)
                throw new InvalidOperationException($"IR_ListSerialPorts failed: {ErrorText(discErr)}");
            var serialEnd = Array.IndexOf(serialBuf, (byte)0);
            var serialJson = Encoding.UTF8.GetString(serialBuf, 0, serialEnd < 0 ? serialBuf.Length : serialEnd);
            if (!serialJson.StartsWith("["))
                throw new InvalidOperationException($"Unexpected serial JSON: {serialJson}");

            var devBackend = OperatingSystem.IsWindows() ? 1 /* IR_DIRECTSHOW */ : 3 /* IR_V4L2 */;
            discSt = IRStation.ListCaptureDevices(devBackend, null, 0, out var reqDevBytes, discErr, discErr.Length);
            if (discSt != 3 /* IR_BUFFER_TOO_SMALL */ || reqDevBytes < 3)
                throw new InvalidOperationException($"IR_ListCaptureDevices size query failed status={discSt}: {ErrorText(discErr)}");
            var devBuf = new byte[reqDevBytes];
            if (IRStation.ListCaptureDevices(devBackend, devBuf, devBuf.Length, out _, discErr, discErr.Length) != 0)
                throw new InvalidOperationException($"IR_ListCaptureDevices failed: {ErrorText(discErr)}");
            var devEnd = Array.IndexOf(devBuf, (byte)0);
            var devJson = Encoding.UTF8.GetString(devBuf, 0, devEnd < 0 ? devBuf.Length : devEnd);
            if (!devJson.StartsWith("["))
                throw new InvalidOperationException($"Unexpected capture device JSON: {devJson}");

            var source = Utf8Z(Path.GetFullPath(args[0]));
            var error = new byte[1024];
            var geometry = new IRStation.Geometry();
            var status = IRStation.OpenFrames(2, source, 0, out geometry, error, error.Length);
            if (status != 0)
            {
                Console.Error.WriteLine($"IR_OpenFrames status={status}: {ErrorText(error)}");
                return status;
            }

            try
            {
                status = IRStation.StartFrames(error, error.Length);
                if (status != 0)
                {
                    Console.Error.WriteLine($"IR_StartFrames status={status}: {ErrorText(error)}");
                    return status;
                }
                var pixels = new ushort[geometry.Width * geometry.Height * 2];
                var stamps = new IRStation.FrameStamp[2];
                status = IRStation.ReadFrames(2, 0, pixels, pixels.Length, stamps, stamps.Length, out var completed, error, error.Length);
                if (status != 0)
                {
                    Console.Error.WriteLine($"IR_ReadFrames status={status}: {ErrorText(error)}");
                    return status;
                }
                var pixelCount = (long)geometry.Height * geometry.Width;
                var meanMap = new double[pixelCount];
                var stdMap = new double[pixelCount];
                var summary = new double[2];
                status = IRAnalysis.FrameStatisticsU16(pixels, pixels.LongLength, 2, geometry.Height, geometry.Width, 1,
                    meanMap, stdMap, pixelCount, summary, summary.Length, error, error.Length);
                Console.WriteLine($"width={geometry.Width} height={geometry.Height} valid_bits={geometry.ValidBits}");
                Console.WriteLine($"status={status} completed={completed}");
                if (status == 0)
                {
                    Console.WriteLine($"mean_dn={summary[0]:R}");
                    Console.WriteLine($"temporal_noise_dn={summary[1]:R}");
                    var statsStatus = IRStation.GetCaptureStats(out var stats, error, error.Length);
                    if (statsStatus != 0 || stats.FramesDelivered < 2 || stats.QueueCapacity <= 0)
                        throw new InvalidOperationException($"IR_GetCaptureStats failed status={statsStatus}: {ErrorText(error)}");
                    Console.WriteLine($"frames_delivered={stats.FramesDelivered} queue_capacity={stats.QueueCapacity}");
                }
                _ = IRStation.StopFrames(error, error.Length);
            }
            finally
            {
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
