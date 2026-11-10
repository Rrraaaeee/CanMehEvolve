/// @file
/// CanMehSearch -- the search loop.
///
/// A single-population evolutionary search. Each individual is one patch: a
/// unified diff against the stock FIT SDK holding one region rewrite for each
/// ancestor of that patch. When the model answers and CanMehHarness splices the
/// answer, CanMehSearch adds exactly one region rewrite to the parent's patch;
/// when `show`, the model call or `splice` fails, CanMehSearch adds none.
///
/// In each generation CanMehSearch picks a parent patch and a region of source,
/// asks the model to rewrite that one region, and hands the reply to
/// CanMehHarness, which splices the rewrite into a new patch, builds the patched
/// tree and times the decode. CanMehHarness decides how to build and how to
/// measure; CanMehSearch decides only what to try next.
///
/// CLI11 generates the usage text from the option definitions in main. The
/// --llm-* options overwrite the matching fields of ChatClientConfig, which
/// utilities/ChatClient.hpp documents.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <CLI11.hpp>
#include <nlohmann/json.hpp>

#include "utilities/ChatClient.hpp"
#include "utilities/LineSamples.hpp"
#include "utilities/Process.hpp"

using utilities::ChatClientConfig;
using utilities::ModelReply;
using utilities::requestModelReply;
using utilities::readFile;
using utilities::writeFile;
using utilities::truncate;
using utilities::runForJsonReply;


using nlohmann::json;

