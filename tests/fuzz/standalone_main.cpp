// A small mutation fuzzer for compilers without libFuzzer (Apple clang):
//   fuzz_parser <seed dir>                run each seed once
//   fuzz_parser --random N <seed dir>     N inputs mutated from the seeds
// Each input is copied into a buffer of exactly its size, so AddressSanitizer
// catches any read past the end.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

uint64_t rng_state = 0x9E3779B97F4A7C15ull;
uint64_t next_random() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
size_t below(size_t n) { return n == 0 ? 0 : static_cast<size_t>(next_random() % n); }

void run(const std::vector<uint8_t>& input) {
    auto buf = std::make_unique<uint8_t[]>(input.size() + 1);
    std::memcpy(buf.get(), input.data(), input.size());
    LLVMFuzzerTestOneInput(buf.get(), input.size());
}

std::vector<uint8_t> mutate(std::vector<uint8_t> v, const std::vector<std::vector<uint8_t>>& seeds) {
    const size_t steps = 1 + below(4);
    for (size_t s = 0; s < steps; ++s) {
        switch (below(6)) {
            case 0:  // flip a bit
                if (!v.empty()) v[below(v.size())] ^= static_cast<uint8_t>(1u << below(8));
                break;
            case 1:  // random byte
                if (!v.empty()) v[below(v.size())] = static_cast<uint8_t>(next_random());
                break;
            case 2:  // truncate
                v.resize(below(v.size() + 1));
                break;
            case 3: {  // overwrite what may be a length prefix
                if (v.size() >= 2) {
                    size_t i = below(v.size() - 1);
                    v[i] = static_cast<uint8_t>(below(3));
                    v[i + 1] = static_cast<uint8_t>(next_random());
                }
                break;
            }
            case 4: {  // insert random bytes
                size_t at = below(v.size() + 1);
                std::vector<uint8_t> extra(below(16));
                for (auto& b : extra) b = static_cast<uint8_t>(next_random());
                v.insert(v.begin() + static_cast<long>(at), extra.begin(), extra.end());
                break;
            }
            default: {  // splice in part of another seed
                const auto& other = seeds[below(seeds.size())];
                size_t from = below(other.size());
                size_t n = below(other.size() - from + 1);
                v.insert(v.begin() + static_cast<long>(below(v.size() + 1)), other.begin() + static_cast<long>(from),
                         other.begin() + static_cast<long>(from + n));
                break;
            }
        }
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    size_t random_runs = 0;
    std::string dir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--random") == 0 && i + 1 < argc) {
            random_runs = std::stoull(argv[++i]);
        } else {
            dir = argv[i];
        }
    }
    std::vector<std::vector<uint8_t>> seeds;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        std::ifstream in(entry.path(), std::ios::binary);
        seeds.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (seeds.empty()) {
        std::fprintf(stderr, "no seeds in %s\n", dir.c_str());
        return 1;
    }
    for (const auto& s : seeds) run(s);
    for (size_t i = 0; i < random_runs; ++i) run(mutate(seeds[below(seeds.size())], seeds));
    std::printf("ran %zu seeds and %zu mutated inputs\n", seeds.size(), random_runs);
    return 0;
}
