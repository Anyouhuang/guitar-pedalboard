// Standalone test of the Volume/Pan engine (see Harness.h).
#include "Harness.h"
#include "../../Source/DSP/fx/Volume.h"

int main (int argc, char* argv[])
{
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "test_output/standalone/volume";
    return harness::run<fx::VolumeFx> ("volume", fx::volumeModels(), outDir) == 0 ? 0 : 1;
}