namespace {

#ifndef CANMEH_DEFAULT_HARNESS
#define CANMEH_DEFAULT_HARNESS "./CanMehHarness"
#endif
#ifndef CANMEH_DEFAULT_DECODE_FOR_SAMPLER
#define CANMEH_DEFAULT_DECODE_FOR_SAMPLER "./CanMehDecodeForSampler"
#endif

constexpr auto invalidScore = double{1e18};

struct Options {
  std::string work;
  std::string out = "runs/latest";
  std::string harnessPath = CANMEH_DEFAULT_HARNESS;
  std::string decodeForSamplerPath = CANMEH_DEFAULT_DECODE_FOR_SAMPLER;
  /// Zero means CanMehSearch runs no sampler, so the sample-weighted draw
  /// never happens and every draw the bestGain branch does not take is
  /// uniform. Above zero, CanMehSearch builds a library with debug line
  /// tables, samples a decode of it for this long, and draws regions in
  /// proportion to the samples each one took.
  double sampleSeconds = 0.0;
  /// The share of the draws the bestGain branch did not take that CanMehSearch
  /// makes uniformly over every region rather than in proportion to samples.
  /// Not zero by default: a one-line accessor the compiler inlined has no
  /// samples of its own, and rewriting it can still make the decode faster, so
  /// no region is unreachable. At 1.0 CanMehSearch never takes the
  /// sample-weighted draw; the bestGain draw still runs.
  double uniformShare = 0.1;
  /// Adds three sentences to the prompt naming the kinds of work to look
  /// for. Default false: those sentences name kinds of code worth rewriting,
  /// so a run with them answers a different question from a run without them,
  /// and your report has to say which arm produced the result you quote.
  bool promptHints = false;
  int generations = 40;
  int population = 6;
  int maxLines = 250;
  unsigned seed = 1;
  /// Empty means every region CanMehHarness indexes. There is no default list,
  /// because a hand-written one is the experimenter's guess about where the win
  /// is, and a narrowed region set has to come from a measured profile.
  std::string regionFileNames;
};

struct Region {
  int id = 0;
  std::string file;
  std::string label;
  int lines = 0;
  /// 1-based and inclusive, as `CanMehHarness regions` prints them.
  /// weighRanges compares a profiler's line against these two to decide which
  /// region that line is in.
  int start = 0;
  int end = 0;
  /// Ending many profiler samples name a line inside this region. Zero when
  /// --sample-seconds was zero, so CanMehSearch ran no profiler.
  long long samples = 0;
};

/// One patch the search produced, together with what measuring that patch
/// reported. The patch itself is the diff in the file at patchPath; this struct
/// is what CanMehSearch records about that diff.
struct EvaluatedPatch {
  int id = 0;
  int parent = -1;
  int generation = 0;
  int region = -1;
  std::string patchPath;  // empty means the unmodified baseline
  double score = invalidScore;
  bool valid = false;  std::string note;
  /// Both measure-patch calls reported significant.
  bool significant = false;  /// Both measure-patch calls reported significant and the kept measurement's effect
  /// reached measure-patch's --min-effect. CanMehSearch admits a candidate on this
  /// field and not on significant, because signFlipPValue reads only whether
  /// the processes agree in direction: two candidates were called significant
  /// at 0.32 and 0.34 percent, far below what this instrument can resolve.
  ///
  /// measure-patch computed this from the whole accumulated patch against the
  /// reference, which is the only pair measure-patch measured. A child inherits its
  /// parent's rewrites, so a descendant of a patch that already clears
  /// --min-effect clears it too whatever its own rewrite did: 30 of 40
  /// generations of one cluster run reported true, from one discovery and
  /// 29 inheritors. Read improvedOnParent for the one generation.
  bool significantAtMinEffect = false;
};

struct RegionStats {
  int attempts = 0;
  int accepted = 0;
  // The largest ratio improvement this region produced against the parent the
  // candidate came from, counting only a measurement measure-patch called significant.
  // A difference of two ratios, not a duration.
  double bestGain = 0.0;
};

json runHarnessCommand(const Options& options, std::vector<std::string> args,
                        double timeoutSeconds = 900);

/// The decision: did this candidate decode faster than the patch it was spliced
/// onto?
///
/// Phase 1 of README.md requires you to run measure-patch with --no-decision,
/// and CanMehHarness then sends neither `significant` nor `ratio`, so your
/// version computes the decision from reply["timings"] alone. That computation
/// is the assignment, which is why the rule is one function rather than an
/// expression inside the loop.
bool candidateIsFaster(const json& reply, double minEffect);

struct CodeBlock {
  std::string tag;
  std::string text;
};

std::vector<CodeBlock> codeBlocks(const std::string& reply);

/// Reads the replacement text and the include lines out of one model reply.
///
/// splitReply takes the first fenced block whose info string does not contain
/// "include" as the replacement, and the first block whose info string does
/// contain it as the include lines, which `CanMehHarness splice --includes`
/// merges into the file's include block.
///
/// splitReply clears code when the text contains no brace, because every
/// region is a function body and splice replaces its signature and braces as
/// well, so no usable replacement lacks one. One model reply held the word
/// includes and two #include lines and no function at all; splitReply returned
/// that text, splice wrote it into the tree and the compiler reported
/// "'includes' does not name a type", which CanMehSearch then recorded as
/// stage compile for a model that had written no code.
void splitReply(const std::string& reply, std::string* code, std::string* includes);

std::string systemPrompt();

std::string userPrompt(const Region& region, const std::string& currentText,
                        double parentRatio, double bestRatio, double baselineMs,
                        const std::string& failure, bool hints);

std::vector<std::string> splitCommas(const std::string& s) {
  auto out = std::vector<std::string>{};
  auto cur = std::string{};
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (c != ' ') {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// ---------------------------------------------------------------------------
// The decision rule. Everything here reads reply["timings"] and nothing else.
// ---------------------------------------------------------------------------

/// The process-disagreement limit above which a measurement is too noisy to
/// decide. The same 0.25 CanMehHarness uses for its own null check.
constexpr auto maxRatioSpread = double{0.25};
/// The significance level of the two-sided sign-flip test.
constexpr auto alpha = double{0.01};
/// 2^20 sign assignments is about a million sums; beyond that the exact
/// enumeration is not worth its cost, and no run here uses so many processes.
constexpr auto maxExactProcesses = std::size_t{20};

struct Verdict {
  bool measured = false;  // false: the timings cannot support any statement
  std::string why;        // set when measured is false
  double ratio = 1.0;     // median over processes of median(candidate)/median(reference)
  double pValue = 1.0;
  double spread = 0.0;       // largest per-process ratio minus the smallest
  double candidateCv = 0.0;  // the noisiest process's candidate series
  double scoreMs = 0.0;      // median over processes of the candidate's median
  int swapped = 0;
  bool significant = false;  // ratio below 1 and p below alpha
  bool atMinEffect = false;  // significant, stable, and large enough to matter
  bool unstable = false;     // the processes disagree too much to decide
};

double medianOf(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const auto middle = values.size() / 2;
  return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

double coefficientOfVariation(const std::vector<double>& values) {
  auto mean = 0.0;
  for (double x : values) mean += x;
  mean /= static_cast<double>(values.size());
  auto squares = 0.0;
  for (double x : values) squares += (x - mean) * (x - mean);
  return std::sqrt(squares / static_cast<double>(values.size())) / mean;
}

/// Exact two-sided sign-flip test over per-process log-ratios: the share of all
/// 2^N sign assignments whose sum is at least as far from zero as the observed
/// sum. Its smallest possible value is 2/2^N, so one process yields p = 1 and
/// the rule can never accept at --repeat 1.
double signFlipPValue(const std::vector<double>& logRatios) {
  if (logRatios.empty() || logRatios.size() > maxExactProcesses) return 1.0;
  auto observed = 0.0;
  for (double x : logRatios) observed += x;
  observed = std::fabs(observed);

  const auto assignments = std::uint32_t{1} << logRatios.size();
  auto atLeastAsExtreme = std::uint32_t{0};
  for (auto mask = std::uint32_t{0}; mask < assignments; ++mask) {
    auto sum = 0.0;
    for (auto i = std::size_t{0}; i < logRatios.size(); ++i)
      sum += ((mask >> i) & 1U) != 0U ? -logRatios[i] : logRatios[i];
    // The tolerance keeps the identity assignment from losing to rounding.
    if (std::fabs(sum) >= observed - 1e-12) ++atLeastAsExtreme;
  }
  return static_cast<double>(atLeastAsExtreme) / static_cast<double>(assignments);
}

Verdict assess(const json& reply, double minEffect) {
  auto verdict = Verdict{};
  try {
    if (!reply.is_object() || !reply.value("ok", false)) {
      verdict.why = "the harness reported no measurement";
      return verdict;
    }
    const auto& timings = reply.at("timings");
    if (timings.at("ragged").get<bool>()) {
      verdict.why = "the repetition series differ in length";
      return verdict;
    }
    const auto& processes = timings.at("process");
    if (processes.empty()) {
      verdict.why = "the timings hold no process";
      return verdict;
    }

    auto ratios = std::vector<double>{};
    auto logRatios = std::vector<double>{};
    auto candidateMs = std::vector<double>{};
    for (const auto& process : processes) {
      const auto candidate = process.at("candidate_ns").get<std::vector<double>>();
      const auto reference = process.at("reference_ns").get<std::vector<double>>();
      if (candidate.size() < 3 || reference.size() < 3) {
        verdict.why = "a process returned fewer than three repetitions";
        return verdict;
      }
      const auto candidateMedian = medianOf(candidate);
      const auto referenceMedian = medianOf(reference);
      if (candidateMedian <= 0.0 || referenceMedian <= 0.0) {
        verdict.why = "a process reported a non-positive time";
        return verdict;
      }
      ratios.push_back(candidateMedian / referenceMedian);
      logRatios.push_back(std::log(ratios.back()));
      candidateMs.push_back(candidateMedian / 1e6);
      verdict.candidateCv = std::max(verdict.candidateCv, coefficientOfVariation(candidate));
      if (process.at("swapped").get<bool>()) ++verdict.swapped;
    }

    verdict.measured = true;
    verdict.ratio = medianOf(ratios);
    verdict.scoreMs = medianOf(candidateMs);
    verdict.pValue = signFlipPValue(logRatios);
    verdict.spread = *std::max_element(ratios.begin(), ratios.end()) -
                     *std::min_element(ratios.begin(), ratios.end());
    verdict.unstable = verdict.spread > maxRatioSpread;
    verdict.significant = verdict.ratio < 1.0 && verdict.pValue < alpha;
    verdict.atMinEffect =
        verdict.significant && !verdict.unstable && (1.0 - verdict.ratio) >= minEffect;
  } catch (const std::exception& exception) {
    verdict = Verdict{};
    verdict.why = std::string{"malformed timings: "} + exception.what();
  }
  return verdict;
}

bool candidateIsFaster(const json& reply, double minEffect) {
  return assess(reply, minEffect).atMinEffect;
}

// ---------------------------------------------------------------------------
// Talking to CanMehHarness and to the model.
// ---------------------------------------------------------------------------

json runHarnessCommand(const Options& options, std::vector<std::string> args,
                        double timeoutSeconds) {
  const auto subcommand = args.front();
  args.insert(args.begin() + 1, {"--work", options.work});
  // The three subcommands that resolve a region id need the listing's bound,
  // because the id is a position in the list at one setting of --max-lines.
  if (subcommand == "regions" || subcommand == "show" || subcommand == "splice")
    args.insert(args.begin() + 3, {"--max-lines", std::to_string(options.maxLines)});
  const auto scratch = std::filesystem::path{options.out} / "scratch";
  return runForJsonReply(options.harnessPath, std::move(args), scratch / "harness.out",
                         scratch / "harness.err", timeoutSeconds);
}

std::vector<CodeBlock> codeBlocks(const std::string& reply) {
  auto blocks = std::vector<CodeBlock>{};
  auto lines = std::istringstream{reply};
  auto line = std::string{};
  auto inside = false;
  auto current = CodeBlock{};
  while (std::getline(lines, line)) {
    const auto start = line.find_first_not_of(" \t");
    const auto isFence = start != std::string::npos && line.compare(start, 3, "```") == 0;
    if (isFence && !inside) {
      inside = true;
      current = CodeBlock{};
      current.tag = line.substr(start + 3);
      continue;
    }
    if (isFence) {
      inside = false;
      blocks.push_back(current);
      continue;
    }
    if (inside) current.text += line + "\n";
  }
  // A block still open here was cut off by the token limit, so it is dropped
  // rather than handed to the compiler as a truncated function.
  return blocks;
}

void splitReply(const std::string& reply, std::string* code, std::string* includes) {
  code->clear();
  includes->clear();

  // A reasoning model may draft code inside its think block, and that draft
  // must not be mistaken for the answer.
  auto visible = reply;
  const auto thinkEnd = visible.rfind("</think>");
  if (thinkEnd != std::string::npos) visible.erase(0, thinkEnd + 8);

  auto haveCode = false;
  auto haveIncludes = false;
  for (const CodeBlock& block : codeBlocks(visible)) {
    if (block.tag.find("include") != std::string::npos) {
      if (!haveIncludes) *includes = block.text;
      haveIncludes = true;
    } else {
      if (!haveCode) *code = block.text;
      haveCode = true;
    }
  }
  if (code->find('{') == std::string::npos) code->clear();
}

std::string systemPrompt() {
  return "You are an expert C++ performance engineer optimizing the decoder of the Garmin FIT "
         "SDK. You rewrite exactly one C++ function so that decoding runs faster.\n"
         "\n"
         "Rules:\n"
         "- Preserve observable behavior exactly: every decoded value, every return value and "
         "every side effect must stay the same for every input, including inputs absent from "
         "any test file.\n"
         "- Keep the function's name, parameters, return type, qualifiers and any enclosing "
         "class qualification unchanged. Do not change public declarations or add members.\n"
         "- Do not start threads, and do not add caches that depend on global mutable state.\n"
         "- The code is compiled as C++17 at -O2.\n"
         "- You must change the code: a reply identical to the input is rejected. Make the "
         "change you judge most likely to be both safe and faster.\n"
         "\n"
         "Reply format: one fenced ```cpp block holding the complete replacement function, "
         "signature and braces included. If you need new headers, add a second fenced "
         "```includes block holding only #include lines; #include lines inside the function "
         "block are ignored. Keep any explanation short.";
}

std::string userPrompt(const Region& region, const std::string& currentText, double parentRatio,
                        double bestRatio, double baselineMs, const std::string& failure,
                        bool hints) {
  auto prompt = std::ostringstream{};
  prompt << "File: " << region.file << "\nFunction: " << region.label << "\n";
  if (baselineMs > 0.0) prompt << "One decode of the test corpus takes about " << baselineMs << " ms.\n";
  if (parentRatio > 0.0)
    prompt << "The code below currently runs at " << parentRatio
           << " of the original SDK's decode time; the best found so far is " << bestRatio
           << ".\n";
  if (hints)
    prompt << "Look for repeated lookups that can be done once, avoidable copies and heap "
              "allocations, and linear searches over data that could be indexed. "
              "Prefer changes on the per-field and per-byte paths of the decoder. "
              "Avoid changes to code that only runs once per file.\n";
  if (!failure.empty())
    prompt << "\nThe previous candidate was rejected:\n" << truncate(failure, 1500) << "\n";
  prompt << "\nRewrite this function to make decoding faster:\n```cpp\n" << currentText;
  if (!currentText.empty() && currentText.back() != '\n') prompt << "\n";
  prompt << "```\n";
  return prompt.str();
}

// ---------------------------------------------------------------------------
// Helpers for the loop.
// ---------------------------------------------------------------------------

void writeRecord(std::ofstream& stream, const json& record) {
  // Compiler messages and model replies can hold invalid UTF-8, which would
  // otherwise make dump throw and end the run.
  stream << record.dump(-1, ' ', false, json::error_handler_t::replace) << "\n";
  stream.flush();
}

/// A record of the seven keys README.md names, for a generation that produced
/// no candidate to measure.
json shortRecord(int generation, const Region& region, const std::string& stage,
                 const std::string& error) {
  return json{{"generation", generation}, {"region", region.id}, {"file", region.file},
              {"label", region.label},    {"ok", false},         {"stage", stage},
              {"error", truncate(error, 1500)}};
}

/// Profiles one decode and writes each region's sample count into `regions`.
/// Returns false, having said why, when no profile could be taken; the search
/// then carries on drawing uniformly rather than losing the run.
bool profileRegions(const Options& options, const json& meta, std::vector<Region>* regions) {
  const auto built = runHarnessCommand(options, {"sampling-library"}, 1800);
  if (!built.value("ok", false)) {
    std::cout << "profile: sampling-library failed: " << built.dump() << std::endl;
    return false;
  }

  auto request = utilities::SamplingRequest{};
  request.seconds = options.sampleSeconds;
  request.scratch = std::filesystem::path{options.out} / "scratch" / "sampler";
  request.image = std::filesystem::path{built.value("library", "")}.filename().string();
  const auto runSeconds = static_cast<int>(options.sampleSeconds + request.settleSeconds + 10.0);
  request.command = {options.decodeForSamplerPath, "--library", built.value("library", ""),
                     "--data", meta.value("data", ""),
                     "--benchmark_min_time=" + std::to_string(runSeconds) + "s"};

  const auto profile = utilities::measureLineSamples(request);
  if (!profile.ok) {
    std::cout << "profile: " << profile.tool << " failed: " << profile.error << std::endl;
    return false;
  }

  auto ranges = std::vector<utilities::LineRange>{};
  ranges.reserve(regions->size());
  for (const Region& region : *regions)
    ranges.push_back({region.file, region.start, region.end, region.id});
  const auto weights = utilities::weighRanges(profile, ranges, [](const std::string& file) {
    return file.find("/tree/src/") != std::string::npos;
  });
  for (Region& region : *regions) {
    const auto found = weights.byKey.find(region.id);
    if (found != weights.byKey.end()) region.samples = found->second;
  }

  std::cout << "profile: " << profile.tool << " took " << profile.total << " samples, "
            << weights.attributed << " inside a region, " << weights.unattributed
            << " in the tree outside every region" << std::endl;
  writeFile(std::filesystem::path{options.out} / "profile.json",
            json{{"tool", profile.tool},
                 {"total", profile.total},
                 {"located", profile.located},
                 {"attributed", weights.attributed},
                 {"unattributed", weights.unattributed},
                 {"ambiguous", weights.ambiguous}}
                .dump(2));
  return weights.attributed > 0;
}

/// One request to the Claude Messages API, which is not the OpenAI format that
/// utilities::requestModelReply speaks. Sends no temperature, because current
/// Claude models reject it. It returns the same ModelReply, so the loop does not
/// know which API answered: output tokens include the model's thinking tokens.
ModelReply requestClaudeReply(const ChatClientConfig& config, const std::string& systemMessage,
                              const std::string& userMessage, std::string* answeredBy) {
  const auto url = config.baseUrl + "/messages";
  const auto payload =
      json{{"model", config.model},
           {"max_tokens", config.maxTokens},
           {"system", systemMessage},
           {"messages", json::array({{{"role", "user"}, {"content", userMessage}}})}}
          .dump(-1, ' ', false, json::error_handler_t::replace);

  auto lastError = std::string{"requestClaudeReply sent no request, because retries is zero"};
  for (auto attempt = int{1}; attempt <= config.retries; ++attempt) {
    auto handle = utilities::chatClientDetail::EasyHandle{curl_easy_init()};
    if (!handle) return ModelReply{"", "curl_easy_init failed"};
    auto headers = utilities::chatClientDetail::HeaderList{
        curl_slist_append(nullptr, "Content-Type: application/json")};
    headers.reset(curl_slist_append(headers.release(), "anthropic-version: 2026-10-21"));
    headers.reset(curl_slist_append(headers.release(), ("x-api-key: " + config.apiKey).c_str()));

    auto response = std::string{};
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION,
                     utilities::chatClientDetail::appendToString);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, config.timeoutSeconds);

    const auto performed = curl_easy_perform(handle.get());
    auto status = long{0};
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);

    if (performed != CURLE_OK) {
      lastError = std::string{"transport: "} + curl_easy_strerror(performed);
    } else if (status == 429 || status >= 500) {
      lastError = "http " + std::to_string(status) + ": " + response.substr(0, 300);
    } else if (status != 200) {
      return ModelReply{"", "http " + std::to_string(status) + ": " + response.substr(0, 300)};
    } else {
      try {
        const auto parsed = json::parse(response);
        auto reply = ModelReply{};
        const auto& usage = parsed.at("usage");
        reply.promptTokens = usage.value("input_tokens", 0LL);
        reply.completionTokens = usage.value("output_tokens", 0LL);
        reply.totalTokens = reply.promptTokens + reply.completionTokens;
        *answeredBy = parsed.value("model", std::string{"absent from the response"});
        if (parsed.value("stop_reason", std::string{}) == "refusal") {
          reply.error = "the model refused the request";
          return reply;
        }
        for (const auto& block : parsed.at("content"))
          if (block.value("type", std::string{}) == "text")
            reply.content += block.value("text", std::string{});
        return reply;
      } catch (const std::exception& exception) {
        return ModelReply{"", std::string{"malformed response: "} + exception.what()};
      }
    }
    if (attempt < config.retries) std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));
  }
  return ModelReply{"", lastError};
}

