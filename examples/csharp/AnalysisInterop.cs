using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

internal static class IRAnalysis
{
    internal const int ChipGradeUnknown = 0;
    internal const int ChipGradeA = 1;
    internal const int ChipGradeB = 2;
    internal const int ChipGradeC = 3;
    internal const int ChipGradeD = 4;

    internal const int Noise3DModeOrg = 0;
    internal const int Noise3DModeNuc = 1;
    internal const int Noise3DScalarCount = 16;
    internal const int Noise3DStatsCount = 32;
    internal const int Noise3DHistGroupCount = 4;
    internal const int Noise3DMetaCount = 32;

    internal const int SpatialFilterBoxMean = 0;

    internal const int MapOpSub = 0;
    internal const int MapOpAdd = 1;
    internal const int MapOpMul = 2;
    internal const int MapOpSafeDiv = 3;
    internal const int MapOpVarianceSubtract = 4;

    internal const int ThreshAbsolute = 0;
    internal const int ThreshMeanMultiplier = 1;
    internal const int ThreshMeanSigma = 2;
    internal const int ThreshUpperOnly = 3;
    internal const int ThreshLowerOnly = 4;

    internal const int MaskAnd = 0;
    internal const int MaskOr = 1;
    internal const int MaskAndNot = 2;
    internal const int MaskNot = 3;

    internal const int WindowRectangular = 0;
    internal const int WindowHann = 1;
    internal const int WindowHamming = 2;

    internal const int SpectrumAxisColumns = 0;
    internal const int SpectrumAxisRows = 1;

    internal const int NoiseSubtractNone = 0;
    internal const int NoiseSubtractVarianceQuadrature = 1;

    [StructLayout(LayoutKind.Sequential)]
    internal struct CalcParameters
    {
        public double T0C;
        public double T1C;
        public double PixelPitchUm;
        public double Emissivity;
        public double SystemGain;
        public double AdcSwingV;
        public double FNumber;
        public double VmaxPerCount;
        public int IntegrationTimeUs;
        public int InvalidIntegrationUs;
        public int DigitalFull;
        public int Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct GbCriteria
    {
        public double ResponseUpper;
        public double ResponseLower;
        public double NoiseUpper;
        public double NetdUpper;
        public double ShortBadThreshold;
        public double OpenBadThreshold;
        public int CycleUpper;
        public int Reserved;
    }

