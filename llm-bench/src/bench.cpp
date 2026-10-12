// SPDX-License-Identifier: GPL-3.0-or-later
// ps5-llm-bench: measures llama.cpp text generation on the PS5 GPU, with no UI.
//
// It loads one GGUF model with every layer on the Vulkan GPU, then measures
//   1. generation speed: 256 tokens, one at a time, three runs
//   2. prompt reading speed: a ~1,200-token prompt, three runs
// and serves the report on http://<PS5-IP>:9050/ while it runs.
//
// Settings come from files next to eboot.bin:
//   env.txt         KEY=VALUE lines, set as environment variables before the GPU
//                   starts (llama.cpp, ggml-vulkan and RADV switches, e.g.
//                   GGML_VK_PERF_LOGGER=1 for per-operation GPU timings)
//   model_path.txt  full path of the GGUF file (optional)
//
// The screen stays black while it runs; that is expected.
#include "ggml.h"
#include "llama.h"

#include <pthread.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>
#include <vector>

extern "C" {
int sceKernelDebugOutText(int, const char *);
int sceKernelUsleep(unsigned int);
int scePthreadCreate(void **thread, const void *attributes, void *(*entry)(void *), void *argument, const char *name);
int scePthreadJoin(void *thread, void **result);
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetSocket(const char *name, int domain, int type, int protocol);
int sceNetSocketClose(int sock);
int sceNetBind(int sock, const void *address, unsigned int length);
int sceNetListen(int sock, int backlog);
int sceNetAccept(int sock, void *address, unsigned int *length);
int sceNetSend(int sock, const void *buffer, unsigned long length, int flags);
int sceNetRecv(int sock, void *buffer, unsigned long length, int flags);
int sceNetSetsockopt(int sock, int level, int option, const void *value, unsigned int length);
}