/// How often a draw exploits a region that has already paid off, when one has.
constexpr auto bestGainShare = double{0.25};

/// Draws from `drawable`, indices into `regions`. Without feedback (best-of-N)
/// `stats` is empty, so the bestGain branch is never taken.
std::size_t drawRegion(const std::vector<Region>& regions, const std::vector<std::size_t>& drawable,
                       const std::map<int, RegionStats>& stats, bool profiled, double uniformShare,
                       std::mt19937& rng) {
  auto coin = std::uniform_real_distribution<double>{0.0, 1.0};
  const auto weighted = [&](const std::function<double(const Region&)>& weightOf,
                            std::size_t* picked) {
    auto weights = std::vector<double>{};
    auto total = 0.0;
    for (std::size_t index : drawable) {
      weights.push_back(weightOf(regions[index]));
      total += weights.back();
    }
    if (total <= 0.0) return false;
    auto distribution = std::discrete_distribution<std::size_t>{weights.begin(), weights.end()};
    *picked = drawable[distribution(rng)];
    return true;
  };

  auto picked = std::size_t{0};
  if (coin(rng) < bestGainShare &&
      weighted(
          [&](const Region& region) {
            const auto found = stats.find(region.id);
            return found == stats.end() ? 0.0 : found->second.bestGain;
          },
          &picked))
    return picked;
  if (profiled && coin(rng) >= uniformShare &&
      weighted([](const Region& region) { return static_cast<double>(region.samples); }, &picked))
    return picked;
  return drawable[std::uniform_int_distribution<std::size_t>{0, drawable.size() - 1}(rng)];
}

