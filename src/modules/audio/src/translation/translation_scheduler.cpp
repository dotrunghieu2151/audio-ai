#include "translation/translation_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <utility>

#include "translation/utf8.hpp"

namespace dth::audio {

namespace {

using namespace std::string_view_literals;

// Text times have the granularity of the ASR's output chunks (nemotron: 0.32 s), so a word heard
// just before a speaker turn ended may be stamped up to that much later.
constexpr double turn_end_slack_sec = 0.32;

// Committed text the sentence model sees before the pending text (~30 Japanese characters).
constexpr std::size_t boundary_context_bytes = 96;

// Non-ASCII sentence-ending marks, as UTF-8. Unambiguous: they end the sentence at once.
// CJK and full-width (。！？．｡), typographic (‼⁇⁈⁉), Devanagari/Bengali danda (। ॥), Arabic
// question mark (؟), Urdu full stop (۔), Greek question mark (U+037E, looks like ";"), Armenian
// (։), Ethiopic (። ፧), Khmer (។), Myanmar (။), Tibetan (།).
constexpr std::array sentence_marks = {"。"sv, "！"sv, "？"sv, "．"sv, "｡"sv, "‼"sv, "⁇"sv,
                                       "⁈"sv, "⁉"sv,  "।"sv,  "॥"sv,  "؟"sv, "۔"sv, "\u037E"sv,
                                       "։"sv, "።"sv,  "፧"sv,  "។"sv,  "။"sv, "།"sv};
constexpr std::array closers           = {"\""sv, "'"sv, ")"sv, "]"sv, "”"sv,
                                          "’"sv,  "」"sv, "』"sv, "）"sv};
// Non-ASCII clause marks: CJK, Arabic comma and semicolon, Ethiopic comma and semicolon.
constexpr std::array clause_marks      = {"、"sv, "，"sv, "；"sv, "："sv,
                                          "،"sv, "؛"sv, "፣"sv, "፤"sv};
constexpr std::string_view ellipsis    = "…";
// Japanese polite sentence-final forms (opt-in: find_sentence_end's japanese_endings). The ASR
// often leaves out 。, so one of them ends the sentence when the text ends there or what follows
// cannot continue it (see starts_new_sentence). Continuations are hiragana ("ますが",
// "ですから", "ませんでした").
constexpr std::array sentence_final_forms = {"ます"sv,     "です"sv,   "ました"sv,
                                             "でした"sv,   "ません"sv, "ましょう"sv,
                                             "でしょう"sv, "ください"sv};
// Lowercase words that end with a period without ending the sentence ("Mr. Smith", "e.g. this").
constexpr std::array abbreviations = {"mr"sv, "mrs"sv, "ms"sv, "dr"sv,  "prof"sv, "sr"sv, "jr"sv,
                                      "st"sv, "vs"sv,  "e.g"sv, "i.e"sv, "u.s"sv,  "fig"sv};

constexpr bool is_space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

constexpr bool is_ascii_alpha(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

constexpr bool is_ascii_lower(char c) noexcept
{
    return c >= 'a' && c <= 'z';
}

// Length of the element of `options` that `text` starts with at `pos`, or 0.
template <std::size_t N>
std::size_t match_at(std::string_view text, std::size_t pos,
                     const std::array<std::string_view, N>& options)
{
    for (const std::string_view option : options) {
        if (text.substr(pos).starts_with(option)) {
            return option.size();
        }
    }
    return 0;
}

// Position after any closing quotes/brackets at `pos`.
std::size_t skip_closers(std::string_view text, std::size_t pos)
{
    while (const std::size_t length = match_at(text, pos, closers)) {
        pos += length;
    }
    return pos;
}

// Whether the period at `dot` ends an abbreviation or an initial rather than a sentence.
bool is_abbreviation(std::string_view text, std::size_t dot)
{
    std::size_t begin = dot;
    while (begin > 0 && (is_ascii_alpha(text[begin - 1]) || text[begin - 1] == '.')) {
        --begin;
    }
    const std::string_view word = text.substr(begin, dot - begin);
    if (word.size() == 1) {
        return word != "I";  // an initial: "J. Smith"; but "Neither do I. Then"
    }
    std::string lower{word};
    std::ranges::transform(lower, lower.begin(), [](char c) {
        return is_ascii_alpha(c) ? static_cast<char>(c | 0x20) : c;
    });
    return std::ranges::find(abbreviations, std::string_view{lower}) != abbreviations.end();
}

// Unicode or ASCII ellipsis: a sentence end only before a word that does not start lowercase.
bool ellipsis_ends_sentence(std::string_view text, std::size_t after)
{
    while (after < text.size() && is_space(text[after])) {
        ++after;
    }
    return after == text.size() || !is_ascii_lower(text[after]);
}

// A run of terminal punctuation: "?!", "...", "…".
struct TerminalRun
{
    std::size_t end          = 0;  // past the run
    std::size_t periods      = 0;
    bool        has_ellipsis = false;
};

bool is_terminal(char character) noexcept
{
    return character == '.' || character == '!' || character == '?';
}

// Whether a terminal run at the end of text that may continue could still change: a period
// after a digit ("3." -> "3.5") or periods that may grow into an ellipsis. Abbreviations and
// initials ("Mr.", "J.") are no sentence end anyway.
bool may_still_change(std::string_view text, std::size_t begin, const TerminalRun& run)
{
    if (run.has_ellipsis || run.periods > 1) {
        return true;
    }
    const bool single_period = run.end - begin == 1 && text[begin] == '.';
    return single_period && begin > 0 && std::isdigit(static_cast<unsigned char>(text[begin - 1])) != 0;
}

bool starts_terminal_run(std::string_view text, std::size_t pos)
{
    return is_terminal(text[pos]) || text.substr(pos).starts_with(ellipsis);
}

TerminalRun scan_terminal_run(std::string_view text, std::size_t pos)
{
    TerminalRun run{.end = pos};
    while (run.end < text.size()) {
        if (text.substr(run.end).starts_with(ellipsis)) {
            run.has_ellipsis = true;
            run.end += ellipsis.size();
        }
        else if (is_terminal(text[run.end])) {
            run.periods += text[run.end] == '.' ? 1 : 0;
            ++run.end;
        }
        else {
            break;
        }
    }
    return run;
}

// Whether the run at `begin`, followed by closers up to `end`, ends a sentence.
bool run_ends_sentence(std::string_view text, std::size_t begin, const TerminalRun& run,
                       std::size_t end)
{
    if (end < text.size() && !is_space(text[end])) {
        return false;  // "3.5", "U.S.A", "e.g.,"
    }
    if (run.has_ellipsis || run.periods > 1) {
        return ellipsis_ends_sentence(text, end);
    }
    const bool single_period = run.end - begin == 1 && text[begin] == '.';
    return !(single_period && is_abbreviation(text, begin));
}

// Punctuation and symbol blocks: general punctuation (… ” ’), CJK symbols and punctuation
// (、。「」), full-width ASCII punctuation (！？（）), and the sentence and clause marks of other
// scripts that sit inside letter blocks (see sentence_marks, clause_marks).
constexpr bool is_punctuation(char32_t codepoint) noexcept
{
    constexpr std::array marks = {char32_t{0x037E}, char32_t{0x0589}, char32_t{0x060C},
                                  char32_t{0x061B}, char32_t{0x061F}, char32_t{0x06D4},
                                  char32_t{0x0964}, char32_t{0x0965}, char32_t{0x0F0D},
                                  char32_t{0x104B}, char32_t{0x1361}, char32_t{0x1362},
                                  char32_t{0x1363}, char32_t{0x1364}, char32_t{0x1367},
                                  char32_t{0x17D4}};
    if (std::ranges::find(marks, codepoint) != marks.end()) {
        return true;
    }
    return (codepoint >= 0x2000 && codepoint <= 0x206F) || (codepoint >= 0x3000 && codepoint <= 0x303F) ||
           (codepoint >= 0xFF01 && codepoint <= 0xFF0F) || (codepoint >= 0xFF1A && codepoint <= 0xFF20) ||
           (codepoint >= 0xFF3B && codepoint <= 0xFF40) || (codepoint >= 0xFF5B && codepoint <= 0xFF65);
}

// Whether `text` has anything to translate: an ASCII letter or digit, or a non-ASCII character
// that is not punctuation (a lone "」" or "…" is not worth a model call).
bool has_content(std::string_view text)
{
    for (std::size_t pos = 0; pos < text.size();) {
        const auto [codepoint, length] = utf8::decode(text, pos);
        if (codepoint < 0x80 ? std::isalnum(static_cast<int>(codepoint)) != 0
                             : !is_punctuation(codepoint)) {
            return true;
        }
        pos += length;
    }
    return false;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// `text` trimmed, without the clause marks a cut can leave at its start (", and", "、そして"
// when the ASR starts the next utterance with the previous one's comma).
std::string_view trim_unit(std::string_view text)
{
    for (;;) {
        text = trim(text);
        if (!text.empty() && (text.front() == ',' || text.front() == ';' || text.front() == ':')) {
            text.remove_prefix(1);
        }
        else if (const std::size_t length = match_at(text, 0, clause_marks); length > 0) {
            text.remove_prefix(length);
        }
        else {
            return text;
        }
    }
}

// Whether the text at `pos`, after a Japanese sentence-final form, starts a new sentence:
// whitespace (Japanese has no spaces between words), kanji, katakana, or an ASCII letter or digit.
// Hiragana and punctuation (が、ね、...) may continue the sentence.
bool starts_new_sentence(std::string_view text, std::size_t pos)
{
    const char32_t codepoint = utf8::decode(text, pos).codepoint;
    if (codepoint < 0x80) {
        return is_space(static_cast<char>(codepoint)) ||
               std::isalnum(static_cast<int>(codepoint)) != 0;
    }
    const bool kanji    = (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
                       (codepoint >= 0x3400 && codepoint <= 0x4DBF);
    const bool katakana = codepoint >= 0x30A1 && codepoint <= 0x30FA;
    return kanji || katakana;
}

// End of the sentence that a Japanese sentence-final form at `pos` ends, or nullopt.
std::optional<std::size_t> sentence_final_form_end(std::string_view text, std::size_t pos,
                                                   bool text_complete)
{
    const std::size_t length = match_at(text, pos, sentence_final_forms);
    if (length == 0 || text.substr(0, pos).ends_with("ます")) {  // "ますます" (more and more)
        return std::nullopt;
    }
    const std::size_t end = skip_closers(text, pos + length);
    if (trim(text.substr(end)).empty()) {
        // At the end: complete text ends here; otherwise the sentence may go on.
        return text_complete ? std::optional{end} : std::nullopt;
    }
    return starts_new_sentence(text, end) ? std::optional{end} : std::nullopt;
}

// Separator between the text of two utterances: a space between words of space-separated
// scripts, nothing between CJK text or where whitespace already is.
bool needs_space(std::string_view before, std::string_view after)
{
    if (before.empty() || after.empty()) {
        return false;
    }
    const char last  = before.back();
    const char first = after.front();
    const auto ascii = [](char c) { return static_cast<unsigned char>(c) < 0x80U; };
    return !is_space(last) && !is_space(first) && ascii(last) && ascii(first);
}

}  // namespace

std::optional<std::size_t> find_sentence_end(std::string_view text, std::size_t from,
                                             bool text_complete, bool japanese_endings)
{
    for (std::size_t pos = from; pos < text.size();) {
        if (const std::size_t length = match_at(text, pos, sentence_marks)) {
            return skip_closers(text, pos + length);
        }
        if (japanese_endings) {
            if (const auto end = sentence_final_form_end(text, pos, text_complete)) {
                return end;
            }
        }
        if (!starts_terminal_run(text, pos)) {
            ++pos;
            continue;
        }

        const TerminalRun run = scan_terminal_run(text, pos);
        const std::size_t end = skip_closers(text, run.end);
        if (end == text.size() && !text_complete && may_still_change(text, pos, run)) {
            return std::nullopt;  // the text may continue: "3." -> "3.5"
        }
        if (run_ends_sentence(text, pos, run, end)) {
            return end;
        }
        pos = run.end;
    }
    return std::nullopt;
}

namespace {

// After the last clause mark within `limit` bytes of `text`, else after its last space.
std::optional<std::size_t> find_clause_cut(std::string_view text, std::size_t limit)
{
    limit = std::min(limit, text.size());
    std::size_t clause = 0;
    std::size_t space  = 0;
    for (std::size_t pos = 0; pos < limit;) {
        if (const std::size_t length = match_at(text, pos, clause_marks)) {
            if (pos + length <= limit) {
                clause = pos + length;
            }
            pos += length;
            continue;
        }
        const char c = text[pos];
        if ((c == ',' || c == ';' || c == ':') && pos + 1 < text.size() &&
            is_space(text[pos + 1])) {
            clause = pos + 1;
        }
        else if (is_space(c) && pos > 0) {
            space = pos + 1;
        }
        ++pos;
    }
    if (clause > 0) {
        return clause;
    }
    if (space > 0) {
        return space;
    }
    return std::nullopt;
}

}  // namespace

std::size_t find_forced_cut(std::string_view text, std::size_t limit)
{
    if (const auto cut = find_clause_cut(text, limit)) {
        return *cut;
    }
    limit                      = std::min(limit, text.size());
    const std::size_t boundary = utf8::floor_boundary(text, limit);
    return boundary > 0 ? boundary : std::min(text.size(), utf8::sequence_length(text.front()));
}

namespace {

// The committed unit's last word as the final would repeat it: after its last whitespace, or for
// unspaced text (Japanese, Chinese) its last two codepoints.
std::string_view last_word(std::string_view unit)
{
    if (const std::size_t space = unit.find_last_of(" \t\n"); space != std::string_view::npos) {
        return unit.substr(space + 1);
    }
    std::size_t begin = unit.size();
    for (int codepoints = 0; codepoints < 2 && begin > 0; ++codepoints) {
        begin = utf8::floor_boundary(unit, begin - 1);
    }
    return unit.substr(begin);
}

// Where the uncommitted rest of a final utterance text starts, given the text already committed
// from its deltas. Normally the final starts with the committed text. If the final revised it,
// resume after the committed unit's end as found in the final at or after their common prefix:
// its sentence end if it ended at one, else its last word; failing both, where the final starts
// to differ (at the next space of spaced text). No word is split or repeated, and no text is
// dropped.
std::size_t resume_after(std::string_view final_text, std::string_view committed,
                         bool japanese_endings)
{
    if (final_text.starts_with(committed)) {
        return committed.size();
    }
    const std::size_t common = utf8::floor_boundary(
        final_text, static_cast<std::size_t>(
                        std::ranges::mismatch(final_text, committed).in1 - final_text.begin()));
    const std::string_view committed_unit = trim(committed);
    const bool ended_sentence =
        !committed_unit.empty() &&
        find_sentence_end(committed_unit, 0, true, japanese_endings) == committed_unit.size();
    if (ended_sentence) {
        if (const auto end = find_sentence_end(final_text, common, true, japanese_endings)) {
            return *end;
        }
    }
    // A model or forced boundary, or a sentence end the final no longer has.
    if (const std::string_view tail = last_word(committed_unit); !tail.empty()) {
        const std::size_t from = common > tail.size() ? common - tail.size() : 0;
        if (const std::size_t pos = final_text.find(tail, from); pos != std::string_view::npos) {
            return pos + tail.size();
        }
    }
    const bool        spaced = committed_unit.find_first_of(" \t\n") != std::string_view::npos;
    const std::size_t space  = final_text.find_first_of(" \t\n", common);
    return spaced && space != std::string_view::npos ? space : common;
}

}  // namespace

TranslationScheduler::TranslationScheduler(Config config, TokenCounter count_tokens)
    : config_{config}, count_tokens_{std::move(count_tokens)}
{
}

void TranslationScheduler::push(const Input& input, double now)
{
    push(std::span{&input, 1}, now);
}

void TranslationScheduler::push(std::span<const Input> inputs, double now)
{
    bool appended = false;
    for (const Input& input : inputs) {
        if (input.is_final) {
            if (appended) {
                commit_ready(false);
                appended = false;
            }
            end_utterance(input, now);
        }
        else if (!input.text.empty()) {
            append_delta(input, false);
            appended = true;
        }
    }
    if (appended) {
        commit_ready(false);
    }
}

void TranslationScheduler::push_speakers(const SpeakerUpdate& update)
{
    for (const SpeakerTurn& turn : update.turns) {
        const auto at = std::ranges::upper_bound(speaker_turns_, turn.start_sec, {},
                                                 &SpeakerTurn::start_sec);
        speaker_turns_.insert(at, turn);
    }
    speakers_known_until_ = std::max(speakers_known_until_.value_or(update.complete_until_sec),
                                     update.complete_until_sec);
    prune_turns();
    if (pending_size() > 0) {
        commit_ready(false);
    }
}

// Turns that ended before all text still to be committed or named: no boundary or speaker can
// come from them any more.
void TranslationScheduler::prune_turns()
{
    double oldest = text_start_sec_;
    if (in_flight_) {
        oldest = std::min(oldest, in_flight_->heard_from_sec);
    }
    for (const Unit& unit : queue_) {
        oldest = std::min(oldest, unit.heard_from_sec);
    }
    std::erase_if(speaker_turns_, [oldest](const SpeakerTurn& turn) {
        return turn.end_sec + turn_end_slack_sec < oldest;
    });
}

void TranslationScheduler::tick(double now)
{
    if (!open_ && hold_deadline_ && now >= *hold_deadline_) {
        commit_all(true);
    }
}

void TranslationScheduler::finish()
{
    commit_all(false);
    open_.reset();
}

bool TranslationScheduler::waiting_for_speakers() const
{
    if (!speakers_known_until_ || pending_size() == 0) {
        return false;
    }
    if (time_at(pending_size()).value_or(held_end_sec_) > *speakers_known_until_) {
        return true;
    }
    const auto cut = speaker_boundary(pending_size());
    return cut && !cut->decided;
}

std::optional<double> TranslationScheduler::next_deadline() const
{
    // Held text waiting for speakers is committed by the next speaker update, not the clock.
    return open_ || waiting_for_speakers() ? std::nullopt : hold_deadline_;
}

std::optional<TranslationJob> TranslationScheduler::next_job()
{
    if (in_flight_ || queue_.empty()) {
        return std::nullopt;
    }
    in_flight_ = std::move(queue_.front());
    queue_.pop_front();

    if (in_flight_->paragraph != history_paragraph_) {
        history_.clear();
        history_paragraph_ = in_flight_->paragraph;
    }
    return TranslationJob{.unit_id   = in_flight_->id,
                          .text      = in_flight_->text,
                          .start_sec = in_flight_->start_sec,
                          .end_sec   = in_flight_->end_sec,
                          .context        = context(),
                          .speaker        = speaker_of(in_flight_->heard_from_sec,
                                                       in_flight_->heard_to_sec),
                          .heard_from_sec = in_flight_->heard_from_sec,
                          .heard_to_sec   = in_flight_->heard_to_sec};
}

void TranslationScheduler::complete(std::uint64_t unit_id, std::string translation)
{
    if (!in_flight_ || in_flight_->id != unit_id) {
        throw std::logic_error("TranslationScheduler::complete: not the job in flight");
    }
    if (!trim(translation).empty()) {
        const std::size_t tokens = count_tokens_(in_flight_->text) + count_tokens_(translation);
        history_.push_back(
            {.pair = {.source = std::move(in_flight_->text), .translation = std::move(translation)},
             .tokens = tokens});
    }
    in_flight_.reset();
}

std::string TranslationScheduler::pending_text() const
{
    std::string text = held_;
    if (open_) {
        text += std::string_view{open_->text}.substr(open_->committed);
    }
    return text;
}

void TranslationScheduler::start_utterance(double start_sec)
{
    if (last_end_sec_ && start_sec - *last_end_sec_ >= config_.paragraph_gap_sec) {
        // The held fragment ends with its paragraph. The gap (>= paragraph_gap_sec) is longer
        // than the diarizer's delay, so its turns are known.
        commit_all(false);
        ++paragraph_;
        boundary_context_.clear();
    }
    open_.emplace();
    hold_deadline_.reset();
    if (held_.empty()) {
        pending_start_sec_ = start_sec;
        text_start_sec_    = start_sec;
    }
}

void TranslationScheduler::append_delta(const Input& input, bool commit)
{
    if (!open_) {
        start_utterance(input.start_sec);
    }
    if (open_->text.empty() && needs_space(held_, input.text)) {
        held_ += ' ';
    }
    open_->text += input.text;
    open_->end_sec = input.end_sec;
    mark_pending_end(input.end_sec);
    if (commit) {
        commit_ready(false);
    }
}

void TranslationScheduler::end_utterance(const Input& input, double now)
{
    if (!open_) {
        start_utterance(input.start_sec);
    }
    const std::string before = pending_text();
    if (open_->text.empty()) {
        open_->text = input.text;
    }
    else {
        // The final text replaces the deltas; what is committed stays committed.
        const std::string_view committed = std::string_view{open_->text}.substr(0, open_->committed);
        const std::size_t      rest_begin = resume_after(input.text, committed, config_.japanese_sentence_endings);
        open_->text = std::string{committed} + input.text.substr(rest_begin);
    }
    // The deltas' text may have started with the separator the final leaves out.
    if (open_->committed == 0 && needs_space(held_, open_->text)) {
        held_ += ' ';
    }

    held_ += std::string_view{open_->text}.substr(open_->committed);
    open_.reset();
    held_end_sec_ = input.end_sec;
    last_end_sec_ = input.end_sec;
    // The deltas' time marks hold where the final kept their text.
    const auto common = static_cast<std::size_t>(std::ranges::mismatch(before, held_).in1 - before.begin());
    std::erase_if(marks_, [common](const TimeMark& mark) { return mark.end > common; });
    mark_pending_end(input.end_sec);

    commit_ready(true);
    if (trim(held_).empty()) {
        held_.clear();
        marks_.clear();
    }
    else {
        hold_deadline_ = now + config_.fragment_hold_sec;
    }
}

void TranslationScheduler::commit_ready(bool text_complete, bool wait_for_speakers)
{
    const double end_sec = open_ ? open_->end_sec : held_end_sec_;
    for (;;) {
        const std::string text = pending_text();
        if (text.empty()) {
            return;
        }
        std::size_t cut = 0;
        std::optional<std::size_t> end     = sentence_end(text, text_complete || !open_);
        const auto                 speaker = speaker_boundary(text.size());
        if (speaker && (speaker->decided || !wait_for_speakers) && (!end || speaker->end < *end)) {
            end = speaker->end;
        }
        // A sentence longer than max_unit_chars is cut like any long text: at a clause mark or
        // space. Text without one within the limit (unpunctuated Japanese) runs on to twice the
        // limit for one, or for its sentence end, before it is cut inside a word.
        const std::size_t limit = config_.max_unit_chars;
        if (end && *end <= limit) {
            cut = *end;
        }
        else if (text.size() > limit) {
            if (const auto clause = find_clause_cut(text, limit)) {
                cut = *clause;
            }
            else if (const auto later = find_clause_cut(text, 2 * limit)) {
                cut = *later;
            }
            else if (end && *end <= 2 * limit) {
                cut = *end;
            }
            else if (text.size() > 2 * limit) {
                cut = find_forced_cut(text, 2 * limit);
            }
            else {
                return;
            }
        }
        else {
            return;
        }
        // Whether the speaker changed at a turn end before the unit's end is not known yet.
        if (wait_for_speakers && speaker && !speaker->decided && cut > speaker->end) {
            return;
        }
        // A speaker change before the unit's end may not be reported yet.
        if (wait_for_speakers && speakers_known_until_ &&
            time_at(cut).value_or(end_sec) > *speakers_known_until_) {
            return;
        }
        commit(cut, end_sec);
    }
}

std::optional<std::size_t> TranslationScheduler::sentence_end(std::string_view text,
                                                              bool text_complete) const
{
    std::optional<std::size_t> end =
        find_sentence_end(text, 0, text_complete, config_.japanese_sentence_endings);
    if (config_.find_boundary) {
        const std::size_t                from   = boundary_context_.size();
        const std::string                scored = boundary_context_ + std::string{text};
        const std::optional<std::size_t> model  = config_.find_boundary(scored, from);
        if (model && *model > from && *model < scored.size() && (!end || *model - from < *end)) {
            end = *model - from;
        }
    }
    return end;
}

std::size_t TranslationScheduler::pending_size() const
{
    return held_.size() + (open_ ? open_->text.size() - open_->committed : 0);
}

void TranslationScheduler::mark_pending_end(double sec)
{
    const std::size_t end = pending_size();
    if (!marks_.empty() && marks_.back().end >= end) {
        marks_.back().sec = std::max(marks_.back().sec, sec);
        return;
    }
    // Times never decrease along the text.
    marks_.push_back({.end = end, .sec = marks_.empty() ? sec : std::max(sec, marks_.back().sec)});
}

std::optional<double> TranslationScheduler::time_at(std::size_t pos) const
{
    const auto mark = std::ranges::find_if(marks_, [pos](const TimeMark& m) { return m.end >= pos; });
    return mark == marks_.end() ? std::nullopt : std::optional{mark->sec};
}

std::size_t TranslationScheduler::text_before(double sec) const
{
    std::size_t end = 0;
    for (const TimeMark& mark : marks_) {
        if (mark.sec > sec) {
            break;
        }
        end = mark.end;
    }
    return end;
}

// The first speaker change inside the pending text: the text heard before it ends a unit.
// A change is where consecutive turns (ignoring ones shorter than min_speaker_turn_sec) belong
// to different speakers; the cut lies between the end of the earlier turn's text (stamped up to
// turn_end_slack_sec late) and the start of the later turn. The last turn's end is undecided
// until the next turn is reported or speaker_change_wait_sec passed (then: no change).
// Text times have the ASR's granularity (a delta may end inside a word, "rou" + "te"): a cut
// inside a word of a spaced script moves to the word's end.
std::optional<TranslationScheduler::SpeakerCut>
TranslationScheduler::speaker_boundary(std::size_t text_size) const
{
    if (!speakers_known_until_) {
        return std::nullopt;
    }
    std::optional<SpeakerCut> first;
    const auto consider = [&](double time, double turn_sec, bool decided) {
        const std::size_t end = text_before(time);
        if (end == 0 || end >= text_size) {
            return;
        }
        if (!first || end < first->end) {
            first = SpeakerCut{.end = end, .turn_sec = turn_sec, .decided = decided};
        }
    };
    // A turn mostly inside a longer turn of another speaker is no change either: the diarizer
    // briefly hears a second voice in one speaker's speech.
    const auto inside_longer_turn = [this](const SpeakerTurn& turn) {
        const double length = turn.end_sec - turn.start_sec;
        return std::ranges::any_of(speaker_turns_, [&](const SpeakerTurn& other) {
            const double overlap = std::min(turn.end_sec, other.end_sec) -
                                   std::max(turn.start_sec, other.start_sec);
            return other.speaker != turn.speaker && other.end_sec - other.start_sec > length &&
                   overlap >= length / 2.0;
        });
    };
    const SpeakerTurn* previous = nullptr;
    for (const SpeakerTurn& turn : speaker_turns_) {
        if (turn.end_sec - turn.start_sec < config_.min_speaker_turn_sec || inside_longer_turn(turn)) {
            continue;
        }
        if (previous != nullptr && previous->speaker != turn.speaker) {
            consider((previous->end_sec + turn_end_slack_sec + turn.start_sec) / 2.0,
                     turn.start_sec, true);
        }
        previous = &turn;
    }
    if (previous != nullptr) {
        const double text_end = previous->end_sec + turn_end_slack_sec;
        if (*speakers_known_until_ < text_end + config_.speaker_change_wait_sec) {
            consider(text_end, previous->end_sec, false);
        }
    }
    if (!first) {
        return std::nullopt;
    }
    const std::string text      = pending_text();
    const auto        word_char = [&](std::size_t i) {
        const char c = text[i];
        return is_ascii_alpha(c) || (c >= '0' && c <= '9') || c == '\'';
    };
    while (first->end < text.size() && word_char(first->end - 1) && word_char(first->end)) {
        ++first->end;
    }
    return first->end < text_size ? first : std::nullopt;
}

// The speaker of the turn that overlaps [begin_sec, end_sec] most, if it covers at least half of
// it (text times include VAD padding, so they extend a little past the turns); empty otherwise:
// the text belongs to a turn still in progress.
std::string TranslationScheduler::speaker_of(double begin_sec, double end_sec) const
{
    const SpeakerTurn* best         = nullptr;
    double             best_overlap = std::max(0.0, (end_sec - begin_sec) / 2.0);
    for (const SpeakerTurn& turn : speaker_turns_) {
        const double overlap =
            std::min(end_sec, turn.end_sec) - std::max(begin_sec, turn.start_sec);
        if (overlap > best_overlap) {
            best         = &turn;
            best_overlap = overlap;
        }
    }
    return best != nullptr ? best->speaker : std::string{};
}

void TranslationScheduler::commit(std::size_t length, double end_sec)
{
    const std::string text    = pending_text().substr(0, length);
    const double heard_from = text_start_sec_;
    const double heard_by   = time_at(length).value_or(end_sec);
    // The rest was heard after this unit, or after the turn start or end it was cut at.
    const auto speaker_cut = speaker_boundary(pending_size());
    text_start_sec_ = speaker_cut && speaker_cut->end == length ? speaker_cut->turn_sec : heard_by;
    std::vector<TimeMark> rest;
    for (const TimeMark& mark : marks_) {
        if (mark.end > length) {
            rest.push_back({.end = mark.end - length, .sec = mark.sec});
        }
    }
    marks_ = std::move(rest);

    const std::size_t from_held = std::min(length, held_.size());
    held_.erase(0, from_held);
    if (open_) {
        open_->committed += length - from_held;
    }
    if (held_.empty()) {
        hold_deadline_.reset();
    }

    // The sentence model's left context: the last bytes committed, from a codepoint boundary.
    boundary_context_ += text;
    if (boundary_context_.size() > boundary_context_bytes) {
        std::size_t start = boundary_context_.size() - boundary_context_bytes;
        while (start < boundary_context_.size() && utf8::is_continuation(boundary_context_[start])) {
            ++start;
        }
        boundary_context_.erase(0, start);
    }

    if (const std::string_view unit = trim_unit(text); has_content(unit)) {
        queue_.push_back({.id        = next_unit_id_++,
                          .text      = std::string{unit},
                          .start_sec = pending_start_sec_,
                          .end_sec   = end_sec,
                          .paragraph      = paragraph_,
                          .heard_from_sec = heard_from,
                          .heard_to_sec   = heard_by});
    }
    pending_start_sec_ = end_sec;
}

void TranslationScheduler::commit_all(bool wait_for_speakers)
{
    // Its sentences first, as separate units; with `wait_for_speakers` the rest stays held (its
    // hold expired) until the speakers are known through it.
    commit_ready(true, wait_for_speakers);
    const double end_sec = open_ ? open_->end_sec : held_end_sec_;
    while (const auto boundary = speaker_boundary(pending_size())) {
        if (wait_for_speakers && !boundary->decided) {
            return;
        }
        commit(boundary->end, end_sec);
    }
    if (wait_for_speakers && waiting_for_speakers()) {
        return;
    }
    if (pending_size() > 0) {
        commit(pending_size(), end_sec);
    }
    hold_deadline_.reset();
}

std::vector<ContextPair> TranslationScheduler::context()
{
    const auto window_tokens = [this] {
        std::size_t tokens = 0;
        for (std::size_t i = 0; i < history_.size(); ++i) {
            tokens += history_[i].tokens;
        }
        return tokens;
    };
    // Old pairs are dropped in blocks of half the window, so the prompt prefix (and its KV cache)
    // stays the same for the next units.
    while (!history_.empty() && (history_.size() > config_.context_units ||
                                 window_tokens() > config_.context_tokens)) {
        const auto drop = static_cast<std::ptrdiff_t>(std::max<std::size_t>(1, history_.size() / 2));
        history_.erase(history_.begin(), history_.begin() + drop);
    }

    std::vector<ContextPair> pairs;
    pairs.reserve(history_.size());
    for (std::size_t i = 0; i < history_.size(); ++i) {
        pairs.push_back(history_[i].pair);
    }
    return pairs;
}

}  // namespace dth::audio
