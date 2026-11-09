#pragma once

#include <QString>

#include "audio/frame.h"
#include "audio/types.h"
#include "track/track_decl.h"
#include "util/color/rgbcolor.h"

namespace mixxx::rekordbox {

/// rekordbox's colour index (1 = pink … 8 = purple, 0 = none) as a Mixxx colour.
RgbColor colorFromID(int colorID);

/// Decoder timing offset (ms) for the track at `location`. rekordbox timings are written for
/// its own decoder; MP3 and M4A files need this when read through another one.
int timingOffsetFor(const QString& location);

/// Read the beat grid and cues from one ANLZ file and apply them to `track`.
///
/// `ignoreCues` is used for the legacy `.DAT` file: its beat grid is the one to trust, while its
/// cues are superseded by the `.EXT` file.
void readAnalyze(TrackPointer track,
        audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& anlzPath);

} // namespace mixxx::rekordbox
