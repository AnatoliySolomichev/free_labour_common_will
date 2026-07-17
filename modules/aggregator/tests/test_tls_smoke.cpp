// TLS smoke test: proves httplib is compiled with CPPHTTPLIB_OPENSSL_SUPPORT
// and an https:// URL reaches an SSL server end-to-end. The production
// aggregator itself stays plain HTTP behind a TLS-terminating proxy; what
// must speak https is the client side (`--via https://...`, peer sync).

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#error "httplib must be compiled with CPPHTTPLIB_OPENSSL_SUPPORT (see third_party/CMakeLists.txt)"
#endif

namespace {

// Self-signed localhost certificate, valid to 2126 (test fixture only).
constexpr const char* kCertPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDCzCCAfOgAwIBAgIUfGyk2qRlxFqQQZRp7ipKdeRd+LwwDQYJKoZIhvcNAQEL\n"
    "BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MDcxNjE5NDg0MloYDzIxMjYw\n"
    "NjIyMTk0ODQyWjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEB\n"
    "AQUAA4IBDwAwggEKAoIBAQDngMICdubgZND06FPBO7i2Cv+KYf3iKnglPvzbKGhx\n"
    "cpgLkGT30k3EiBxet6kXeXJHDi3nOrnbnRCcxxpt6JVzl53ItbrkRGkIsSBedtxw\n"
    "D4HjGCmpLuGqNWI+vHJ4z/Gw9gaXgUukCqghaZ4Szb3LqpfS1ndlYbjLB98SGjOP\n"
    "UusD95PyWgnJ3aRt4T2r7Oar6EmPwdl2UJ0fZR0aaFiw2WYCZPraZmqRb1AD5GTG\n"
    "LCoKNOyQyiK4aNvSNnbJOyMUYBeQIXhB+/T9lGOy11a/XTUAU72MxglWvdKxogka\n"
    "m05Lq50HN9WBpETAuQjHyW091HbFi+bfKEFESi2Wn8dnAgMBAAGjUzBRMB0GA1Ud\n"
    "DgQWBBSUkeU9cRu24xzPz0gc5HzxHQMyRzAfBgNVHSMEGDAWgBSUkeU9cRu24xzP\n"
    "z0gc5HzxHQMyRzAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQAw\n"
    "784QpRKzXnTViX/hp4cOSH2XeNP+3zQJkvSLlVVWpeOdbLChW9xveGITw5Sn5te5\n"
    "UJlBY5XuRvNVG9ngPSSJ+HsICjth5wFJ2+pHGY9H1ea6itRcACi7DSjrR8rUbI+I\n"
    "mzJS9B4+o7FvDV0Rdhq4Oz87OVT4h/bT1q0fX1VURu7EIa60gZAzQY/kO/DWK/0p\n"
    "k05UzSNUh/nNO9nGOX/D2bEqRYLtXqksPRgiVCPOY/LeAUl+CuBfk13SdPwr1uTB\n"
    "EyzIyOk+PMiKt9id27/6Zsbig21MhiozGS2MJferKsaSS4n7/nnEgZbtiSu2mEeH\n"
    "mio1j+vEyl0yqRsbYxJ2\n"
    "-----END CERTIFICATE-----\n";

constexpr const char* kKeyPem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDngMICdubgZND0\n"
    "6FPBO7i2Cv+KYf3iKnglPvzbKGhxcpgLkGT30k3EiBxet6kXeXJHDi3nOrnbnRCc\n"
    "xxpt6JVzl53ItbrkRGkIsSBedtxwD4HjGCmpLuGqNWI+vHJ4z/Gw9gaXgUukCqgh\n"
    "aZ4Szb3LqpfS1ndlYbjLB98SGjOPUusD95PyWgnJ3aRt4T2r7Oar6EmPwdl2UJ0f\n"
    "ZR0aaFiw2WYCZPraZmqRb1AD5GTGLCoKNOyQyiK4aNvSNnbJOyMUYBeQIXhB+/T9\n"
    "lGOy11a/XTUAU72MxglWvdKxogkam05Lq50HN9WBpETAuQjHyW091HbFi+bfKEFE\n"
    "Si2Wn8dnAgMBAAECggEAAtuTCYbAONrlvXA0wHjlQdIG74VgJe+SRhtt8aKhD21z\n"
    "tQjWRjzhWuC3QPARdUm5gGkLZgRSkQRyVQ0cJWkulxuzBexAX1r81O+iz6e19ugq\n"
    "GUyODxmWDYzVJqMa4qTmfeG7mRkuftrm0mrEWcUSZ7Y2Y5LkLGs1ZcQ2G7oZEtbU\n"
    "LjlOgxZcyOyuJ8FuBtXtrOtG07Fbw0GVo0deiqknCcM0Mi873xALRuIdfc9KL3EB\n"
    "9pHs6aHyEbqGfsBr1yx6j1AHD+2ZWxb1rPCxwBBmlT2n2uOYfkRuRvD3yi2LLQX+\n"
    "PDXEfhaIu/lekUHyIOt93DycmkTH/eqr/aWjKlHFkQKBgQD8WgecXDTuPWX1JOXk\n"
    "hgIWLqD5PElIuJIl2IYbVVslDViPugPAmt3tvfUlIokexD8gvFiRZ/D6DEwh9sKp\n"
    "w5rTukRrhLBT1dAmzNk3cPMa4vC1y89RAwprm17oJL0mEHGyFa8SOkoRlqX+toyb\n"
    "atIKFDF515dXWTGlMTiV83zHXwKBgQDq2ZDNYBLLZgf0w5NO8CL6n8kic7BuXwS/\n"
    "yOz8snp4BEHg6zXbiexgDZP/PD56nyKz/7w+L9rV3jzunXThFfrlaRyAtNg2Pd78\n"
    "Gxie4phmYJ7H0GEtnBxGRtfhpNaI/cSVgFlabk4E8ZsOdt3T7qYhvtjVaY/ZqnXZ\n"
    "C+iDtxek+QKBgHS+dczpf3dEIloR8uUQ9gArHD+Rm1mwQbSOgiQ9DciLbxA2/yto\n"
    "ugkVm0bNvl/kbEwJtnO2gW48qDACR7ZgDs2q26JeJXDzdrOsi0Ux1NX4bWG9YzDY\n"
    "VkbH/1UoQfVNIDxB+ddV7hRK/IplC5GPDpKpGuaCTbqUQfppVgNRPMXDAoGBAKK0\n"
    "MJnYoazA92ofxQK/Y7x2zwZLWERfGA82yNkZXbegW3PUAPAkUdsKDSqbNj4F2ikS\n"
    "V/xNczMQUO6Pr9XxQG2HsPOKVvDdVIscyqXpHuRutBKCz5ClwwD9O7tcVDV8eqGI\n"
    "1l7MZMkQCfivaWfWwspGSjswczS93/+LPH9kbcEhAoGALGaj1Ns08k2Dz+YFC962\n"
    "cgfub8+IPOwBvyzs0200flkcUoT0bdQ4/p6Qcwjk+fpjwkSexJAE2S1OVaH/1DUm\n"
    "izxkW5GOY0nQ9CmY0nIpBNmkUMHKqSYndWPALbZFFloq+yUrKdoo8VDhzaoTlKFo\n"
    "a4H4TiaFRLBGcdY2XF0T4c4=\n"
    "-----END PRIVATE KEY-----\n";

// httplib::SSLServer wants file paths — drop the PEMs into temp files.
class PemFiles {
public:
    PemFiles() {
        const auto dir = std::filesystem::temp_directory_path();
        cert_ = (dir / "bc_tls_smoke_cert.pem").string();
        key_  = (dir / "bc_tls_smoke_key.pem").string();
        write(cert_, kCertPem);
        write(key_, kKeyPem);
    }
    ~PemFiles() {
        std::remove(cert_.c_str());
        std::remove(key_.c_str());
    }
    const char* cert() const { return cert_.c_str(); }
    const char* key() const { return key_.c_str(); }

private:
    static void write(const std::string& path, const char* body) {
        if (auto* f = std::fopen(path.c_str(), "w")) {
            std::fputs(body, f);
            std::fclose(f);
        }
    }
    std::string cert_;
    std::string key_;
};

} // namespace