    [InlineArray(15)]
    internal struct GbClusterCountGrid
    {
        private int _element0;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct GbBadPixelSummary
    {
        public double TotalBadRatePct;
        public int ResponseBadCount;
        public int ResponseUpperBadCount;
        public int ResponseLowerBadCount;
        public int NoiseBadCount;
        public int NetdBadCount;
        public int TotalBadCount;
        public int IterationCount;
        public int ChipGrade;
        public GbClusterCountGrid ClusterCounts;
        public int Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct GbBadPixelMasks
    {
        public IntPtr ValidMask;
        public IntPtr TotalBadMask;
        public IntPtr ResponseBadMask;
        public IntPtr NoiseBadMask;
        public IntPtr NetdBadMask;
        public IntPtr ShortBadMask;
        public IntPtr OpenBadMask;
        public long PixelCapacity;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct GbSummary
    {
        public double BeforeAvgPixelResponseCount;
        public double BeforeAvgPixelResponseVoltage;
        public double VoltsPerCount;
        public double BeforeAvgSnr;
        public double IrradiancePowerDiff;
        public double BeforeAvgBlackbodyResponse;
        public double BeforeAvgNoiseVoltageV;
        public double BeforeAvgNetdMk;
        public double BeforeAvgDetectivity;
        public double BeforeAvgResponsivity;
        public double BeforeSitf;
        public double AfterResponseMvPerMsK;
        public double AfterAvgResponseVoltageV;
        public double AfterAvgResponsivity;
        public double AfterAvgNepW;
        public double AfterResponseNonuniformityPct;
        public double AfterAvgNoiseVoltageMv;
        public double AfterAvgNetdMk;
        public double AfterAvgDetectivity;
        public int ShortBadPixelCount;
        public int OpenBadPixelCount;
        public GbBadPixelSummary BadPixels;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct GjbCriteria
    {
        public double K1;
        public double K2;
        public double K3;
        public double K4;
        public int CycleUpper;
        public int Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct GjbClusterStats
    {
        public int Count1To6;
        public int Count7To12;
        public int Count13To19;
        public int Count20To35;
        public int CountGt35;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct GjbBadPixelSummary
    {
        public double TotalBadRatePct;
        public int TotalBadCount;
        public int IterationCount;
        public int Center3x3BlockCount;
        public int Filtered4x4BlockCount;
        public GjbClusterStats TotalClusters;
        public GjbClusterStats CenterClusters;
        public GjbClusterStats EdgeClusters;
        public int Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct GjbBadPixelMasks
    {
        public IntPtr ValidMask;
        public IntPtr TotalBadMask;
        public IntPtr K1BadMask;
        public IntPtr K2BadMask;
        public IntPtr K3BadMask;
        public IntPtr K4BadMask;
        public IntPtr BlockFilteredBadMask;
        public long PixelCapacity;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct GjbSummary
    {
        public double BeforeAvgPixelResponseCount;
        public double BeforeAvgPixelResponseVoltage;
        public double VoltsPerCount;
        public double BeforeAvgSnr;
        public double Fov;
        public double IrradiancePowerDiff;
        public double BeforeAvgNoiseVoltageV;
        public double BeforeAvgNetdMk;
        public double BeforeAvgDetectivity;
        public double BeforeAvgResponsivity;
        public double AfterAvgResponseVoltageV;
        public double AfterAvgResponsivity;
        public double AfterAvgNepW;
        public double AfterResponseNonuniformityPct;
        public double AfterAvgNoiseVoltageMv;
        public double AfterAvgNetdMk;
        public double AfterAvgDetectivity;
        public double AfterSpatialNoise;
        public double AfterMaxDynamicRangeDb;
        public double AfterRfpnV;
        public GjbBadPixelSummary BadPixels;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Noise3DCalibParams
    {
        public double ResponseLowerMul;
        public double ResponseUpperMul;
        public double RmsLowerMul;
        public double RmsUpperMul;
        public double KEpsilon;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Noise3DCalibOutputs
    {
        public IntPtr ResponseBlindMask;
        public IntPtr RmsBlindMask;
        public IntPtr TotalBlindMask;
        public IntPtr ValidMask;
        public IntPtr KMap;
        public IntPtr Stats;
        public long PixelCapacity;
        public int StatsCapacity;
        public int Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Noise3DComputeParams
    {
        public int ComputeMode;
        public int ThreadCount;
        public int HistInterval;
        public int Reserved;
        public double BAvg;
        public double InvalidFill;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Noise3DComputeOutputs
    {
        public IntPtr NucFrames;
        public IntPtr Scalars;
        public IntPtr Hist;
        public IntPtr HistMeta;
        public IntPtr MapIJ;
        public IntPtr MapTI;
        public IntPtr MapTJ;
        public IntPtr Meta;
        public long NucFramesCapacity;
        public long HistCapacity;
        public long MapIJCapacity;
        public long MapTICapacity;
        public long MapTJCapacity;
        public int ScalarsCapacity;
        public int HistMetaCapacity;
        public int MetaCapacity;
        public int Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct MapSummary
    {
        public double Mean;
        public double PopStd;
        public double SampleStd;
        public double MinVal;
        public double MaxVal;
        public double Span;
        public double PercentileLow;
        public double PercentileHigh;
        public double PercentileSpan;
        public long ValidCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct GjbDriftContext
    {
        public double VoltsPerCount;
        public double DeltaTK;
        public double RefResponseCount;
        public double RefResponsivity;
        public double VmaxPerCount;
        public double NoiseSubtractFactor;
        public int BurstFrameCount;
        public int NoiseSubtractMode;
        public int LrnuKernelSize;
        public int Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct GjbBurstMetrics
    {
        public double MeanDn;
        public double MeanV;
        public double SpanDn;
        public double SpatialStdDn;
        public double SpatialStdV;
        public double SpatialStdDenoisedDn;
        public double SpatialStdDenoisedV;
        public double NonuniformityPct;
        public double AvgTemporalNoiseDn;
        public double AvgTemporalNoiseMv;
        public double RmsTemporalNoiseDn;
        public double EstimatedNetdMk;
        public double EstimatedNepW;
        public double SpatialNoise;
        public double MaxDynamicRangeDb;
        public double LrnuStdDn;
        public double LrnuSpanDn;
        public double LrnuPct;
        public double HrnuStdDn;
        public double HrnuPct;
        public long ValidPixelCount;
    }

    internal static void VerifyAbiSizes()
    {
        if (Marshal.SizeOf<CalcParameters>() != 80 ||
            Marshal.SizeOf<GbCriteria>() != 56 ||
            Marshal.SizeOf<GbBadPixelSummary>() != 104 ||
            Marshal.SizeOf<GbSummary>() != 264 ||
            Marshal.SizeOf<GjbCriteria>() != 40 ||
            Marshal.SizeOf<GjbClusterStats>() != 20 ||
            Marshal.SizeOf<GjbBadPixelSummary>() != 88 ||
            Marshal.SizeOf<GjbSummary>() != 248 ||
            Marshal.SizeOf<Noise3DCalibParams>() != 40 ||
            Marshal.SizeOf<Noise3DComputeParams>() != 32 ||
            Marshal.SizeOf<MapSummary>() != 80 ||
            Marshal.SizeOf<GjbDriftContext>() != 64 ||
            Marshal.SizeOf<GjbBurstMetrics>() != 168 ||
            (IntPtr.Size == 8 && (
                Marshal.SizeOf<GbBadPixelMasks>() != 64 ||
                Marshal.SizeOf<GjbBadPixelMasks>() != 64 ||
                Marshal.SizeOf<Noise3DCalibOutputs>() != 64 ||
                Marshal.SizeOf<Noise3DComputeOutputs>() != 120)))
        {
            throw new InvalidOperationException("C# IRAnalysis structs do not match IR SDK C ABI layout.");
        }
    }

    [DllImport("IR_analysis", EntryPoint = "IR_AnalysisVersion", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr AnalysisVersionPtr();

    internal static string AnalysisVersion() => Marshal.PtrToStringUTF8(AnalysisVersionPtr()) ?? string.Empty;

    [DllImport("IR_analysis", EntryPoint = "IR_FrameStatisticsU16", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FrameStatisticsU16(
        ushort[] frames, long frameElements, int frameCount, int rows, int cols, int sampleStd,
        double[] meanMap, double[] stdMap, long pixelCapacity, double[] summary, int summaryCount,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_ROIMeanU16", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RoiMeanU16(
        ushort[] frames, long frameElements, int frameCount, int rows, int cols,
        int x, int y, int roiWidth, int roiHeight, out double meanDn,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_GbProcessBadPixels", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GbProcessBadPixels(
        double[] responseMap, double[] noiseMap, double[] netdMap,
        int rows, int cols, long pixelCapacity,
        ref GbCriteria criteria, out GbBadPixelSummary summary, IntPtr masks,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_GbEvaluateMaps", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GbEvaluateMaps(
        double[] v1Map, double[] v2Map, double[] vnMap,
        int rows, int cols, long pixelCapacity,
        ref CalcParameters parameters, ref GbCriteria criteria, out GbSummary summary, IntPtr masks,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_GjbProcessBadPixels", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GjbProcessBadPixels(
        double[] responsivityMap, double[] signalT1VMap, double[] signalT2VMap,
        double[] noiseVMap, double[] netdMkMap,
        int rows, int cols, long pixelCapacity,
        ref GjbCriteria criteria, out GjbBadPixelSummary summary, IntPtr masks,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_GjbEvaluateMaps", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GjbEvaluateMaps(
        double[] v1Map, double[] v2Map, double[] vnMap, double[] vmMap,
        int rows, int cols, long pixelCapacity,
        ref CalcParameters parameters, ref GjbCriteria criteria, out GjbSummary summary, IntPtr masks,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_Noise3DBuildCalibDn", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Noise3DBuildCalibDn(
        double[] meanHighMap, double[] meanLowMap, double[] rmsMap, double[] bMap,
        int rows, int cols, long inputPixelCapacity,
        ref Noise3DCalibParams parameters, ref Noise3DCalibOutputs outputs,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_Noise3DComputeFramesDn", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Noise3DComputeFramesDn(
        ushort[] orgFrames, long frameElements, int frameCount, int rows, int cols,
        byte[] totalBlindMask, long maskCapacity, double[]? kMap, double[]? bMap, long calibMapCapacity,
        ref Noise3DComputeParams parameters, ref Noise3DComputeOutputs outputs,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_BuildTwoPointNucF64", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int BuildTwoPointNucF64(
        double[] v1Map, double[] v2Map, byte[]? validMask, double[]? singlePointRefMap,
        int rows, int cols, long pixelCapacity,
        double[] outKMap, double[] outBMap, double[]? outSummary, int summaryCount,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_ApplyAffineMapF64", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ApplyAffineMapF64(
        double[] inMap, double[] kMap, double[] bMap,
        int rows, int cols, long pixelCapacity, double[] outMap,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_FrameStatisticsNucU16", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int FrameStatisticsNucU16(
        ushort[] frames, long frameElements, int frameCount, int rows, int cols, int sampleStd,
        double[]? kMap, double[]? bMap, double[]? darkMap, byte[]? validMask,
        double[] meanMap, double[] stdMap, long pixelCapacity, double[]? summary, int summaryCount,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_MapSummaryF64", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int MapSummaryF64(
        double[] map, byte[]? validMask, int rows, int cols, long pixelCapacity,
        int roiX, int roiY, int roiW, int roiH, double pLow, double pHigh,
        out MapSummary summary, byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_SpatialLowHighPassF64", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SpatialLowHighPassF64(
        double[] inMap, byte[]? validMask, int rows, int cols, long pixelCapacity,
        int filterKind, int kernelW, int kernelH, double[]? outLowMap, double[]? outHighMap,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_MapBinaryOpF64", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int MapBinaryOpF64(
        double[] aMap, double[] bMap, int rows, int cols, long pixelCapacity,
        int op, double param, double[] outMap, byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_ThresholdMaskF64", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ThresholdMaskF64(
        double[] map, byte[]? baseValidMask, int rows, int cols, long pixelCapacity,
        int mode, double lowParam, double highParam,
        byte[] outPassMask, out int outPassCount, out int outFailCount,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_MaskCombine", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int MaskCombine(
        byte[] maskA, byte[]? maskB, int rows, int cols, long pixelCapacity,
        int op, byte[] outMask, out int outNonzeroCount, byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_ComputeRfft1D", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ComputeRfft1D(
        double[] signal, int length, double sampleRateHz, int windowKind, int detrendMean,
        double[]? outFreqsHz, double[] outAmplitude, double[]? outPsd, int outBinsCapacity,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_ChannelSpectrumU16", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ChannelSpectrumU16(
        ushort[] frames, long frameElements, int frameCount, int rows, int cols,
        int axis, int channelCount, double sampleRateHz, int windowKind,
        double[]? outFreqsHz, int freqCapacity, double[]? outChannelSeries, long seriesCapacity,
        double[] outChannelAmplitudes, long amplitudeCapacity,
        double[]? outTemporalMeans, double[]? outTemporalStds, int channelStatsCapacity,
        byte[] error, int errorBytes);

    [DllImport("IR_analysis", EntryPoint = "IR_GjbEvaluateBurstMetrics", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int GjbEvaluateBurstMetrics(
        double[] meanMap, double[] vnMap, byte[]? validMask, int rows, int cols, long pixelCapacity,
        ref GjbDriftContext ctx, out GjbBurstMetrics metrics, byte[] error, int errorBytes);
}