const EvaluatedPatch& pickParent(const std::vector<EvaluatedPatch>& population, std::mt19937& rng) {
  auto index = std::uniform_int_distribution<std::size_t>{0, population.size() - 1};
  const auto& first = population[index(rng)];
  const auto& second = population[index(rng)];
  return first.score <= second.score ? first : second;
}

void writeRegionsTried(const std::filesystem::path& path, const std::vector<Region>& regions,
                       const std::map<int, RegionStats>& stats) {
  auto tried = json::array();
  for (const Region& region : regions) {
    const auto found = stats.find(region.id);
    if (found == stats.end()) continue;
    tried.push_back({{"region", region.id},
                     {"file", region.file},
                     {"label", region.label},
                     {"samples", region.samples},
                     {"attempts", found->second.attempts},
                     {"accepted", found->second.accepted},
                     {"best_gain", found->second.bestGain}});
  }
  writeFile(path, tried.dump(2, ' ', false, json::error_handler_t::replace));
}

}  // namespace

int main(int argc, char** argv) {
  auto options = Options{};
  auto chat = ChatClientConfig{};
  auto keyFile = std::string{};
  auto judgeFile = std::string{};
  auto minEffect = double{0.02};
  auto noSearch = false;

  auto app = CLI::App{"Evolves patches to the FIT SDK decoder with a language model."};
  app.add_option("--work", options.work, "work directory CanMehHarness init created");
  app.add_option("--out", options.out, "directory for logs and patches")->capture_default_str();
  app.add_option("--generations", options.generations, "model calls to make")->capture_default_str();
  app.add_option("--population", options.population, "patches kept between generations")
      ->capture_default_str();
  app.add_option("--max-lines", options.maxLines, "largest region to index")->capture_default_str();
  app.add_option("--seed", options.seed, "seed for parent and region draws")->capture_default_str();
  app.add_option("--sample-seconds", options.sampleSeconds, "seconds to profile; 0 draws uniformly")
      ->capture_default_str();
  app.add_option("--uniform-share", options.uniformShare,
                 "share of non-bestGain draws that ignore the profile")
      ->capture_default_str();
  app.add_option("--region-files", options.regionFileNames,
                 "comma-separated source files to restrict regions to");
  app.add_flag("--prompt-hints", options.promptHints, "add three sentences naming what to rewrite");
  app.add_flag("--no-search", noSearch,
               "best-of-N control: every call starts from the baseline, with no feedback");
  app.add_option("--min-effect", minEffect, "smallest fractional improvement the rule accepts")
      ->capture_default_str();
  app.add_option("--harness", options.harnessPath, "CanMehHarness binary")->capture_default_str();
  app.add_option("--decode-for-sampler", options.decodeForSamplerPath,
                 "CanMehDecodeForSampler binary")
      ->capture_default_str();
  auto llmApi = std::string{"openai"};
  app.add_option("--llm-api", llmApi, "request format: openai, or anthropic for the Claude API")
      ->check(CLI::IsMember({"openai", "anthropic"}))
      ->capture_default_str();
  auto baseUrlOption =
      app.add_option("--llm-base-url", chat.baseUrl, "endpoint")->capture_default_str();
  auto maxTokensOption = app.add_option("--llm-max-tokens", chat.maxTokens, "reply token limit")
                             ->capture_default_str();
  app.add_option("--llm-model", chat.model, "model id")->capture_default_str();
  app.add_option("--llm-temperature", chat.temperature, "sampling temperature")->capture_default_str();
  app.add_option("--llm-retries", chat.retries, "attempts per request")->capture_default_str();
  app.add_option("--llm-timeout", chat.timeoutSeconds, "seconds per request")->capture_default_str();
  app.add_option("--llm-api-key-file", keyFile, "file holding the API key");
  app.add_option("--judge", judgeFile,
                 "apply the decision rule to one saved --no-decision reply, print it and exit");
  CLI11_PARSE(app, argc, argv);

  const auto useClaude = llmApi == "anthropic";
  if (useClaude && baseUrlOption->count() == 0) chat.baseUrl = "https://api.anthropic.com/v1";
  // Thinking counts against max_tokens on Claude, so the OpenAI default of 4096
  // could cut a reply off before its code block closes.
  if (useClaude && maxTokensOption->count() == 0) chat.maxTokens = 16000;

  // Phase 0 calls this over measure-pair replies, so the rule calibrated there
  // is this code rather than a copy of it.
  if (!judgeFile.empty()) {
    const auto reply = utilities::readJson(judgeFile);
    const auto verdict = assess(reply, minEffect);
    std::cout << json{{"measured", verdict.measured}, {"why", verdict.why},
                      {"ratio", verdict.ratio},       {"p_value", verdict.pValue},
                      {"spread", verdict.spread},     {"candidate_cv", verdict.candidateCv},
                      {"unstable", verdict.unstable}, {"significant", verdict.significant},
                      {"faster", candidateIsFaster(reply, minEffect)}, {"min_effect", minEffect}}
                     .dump()
              << std::endl;
    return verdict.measured ? 0 : 1;
  }

  if (options.work.empty() || keyFile.empty()) {
    std::cerr << "--work and --llm-api-key-file are required\n";
    return 2;
  }
  chat.apiKey = readFile(keyFile);
  while (!chat.apiKey.empty() && std::isspace(static_cast<unsigned char>(chat.apiKey.back())))
    chat.apiKey.pop_back();
  if (chat.apiKey.empty()) {
    std::cerr << "cannot read an API key from " << keyFile << "\n";
    return 2;
  }

  const auto outDirectory = std::filesystem::path{options.out};
  std::filesystem::create_directories(outDirectory / "patches");
  std::filesystem::create_directories(outDirectory / "scratch");

  const auto meta = utilities::readJson(std::filesystem::path{options.work} / "meta.json");
  if (meta.is_discarded()) {
    std::cerr << "no meta.json in " << options.work << ": run CanMehHarness init first\n";
    return 2;
  }
  const auto baselineMs = meta.value("baseline_ms", 0.0);
  const auto nullRatio = meta.value("null_ratio", 1.0);
  std::cout << "null run: ratio " << nullRatio << ", p " << meta.value("null_p", 1.0)
            << ", baseline " << baselineMs << " ms" << std::endl;
  // Judged on effect size, never p: under the null p is uniform.
  if (std::fabs(1.0 - nullRatio) >= 0.02) {
    std::cerr << "the null run differs from 1.000 by two percent or more, so no ratio from this "
                 "work directory can be trusted\n";
    return 1;
  }

  const auto listing = runHarnessCommand(options, {"regions"});
  if (!listing.value("ok", false)) {
    std::cerr << "regions failed: " << listing.dump() << "\n";
    return 1;
  }
  auto regions = std::vector<Region>{};
  for (const auto& entry : listing.at("regions")) {
    auto region = Region{};
    region.id = entry.at("id").get<int>();
    region.file = entry.at("file").get<std::string>();
    region.label = entry.at("label").get<std::string>();
    region.lines = entry.at("lines").get<int>();
    region.start = entry.at("start").get<int>();
    region.end = entry.at("end").get<int>();
    regions.push_back(region);
  }

  const auto wanted = splitCommas(options.regionFileNames);
  auto drawable = std::vector<std::size_t>{};
  for (auto i = std::size_t{0}; i < regions.size(); ++i) {
    const auto name = std::filesystem::path{regions[i].file}.filename().string();
    if (wanted.empty() || std::find(wanted.begin(), wanted.end(), name) != wanted.end())
      drawable.push_back(i);
  }
  if (drawable.empty()) {
    std::cerr << "--region-files left no region to draw from\n";
    return 1;
  }
  std::cout << drawable.size() << " of " << regions.size() << " regions are drawable" << std::endl;

  const auto profiled = options.sampleSeconds > 0.0 && profileRegions(options, meta, &regions);
  if (options.sampleSeconds > 0.0 && !profiled)
    std::cout << "no usable profile, so regions are drawn uniformly" << std::endl;

  curl_global_init(CURL_GLOBAL_DEFAULT);
  auto results = std::ofstream{outDirectory / "results.jsonl"};
  auto credits = std::ofstream{outDirectory / "credits.csv"};
  credits << "generation,calls,prompt_tokens,completion_tokens,total_tokens,"
             "best_confirmed_speedup,best_retained_speedup,elapsed_s\n";
  // The README asks which model id answered, which may differ from the id requested.
  auto models = std::ofstream{outDirectory / "models.csv"};
  models << "generation,model\n";
  writeFile(outDirectory / "system_prompt.txt", systemPrompt());

  auto rng = std::mt19937{options.seed};
  auto stats = std::map<int, RegionStats>{};
  const auto start = std::chrono::steady_clock::now();
  auto calls = 0LL;
  auto promptTokens = 0LL;
  auto completionTokens = 0LL;
  auto totalTokens = 0LL;
  auto lastFailure = std::string{};

  // EvaluatedPatch::score holds the ratio to the reference. Milliseconds drift
  // between measurements; the ratio is taken within each one.
  auto baseline = EvaluatedPatch{};
  baseline.score = 1.0;
  baseline.valid = true;
  baseline.note = "baseline";
  auto population = std::vector<EvaluatedPatch>{baseline};
  auto bestConfirmedRatio = 1.0;
  auto bestConfirmedPatch = std::string{};
  auto bestRetainedRatio = 1.0;

  const auto outcomeOfGeneration = [&](int generation) {
    credits << generation << ',' << calls << ',' << promptTokens << ',' << completionTokens << ','
            << totalTokens << ',' << 1.0 / bestConfirmedRatio << ',' << 1.0 / bestRetainedRatio
            << ','
            << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
            << "\n";
    credits.flush();
    writeRegionsTried(outDirectory / "regions_tried.json", regions, stats);
  };

  for (auto generation = int{1}; generation <= options.generations; ++generation) {
    const auto parent = noSearch ? baseline : pickParent(population, rng);
    const auto empty = std::map<int, RegionStats>{};
    const auto& region =
        regions[drawRegion(regions, drawable, noSearch ? empty : stats, profiled,
                           options.uniformShare, rng)];
    ++stats[region.id].attempts;

    auto showArgs = std::vector<std::string>{"show", "--region", std::to_string(region.id)};
    if (!parent.patchPath.empty()) showArgs.insert(showArgs.end(), {"--patch", parent.patchPath});
    const auto shown = runHarnessCommand(options, showArgs);
    if (!shown.value("ok", false)) {
      writeRecord(results, shortRecord(generation, region, shown.value("stage", "show"),
                                       shown.value("error", "show failed")));
      outcomeOfGeneration(generation);
      continue;
    }

    const auto bestRatio = std::min(bestConfirmedRatio, bestRetainedRatio);
    const auto prompt =
        noSearch ? userPrompt(region, shown.value("text", ""), 0.0, 0.0, baselineMs, "",
                              options.promptHints)
                 : userPrompt(region, shown.value("text", ""), parent.score, bestRatio, baselineMs,
                              lastFailure, options.promptHints);
    const auto stem = outDirectory / "patches" / ("gen_" + std::to_string(generation));
    writeFile(stem.string() + ".prompt", prompt);
    // utilities::requestModelReply does not expose the id its endpoint reports.
    auto answeredBy = std::string{"not reported by this client"};
    const auto reply = useClaude ? requestClaudeReply(chat, systemPrompt(), prompt, &answeredBy)
                                 : requestModelReply(chat, systemPrompt(), prompt);
    models << generation << ',' << answeredBy << "\n";
    models.flush();
    ++calls;
    promptTokens += reply.promptTokens;
    completionTokens += reply.completionTokens;
    totalTokens += reply.totalTokens;
    writeFile(stem.string() + ".repl", reply.content);

    auto code = std::string{};
    auto includes = std::string{};
    splitReply(reply.content, &code, &includes);
    if (code.empty()) {
      const auto error = reply.error.empty() ? std::string{"the reply held no code block with a brace"}
                                             : reply.error;
      writeRecord(results, shortRecord(generation, region, "llm", error));
      std::cout << "gen " << generation << ": llm: " << truncate(error, 200) << std::endl;
      outcomeOfGeneration(generation);
      continue;
    }

    const auto replacementPath = (outDirectory / "scratch" / "replacement.cpp").string();
    writeFile(replacementPath, code);
    const auto patchPath = stem.string() + ".patch";
    auto spliceArgs = std::vector<std::string>{"splice", "--region", std::to_string(region.id),
                                               "--replacement", replacementPath, "--out", patchPath};
    if (!parent.patchPath.empty()) spliceArgs.insert(spliceArgs.end(), {"--patch", parent.patchPath});
    if (!includes.empty()) {
      const auto includesPath = (outDirectory / "scratch" / "includes.txt").string();
      writeFile(includesPath, includes);
      spliceArgs.insert(spliceArgs.end(), {"--includes", includesPath});
    }
    const auto spliced = runHarnessCommand(options, spliceArgs);
    if (!spliced.value("ok", false)) {
      writeRecord(results, shortRecord(generation, region, spliced.value("stage", "splice"),
                                       spliced.value("error", "splice failed")));
      outcomeOfGeneration(generation);
      continue;
    }

    // A reply that changes nothing produces the parent's diff again. Measuring
    // it would spend a generation's time on a null comparison.
    if (readFile(patchPath) == (parent.patchPath.empty() ? std::string{} : readFile(parent.patchPath))) {
      writeRecord(results, shortRecord(generation, region, "unchanged",
                                       "the reply was identical to the code it was given"));
      lastFailure = "stage unchanged: your reply was identical to the code you were given";
      std::cout << "gen " << generation << ": unchanged" << std::endl;
      outcomeOfGeneration(generation);
      continue;
    }

    const auto measure = [&]() {
      return runHarnessCommand(options, {"measure-patch", "--patch", patchPath, "--no-decision"},
                               1800);
    };

    auto record = json{{"generation", generation},   {"id", generation},
                       {"parent", parent.id},        {"region", region.id},
                       {"file", region.file},        {"label", region.label},
                       {"ok", false},                {"score_ms", nullptr},
                       {"significant", false},       {"significant_at_min_effect", false},
                       {"min_effect", minEffect},    {"improved_on_parent", false},
                       {"p_value", nullptr},         {"confirmed_p", -1.0},
                       {"unstable", false},          {"ratio", nullptr},
                       {"speedup", nullptr},         {"ratio_spread", nullptr},
                       {"candidate_cv", nullptr},    {"swapped_processes", 0},
                       {"stage", ""},                {"note", ""}};

    const auto firstReply = measure();
    const auto first = assess(firstReply, minEffect);
    if (!first.measured) {
      const auto stage = firstReply.value("stage", std::string{"rule"});
      const auto error = firstReply.value("error", first.why);
      record["stage"] = stage;
      record["note"] = truncate(error, 1500);
      writeRecord(results, record);
      lastFailure = "stage " + stage + ": " + error;
      std::cout << "gen " << generation << ": rejected at " << stage << std::endl;
      outcomeOfGeneration(generation);
      continue;
    }
    lastFailure.clear();

    // A passing or undecidable candidate is measured again, and the less
    // flattering of the two measurements is the one kept.
    auto kept = first;
    auto confirmedP = -1.0;
    auto bothSignificant = false;
    auto bothFaster = false;
    auto note = std::string{};
    if (first.significant || first.unstable) {
      const auto secondReply = measure();
      const auto second = assess(secondReply, minEffect);
      if (second.measured) {
        confirmedP = second.pValue;
        if (second.ratio > first.ratio) kept = second;
        bothSignificant = first.significant && second.significant;
        bothFaster = candidateIsFaster(firstReply, minEffect) &&
                     candidateIsFaster(secondReply, minEffect);
      } else {
        note = "the second measurement failed: " + secondReply.value("error", second.why);
      }
    }

    const auto improved = bothFaster && kept.ratio < parent.score;
    auto& regionStats = stats[region.id];
    if (improved) {
      ++regionStats.accepted;
      regionStats.bestGain = std::max(regionStats.bestGain, parent.score - kept.ratio);
    }

    record["ok"] = true;
    record["score_ms"] = kept.scoreMs;
    record["significant"] = bothSignificant;
    record["significant_at_min_effect"] = bothFaster;
    record["improved_on_parent"] = improved;
    record["p_value"] = first.pValue;
    record["confirmed_p"] = confirmedP;
    record["unstable"] = kept.unstable;
    record["ratio"] = kept.ratio;
    record["speedup"] = 1.0 / kept.ratio;
    record["ratio_spread"] = kept.spread;
    record["candidate_cv"] = kept.candidateCv;
    record["swapped_processes"] = kept.swapped;
    record["note"] = note;
    writeRecord(results, record);

    if (bothFaster && kept.ratio < bestConfirmedRatio) {
      bestConfirmedRatio = kept.ratio;
      bestConfirmedPatch = patchPath;
    }

    auto candidate = EvaluatedPatch{};
    candidate.id = generation;
    candidate.parent = parent.id;
    candidate.generation = generation;
    candidate.region = region.id;
    candidate.patchPath = patchPath;
    candidate.score = kept.ratio;
    candidate.valid = true;
    candidate.significant = bothSignificant;
    candidate.significantAtMinEffect = bothFaster;
    population.push_back(candidate);
    std::stable_sort(population.begin(), population.end(),
                     [](const EvaluatedPatch& x, const EvaluatedPatch& y) { return x.score < y.score; });
    if (population.size() > static_cast<std::size_t>(std::max(1, options.population)))
      population.pop_back();
    bestRetainedRatio = std::min(bestRetainedRatio, population.front().score);

    std::cout << "gen " << generation << ": ratio " << kept.ratio << " p " << first.pValue
              << (bothFaster ? " CONFIRMED" : "") << (improved ? " improved" : "") << std::endl;
    outcomeOfGeneration(generation);
  }

  // best.patch names only a confirmed candidate. An empty file says the run
  // confirmed nothing, which a retained candidate's lucky ratio must not hide.
  writeFile(outDirectory / "best.patch",
            bestConfirmedPatch.empty() ? std::string{} : readFile(bestConfirmedPatch));
  std::cout << "best confirmed speedup " << 1.0 / bestConfirmedRatio << " after " << calls
            << " calls and " << totalTokens << " tokens"
            << (bestConfirmedPatch.empty() ? " (nothing confirmed; best.patch is empty)" : "")
            << std::endl;
  curl_global_cleanup();
  return 0;
}