TEST(TlsSmoke, HttpsUrlReachesSslServer) {
    PemFiles pems;
    httplib::SSLServer server(pems.cert(), pems.key());
    ASSERT_TRUE(server.is_valid()) << "SSLServer rejected the test certificate";

    server.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
    });

    const int port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    std::thread server_thread([&server] { server.listen_after_bind(); });

    server.wait_until_ready();

    // The exact form a user passes to --via, scheme included.
    httplib::Client cli("https://127.0.0.1:" + std::to_string(port));
    cli.enable_server_certificate_verification(false);  // self-signed fixture
    cli.set_connection_timeout(5);

    const auto res = cli.Get("/ping");
    server.stop();
    server_thread.join();

    ASSERT_TRUE(res) << "https request failed: " << httplib::to_string(res.error());
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->body, "pong");
}

// Plain host:port and http:// forms must keep working after the switch to
// scheme-aware client construction.
TEST(TlsSmoke, PlainHttpStillWorks) {
    httplib::Server server;
    server.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
    });

    const int port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    std::thread server_thread([&server] { server.listen_after_bind(); });

    server.wait_until_ready();

    for (const std::string base : {"http://127.0.0.1:" + std::to_string(port),
                                   "127.0.0.1:" + std::to_string(port)}) {
        httplib::Client cli(base);
        cli.set_connection_timeout(5);
        const auto res = cli.Get("/ping");
        ASSERT_TRUE(res) << "request via '" << base << "' failed";
        EXPECT_EQ(res->body, "pong");
    }

    server.stop();
    server_thread.join();
}
