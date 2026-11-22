// Real-time latency of the translation layer.
//
//   translation_latency --model <translate.gguf> [--transcript transcription.txt] [--to ja]
//                       [--from auto] [--speeds 1,2,4] [--gpu-layers -1] [--threads 4]
//                       [--asr-load <asr.gguf> <family>]
//
// 1. Engine: per unit length (5/15/30/60 words), time to first translated text and to the whole
//    translation, on a cold context and on a warm one (context pairs already in the KV cache).
// 2. Stream replay: a transcript ("[start - end] text" or "[start] text" lines) is pushed into a
//    TextStream with its original timing, scaled by each speed, as finals only and as
//    transcriber-style word deltas. Latency of a unit = callback time - wall time of the push that
//    completed it. The stream keeps up iff latency does not grow over the replay.
// --asr-load runs an AudioTranscriber on noise in a loop meanwhile (GPU/CPU contention).
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dth/audio/audio_transcriber.hpp"
#include "dth/audio/audio_translator.hpp"
#include "translation/llama_support.hpp"
#include "translation/translation_engine.hpp"

namespace {

using dth::audio::AudioTranslator;
using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point from, Clock::time_point to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

struct Stats
{
    double p50 = 0.0;
    double p95 = 0.0;
    double max = 0.0;
};

Stats stats(std::vector<double> values)
{
    if (values.empty()) {
        return {};
    }
    std::ranges::sort(values);
    const auto at = [&](double q) {
        return values[static_cast<std::size_t>(std::lround(q * static_cast<double>(values.size() - 1)))];
    };
    return {.p50 = at(0.5), .p95 = at(0.95), .max = values.back()};
}

struct Options
{
    std::string                model;
    std::string                transcript = "transcription.txt";
    AudioTranslator::Languages languages{.source = "auto", .target = "ja"};
    std::vector<double>        speeds{1.0, 2.0, 4.0};
    int                        gpu_layers = -1;
    int                        threads    = 4;
    std::optional<std::pair<std::string, std::string>> asr_load;  // model, family
};

Options parse(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg  = argv[i];
        const auto        next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + arg);
            }
            return argv[++i];
        };
        if (arg == "--model") {
            options.model = next();
        }
        else if (arg == "--transcript") {
            options.transcript = next();
        }
        else if (arg == "--to") {
            options.languages.target = next();
        }
        else if (arg == "--from") {
            options.languages.source = next();
        }
        else if (arg == "--gpu-layers") {
            options.gpu_layers = std::stoi(next());
        }
        else if (arg == "--threads") {
            options.threads = std::stoi(next());
        }
        else if (arg == "--speeds") {
            options.speeds.clear();
            std::stringstream list{next()};
            for (std::string item; std::getline(list, item, ',');) {
                options.speeds.push_back(std::stod(item));
            }
        }
        else if (arg == "--asr-load") {
            const std::string model = next();
            options.asr_load        = {model, next()};
        }
        else {
            throw std::invalid_argument("unknown argument " + arg);
        }
    }
    if (options.model.empty()) {
        throw std::invalid_argument("--model is required");
    }
    return options;
}

// --- 1. Engine ----------------------------------------------------------------------------------

constexpr std::string_view sample_text =
    "I studied computer science in college and I have been working as a developer for several "
    "years so naturally a lot of the content on the channel has been an extension of whatever I "
    "learned about and you are also interested in editors terminal based tools and so on now as "
    "most of you are aware the coding landscape has changed pretty dramatically the way tools are "
    "used is changing and along with that my interests have been evolving as well I have had to "
    "think about what I really want to focus on moving forward";

std::string first_words(std::size_t count)
{
    std::stringstream words{std::string{sample_text}};
    std::string       out;
    std::string       word;
    for (std::size_t i = 0; i < count && words >> word; ++i) {
        out += (i == 0 ? "" : " ") + word;
    }
    return out + ".";
}

struct EngineRun
{
    double first_ms = 0.0;
    double total_ms = 0.0;
    double tokens   = 0.0;
};

EngineRun time_translation(dth::audio::TranslationEngine& engine, const dth::audio::TranslationJob& job,
                           const AudioTranslator::Languages& languages)
{
    const auto                       start = Clock::now();
    std::optional<Clock::time_point> first;
    const std::string translation = engine.translate(job, languages, [&](std::string_view) {
        if (!first) {
            first = Clock::now();
        }
    });
    const auto end = Clock::now();
    return {.first_ms = first ? ms_since(start, *first) : ms_since(start, end),
            .total_ms = ms_since(start, end),
            .tokens   = static_cast<double>(engine.count_tokens(translation))};
}

