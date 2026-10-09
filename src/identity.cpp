#include "pxc/identity.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <sys/stat.h>
#endif

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <nlohmann/json.hpp>

#include "pxc/crypto.h"

namespace pxc {
namespace {

constexpr size_t kEd25519PubKey = 32;
constexpr size_t kEd25519SecKey = 32;
constexpr size_t kEd25519Sig    = 64;

// Crockford base32：去掉容易看错的 I/L/O/U，方便人工念读
const char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::string sha256_head(const std::vector<uint8_t>& data, size_t nbytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) throw std::runtime_error("sha256 failed");

    uint64_t v = 0;
    for (size_t i = 0; i < nbytes; ++i) v = (v << 8) | digest[i];

    std::string id(nbytes * 8 / 5, '0');
    for (int i = static_cast<int>(id.size()) - 1; i >= 0; --i) {
        id[i] = kCrockford[v & 0x1F];
        v >>= 5;
    }
    return id;
}

std::string format_device_id(const std::string& raw) {
    if (raw.size() != 8) return raw;
    return raw.substr(0, 4) + "-" + raw.substr(4, 4);
}

bool write_file_0600(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << content;
    f.close();

    // 私钥文件必须是仅当前用户可读写，否则同机其他用户可读取。
    // Windows 的 _chmod 只有用户/只读语义；更强的隔离应使用 DPAPI，
    // 第一版与 Linux 的 0600 保持语义对齐即可。
#if defined(_WIN32)
    return ::_chmod(path.c_str(), _S_IREAD | _S_IWRITE) == 0;
#else
    return ::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
#endif
}

}  // namespace

// ----------------------------------------------------------------- base64

std::string base64_encode(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};
    std::string out(4 * ((data.size() + 2) / 3), '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]),
                            data.data(), static_cast<int>(data.size()));
    if (n < 0) return {};
    out.resize(static_cast<size_t>(n));
    return out;
}

bool base64_decode(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    if (text.empty()) return true;
    if (text.size() % 4 != 0) return false;

    std::vector<uint8_t> buf(3 * (text.size() / 4));
    int n = EVP_DecodeBlock(buf.data(),
                            reinterpret_cast<const unsigned char*>(text.data()),
                            static_cast<int>(text.size()));
    if (n < 0) return false;

    size_t pad = 0;
    if (text[text.size() - 1] == '=') ++pad;
    if (text.size() > 1 && text[text.size() - 2] == '=') ++pad;

    buf.resize(static_cast<size_t>(n) - pad);
    out = std::move(buf);
    return true;
}

// -------------------------------------------------------------- Identity

Identity Identity::generate() {
    Identity id;

    EVP_PKEY*     pkey = nullptr;
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!pctx || EVP_PKEY_keygen_init(pctx) != 1 || EVP_PKEY_keygen(pctx, &pkey) != 1) {
        if (pctx) EVP_PKEY_CTX_free(pctx);
        throw std::runtime_error("Ed25519 keygen failed");
    }
    EVP_PKEY_CTX_free(pctx);

    id.pubkey_.resize(kEd25519PubKey);
    id.seckey_.resize(kEd25519SecKey);
    size_t plen = id.pubkey_.size();
    size_t slen = id.seckey_.size();
    if (EVP_PKEY_get_raw_public_key(pkey, id.pubkey_.data(), &plen) != 1 ||
        EVP_PKEY_get_raw_private_key(pkey, id.seckey_.data(), &slen) != 1) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Ed25519 export failed");
    }
    EVP_PKEY_free(pkey);

    id.pubkey_.resize(plen);
    id.seckey_.resize(slen);
    id.device_id_  = format_device_id(derive_device_id(id.pubkey_));
    id.pubkey_b64_ = base64_encode(id.pubkey_);
    return id;
}

std::string Identity::derive_device_id(const std::vector<uint8_t>& pubkey) {
    return sha256_head(pubkey, 5);
}

std::string Identity::normalize_device_id(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (char c : id) {
        if (c == '-' || c == ' ' || c == '_') continue;
        out.push_back(static_cast<char>(::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool Identity::load(const std::string& path, Identity& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    try {
        nlohmann::json j;
        f >> j;

        Identity    id;
        std::string pub64 = j.at("pubkey").get<std::string>();
        std::string sec64 = j.at("seckey").get<std::string>();
        if (!base64_decode(pub64, id.pubkey_) || !base64_decode(sec64, id.seckey_)) return false;
        if (id.pubkey_.size() != kEd25519PubKey || id.seckey_.size() != kEd25519SecKey) return false;

        id.pubkey_b64_ = pub64;
        id.device_id_  = format_device_id(derive_device_id(id.pubkey_));
        // 连接密钥在「添加本机」时才生成，老身份文件里可能没有
        id.connection_key_ = j.value("connection_key", "");

        out = std::move(id);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

Identity Identity::load_or_create(const std::string& path) {
    Identity loaded;
    if (Identity::load(path, loaded)) return loaded;

    // 文件不存在或不完整时生成新身份。
    // 注意：这是「首次运行」语义，账号模式下不要用它，
    // 否则损坏的身份文件会被静默替换成一台新设备。
    Identity id = Identity::generate();
    id.save(path);
    return id;
}

bool Identity::save(const std::string& path) const {
    nlohmann::json j{{"pubkey", pubkey_b64_}, {"seckey", base64_encode(seckey_)}};
    // 连接密钥与设备私钥同文件、同 0600 权限；为空时不写入该字段
    if (!connection_key_.empty()) j["connection_key"] = connection_key_;
    return write_file_0600(path, j.dump(2));
}

std::string Identity::connection_verifier_hex() const {
    if (connection_key_.empty()) return {};
    const auto verifier = crypto::derive_connection_verifier(connection_key_, device_id_);
    return crypto::to_hex(verifier);
}

std::string Identity::current_platform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::vector<uint8_t> Identity::sign(const std::vector<uint8_t>& data) const {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, seckey_.data(), seckey_.size());
    if (!pkey) throw std::runtime_error("load private key failed");

    EVP_MD_CTX*          ctx = EVP_MD_CTX_new();
    std::vector<uint8_t> sig(kEd25519Sig);
    size_t               siglen = sig.size();

    bool ok = ctx &&
              EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
              EVP_DigestSign(ctx, sig.data(), &siglen, data.data(), data.size()) == 1;

    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    if (!ok) throw std::runtime_error("sign failed");

    sig.resize(siglen);
    return sig;
}

bool Identity::verify(const std::vector<uint8_t>& pubkey,
                      const std::vector<uint8_t>& data,
                      const std::vector<uint8_t>& sig) {
    if (pubkey.size() != kEd25519PubKey) return false;

    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, pubkey.data(), pubkey.size());
    if (!pkey) return false;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool        ok = ctx &&
              EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
              EVP_DigestVerify(ctx, sig.data(), sig.size(), data.data(), data.size()) == 1;

    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

bool Identity::from_base64_pubkey(const std::string& b64, std::vector<uint8_t>& out) {
    if (!base64_decode(b64, out)) return false;
    return out.size() == kEd25519PubKey;
}

}  // namespace pxc