namespace {

constexpr const char *kVersion = "ps5-llm-bench v1";
constexpr int kHttpPort = 9050;
constexpr int kGenTokens = 256;
constexpr int kRuns = 3;

std::mutex g_report_mutex;
std::string g_report;
volatile int g_finished = 0;

// ---------------------------------------------------------------- report

void append(const std::string &text) {
    {
        std::lock_guard<std::mutex> lock(g_report_mutex);
        g_report += text;
    }
    // klog gets it too (in pieces: the kernel truncates long lines).
    for (size_t i = 0; i < text.size(); i += 900) {
        std::string piece = "[llm-bench] " + text.substr(i, 900);
        sceKernelDebugOutText(0, piece.c_str());
    }
}

void emit(const char *format, ...) {
    char line[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    append(line);
}

std::string report_copy() {
    std::lock_guard<std::mutex> lock(g_report_mutex);
    return g_report;
}

void save_report() {
    const std::string text = report_copy();
    const char *paths[] = {"/data/ps5-llm-bench-report.txt", "/download0/ps5-llm-bench-report.txt"};
    for (const char *path : paths) {
        if (FILE *f = std::fopen(path, "w")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }
}

// llama.cpp / ggml messages. Model loading prints hundreds of lines; keep the
// Vulkan device lines, our knob line, warnings and errors.
void log_callback(ggml_log_level level, const char *text, void *) {
    if (level >= GGML_LOG_LEVEL_WARN || std::strstr(text, "ggml_vulkan") || std::strstr(text, "ps5-knobs") ||
        std::strstr(text, "llama_perf") || std::strstr(text, "Vulkan0")) {
        append(std::string("  [llama] ") + text);
    }
}

// ggml-vulkan's GGML_VK_PERF_LOGGER writes to std::cerr; send that into the report.
class ReportBuf : public std::streambuf {
  protected:
    int overflow(int c) override {
        if (c != EOF) {
            line_ += static_cast<char>(c);
            if (c == '\n') {
                append("  [perf] " + line_);
                line_.clear();
            }
        }
        return c;
    }
    std::streamsize xsputn(const char *s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; ++i) overflow(static_cast<unsigned char>(s[i]));
        return n;
    }

  private:
    std::string line_;
};
ReportBuf g_cerr_buf;

// ---------------------------------------------------------------- web page

struct SockAddrIn {  // FreeBSD layout
    uint8_t sin_len, sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
};

void send_all(int sock, const char *data, size_t length) {
    while (length > 0) {
        const int sent = sceNetSend(sock, data, length, 0);
        if (sent <= 0) return;
        data += sent;
        length -= static_cast<size_t>(sent);
    }
}

void *http_thread(void *) {
    if (sceNetInit() < 0 || sceNetPoolCreate("llm-bench", 1 << 20, 0) < 0) {
        emit("web page: network init failed; use klog\n");
        return nullptr;
    }
    const int listen_sock = sceNetSocket("llm-bench", 2, 1, 0);
    int reuse = 1;
    sceNetSetsockopt(listen_sock, 0xffff, 0x0004, &reuse, sizeof(reuse));
    SockAddrIn address = {};
    address.sin_len = sizeof(address);
    address.sin_family = 2;
    address.sin_port = static_cast<uint16_t>((kHttpPort >> 8) | ((kHttpPort & 0xff) << 8));
    if (listen_sock < 0 || sceNetBind(listen_sock, &address, sizeof(address)) < 0 || sceNetListen(listen_sock, 4) < 0) {
        emit("web page: bind failed; use klog\n");
        return nullptr;
    }
    for (;;) {
        SockAddrIn client;
        unsigned int client_length = sizeof(client);
        const int sock = sceNetAccept(listen_sock, &client, &client_length);
        if (sock < 0) {
            sceKernelUsleep(100000);
            continue;
        }
        char request[2048];
        sceNetRecv(sock, request, sizeof(request), 0);
        std::string body = g_finished ? "" : "STILL RUNNING - refresh in a few seconds.\n\n";
        body += report_copy();
        char header[256];
        snprintf(header, sizeof(header),
                 "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\n"
                 "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                 body.size());
        send_all(sock, header, std::strlen(header));
        send_all(sock, body.data(), body.size());
        sceNetSocketClose(sock);
    }
    return nullptr;
}

// ---------------------------------------------------------------- settings

std::string read_file(const char *path) {
    std::string text;
    if (FILE *f = std::fopen(path, "r")) {
        char buffer[4096];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, n);
        std::fclose(f);
    }
    return text;
}

std::string trim(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string g_env_summary;

void apply_env_file() {
    const std::string text = read_file("/app0/env.txt");
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        setenv(key.c_str(), value.c_str(), 1);
        g_env_summary += (g_env_summary.empty() ? "" : " ") + key + "=" + value;
    }
    emit("settings from env.txt: %s\n", g_env_summary.empty() ? "(none)" : g_env_summary.c_str());
}

bool file_exists(const std::string &path) {
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}

std::string find_model() {
    std::vector<std::string> candidates;
    const std::string chosen = trim(read_file("/app0/model_path.txt"));
    if (!chosen.empty()) candidates.push_back(chosen);
    candidates.push_back("/data/prosperoai/models/Mistral-7B-Instruct-v0.3.Q4_0.gguf");
    candidates.push_back("/app0/models/Mistral-7B-Instruct-v0.3.Q4_0.gguf");
    for (const auto &path : candidates) {
        const bool ok = file_exists(path);
        emit("model: %s %s\n", path.c_str(), ok ? "found" : "not readable");
        if (ok) return path;
    }
    return "";
}

// ---------------------------------------------------------------- benchmark

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
}

std::vector<llama_token> tokenize(const llama_vocab *vocab, const std::string &text) {
    std::vector<llama_token> tokens(text.size() + 16);
    int n = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()), tokens.data(),
                           static_cast<int32_t>(tokens.size()), true, false);
    if (n < 0) {
        tokens.resize(-n);
        n = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()), tokens.data(),
                           static_cast<int32_t>(tokens.size()), true, false);
    }
    tokens.resize(n > 0 ? n : 0);
    return tokens;
}

