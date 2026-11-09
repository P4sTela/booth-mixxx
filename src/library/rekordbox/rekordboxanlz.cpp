#include "library/rekordbox/rekordboxanlz.h"

#include <mp3guessenc.h>

#include <rekordbox_anlz.h>

#include <QFile>
#include <QTextCodec>
#include <QtDebug>

#include <algorithm>
#include <fstream>

#include "library/rekordbox/rekordboxconstants.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"
#include "util/color/color.h"

namespace {

enum class IDForColor : uint8_t {
    Pink = 1,
    Red,
    Orange,
    Yellow,
    Green,
    Aqua,
    Blue,
    Purple
};

constexpr mixxx::RgbColor kColorForIDPink(0xF870F8);
constexpr mixxx::RgbColor kColorForIDRed(0xF870900);
constexpr mixxx::RgbColor kColorForIDOrange(0xF8A030);
constexpr mixxx::RgbColor kColorForIDYellow(0xF8E331);
constexpr mixxx::RgbColor kColorForIDGreen(0x1EE000);
constexpr mixxx::RgbColor kColorForIDAqua(0x16C0F8);
constexpr mixxx::RgbColor kColorForIDBlue(0x0150F8);
constexpr mixxx::RgbColor kColorForIDPurple(0x9808F8);
constexpr mixxx::RgbColor kColorForIDNoColor(0x0);

struct memory_cue_loop_t {
    mixxx::audio::FramePos startPosition;
    mixxx::audio::FramePos endPosition;
    QString comment;
    mixxx::RgbColor::optional_t color;
};

QString fromUtf16BeString(const std::string& toConvert) {
    // Kaitai uses std::string as single container for all string encodings.
    int length = static_cast<int>(toConvert.length()) - 2; // strip off trailing nullbyte
    return QTextCodec::codecForName("UTF-16BE")->toUnicode(toConvert.data(), length);
}

void setHotCue(TrackPointer track,
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        int id,
        const QString& label,
        mixxx::RgbColor::optional_t color) {
    CuePointer pCue;
    const QList<CuePointer> cuePoints = track->getCuePoints();
    for (const CuePointer& trackCue : cuePoints) {
        if (trackCue->getHotCue() == id) {
            pCue = trackCue;
            break;
        }
    }

    mixxx::CueType type = mixxx::CueType::HotCue;
    if (endPosition.isValid()) {
        type = mixxx::CueType::Loop;
    }

    if (pCue) {
        pCue->setStartAndEndPosition(startPosition, endPosition);
    } else {
        pCue = track->createAndAddCue(
                type,
                id,
                startPosition,
                endPosition);
    }
    pCue->setLabel(label);
    if (color) {
        pCue->setColor(*color);
    }
}

} // namespace

