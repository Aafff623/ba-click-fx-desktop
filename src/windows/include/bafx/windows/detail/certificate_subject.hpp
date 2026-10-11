#pragma once

#include <windows.h>
#include <wincrypt.h>

#include <string>

namespace bafx::windows::detail
{

[[nodiscard]] inline HRESULT formatCertificateSubject(
    PCCERT_CONTEXT certificate,
    std::wstring& subject)
{
    if (certificate == nullptr || certificate->pCertInfo == nullptr)
    {
        return E_INVALIDARG;
    }
    // Equal X.500 names can use different ASN.1 string types. Format the
    // complete name so encoding differences do not weaken identity comparison.
    const DWORD size = CertNameToStrW(
        X509_ASN_ENCODING,
        &certificate->pCertInfo->Subject,
        CERT_X500_NAME_STR,
        nullptr,
        0U);
    if (size == 0U)
    {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
    }
    subject.resize(size);
    if (CertNameToStrW(
            X509_ASN_ENCODING,
            &certificate->pCertInfo->Subject,
            CERT_X500_NAME_STR,
            subject.data(),
            size) == 0U)
    {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
    }
    if (!subject.empty() && subject.back() == L'\0')
    {
        subject.pop_back();
    }
    return S_OK;
}

}