// Feeds tokens in chunks of at most n_batch. Returns seconds, or -1 on error.
double decode_prompt(llama_context *ctx, std::vector<llama_token> &tokens) {
    const int n_batch = static_cast<int>(llama_n_batch(ctx));
    const int64_t start = ggml_time_us();
    for (size_t pos = 0; pos < tokens.size(); pos += n_batch) {
        const int count = std::min<int>(n_batch, static_cast<int>(tokens.size() - pos));
        if (llama_decode(ctx, llama_batch_get_one(tokens.data() + pos, count)) != 0) return -1;
    }
    llama_synchronize(ctx);
    return (ggml_time_us() - start) / 1e6;
}

bool run_bench() {
    const std::string model_path = find_model();
    if (model_path.empty()) {
        emit("ERROR: no model. Put the GGUF in /data/prosperoai/models/, or in a models/ folder next to "
             "eboot.bin, or write its full path into model_path.txt.\n");
        return false;
    }

    ggml_backend_dev_t devices[] = {ggml_backend_dev_by_name("Vulkan0"), nullptr};
    if (!devices[0]) {
        emit("ERROR: no Vulkan0 device\n");
        return false;
    }
    size_t free_bytes = 0, total_bytes = 0;
    ggml_backend_dev_memory(devices[0], &free_bytes, &total_bytes);
    emit("Vulkan memory: free %.2f GiB of %.2f GiB\n", free_bytes / 1073741824.0, total_bytes / 1073741824.0);

    auto mp = llama_model_default_params();
    mp.devices = devices;
    mp.n_gpu_layers = 99;
    mp.load_mode = LLAMA_LOAD_MODE_NONE;  // as ProsperoAI: no mmap on the PS5
    int64_t t0 = ggml_time_us();
    llama_model *model = llama_model_load_from_file(model_path.c_str(), mp);
    if (!model) {
        emit("ERROR: model failed to load\n");
        return false;
    }
    emit("model loaded in %.1f s: %s, %.2f GiB, %.2f B parameters\n", (ggml_time_us() - t0) / 1e6,
         model_path.c_str(), llama_model_size(model) / 1073741824.0, llama_model_n_params(model) / 1e9);
    const double weight_gb = llama_model_size(model) / 1e9;

    auto cp = llama_context_default_params();
    cp.n_ctx = 4096;
    cp.n_batch = 512;
    cp.n_ubatch = 512;
    cp.n_threads = 4;
    cp.n_threads_batch = 4;
    cp.no_perf = false;
    llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        emit("ERROR: context failed\n");
        llama_model_free(model);
        return false;
    }
    emit("context: n_ctx=%u batch=%u ubatch=%u\n\n", llama_n_ctx(ctx), llama_n_batch(ctx), llama_n_ubatch(ctx));

    const llama_vocab *vocab = llama_model_get_vocab(model);
    auto *sampler = llama_sampler_init_greedy();
    llama_memory_t memory = llama_get_memory(ctx);

    // Warm-up: compiles/caches pipelines and fills the driver's queues.
    {
        auto warm = tokenize(vocab, "Hello, how are you today?");
        llama_memory_clear(memory, true);
        decode_prompt(ctx, warm);
        llama_token t = llama_sampler_sample(sampler, ctx, -1);
        for (int i = 0; i < 16; ++i) {
            if (llama_decode(ctx, llama_batch_get_one(&t, 1)) != 0) break;
            t = llama_sampler_sample(sampler, ctx, -1);
        }
        llama_synchronize(ctx);
        emit("warm-up done\n\n");
    }

    // 1. Generation.
    const std::string question = "Explain in detail how a GPU executes a matrix multiplication, covering threads, "
                                 "memory tiers and synchronisation.";
    std::vector<double> gen_rates, gen_ms;
    for (int run = 1; run <= kRuns; ++run) {
        auto tokens = tokenize(vocab, "Run " + std::to_string(run) + ". " + question);
        llama_memory_clear(memory, true);
        if (decode_prompt(ctx, tokens) < 0) {
            emit("ERROR: prompt decode failed\n");
            break;
        }
        llama_token t = llama_sampler_sample(sampler, ctx, -1);
        std::vector<double> per_token;
        const int64_t start = ggml_time_us();
        int generated = 0;
        for (int i = 1; i < kGenTokens; ++i) {
            const int64_t a = ggml_time_us();
            if (llama_decode(ctx, llama_batch_get_one(&t, 1)) != 0) break;
            t = llama_sampler_sample(sampler, ctx, -1);  // waits for the GPU result
            per_token.push_back((ggml_time_us() - a) / 1000.0);
            ++generated;
        }
        const double seconds = (ggml_time_us() - start) / 1e6;
        const double rate = generated / seconds;
        const double ms = median(per_token);
        gen_rates.push_back(rate);
        gen_ms.push_back(ms);
        emit("generation run %d: %d tokens in %.3f s = %.1f tok/s (median %.2f ms per token, "
             "fastest %.2f ms) -> weights read at %.0f GB/s\n",
             run, generated, seconds, rate, ms, *std::min_element(per_token.begin(), per_token.end()),
             weight_gb * rate);
    }

    // 2. Prompt reading.
    std::string paragraph =
        "A graphics processor runs thousands of small threads at the same time. Threads are grouped into waves that "
        "execute the same instruction together, and waves are grouped into workgroups that share a small fast "
        "memory. Data moves from the large slow memory into caches, then into the shared memory of a workgroup, and "
        "finally into registers where the arithmetic happens. Matrix multiplication splits the output into tiles. "
        "Each workgroup loads a tile of each input, multiplies them, and adds the result into its tile of the output "
        "before moving on to the next pair of input tiles. Barriers make every thread in the workgroup wait until "
        "the shared tile is fully loaded before anyone reads it, and again before it is overwritten. ";
    std::string long_text;
    for (int i = 0; i < 8; ++i) long_text += paragraph;
    long_text += "Summarise the text above in one sentence.";
    std::vector<double> prompt_rates;
    for (int run = 1; run <= kRuns; ++run) {
        auto tokens = tokenize(vocab, "Run " + std::to_string(run) + ". " + long_text);
        llama_memory_clear(memory, true);
        const double seconds = decode_prompt(ctx, tokens);
        if (seconds <= 0) {
            emit("ERROR: long prompt decode failed\n");
            break;
        }
        prompt_rates.push_back(tokens.size() / seconds);
        emit("prompt-reading run %d: %zu tokens in %.3f s = %.0f tok/s\n", run, tokens.size(), seconds,
             tokens.size() / seconds);
    }

    emit("\n");
    llama_perf_context_print(ctx);
    emit("\nSUMMARY %s | gen_tok_s=%.1f gen_ms_per_token=%.2f weights_GBps=%.0f prompt_tok_s=%.0f | %s\n", kVersion,
         median(gen_rates), median(gen_ms), weight_gb * median(gen_rates), median(prompt_rates),
         g_env_summary.empty() ? "default settings" : g_env_summary.c_str());

    llama_sampler_free(sampler);
    llama_free(ctx);
    llama_model_free(model);
    return true;
}