void bench_engine(const Options& options)
{
    auto model = dth::audio::llama::load_model(options.model, options.gpu_layers);
    const auto shared = std::make_shared<const dth::audio::TranslationModel>(
        dth::audio::TranslationModel{
            .model   = std::move(model),
            .adapter = dth::audio::make_translation_model_adapter("plamo_translate")});
    const dth::audio::TranslationEngine::Settings settings{.threads = options.threads};

    std::vector<dth::audio::ContextPair> context;
    {
        dth::audio::TranslationEngine warmup{shared, settings};
        for (const std::size_t words : std::array<std::size_t, 4>{6, 9, 12, 8}) {
            const std::string source = first_words(words);
            context.push_back(
                {.source = source,
                 .translation = warmup.translate({.text = source}, options.languages, [](auto) {})});
        }
    }

    std::printf("\n== Engine (gpu_layers %d, threads %d, %s -> %s), median of 3\n", options.gpu_layers,
                options.threads, options.languages.source.c_str(), options.languages.target.c_str());
    std::printf("%-6s %-5s %10s %10s %8s %8s\n", "words", "cache", "first_ms", "total_ms", "out_tok",
                "tok/s");
    for (const std::size_t words : std::array<std::size_t, 4>{5, 15, 30, 60}) {
        const dth::audio::TranslationJob job{.text = first_words(words), .context = context};
        for (const bool warm : {false, true}) {
            std::vector<double> first;
            std::vector<double> total;
            double              tokens = 0.0;
            for (int repeat = 0; repeat < 3; ++repeat) {
                dth::audio::TranslationEngine engine{shared, settings};
                if (warm) {
                    // Same context, other input: the prompt prefix is cached.
                    (void)engine.translate({.text = "Hello.", .context = context}, options.languages,
                                           [](auto) {});
                }
                const EngineRun run = time_translation(engine, job, options.languages);
                first.push_back(run.first_ms);
                total.push_back(run.total_ms);
                tokens = run.tokens;
            }
            const double first_ms = stats(first).p50;
            const double total_ms = stats(total).p50;
            const double decode_s = (total_ms - first_ms) / 1000.0;
            std::printf("%-6zu %-5s %10.0f %10.0f %8.0f %8.1f\n", words, warm ? "warm" : "cold",
                        first_ms, total_ms, tokens,
                        decode_s > 0.0 ? (tokens - 1.0) / decode_s : 0.0);
        }
    }
}

// --- 2. Stream replay ---------------------------------------------------------------------------

struct Utterance
{
    double      start_sec = 0.0;
    double      end_sec   = 0.0;
    std::string text;
};

std::vector<Utterance> read_transcript(const std::string& path)
{
    std::ifstream file{path};
    if (!file) {
        throw std::runtime_error("cannot read " + path);
    }
    std::vector<Utterance> utterances;
    for (std::string line; std::getline(file, line);) {
        // "[start - end] text" or "[start] text"
        const std::size_t close = line.find(']');
        if (line.empty() || line[0] != '[' || close == std::string::npos) {
            continue;
        }
        const std::string times = line.substr(1, close - 1);
        const std::size_t dash  = times.find(" - ");
        Utterance         utterance{.start_sec = std::stod(times.substr(0, dash)),
                                    .text      = line.substr(close + 1)};
        utterance.end_sec = dash == std::string::npos ? -1.0 : std::stod(times.substr(dash + 3));
        if (!utterance.text.empty() && utterance.text.front() == ' ') {
            utterance.text.erase(0, 1);
        }
        utterances.push_back(std::move(utterance));
    }
    for (std::size_t i = 0; i < utterances.size(); ++i) {
        if (utterances[i].end_sec < 0.0) {
            utterances[i].end_sec = i + 1 < utterances.size() ? utterances[i + 1].start_sec
                                                               : utterances[i].start_sec + 10.0;
        }
    }
    return utterances;
}

struct Push
{
    double                 at_sec = 0.0;  // transcript time
    dth::audio::TextStream::Text text;
};

struct UnitTiming
{
    double                           end_sec = 0.0;
    std::optional<Clock::time_point> first;
    std::optional<Clock::time_point> final;
};

struct Recorder
{
    std::mutex                              mutex;
    std::map<std::uint64_t, UnitTiming>     units;

    static void callback(void* user_data, const AudioTranslator::TranslatedSegment& segment)
    {
        auto&                  self = *static_cast<Recorder*>(user_data);
        const auto             now  = Clock::now();
        const std::scoped_lock lock{self.mutex};
        UnitTiming&            unit = self.units[segment.unit_id];
        unit.end_sec                = segment.end_sec;
        if (!unit.first) {
            unit.first = now;
        }
        if (segment.is_final) {
            unit.final = now;
        }
    }
};

