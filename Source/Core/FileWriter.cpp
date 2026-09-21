// SPDX-License-Identifier: MIT
#include "FileWriter.h"
#include "Paths.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>

namespace Sleeve::FileWriter {

namespace fs = std::filesystem;

// Compact SHA-256 implementation
namespace Sha256Internal {
  struct Context {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
  };

  static inline uint32_t RightRotate(uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32 - count));
  }

  static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
  };

  static void Transform(uint32_t state[8], const uint8_t data[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    uint32_t m[64];

    for (int i = 0; i < 16; ++i) {
      m[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
             (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
             (static_cast<uint32_t>(data[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = RightRotate(m[i - 15], 7) ^ RightRotate(m[i - 15], 18) ^ (m[i - 15] >> 3);
      uint32_t s1 = RightRotate(m[i - 2], 17) ^ RightRotate(m[i - 2], 19) ^ (m[i - 2] >> 10);
      m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }

    for (int i = 0; i < 64; ++i) {
      uint32_t S1 = RightRotate(e, 6) ^ RightRotate(e, 11) ^ RightRotate(e, 25);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t temp1 = h + S1 + ch + K[i] + m[i];
      uint32_t S0 = RightRotate(a, 2) ^ RightRotate(a, 13) ^ RightRotate(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = S0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }

  static void Init(Context* ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
  }

  static void Update(Context* ctx, const uint8_t* data, size_t len) {
    size_t buffer_index = static_cast<size_t>(ctx->count & 63);
    ctx->count += len;
    size_t left = len;
    size_t offset = 0;

    if (buffer_index > 0) {
      size_t fill = 64 - buffer_index;
      if (left < fill) {
        std::memcpy(ctx->buffer + buffer_index, data, left);
        return;
      }
      std::memcpy(ctx->buffer + buffer_index, data, fill);
      Transform(ctx->state, ctx->buffer);
      offset += fill;
      left -= fill;
    }

    while (left >= 64) {
      Transform(ctx->state, data + offset);
      offset += 64;
      left -= 64;
    }

    if (left > 0) {
      std::memcpy(ctx->buffer, data + offset, left);
    }
  }

  static void Final(Context* ctx, uint8_t digest[32]) {
    uint64_t total_bits = ctx->count * 8;
    uint8_t pad = 0x80;
    Update(ctx, &pad, 1);
    uint8_t zero = 0;
    while ((ctx->count & 63) != 56) {
      Update(ctx, &zero, 1);
    }
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) {
      len_bytes[i] = static_cast<uint8_t>(total_bits >> (56 - i * 8));
    }
    Update(ctx, len_bytes, 8);

    for (int i = 0; i < 8; ++i) {
      digest[i * 4]     = static_cast<uint8_t>(ctx->state[i] >> 24);
      digest[i * 4 + 1] = static_cast<uint8_t>(ctx->state[i] >> 16);
      digest[i * 4 + 2] = static_cast<uint8_t>(ctx->state[i] >> 8);
      digest[i * 4 + 3] = static_cast<uint8_t>(ctx->state[i]);
    }
  }
} // namespace Sha256Internal

std::string ComputeSha256(const std::string& content) {
  Sha256Internal::Context ctx;
  Sha256Internal::Init(&ctx);
  Sha256Internal::Update(&ctx, reinterpret_cast<const uint8_t*>(content.data()), content.size());
  uint8_t digest[32];
  Sha256Internal::Final(&ctx, digest);

  std::ostringstream ss;
  ss << "sha256:";
  for (int i = 0; i < 32; ++i) {
    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
  }
  return ss.str();
}

std::string ComputeFileSha256(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return "";

  Sha256Internal::Context ctx;
  Sha256Internal::Init(&ctx);

  char buf[8192];
  while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
    Sha256Internal::Update(&ctx, reinterpret_cast<const uint8_t*>(buf), f.gcount());
  }
  uint8_t digest[32];
  Sha256Internal::Final(&ctx, digest);

  std::ostringstream ss;
  ss << "sha256:";
  for (int i = 0; i < 32; ++i) {
    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
  }
  return ss.str();
}

static bool HasSleeveMarker(const std::string& content) {
  // Markers:
  // Shell: "# generated by sleeve" or "# generated by armory"
  // Desktop: "X-Sleeve-Managed=true"
  return (content.find("generated by sleeve") != std::string::npos ||
          content.find("generated by armory") != std::string::npos ||
          content.find("X-Sleeve-Managed=true") != std::string::npos);
}

FileStatus CheckStatus(const std::string& path, const std::string& recordedSha256) {
  FileStatus st;
  st.path = path;

  std::error_code ec;
  if (!fs::exists(path, ec)) {
    st.verdict = FileVerdict::New;
    return st;
  }

  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    st.verdict = FileVerdict::Foreign;
    return st;
  }

  std::stringstream ss;
  ss << f.rdbuf();
  st.existing_content = ss.str();
  st.disk_sha256 = ComputeSha256(st.existing_content);

  bool hasMarker = HasSleeveMarker(st.existing_content);
  if (!hasMarker) {
    st.verdict = FileVerdict::Foreign;
  } else if (!recordedSha256.empty() && st.disk_sha256 == recordedSha256) {
    st.verdict = FileVerdict::Managed;
  } else if (!recordedSha256.empty()) {
    st.verdict = FileVerdict::HandEdited;
  } else {
    st.verdict = FileVerdict::Managed;
  }

  return st;
}

bool AtomicWrite(const std::string& path, const std::string& content, mode_t mode) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);

  std::string tmpPath = path + ".tmp." + std::to_string(::getpid());
  {
    std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f << content;
    f.flush();
    if (!f.good()) {
      fs::remove(tmpPath, ec);
      return false;
    }
  }

  ::chmod(tmpPath.c_str(), mode);
  fs::rename(tmpPath, path, ec);
  return !ec;
}

bool MoveAside(const std::string& path) {
  std::string backup = path + ".pre-sleeve";
  std::error_code ec;
  fs::rename(path, backup, ec);
  return !ec;
}

} // namespace Sleeve::FileWriter
