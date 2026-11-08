/* GPM2 resources for Mac OS 9: memory partition. The icon and BNDL/FREF come from icons_gen.r (generated in CI). */
#include "Types.r"

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    doesActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    128 * 1024 * 1024,                                         /* preferred: guest memory (64 MB) + guest stack (16 MB) + host heap + image */
    100 * 1024 * 1024                                          /* minimum */
};

resource 'vers' (1) {
    0x01, 0x00, release, 0x00, verUS,
    "1.0",
    "1.0, Grand Prix Manager 2"
};
