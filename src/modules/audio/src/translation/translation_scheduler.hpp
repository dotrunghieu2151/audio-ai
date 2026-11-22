#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "translation/translation_model_adapter.hpp"

namespace dth::audio {

// Sentence end in `text` at or after `from`: the position just past the terminal punctuation
// (. ! ? … and the sentence marks of other scripts: 。！？ । ؟ ۔ ። ..., plus closing
// quotes/brackets after it). `text_complete` false: `text` may still grow, so a terminal at its
// very end that could still change is not yet a sentence end ("3." may become "3.5", ".." may
// become "..."). Not sentence ends: decimals, common abbreviations (Mr. Dr. e.g. ...),
// single-letter initials, an ellipsis followed by a lowercase word.
// `japanese_endings`: also a Japanese polite sentence-final form (ます です ました ...) where the
// text is complete after it or what follows cannot continue the sentence.
[[nodiscard]] std::optional<std::size_t> find_sentence_end(std::string_view text, std::size_t from,
                                                           bool text_complete,
                                                           bool japanese_endings = false);

// Where to cut `text` so the first part is at most `limit` bytes: after its last clause mark
// (, ; : 、 ，) within the limit, else after its last whitespace, else at the last codepoint
// boundary. Always > 0 for a non-empty text and limit > 0.
[[nodiscard]] std::size_t find_forced_cut(std::string_view text, std::size_t limit);

// Turns pushed text (TextStream::Text contract: utterance deltas, then the whole utterance) into
// translation units with stable boundaries, and hands out one translation job at a time with the
// earlier units of its paragraph as context. The text analogue of SpeechSegmenter.
//
// Units are committed as early as they are stable: at every sentence end, also inside an
// utterance's deltas. A sentence end is punctuation (find_sentence_end) or, for unpunctuated text,
// a boundary found by Config::find_boundary (the sentence model); the earlier one wins. With
// speaker turns (push_speakers), each speaker change is a boundary too, and a unit waits until
// the turns are known through its end, so no unit holds two speakers' words; a speaker's own
// pause and turns shorter than Config::min_speaker_turn_sec are no change, so they do not split
// sentences. A committed unit never changes, even if the utterance's final text differs
// from its deltas (only the uncommitted rest follows the final). Text an utterance leaves
// mid-sentence is held until the next utterance completes it, or `fragment_hold_sec` passes.
//
// Deterministic: time is passed in (`now`, seconds on any monotonic clock). Single-threaded.
class TranslationScheduler
{
  public:
    // The first sentence boundary in `text` after byte `from` (a byte offset, at a codepoint
    // boundary) that the finder is sure of, or nothing. text[0, from) is left context: the end of
    // the text already committed (the model misjudges a boundary without the sentence's start).
    // Must be deterministic. Offsets <= from and text.size() are ignored: a boundary needs text
    // on both sides.
    using BoundaryFinder =
        std::function<std::optional<std::size_t>(std::string_view text, std::size_t from)>;

    struct Config
    {
        std::size_t max_unit_chars    = 200;
        double      fragment_hold_sec = 0.8;
        double      paragraph_gap_sec = 3.0;
        std::size_t context_units     = 6;
        std::size_t context_tokens    = 1024;
        // find_sentence_end's japanese_endings.
        bool japanese_sentence_endings = false;
        // Boundaries of unpunctuated text; empty: punctuation only.
        BoundaryFinder find_boundary;
        // Diarization. Turns shorter than this, or mostly inside a longer turn of another
        // speaker, are no speaker change: the low-latency diarizer flips to another voice for a
        // few frames, mostly at a speaker's onset.
        double min_speaker_turn_sec = 0.6;
        // A turn's end cuts a unit only where the next reported turn is another speaker's (a
        // speaker's own pause ends a turn too). The next turn is reported once it ends; if it is
        // not reported within this long after the turn end (on the clock of the speaker
        // updates), the speaker kept talking: no cut, sentence boundaries decide.
        double speaker_change_wait_sec = 2.5;
    };

    struct Input
    {
        std::string text;
        double      start_sec = 0.0;
        double      end_sec   = 0.0;
        bool        is_final  = true;
    };

    using TokenCounter = std::function<std::size_t(std::string_view text)>;

    TranslationScheduler(Config config, TokenCounter count_tokens);

    void push(const Input& input, double now);
    // Inputs in order; consecutive deltas are appended before committing, so the sentence model
    // runs once for them (a backlog after a translation) and their times stay apart.
    void push(std::span<const Input> inputs, double now);

    // Speaker turns that ended (diarization), on the clock of Input times: every turn that ended
    // before `complete_until_sec` is reported.
    struct SpeakerTurn
    {
        std::string speaker;
        double      start_sec = 0.0;
        double      end_sec   = 0.0;
    };
    struct SpeakerUpdate
    {
        std::vector<SpeakerTurn> turns;
        double                   complete_until_sec = 0.0;
    };
    void push_speakers(const SpeakerUpdate& update);

