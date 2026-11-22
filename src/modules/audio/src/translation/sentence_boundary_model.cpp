#include "translation/sentence_boundary_model.hpp"

#include <onnxruntime_cxx_api.h>
#include <sentencepiece_processor.h>

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dth::audio {

namespace {

// XLM-R (fairseq) ids: <s> 0, <pad> 1, </s> 2, <unk> 3; sentencepiece ids are shifted by one.
constexpr std::int64_t bos_id = 0;
constexpr std::int64_t eos_id = 2;
constexpr std::int64_t unk_id = 3;

// Few threads: the ASR and the translation model run at the same time.
constexpr int intra_op_threads = 2;

// Never destroyed: a session (held by a stream, possibly until static destruction) must not
// outlive its environment.
Ort::Env& ort_env()
{
    static auto* env = new Ort::Env{ORT_LOGGING_LEVEL_WARNING, "dth_sentence_boundary"};
    return *env;
}

ONNXTensorElementDataType element_type(const Ort::TypeInfo& info)
{
    return info.GetTensorTypeAndShapeInfo().GetElementType();
}

// The SaT ONNX export this class feeds: input_ids int64 and attention_mask fp16 in, logits fp16
// [batch, sequence, 1] out. Another export would be misread, so it is rejected at load.
void check_signature(const Ort::Session& session)
{
    Ort::AllocatorWithDefaultOptions allocator;
    const bool inputs_ok =
        session.GetInputCount() == 2 &&
        std::string_view{session.GetInputNameAllocated(0, allocator).get()} == "input_ids" &&
        element_type(session.GetInputTypeInfo(0)) == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 &&
        std::string_view{session.GetInputNameAllocated(1, allocator).get()} == "attention_mask" &&
        element_type(session.GetInputTypeInfo(1)) == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
    bool outputs_ok = session.GetOutputCount() == 1 &&
                      std::string_view{session.GetOutputNameAllocated(0, allocator).get()} == "logits";
    if (outputs_ok) {
        const Ort::TypeInfo info   = session.GetOutputTypeInfo(0);  // owns what `output` views
        const auto          output = info.GetTensorTypeAndShapeInfo();
        const auto          shape  = output.GetShape();
        outputs_ok = output.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 &&
                     shape.size() == 3 && shape[2] == 1;
    }
    if (!inputs_ok || !outputs_ok) {
        throw std::runtime_error(
            "not a SaT model: expected inputs input_ids (int64), attention_mask (float16) and "
            "output logits (float16, [batch, sequence, 1])");
    }
}

void require_file(const std::filesystem::path& path)
{
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("Sentence boundary model file not found: " + path.string());
    }
}

}  // namespace

struct SentenceBoundaryModel::Impl
{
    sentencepiece::SentencePieceProcessor tokenizer;
    Ort::Session                          session{nullptr};
};

SentenceBoundaryModel::SentenceBoundaryModel(const std::filesystem::path& directory)
{
    const std::filesystem::path model_path     = directory / "model.onnx";
    const std::filesystem::path tokenizer_path = directory / "sentencepiece.bpe.model";
    require_file(model_path);
    require_file(tokenizer_path);

    auto impl = std::make_unique<Impl>();
    if (const auto status = impl->tokenizer.Load(tokenizer_path.string()); !status.ok()) {
        throw std::runtime_error("Cannot load " + tokenizer_path.string() + ": " +
                                 status.ToString());
    }
    try {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(intra_op_threads);
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        impl->session = Ort::Session{ort_env(), model_path.native().c_str(), options};
        check_signature(impl->session);
    } catch (const std::exception& e) {  // Ort::Exception or the signature check
        throw std::runtime_error("Cannot load " + model_path.string() + ": " + e.what());
    }
    impl_ = std::move(impl);
}

SentenceBoundaryModel::~SentenceBoundaryModel() = default;

SentenceBoundaryModel::Tokens SentenceBoundaryModel::tokenize(std::string_view text) const
{
    Tokens tokens;
    for (const auto& piece : impl_->tokenizer.EncodeAsImmutableProto(text).pieces()) {
        if (tokens.ids.size() == max_tokens) {
            break;
        }
        const auto id = static_cast<std::int64_t>(piece.id());
        tokens.ids.push_back(id == impl_->tokenizer.unk_id() ? unk_id : id + 1);
        tokens.ends.push_back(piece.end());
    }
    return tokens;
}

std::vector<SentenceBoundaryModel::ScoredToken>
SentenceBoundaryModel::score(std::string_view text) const
{
    const Tokens tokens = tokenize(text);
    if (tokens.ids.empty()) {
        return {};
    }

    std::vector<std::int64_t> ids;
    ids.reserve(tokens.ids.size() + 2);
    ids.push_back(bos_id);
    ids.insert(ids.end(), tokens.ids.begin(), tokens.ids.end());
    ids.push_back(eos_id);
    std::vector<Ort::Float16_t> mask(ids.size(), Ort::Float16_t{1.0F});

    const std::array<std::int64_t, 2> shape{1, static_cast<std::int64_t>(ids.size())};
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::array<Ort::Value, 2> inputs{
        Ort::Value::CreateTensor<std::int64_t>(memory, ids.data(), ids.size(), shape.data(),
                                               shape.size()),
        Ort::Value::CreateTensor<Ort::Float16_t>(memory, mask.data(), mask.size(), shape.data(),
                                                 shape.size())};
    constexpr std::array input_names  = {"input_ids", "attention_mask"};
    constexpr std::array output_names = {"logits"};

    std::vector<Ort::Value> outputs;
    try {
        // Session::Run is thread-safe.
        outputs = const_cast<Ort::Session&>(impl_->session)
                      .Run(Ort::RunOptions{nullptr}, input_names.data(), inputs.data(),
                           inputs.size(), output_names.data(), output_names.size());
    } catch (const Ort::Exception& e) {
        throw std::runtime_error(std::string{"Sentence boundary model inference failed: "} +
                                 e.what());
    }
    if (outputs.front().GetTensorTypeAndShapeInfo().GetElementCount() < ids.size()) {
        throw std::runtime_error("Sentence boundary model returned fewer logits than tokens");
    }
    const auto* logits = outputs.front().GetTensorData<Ort::Float16_t>();

    std::vector<ScoredToken> scored(tokens.ids.size());
    for (std::size_t i = 0; i < scored.size(); ++i) {
        const float logit = logits[i + 1].ToFloat();  // [1, n + 2, 1], after <s>
        scored[i]         = {.end = tokens.ends[i], .probability = 1.0F / (1.0F + std::exp(-logit))};
    }
    return scored;
}

std::optional<std::size_t> SentenceBoundaryModel::first_boundary(std::string_view text,
                                                                 float            threshold,
                                                                 std::size_t      lookahead,
                                                                 std::size_t      from) const
{
    const std::vector<ScoredToken> scored = score(text);
    for (std::size_t i = 0; i + lookahead < scored.size(); ++i) {
        if (scored[i].end > from && scored[i].probability >= threshold) {
            return scored[i].end;
        }
    }
    return std::nullopt;
}

}  // namespace dth::audio