void bench_stream(AudioTranslator& translator, const Options& options,
                  const std::vector<Utterance>& utterances, double speed, bool deltas)
{
    // Words of delta pushes need stable storage for their string views.
    std::vector<std::string> storage;
    std::vector<Push>        pushes;
    for (const Utterance& utterance : utterances) {
        if (deltas) {
            std::stringstream        stream{utterance.text};
            std::vector<std::string> words;
            for (std::string word; stream >> word;) {
                words.push_back(" " + word);
            }
            const double step = (utterance.end_sec - utterance.start_sec) /
                                static_cast<double>(std::max<std::size_t>(1, words.size()));
            for (std::size_t i = 0; i < words.size(); ++i) {
                storage.push_back(words[i]);
                const double at = utterance.start_sec + (step * static_cast<double>(i + 1));
                pushes.push_back({.at_sec = at,
                                  .text   = {.start_sec = utterance.start_sec,
                                             .end_sec   = at,
                                             .is_final  = false}});
            }
        }
        pushes.push_back({.at_sec = utterance.end_sec,
                          .text   = {.text      = utterance.text,
                                     .start_sec = utterance.start_sec,
                                     .end_sec   = utterance.end_sec,
                                     .is_final  = true}});
    }
    // Bind the delta views now that `storage` no longer reallocates.
    std::size_t word = 0;
    for (Push& push : pushes) {
        if (!push.text.is_final) {
            push.text.text = storage[word++];
        }
    }

    Recorder recorder;
    auto     stream = translator.make_stream({.languages = options.languages}, &Recorder::callback,
                                             &recorder);
    std::map<double, Clock::time_point> pushed_at;  // input end_sec -> wall time of its push
    const double origin = pushes.empty() ? 0.0 : pushes.front().at_sec;
    const auto   start  = Clock::now();
    for (const Push& push : pushes) {
        std::this_thread::sleep_until(
            start + std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>((push.at_sec - origin) / speed)));
        pushed_at[push.text.end_sec] = Clock::now();
        if (auto result = stream.push(push.text); !result) {
            std::printf("push failed: %s\n", result.error().message.c_str());
            return;
        }
    }
    const auto input_done = Clock::now();
    if (auto result = stream.stop(); !result) {
        std::printf("stop failed: %s\n", result.error().message.c_str());
        return;
    }

    std::vector<double> first;
    std::vector<double> final;
    for (const auto& [id, unit] : recorder.units) {
        const auto it   = pushed_at.find(unit.end_sec);
        const auto from = it != pushed_at.end() ? it->second : input_done;
        first.push_back(ms_since(from, unit.first.value_or(from)));
        final.push_back(ms_since(from, unit.final.value_or(from)));
    }
    // Lag trend: mean final latency of the last quarter of units minus the first quarter's.
    const std::size_t quarter = std::max<std::size_t>(1, final.size() / 4);
    const auto        mean    = [](std::span<const double> values) {
        double sum = 0.0;
        for (const double value : values) {
            sum += value;
        }
        return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
    };
    const double trend = final.size() < 2 ? 0.0
                                          : mean(std::span{final}.last(quarter)) -
                                                mean(std::span{final}.first(quarter));
    const Stats first_stats = stats(first);
    const Stats final_stats = stats(final);
    std::printf("%-6.1f %-7s %6zu %9.0f %9.0f %9.0f %9.0f %9.0f %10.0f\n", speed,
                deltas ? "deltas" : "finals", final.size(), first_stats.p50, first_stats.p95,
                final_stats.p50, final_stats.p95, final_stats.max, trend);
}

// --- ASR load -----------------------------------------------------------------------------------

class AsrLoad
{
  public:
    AsrLoad(const std::string& model, const std::string& family)
        : transcriber_{dth::audio::AudioTranscriber::Config{
              .model_path = model,
              .model      = family,
              .backend    = dth::audio::AudioTranscriber::Config::Backend::Metal,
              .threads    = 4}}
    {
        std::mt19937                          random{42};
        std::normal_distribution<float>       noise{0.0F, 0.05F};
        audio_.resize(std::size_t{16000} * 10);
        for (float& sample : audio_) {
            sample = noise(random);
        }
        thread_ = std::jthread{[this](const std::stop_token& stop) {
            while (!stop.stop_requested()) {
                (void)transcriber_.transcribe(audio_, 16000, 1, static_cast<int>(audio_.size()));
                runs_.fetch_add(1);
            }
        }};
    }

    [[nodiscard]] int runs() const { return runs_.load(); }

  private:
    dth::audio::AudioTranscriber transcriber_;
    std::vector<float>           audio_;
    std::atomic<int>             runs_{0};
    std::jthread                 thread_;
};

}  // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parse(argc, argv);

        std::optional<AsrLoad> load;
        if (options.asr_load) {
            load.emplace(options.asr_load->first, options.asr_load->second);
            std::printf("ASR load: %s (%s) transcribing noise in a loop\n",
                        options.asr_load->first.c_str(), options.asr_load->second.c_str());
        }

        bench_engine(options);

        AudioTranslator translator({.model_path     = options.model,
                                    .gpu_layers     = options.gpu_layers,
                                    .threads        = options.threads});
        const auto      utterances = read_transcript(options.transcript);
        std::printf("\n== Stream replay of %s (%zu utterances), latency ms after the completing push\n",
                    options.transcript.c_str(), utterances.size());
        std::printf("%-6s %-7s %6s %9s %9s %9s %9s %9s %10s\n", "speed", "input", "units",
                    "first_p50", "first_p95", "final_p50", "final_p95", "final_max", "lag_trend");
        for (const double speed : options.speeds) {
            for (const bool deltas : {false, true}) {
                bench_stream(translator, options, utterances, speed, deltas);
            }
        }
        if (load) {
            std::printf("\nASR load ran %d transcriptions of 10 s\n", load->runs());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