    // Commits held text whose wait is over.
    void tick(double now);

    // End of input: commits everything pending.
    void finish();

    // When tick() may next commit something; nothing if no text waits for a deadline.
    [[nodiscard]] std::optional<double> next_deadline() const;

    // Whether next_job() would return a job.
    [[nodiscard]] bool has_job() const noexcept { return !in_flight_ && !queue_.empty(); }

    // The next unit to translate, once the previous job's translation came back (it is context).
    [[nodiscard]] std::optional<TranslationJob> next_job();

    // The translation of the job next_job() last returned.
    void complete(std::uint64_t unit_id, std::string translation);

    // The speaker of the reported turn covering at least half of [begin_sec, end_sec]; empty
    // when none does (the turn is still in progress). A job's speaker is named when next_job()
    // returns it; this names it again later, once its turn may have ended
    // (TranslationJob::heard_*_sec).
    [[nodiscard]] std::string speaker_of(double begin_sec, double end_sec) const;

  private:
    struct Unit
    {
        std::uint64_t id = 0;
        std::string   text;
        double        start_sec = 0.0;
        double        end_sec   = 0.0;
        std::uint64_t paragraph = 0;
        double        heard_from_sec = 0.0;
        double        heard_to_sec   = 0.0;
    };

    // pending_text()[0, end) was heard by `sec` (Input times of the text's deltas and finals).
    struct TimeMark
    {
        std::size_t end = 0;
        double      sec = 0.0;
    };

    struct HistoryEntry
    {
        ContextPair pair;
        std::size_t tokens = 0;
    };

    [[nodiscard]] std::string pending_text() const;
    void                      start_utterance(double start_sec);
    void                      append_delta(const Input& input, bool commit = true);
    void                      end_utterance(const Input& input, double now);
    // Commits complete sentences (and over-long text) from the pending text; with
    // `wait_for_speakers`, only once the speakers are known through a unit's end.
    void commit_ready(bool text_complete, bool wait_for_speakers = true);
    // The first sentence end in `text`: punctuation or a model boundary, whichever comes first.
    [[nodiscard]] std::optional<std::size_t> sentence_end(std::string_view text,
                                                          bool text_complete) const;
    // Commits the first `length` bytes of the pending text as a unit.
    void commit(std::size_t length, double end_sec);
    // Commits all pending text: its sentences, then the rest, split at speaker turns. With
    // `wait_for_speakers`, the rest stays held while the speakers are not known through it.
    void commit_all(bool wait_for_speakers);
    [[nodiscard]] bool waiting_for_speakers() const;
    void               prune_turns();
    struct SpeakerCut
    {
        std::size_t end      = 0;    // where the unit ends in the pending text
        double      turn_sec = 0.0;  // the turn start or end it was cut at
        // false: a turn end whose next turn is not reported yet; whether the speaker changed
        // there is not known, so no unit may extend past it yet.
        bool decided = true;
    };
    [[nodiscard]] std::size_t                pending_size() const;
    void                                     mark_pending_end(double sec);
    [[nodiscard]] std::optional<double>      time_at(std::size_t pos) const;
    [[nodiscard]] std::size_t                text_before(double sec) const;
    [[nodiscard]] std::optional<SpeakerCut>  speaker_boundary(std::size_t text_size) const;
    void start_paragraph(const Unit& unit);
    // Context pairs for the next job: the newest translated units of its paragraph.
    [[nodiscard]] std::vector<ContextPair> context();

    struct OpenUtterance
    {
        std::string text;               // the utterance's deltas so far
        std::size_t committed = 0;      // bytes of `text` already in units
        double      end_sec   = 0.0;
    };

    Config       config_;
    TokenCounter count_tokens_;

    std::string                  held_;  // uncommitted text of ended utterances
    double                       pending_start_sec_ = 0.0;
    double                       held_end_sec_      = 0.0;
    std::optional<double>        hold_deadline_;
    std::optional<OpenUtterance> open_;
    std::optional<double>        last_end_sec_;  // end of the last input utterance

    std::uint64_t    next_unit_id_ = 1;
    std::uint64_t    paragraph_    = 0;
    std::deque<Unit> queue_;
    std::optional<Unit> in_flight_;

    std::vector<TimeMark>      marks_;  // increasing `end` and `sec`
    double                     text_start_sec_ = 0.0;  // when the pending text began to be heard
    std::vector<SpeakerTurn>   speaker_turns_;  // by start
    std::optional<double>      speakers_known_until_;  // none: no diarization, nothing waits

    std::string               boundary_context_;  // end of the committed text of this paragraph

    std::uint64_t             history_paragraph_ = 0;
    std::vector<HistoryEntry> history_;  // context window: translated units of history_paragraph_
};

}  // namespace dth::audio
