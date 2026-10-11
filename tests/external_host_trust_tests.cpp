#include "test_support.hpp"

#include "bafx/windows/external_host_trust.hpp"
#include "bafx/windows/detail/certificate_subject.hpp"
#include "bafx/windows/detail/certificate_usage.hpp"

#include <string>
#include <vector>

namespace
{

std::wstring formattedSubject(const wchar_t* name, const DWORD flags)
{
    DWORD size = 0U;
    BAFX_CHECK(CertStrToNameW(X509_ASN_ENCODING, name,
        CERT_X500_NAME_STR | flags, nullptr, nullptr, &size, nullptr));
    std::vector<BYTE> bytes(size);
    BAFX_CHECK(CertStrToNameW(X509_ASN_ENCODING, name,
        CERT_X500_NAME_STR | flags, nullptr, bytes.data(), &size, nullptr));
    CERT_INFO info{};
    info.Subject = {size, bytes.data()};
    CERT_CONTEXT context{};
    context.pCertInfo = &info;
    std::wstring subject;
    BAFX_CHECK(SUCCEEDED(bafx::windows::detail::formatCertificateSubject(&context, subject)));
    return subject;
}

BAFX_TEST(certificate_usage_preserves_query_error_and_wrong_usage)
{
    const auto failedQuery = +[](PCCERT_CONTEXT, DWORD, PCERT_ENHKEY_USAGE, DWORD*) -> BOOL
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    };
    const auto missingUsage = +[](PCCERT_CONTEXT, DWORD, PCERT_ENHKEY_USAGE usage, DWORD* size) -> BOOL
    {
        *size = sizeof(CERT_ENHKEY_USAGE);
        if (usage != nullptr)
        {
            *usage = {};
        }
        return TRUE;
    };
    BAFX_CHECK(bafx::windows::detail::certificateCodeSigningUsage(nullptr, failedQuery)
        == E_ACCESSDENIED);
    BAFX_CHECK(bafx::windows::detail::certificateCodeSigningUsage(nullptr, missingUsage)
        == CERT_E_WRONG_USAGE);
}

}

BAFX_TEST(certificate_subject_normalizes_encoding_without_ignoring_identity)
{
    constexpr wchar_t expected[] = L"CN=BaClickFx.Local";
    BAFX_CHECK(formattedSubject(expected, 0U) == expected);
    BAFX_CHECK(formattedSubject(expected, CERT_NAME_STR_FORCE_UTF8_DIR_STR_FLAG) == expected);
    BAFX_CHECK(formattedSubject(L"CN=OtherPublisher", 0U) != expected);
    BAFX_CHECK(formattedSubject(L"CN=BaClickFx.Local, O=OtherPublisher", 0U) != expected);
}

BAFX_TEST(external_host_trust_valid_certificate_is_trusted)
{
    bafx::windows::ExternalHostTrustResult result{};
    result.status = bafx::windows::ExternalHostTrustStatus::Trusted;
    result.error = S_OK;

    BAFX_CHECK(bafx::windows::externalHostTrusted(result));
    BAFX_CHECK(
        bafx::windows::externalHostTrustStatusName(result.status)
        == "trusted");
    const std::string diagnostic =
        bafx::windows::externalHostTrustDiagnostic(result);
    BAFX_CHECK(diagnostic.find("Package.ExternalHostTrust=trusted")
        != std::string::npos);
    BAFX_CHECK(diagnostic.find("Certificate") == std::string::npos);
}

BAFX_TEST(external_host_trust_expired_certificate_fails_closed)
{
    bafx::windows::ExternalHostTrustResult result{};
    result.status = bafx::windows::ExternalHostTrustStatus::CertificateInvalid;
    result.error = CERT_E_EXPIRED;
    result.certificateCheck = "validity";
    result.certificateReason = "expired";

    BAFX_CHECK(!bafx::windows::externalHostTrusted(result));
    BAFX_CHECK(
        bafx::windows::externalHostTrustStatusName(result.status)
        == "certificate-invalid");
    const std::string diagnostic = bafx::windows::externalHostTrustDiagnostic(result);
    BAFX_CHECK(diagnostic.find("CertificateCheck=validity;CertificateReason=expired")
        != std::string::npos);
}

BAFX_TEST(external_host_trust_state_pair_mismatch_is_named)
{
    bafx::windows::ExternalHostTrustResult result{};
    result.status = bafx::windows::ExternalHostTrustStatus::StatePairMismatch;
    result.error = E_ACCESSDENIED;

    BAFX_CHECK(!bafx::windows::externalHostTrusted(result));
    BAFX_CHECK(
        bafx::windows::externalHostTrustStatusName(result.status)
        == "state-pair-mismatch");
}
