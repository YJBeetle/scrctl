#include "wifi/Srp.h"

#include <openssl/evp.h>

#include <cstdio>
#include <string_view>

// Independent oracle: RFC5054 server formula with Python pow/hashlib (3072/SHA512),
// plus this repository's Apple minimal-integer M1/M2 encoding. No peer implementation copied.
namespace {
using scrctl::wifi::Bytes;
using scrctl::wifi::SrpClient;
using scrctl::wifi::SrpServer;
int checks = 0, failures = 0;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
Bytes hex(std::string_view text) {
    Bytes result;
    for (size_t i = 0; i < text.size(); i += 2) {
        auto value = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        result.push_back(static_cast<uint8_t>(value(text[i]) * 16 + value(text[i + 1])));
    }
    return result;
}
const Bytes salt = hex("00112233445566778899aabbccddeeff");
const Bytes public_a = hex("7d");
const Bytes public_b = hex(
    "c33ac44888e5997617c6ae454a8f1603e641ab68d7b427f94efa9921c199f8874e79854c4d5326ae95b2dfed5d340688"
    "cad78a2eceb85be81deb62c7ade09f0cbd55ee217bdd809da1d2793cc1aee5a47cf1f1025676c9d203dc6423f9ffcdb0"
    "9bbad0506060ced086ad283f54ec0576ac752af8a76cfda8d01e3d3d438899797e81024a9f0c16dd70511106ca7ab653"
    "ae2cf9aeea521405e72874e36284e59c2db1a0532159179049dcd40c56d2dbcc2b074987ed1b8eadf2697ae107b7736b"
    "b50a3c8c516459a9a81b765cbc7abff3792f4be8681e0e21fbc5c1c290bb8ae621cec5433f69ce84ac524b5ca438b872"
    "a556a9d234a4302a1bb8b3e2830f7efc26e474549e4e63772a2e35c3d586e84224bd2ad8cf7db7bf4bdb9a959fa3a0f1"
    "adf3d1e6f9d1e220ff9938d0dcf331d7147eca5d54bcdd2d5148cafd029e0f0a7bb39082f3bd3d3917cb48ff4e9f338f"
    "f5ba2c41d5d6db10b63d5c0be7e0ef05866a7ebd8df1098f1fa14a80a33cf3dec531ce496668a0763c374969316c4742");
const Bytes key = hex("e1fdfd66c4a770d12bbbfb6b944d0a86c9eb58cc9b9087e5b1fc7bb9986e0bb7e8e54b511c151c13f9e4d2b1cb4c5c9101aadc6b6ea345abd21221a9f5aaa676");
const Bytes proof_client = hex("80313d46ff4ede08a761e4d4e92b38708870b2497b9277427f0a198da6e8373ea6579493b1ca86fb52c72a9575d3b8840c77fed406b8bc959e1f2eef60e96054");
const Bytes proof_server = hex("1f6388f5f9329425d4fcb68e306b1c0f9db359e6d858167c760d318c7f48ad0ba8887c99129cf2e8a019c152850add4c9052bfdbd58427a7689319ce72d79a68");
const Bytes modulus = hex(
    "ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd129024e088a67cc74020bbea63b139b22514a08798e3404dd"
    "ef9519b3cd3a431b302b0a6df25f14374fe1356d6d51c245e485b576625e7ec6f44c42e9a637ed6b0bff5cb6f406b7ed"
    "ee386bfb5a899fa5ae9f24117c4b1fe649286651ece45b3dc2007cb8a163bf0598da48361c55d39a69163fa8fd24cf5f"
    "83655d23dca3ad961c62f356208552bb9ed529077096966d670c354e4abc9804f1746c08ca18217c32905e462e36ce3b"
    "e39e772c180e86039b2783a2ec07a28fb5c55df06f4c52c9de2bcbf6955817183995497cea956ae515d2261898fa0510"
    "15728e5a8aaac42dad33170d04507a33a85521abdf1cba64ecfb850458dbef0a8aea71575d060c7db3970f85a6e1e4c7"
    "abf5ae8cdb0933d71e8c94e04a25619dcee3d2261ad2ee6bf12ffa06d98a0864d87602733ec86a64521f2b18177b200c"
    "bbe117577a615d6c770988c0bad946e208e24fa074e5ab3143db5bfce0fd108e4b82d120a93ad2caffffffffffffffff");
void empty_results(const SrpServer &server, const char *message) {
    check(server.session_key().empty() && server.server_proof().empty(), message);
}
void fixed_oracle() {
    SrpServer server("Pair-Setup", "393039", "07");
    std::string error = "old error";
    check(server.server_public().empty(), "server publishes no challenge before initialize");
    empty_results(server, "server starts with no authenticated key or proof");
    check(server.initialize(salt, error) && error.empty(), "fixed challenge initializes and clears old error");
    check(server.server_public() == public_b, "B matches independent Python/RFC oracle");
    empty_results(server, "initialize does not publish an unverified session key");
    check(server.process(public_a, proof_client, error) && error.empty(), "independent M1 authenticates");
    check(server.session_key() == key, "authenticated K matches independent oracle");
    check(server.server_proof() == proof_server, "M2 matches independent oracle");
    check(!server.process(public_a, proof_client, error) && !error.empty(), "completed challenge cannot accept a second proof");
    empty_results(server, "duplicate process clears old K/M2");
    check(server.initialize(salt, error), "explicit initialize allows a fresh challenge after completion");
    check(server.process(public_a, proof_client, error), "fresh challenge accepts its fixed proof");
    check(!server.initialize({}, error) && !error.empty() && server.server_public().empty(), "invalid reinitialize clears old public challenge");
    empty_results(server, "failed reinitialize clears previously authenticated K/M2");
}
void client_roundtrip() {
    for (const std::string private_b : {std::string("07"), std::string()}) {
        SrpServer server("Pair-Setup", "393039", private_b);
        SrpClient client("Pair-Setup", "393039", "03");
        std::string error;
        check(server.initialize(salt, error), "challenge initializes with fixed or random private b");
        check(client.process(salt, server.server_public(), error), "existing production client computes A/M1");
        check(client.client_public() == public_a, "small A remains minimal one-byte integer");
        // Wire padding is allowed within the group width; proof hashes the integer minimally.
        Bytes padded_a(384 - client.client_public().size(), 0);
        padded_a.insert(padded_a.end(), client.client_public().begin(), client.client_public().end());
        check(server.process(padded_a, client.client_proof(), error), "server accepts canonical-equivalent padded A");
        check(server.session_key() == client.session_key(), "both production peers derive the same authenticated key");
        check(client.verify_server_proof(server.server_proof()), "existing client verifies the server M2");
    }
    SrpServer server("Pair-Setup", "393039", "07");
    std::string error;
    check(server.initialize(salt, error), "first pending challenge initializes");
    Bytes other_salt = salt; other_salt.back() ^= 1;
    check(server.initialize(other_salt, error) && server.server_public() != public_b, "explicit reinitialize replaces pending challenge and salt");
    check(!server.process(public_a, proof_client, error), "proof for replaced challenge cannot authenticate");
    empty_results(server, "replaced-challenge mismatch publishes no key or proof");
}
void wrong_credentials_and_proof() {
    SrpServer server("Pair-Setup", "393039", "07");
    SrpClient wrong_pin("Pair-Setup", "393038", "03");
    std::string error;
    check(server.initialize(salt, error) && wrong_pin.process(salt, server.server_public(), error), "wrong-PIN peer can compute its own SRP proof");
    check(!server.process(wrong_pin.client_public(), wrong_pin.client_proof(), error) && !error.empty(), "server rejects wrong PIN");
    empty_results(server, "wrong PIN cannot publish K/M2");
    check(!server.process(public_a, proof_client, error), "failed challenge is consumed even if next proof would be correct");
    empty_results(server, "second attempt after bad PIN has no results");
    for (size_t index : {size_t(0), size_t(31), size_t(63)}) {
        check(server.initialize(salt, error), "tamper test starts a fresh challenge");
        Bytes corrupted = proof_client; corrupted[index] ^= 0x80;
        check(!server.process(public_a, corrupted, error) && !error.empty(), "tampered 64-byte M1 is rejected");
        empty_results(server, "tampered proof publishes no session secrets");
    }
    SrpClient wrong_user("Other-Setup", "393039", "03");
    check(server.initialize(salt, error) && wrong_user.process(salt, server.server_public(), error), "wrong-user peer computes its own proof");
    check(!server.process(wrong_user.client_public(), wrong_user.client_proof(), error), "SRP proof binds the configured username");
    empty_results(server, "wrong username cannot publish K/M2");
}
void input_and_order() {
    SrpServer server("Pair-Setup", "393039", "07");
    std::string error;
    check(!server.process(public_a, proof_client, error) && !error.empty(), "process before initialize is rejected");
    empty_results(server, "out-of-order process has no key or proof");
    for (const Bytes &invalid_a : {Bytes(), Bytes{0}, Bytes(384, 0), Bytes(385, 0xff), modulus}) {
        check(server.initialize(salt, error), "invalid-A test initializes");
        check(!server.process(invalid_a, proof_client, error) && !error.empty(), "empty, oversized, zero or N client public integer is rejected");
        empty_results(server, "invalid A publishes no key or proof");
        check(!server.process(public_a, proof_client, error), "invalid A consumes the challenge");
    }
    for (const Bytes &invalid_proof : {Bytes(), Bytes(63, 0), Bytes(65, 0)}) {
        check(server.initialize(salt, error), "invalid proof-length test initializes");
        check(!server.process(public_a, invalid_proof, error) && !error.empty(), "M1 must be exactly 64 bytes");
        empty_results(server, "invalid proof length publishes no key or proof");
    }
    for (const Bytes &invalid_salt : {Bytes(), Bytes(256, 0)}) {
        check(!server.initialize(invalid_salt, error) && !error.empty(), "salt must be nonempty and fit the SRP salt field");
        check(server.server_public().empty(), "invalid salt publishes no public challenge");
        empty_results(server, "invalid salt publishes no session secret");
    }
    for (const std::string private_b : {std::string("0"), std::string("0000"), std::string("-1"), std::string("xz"), std::string(769, '1')}) {
        SrpServer invalid("Pair-Setup", "393039", private_b);
        check(!invalid.initialize(salt, error) && !error.empty(), "injected b must be positive hex within group width");
        check(invalid.server_public().empty(), "invalid b publishes no challenge");
        empty_results(invalid, "invalid b publishes no K/M2");
    }
    check(server.initialize(Bytes(255, 0x42), error), "maximum RFC salt width initializes");
    empty_results(server, "salt-width boundary still requires client authentication");
}
void digest_failure() {
    SrpServer server("Pair-Setup", "393039", "07");
    std::string error;
    check(server.initialize(salt, error) && server.process(public_a, proof_client, error), "crypto-failure fixture starts authenticated");
    check(EVP_set_default_properties(nullptr, "provider=scrctl-missing-provider") == 1, "fixture disables digest provider in this test process");
    const bool initialized = server.initialize(salt, error);
    const bool error_recorded = !error.empty();
    const bool empty = server.server_public().empty() && server.session_key().empty() && server.server_proof().empty();
    check(EVP_set_default_properties(nullptr, "") == 1, "fixture restores its default digest properties");
    check(!initialized && error_recorded && empty, "crypto failure during initialize clears old challenge and authenticated secrets");
    check(server.initialize(salt, error), "challenge works after digest properties are restored");
    check(EVP_set_default_properties(nullptr, "provider=scrctl-missing-provider") == 1, "fixture disables digest during client-proof processing");
    const bool processed = server.process(public_a, proof_client, error);
    const bool process_error = !error.empty();
    const bool process_empty = server.session_key().empty() && server.server_proof().empty();
    check(EVP_set_default_properties(nullptr, "") == 1, "fixture restores digest after failed process");
    check(!processed && process_error && process_empty, "crypto failure during proof processing cannot publish secrets");
    check(!server.process(public_a, proof_client, error), "crypto failure also consumes its challenge");
}
} // namespace
int main() {
    fixed_oracle(); client_roundtrip(); wrong_credentials_and_proof(); input_and_order(); digest_failure();
    std::printf("srp_server: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
