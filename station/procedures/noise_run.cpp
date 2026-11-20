#include "save_raw.h"
#include "station/acquisition/frame_source.h"
#include "api_common.h"
#include "optmeas_analysis.h"
using namespace optmeas;
using namespace optmeas::api;
namespace {
void checked(int32_t code, const char *message)
{
    if (code != OPTMEAS_OK) throw Error(code,message);
}

}
extern "C" int32_t OptMeas_RunNoise(const OptMeasNoiseRecipe *recipe, const char *output,
    OptMeasNoiseResult *result, char *err, int32_t bytes)
{
    if (result) *result = {};
    return guarded(err,bytes,[&] {
        needed(recipe,1,1); needed(result,1,1);
        const auto &r = *recipe;
        require(r.recipe_id && *r.recipe_id && r.frames >= 2 && r.frame_timeout_ms >= 0,
            OPTMEAS_BAD_ARGUMENT,"Specify recipe ID, at least two frames, and a nonnegative timeout");
        require(!r.blackbody && !r.detector, OPTMEAS_UNSUPPORTED, "Offline noise requires zero instrument handles");
        auto lock = acquisition::lockCapture();
        auto &c = acquisition::capture();
        c.checkOwner();
        require(!c.running,OPTMEAS_BUSY,"Stop the stream before starting a measurement");
        c.cancelled = false;
        const auto path = QString::fromUtf8(output);
        if (output) procedures::checkDestination(path);
        const auto g = c.source->geometry();
        const auto n = pixels(g.height,g.width);
        const auto total = stackSize(n,r.frames);
        std::vector<uint16_t> frames(static_cast<size_t>(total));
        std::vector<OptMeasFrameStamp> stamps(r.frames);
        std::vector<double> means(static_cast<size_t>(n)), stds(static_cast<size_t>(n));
        char detail[1024]{};
        c.checkStop();
        c.start();
        try {
            c.read(r.frames,r.frame_timeout_ms,frames.data(),total,stamps.data(),r.frames,result->completed);
        } catch (...) { c.stop(); throw; }
        c.stop();
        double summary[2]{};
        checked(OptMeas_FrameStatisticsU16(frames.data(),total,r.frames,g.height,g.width,1,means.data(),
            stds.data(),n,summary,2,detail,sizeof detail),detail);
        c.checkStop();
        OptMeasNoiseResult measured{result->completed,summary[0],summary[1]};
        if (output) procedures::saveNoiseRun(path,r,c.backend,g,frames,stamps,measured);
        *result = measured;
    });
}