namespace mixxx::rekordbox {

RgbColor colorFromID(int colorID) {
    switch (static_cast<IDForColor>(colorID)) {
    case IDForColor::Pink:
        return kColorForIDPink;
    case IDForColor::Red:
        return kColorForIDRed;
    case IDForColor::Orange:
        return kColorForIDOrange;
    case IDForColor::Yellow:
        return kColorForIDYellow;
    case IDForColor::Green:
        return kColorForIDGreen;
    case IDForColor::Aqua:
        return kColorForIDAqua;
    case IDForColor::Blue:
        return kColorForIDBlue;
    case IDForColor::Purple:
        return kColorForIDPurple;
    }
    return kColorForIDNoColor;
}

int timingOffsetFor(const QString& location) {
    // The following code accounts for timing offsets required to
    // correctly align timing information (cue points, loops, beatgrids)
    // exported from Rekordbox. This is caused by different MP3
    // decoders treating MP3s encoded in a variety of different cases
    // differently. The mp3guessenc library is used to determine which
    // case the MP3 is classified in. See the following PR for more
    // detailed information:
    // https://github.com/mixxxdj/mixxx/pull/2119

    int timingOffset = 0;

    if (location.endsWith(".mp3", Qt::CaseInsensitive)) {
        int timingShiftCase = mp3guessenc_timing_shift_case(location.toStdString().c_str());

        qDebug() << "Timing shift case:" << timingShiftCase << "for MP3 file:" << location;

        switch (timingShiftCase) {
#ifdef __COREAUDIO__
        case EXIT_CODE_CASE_A:
            timingOffset = 12;
            break;
        case EXIT_CODE_CASE_B:
            timingOffset = 13;
            break;
        case EXIT_CODE_CASE_C:
            timingOffset = 26;
            break;
        case EXIT_CODE_CASE_D:
            timingOffset = 50;
            break;
#elif defined(__MAD__)
        case EXIT_CODE_CASE_A:
        case EXIT_CODE_CASE_D:
            timingOffset = 26;
            break;
#elif defined(__FFMPEG__)
        case EXIT_CODE_CASE_D:
            timingOffset = 26;
            break;
#endif
        }
    }

#ifdef __COREAUDIO__
    if (location.toLower().endsWith(".m4a")) {
        timingOffset = 48;
    }
#endif

    return timingOffset;
}

void readAnalyze(TrackPointer track,
        audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& anlzPath) {
    if (!QFile(anlzPath).exists()) {
        return;
    }

    qDebug() << "Rekordbox ANLZ path:" << anlzPath << " for: " << track->getTitle();

    std::ifstream ifs(anlzPath.toStdString(), std::ifstream::binary);
    kaitai::kstream ks(&ifs);

    rekordbox_anlz_t anlz = rekordbox_anlz_t(&ks);

    const double sampleRateKhz = sampleRate / 1000.0;

    QList<memory_cue_loop_t> memoryCuesAndLoops;
    int lastHotCueIndex = 0;

    for (const auto& section : *anlz.sections()) {
        switch (section->fourcc()) {
        case rekordbox_anlz_t::SECTION_TAGS_BEAT_GRID: {
            if (!ignoreCues) {
                break;
            }

            auto* beatGridTag =
                    static_cast<rekordbox_anlz_t::beat_grid_tag_t*>(
                            section->body());

            QVector<mixxx::audio::FramePos> beats;

            for (const auto& beat : *beatGridTag->beats()) {
                int time = static_cast<int>(beat->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                beats << mixxx::audio::FramePos(sampleRateKhz * static_cast<double>(time));
            }

            const auto pBeats = mixxx::Beats::fromBeatPositions(
                    sampleRate,
                    beats,
                    rekordboxconstants::beatsSubversion);
            track->trySetBeats(pBeats);
        } break;
        case rekordbox_anlz_t::SECTION_TAGS_CUES: {
            if (ignoreCues) {
                break;
            }

            auto* cuesTag =
                    static_cast<rekordbox_anlz_t::cue_tag_t*>(
                            section->body());

            for (const auto& cueEntry : *cuesTag->cues()) {
                int time = static_cast<int>(cueEntry->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                const auto position = mixxx::audio::FramePos(
                        sampleRateKhz * static_cast<double>(time));

                switch (cuesTag->type()) {
                case rekordbox_anlz_t::CUE_LIST_TYPE_MEMORY_CUES: {
                    switch (cueEntry->type()) {
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_MEMORY_CUE: {
                        memory_cue_loop_t memoryCue;
                        memoryCue.startPosition = position;
                        memoryCue.endPosition = mixxx::audio::kInvalidFramePos;
                        memoryCue.color = mixxx::RgbColor::nullopt();
                        memoryCuesAndLoops << memoryCue;
                    } break;
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP: {
                        int endTime = static_cast<int>(cueEntry->loop_time()) - timingOffset;
                        // Ensure no offset times are less than 1
                        if (endTime < 1) {
                            endTime = 1;
                        }

                        memory_cue_loop_t loop;
                        loop.startPosition = position;
                        loop.endPosition = mixxx::audio::FramePos(
                                sampleRateKhz * static_cast<double>(endTime));
                        loop.color = mixxx::RgbColor::nullopt();
                        memoryCuesAndLoops << loop;
                    } break;
                    }
                } break;
                case rekordbox_anlz_t::CUE_LIST_TYPE_HOT_CUES: {
                    int hotCueIndex = static_cast<int>(cueEntry->hot_cue() - 1);
                    if (hotCueIndex > lastHotCueIndex) {
                        lastHotCueIndex = hotCueIndex;
                    }
                    setHotCue(
                            track,
                            position,
                            mixxx::audio::kInvalidFramePos,
                            hotCueIndex,
                            QString(),
                            mixxx::RgbColor::nullopt());
                } break;
                }
            }
        } break;
        case rekordbox_anlz_t::SECTION_TAGS_CUES_2: {
            if (ignoreCues) {
                break;
            }

            auto* cuesExtendedTag =
                    static_cast<rekordbox_anlz_t::cue_extended_tag_t*>(
                            section->body());

            for (const auto& cueExtendedEntry : *cuesExtendedTag->cues()) {
                int time = static_cast<int>(cueExtendedEntry->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                const auto position = mixxx::audio::FramePos(
                        sampleRateKhz * static_cast<double>(time));

                switch (cuesExtendedTag->type()) {
                case rekordbox_anlz_t::CUE_LIST_TYPE_MEMORY_CUES: {
                    switch (cueExtendedEntry->type()) {
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_MEMORY_CUE: {
                        memory_cue_loop_t memoryCue;
                        memoryCue.startPosition = position;
                        memoryCue.endPosition = mixxx::audio::kInvalidFramePos;
                        memoryCue.comment = fromUtf16BeString(cueExtendedEntry->comment());
                        memoryCue.color = colorFromID(static_cast<int>(
                                cueExtendedEntry->color_id()));
                        memoryCuesAndLoops << memoryCue;
                    } break;
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP: {
                        int endTime =
                                static_cast<int>(
                                        cueExtendedEntry->loop_time()) -
                                timingOffset;
                        // Ensure no offset times are less than 1
                        if (endTime < 1) {
                            endTime = 1;
                        }

                        memory_cue_loop_t loop;
                        loop.startPosition = position;
                        loop.endPosition = mixxx::audio::FramePos(
                                sampleRateKhz * static_cast<double>(endTime));
                        loop.comment = fromUtf16BeString(cueExtendedEntry->comment());
                        loop.color = colorFromID(static_cast<int>(cueExtendedEntry->color_id()));
                        memoryCuesAndLoops << loop;
                    } break;
                    }
                } break;
                case rekordbox_anlz_t::CUE_LIST_TYPE_HOT_CUES: {
                    int hotCueIndex = static_cast<int>(cueExtendedEntry->hot_cue() - 1);
                    if (hotCueIndex > lastHotCueIndex) {
                        lastHotCueIndex = hotCueIndex;
                    }
                    setHotCue(track,
                            position,
                            mixxx::audio::kInvalidFramePos,
                            hotCueIndex,
                            fromUtf16BeString(cueExtendedEntry->comment()),
                            mixxx::RgbColor(qRgb(
                                    static_cast<int>(
                                            cueExtendedEntry->color_red()),
                                    static_cast<int>(
                                            cueExtendedEntry->color_green()),
                                    static_cast<int>(cueExtendedEntry
                                                    ->color_blue()))));
                } break;
                }
            }
        } break;
        default:
            break;
        }
    }

    if (memoryCuesAndLoops.size() > 0) {
        std::sort(memoryCuesAndLoops.begin(),
                memoryCuesAndLoops.end(),
                [](const memory_cue_loop_t& a, const memory_cue_loop_t& b)
                        -> bool { return a.startPosition < b.startPosition; });

        bool mainCueFound = false;

        // Add memory cues and loops
        for (int memoryCueOrLoopIndex = 0;
                memoryCueOrLoopIndex < memoryCuesAndLoops.size();
                memoryCueOrLoopIndex++) {
            memory_cue_loop_t memoryCueOrLoop = memoryCuesAndLoops[memoryCueOrLoopIndex];

            if (!mainCueFound && !memoryCueOrLoop.endPosition.isValid()) {
                // Set first chronological memory cue as Mixxx MainCue
                track->setMainCuePosition(memoryCueOrLoop.startPosition);
                CuePointer pMainCue = track->findCueByType(mixxx::CueType::MainCue);
                pMainCue->setLabel(memoryCueOrLoop.comment);
                pMainCue->setColor(*memoryCueOrLoop.color);
                mainCueFound = true;
            } else {
                // Mixxx v2.4 will feature multiple loops, so these saved here will be usable
                // For 2.3, Mixxx treats them as hotcues and the first one will be loaded as the single loop Mixxx supports
                lastHotCueIndex++;
                setHotCue(
                        track,
                        memoryCueOrLoop.startPosition,
                        memoryCueOrLoop.endPosition,
                        lastHotCueIndex,
                        memoryCueOrLoop.comment,
                        memoryCueOrLoop.color);
            }
        }
    }
}

} // namespace mixxx::rekordbox
