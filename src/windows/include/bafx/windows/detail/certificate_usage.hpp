#pragma once

#include <windows.h>
#include <wincrypt.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace bafx::windows::detail
{

[[nodiscard]] inline HRESULT certificateCodeSigningUsage(
    PCCERT_CONTEXT certificate,
    decltype(&CertGetEnhancedKeyUsage) query = &CertGetEnhancedKeyUsage)
{
    DWORD size = 0U;
    if (!query(certificate, 0U, nullptr, &size))
    {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
    }
    std::vector<std::byte> buffer(size);
    auto* usage = reinterpret_cast<PCERT_ENHKEY_USAGE>(buffer.data());
    if (!query(certificate, 0U, usage, &size))
    {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
    }
    constexpr std::string_view codeSigningOid = "1.3.6.1.5.5.7.3.3";
    for (DWORD index = 0U; index < usage->cUsageIdentifier; ++index)
    {
        if (usage->rgpszUsageIdentifier[index] != nullptr
            && codeSigningOid == usage->rgpszUsageIdentifier[index])
        {
            return S_OK;
        }
    }
    // A missing code-signing EKU is not evidence that the certificate expired.
    return CERT_E_WRONG_USAGE;
}

}