void *bench_thread(void *) {
    try {
        llama_backend_init();
        run_bench();
    } catch (const std::exception &e) {
        emit("ERROR: exception: %s\n", e.what());
    }
    std::cerr.flush();
    return nullptr;
}

int start_thread(void **thread, void *(*entry)(void *), const char *name, size_t stack) {
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error) return error;
    error = pthread_attr_setstacksize(&attributes, stack);
    if (!error) error = scePthreadCreate(thread, &attributes, entry, nullptr, name);
    pthread_attr_destroy(&attributes);
    return error;
}

}  // namespace

int main() {
    emit("%s\n", kVersion);
    std::cerr.rdbuf(&g_cerr_buf);
    llama_log_set(log_callback, nullptr);
    ggml_log_set(log_callback, nullptr);
    setenv("GGML_NO_BACKTRACE", "1", 1);
    apply_env_file();

    void *http = nullptr, *worker = nullptr;
    if (start_thread(&http, http_thread, "llm-bench-http", 256 * 1024) != 0) emit("no web page thread\n");
    sceKernelUsleep(500000);
    if (start_thread(&worker, bench_thread, "llm-bench-gpu", 8 * 1024 * 1024) == 0) {
        scePthreadJoin(worker, nullptr);
    } else {
        bench_thread(nullptr);
    }
    emit("\nBENCH COMPLETE. Close the app with the PS button.\n");
    save_report();
    g_finished = 1;
    for (;;) sceKernelUsleep(1000000);
}
