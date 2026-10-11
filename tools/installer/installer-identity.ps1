# Identity inspection is deliberately independent of current certificate trust.
# Recovery authenticates historical bytes against protected state before signing
# a fresh package; normal activation still performs full Windows trust checks.
Set-StrictMode -Version Latest

function Get-IdentityCertificateSha256
{
    param([Parameter(Mandatory = $true)]
        [Security.Cryptography.X509Certificates.X509Certificate2]$Certificate)

    $hasher = [Security.Cryptography.SHA256]::Create()
    try
    {
        return ([BitConverter]::ToString($hasher.ComputeHash($Certificate.RawData))).Replace('-', '')
    }
    finally
    {
        $hasher.Dispose()
    }
}

function Read-IdentityPackageSigner
{
    param([Parameter(Mandatory = $true)][string]$Path)

    Add-Type -AssemblyName System.Security
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try
    {
        $entry = $archive.GetEntry('AppxSignature.p7x')
        if ($null -eq $entry -or $entry.Length -lt 5 -or $entry.Length -gt 1048576)
        {
            throw 'The recorded package has no readable AppX signature.'
        }
        $stream = $entry.Open()
        $buffer = New-Object IO.MemoryStream
        try
        {
            $stream.CopyTo($buffer)
            $bytes = $buffer.ToArray()
        }
        finally
        {
            $stream.Dispose()
            $buffer.Dispose()
        }
        if ($bytes[0] -ne 0x50 -or $bytes[1] -ne 0x4b -or
            $bytes[2] -ne 0x43 -or $bytes[3] -ne 0x58)
        {
            throw 'The recorded package has an invalid AppX signature header.'
        }
        $encoded = New-Object byte[] ($bytes.Length - 4)
        [Array]::Copy($bytes, 4, $encoded, 0, $encoded.Length)
        $message = New-Object Security.Cryptography.Pkcs.SignedCms
        $message.Decode($encoded)
        $message.CheckSignature($true)
        if ($message.SignerInfos.Count -ne 1 -or
            $null -eq $message.SignerInfos[0].Certificate)
        {
            throw 'The recorded package must have exactly one signing certificate.'
        }
        return [Security.Cryptography.X509Certificates.X509Certificate2]::new(
            $message.SignerInfos[0].Certificate.RawData)
    }
    finally
    {
        $archive.Dispose()
    }
}

function Assert-RecordedPackageSigner
{
    param([Parameter(Mandatory = $true)][object]$State,
        [Parameter(Mandatory = $true)][string]$PackagePath)

    foreach ($name in @('packageSha256', 'certificateSha256', 'certificateThumbprint', 'publisher'))
    {
        if ($null -eq $State.PSObject.Properties[$name])
        {
            throw "Historical identity evidence is missing: $name"
        }
    }
    if ([string]$State.packageSha256 -notmatch '^[0-9A-Fa-f]{64}$' -or
        (Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash -ine
            [string]$State.packageSha256)
    {
        throw 'The historical package does not match protected install state.'
    }
    $certificate = Read-IdentityPackageSigner -Path $PackagePath
    try
    {
        if ($certificate.Thumbprint -ine [string]$State.certificateThumbprint -or
            (Get-IdentityCertificateSha256 -Certificate $certificate) -ine
                [string]$State.certificateSha256 -or
            $certificate.Subject -cne [string]$State.publisher)
        {
            throw 'The historical signer does not match protected install state.'
        }
    }
    finally
    {
        $certificate.Dispose()
    }
}

function Read-RollbackIdentityJournal
{
    param([Parameter(Mandatory = $true)][object]$State,
        [Parameter(Mandatory = $true)][string]$InstallRoot,
        [switch]$RequireReady)

    if ([int]$State.schema -lt 3)
    {
        return $null
    }
    if ($null -eq $State.PSObject.Properties['rollbackIdentity'] -or
        $null -eq $State.PSObject.Properties['rollbackInstallState'])
    {
        throw 'The transaction is missing its rollback identity evidence.'
    }
    if ($null -eq $State.oldInstallState)
    {
        if ($null -ne $State.rollbackIdentity -or $null -ne $State.rollbackInstallState)
        {
            throw 'A first installation cannot claim a previous rollback identity.'
        }
        return $null
    }
    if ($null -eq $State.rollbackIdentity)
    {
        if ($RequireReady)
        {
            throw 'The rollback identity was not prepared before replacement.'
        }
        return $null
    }
    $root = [IO.Path]::GetFullPath((Join-Path $InstallRoot (
        '.rollback\' + [string]$State.transactionId + '\identity')))
    $identity = $State.rollbackIdentity
    if ([string]$identity.transactionId -notmatch '^[0-9A-Fa-f]{32}$' -or
        [IO.Path]::GetFullPath([string]$identity.root) -ne $root)
    {
        throw 'The rollback identity has an invalid transaction or location.'
    }
    Assert-NoReparsePath -Path $root -AllowMissing
    $journalPath = Join-Path $root 'Installer\PREPARE-STATE.json'
    if (-not (Test-Path -LiteralPath $journalPath -PathType Leaf))
    {
        if ($RequireReady -or $null -ne $State.rollbackInstallState)
        {
            throw 'The rollback certificate journal is missing.'
        }
        return $null
    }
    Assert-ProtectedStateAcl -Path $journalPath
    $journal = Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json
    if ([int]$journal.schema -ne 2 -or
        [string]$journal.transactionId -ne [string]$identity.transactionId -or
        [string]$journal.userSid -ne [string]$State.userSid -or
        [string]$journal.packageName -cne [string]$State.packageName -or
        [string]$journal.applicationId -cne [string]$State.applicationId -or
        [string]$journal.productVersion -cne [string]$State.oldInstallState.productVersion -or
        [string]$journal.packageVersion -cne [string]$State.oldInstallState.packageVersion -or
        [string]$journal.publisher -cne [string]$State.publisher -or
        [string]$journal.certificateSanUri -cne
            "urn:bafx:installer:$([string]$identity.transactionId)" -or
        (Get-StateDigest -Value $journal) -ine [string]$journal.stateDigest)
    {
        throw 'The rollback certificate journal does not match its transaction.'
    }
    if ([string]$journal.certificatePhase -eq 'creating')
    {
        if ($RequireReady)
        {
            throw 'The rollback certificate transaction is not ready.'
        }
        return $journal
    }
    if ([string]$journal.certificatePhase -ne 'ready' -or
        $null -eq $State.rollbackInstallState)
    {
        if ($RequireReady)
        {
            throw 'The signed rollback identity is incomplete.'
        }
        return $journal
    }
    $candidate = $State.rollbackInstallState
    $old = $State.oldInstallState
    $fileName = [string]$candidate.packageFile
    if ([IO.Path]::GetFileName($fileName) -cne $fileName -or
        $fileName.Contains('..') -or $fileName -notmatch '\.msix$')
    {
        throw 'The rollback package has an unsafe file name.'
    }
    foreach ($name in @('packageName', 'applicationId', 'publisher', 'productVersion',
            'packageVersion', 'packageFullName', 'packageFamilyName', 'installedUserSid',
            'externalLocation', 'hostFile', 'hostSha256', 'templateSha256'))
    {
        if ([string]$candidate.$name -cne [string]$old.$name)
        {
            throw "The rollback identity changed the previous installation: $name"
        }
    }
    foreach ($name in @('transactionId', 'packageFile', 'packageSha256',
            'certificateThumbprint', 'certificateSha256', 'certificateNotAfterUtc'))
    {
        if ([string]$candidate.$name -cne [string]$journal.$name)
        {
            throw "The rollback state does not match its signing journal: $name"
        }
    }
    if ((Get-StateDigest -Value $candidate) -ine [string]$candidate.stateDigest)
    {
        throw 'The rollback install-state digest does not match its content.'
    }
    $packagePath = Join-Path (Join-Path $root 'Identity') ([string]$candidate.packageFile)
    if ([IO.Path]::GetFullPath([string]$journal.packagePath) -ne
        [IO.Path]::GetFullPath($packagePath))
    {
        throw 'The rollback package escaped its protected material directory.'
    }
    if ($RequireReady)
    {
        Assert-NoReparsePath -Path $packagePath
        Assert-RecordedPackageSigner -State $candidate -PackagePath $packagePath
        $signature = Get-AuthenticodeSignature -LiteralPath $packagePath
        if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
            $null -eq $signature.SignerCertificate -or
            $signature.SignerCertificate.Thumbprint -ine [string]$candidate.certificateThumbprint -or
            (Test-Path -LiteralPath ("Cert:\LocalMachine\My\" + [string]$candidate.certificateThumbprint)))
        {
            throw 'The prepared rollback package is not trusted or retains its signing key.'
        }
    }
    return $journal
}

function Prepare-RollbackIdentity
{
    param([Parameter(Mandatory = $true)][string]$InstallRoot,
        [Parameter(Mandatory = $true)][string]$PayloadRoot,
        [Parameter(Mandatory = $true)][string]$PendingPath)

    $parent = Get-Content -LiteralPath $PendingPath -Raw | ConvertFrom-Json
    if ($null -eq $parent.oldInstallState)
    {
        return
    }
    $old = $parent.oldInstallState
    if ([int]$old.schema -ne 2 -or
        $null -eq $old.PSObject.Properties['templateSha256'] -or
        [string]$old.templateSha256 -notmatch '^[0-9A-Fa-f]{64}$')
    {
        throw 'The previous installation lacks a protected unsigned-template hash for rollback.'
    }
    $oldPackagePath = Join-Path (Join-Path $InstallRoot 'Identity') ([string]$old.packageFile)
    Assert-RecordedPackageSigner -State $old -PackagePath $oldPackagePath
    $baseName = 'CialloKing.BaClickFxDesktop-' + [string]$old.packageVersion
    $templatePath = Join-Path (Join-Path $InstallRoot 'Identity') ($baseName + '.unsigned.msix')
    Assert-NoReparsePath -Path $templatePath
    if ((Get-FileHash -LiteralPath $templatePath -Algorithm SHA256).Hash -ine
        [string]$old.templateSha256)
    {
        throw 'The previous unsigned template does not match protected install state.'
    }
    if ((Get-IdentityTemplateContentHash -Path $templatePath) -ne
        (Get-IdentityTemplateContentHash -Path $oldPackagePath))
    {
        throw 'The previous unsigned template content does not match its recorded signed package.'
    }
    $metadataPath = Join-Path (Join-Path $InstallRoot 'Identity') ($baseName + '.identity-template.json')
    Assert-NoReparsePath -Path $metadataPath
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
    foreach ($name in @('productVersion', 'packageVersion', 'packageName',
            'applicationId', 'publisher', 'templateSha256', 'hostSha256'))
    {
        if ([string]$metadata.$name -cne [string]$old.$name)
        {
            throw "The previous template metadata does not match protected state: $name"
        }
    }
    $root = Join-Path $InstallRoot ('.rollback\' + [string]$parent.transactionId + '\identity')
    $childTransaction = [Guid]::NewGuid().ToString('N')
    # Persist ownership before creating the child certificate or its material.
    # The original state stays immutable; a ready rollback snapshot is separate.
    $parent.rollbackIdentity = [pscustomobject]@{ root = $root; transactionId = $childTransaction }
    Write-ProtectedJson -Path $PendingPath -Value $parent -ReadSid ([string]$parent.userSid)
    Assert-NoReparsePath -Path $root -AllowMissing
    foreach ($directory in @($root, (Join-Path $root 'Identity'), (Join-Path $root 'Installer')))
    {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        Assert-NoReparsePath -Path $directory
    }
    $hostPath = Join-Path $InstallRoot 'ba-click-fx-desktop.exe'
    Copy-VerifiedInstallerFile -SourcePath $hostPath `
        -DestinationPath (Join-Path $root 'ba-click-fx-desktop.exe') `
        -ExpectedBytes ([Int64](Get-Item -LiteralPath $hostPath).Length) `
        -ExpectedSha256 ([string]$old.hostSha256)
    Copy-VerifiedInstallerFile -SourcePath $templatePath `
        -DestinationPath (Join-Path (Join-Path $root 'Identity') ([string]$metadata.templateFile)) `
        -ExpectedBytes ([Int64](Get-Item -LiteralPath $templatePath).Length) `
        -ExpectedSha256 ([string]$old.templateSha256)
    $newMetadataPath = Join-Path (Join-Path $PayloadRoot 'Identity') (
        'CialloKing.BaClickFxDesktop-' + [string]$parent.packageVersion + '.identity-template.json')
    $newMetadata = Get-Content -LiteralPath $newMetadataPath -Raw | ConvertFrom-Json
    $signerPath = Join-Path $PayloadRoot 'Installer\BAFX.IdentitySigner.exe'
    Copy-VerifiedInstallerFile -SourcePath $signerPath `
        -DestinationPath (Join-Path $root 'Installer\BAFX.IdentitySigner.exe') `
        -ExpectedBytes ([Int64](Get-Item -LiteralPath $signerPath).Length) `
        -ExpectedSha256 ([string]$newMetadata.signerSha256)
    $metadata.signerSha256 = [string]$newMetadata.signerSha256
    Write-Utf8NoBom -Path (Join-Path (Join-Path $root 'Identity') ($baseName + '.identity-template.json')) `
        -Content ($metadata | ConvertTo-Json -Depth 8)
    $seed = [ordered]@{
        schema = 2
        stateKind = 'prepare'
        commitState = 'prepared'
        filesCommitted = $false
        userSid = [string]$parent.userSid
        transactionId = $childTransaction
        packageName = [string]$old.packageName
        applicationId = [string]$old.applicationId
        publisher = [string]$old.publisher
        productVersion = [string]$old.productVersion
        packageVersion = [string]$old.packageVersion
        templateSha256 = [string]$old.templateSha256
        preexistingPackageFullNames = @()
        oldInstallState = $null
    }
    $childPath = Join-Path $root 'Installer\PREPARE-STATE.json'
    $null = Assert-IdentityPayload -InstallRoot $root -PendingStatePath $childPath `
        -PendingStateSeed $seed -IdentityProductVersion ([string]$old.productVersion) `
        -IdentityPackageVersion ([string]$old.packageVersion)
    $child = Get-Content -LiteralPath $childPath -Raw | ConvertFrom-Json
    $candidate = Get-StatePropertiesWithoutDigest -Value $old
    foreach ($name in @('transactionId', 'packageFile', 'packageSha256',
            'certificateThumbprint', 'certificateSha256', 'certificateNotAfterUtc', 'certificateOwnership'))
    {
        $candidate[$name] = $child.$name
    }
    $candidate.certificateInstalledBySetup = -not [bool]$child.certificateWasPresent
    $candidate.ownedCertificateThumbprints = Join-Ledger -Separator Comma -Values @(
        @(Get-ExplicitlyOwnedCertificateThumbprints -State $old) +
        @(Get-ExplicitlyOwnedCertificateThumbprints -State $child))
    $candidate.ownedPackageFiles = Join-Ledger -Separator Pipe -Values @(
        @(Split-Ledger -Value $old.ownedPackageFiles -Separator Pipe) + @([string]$child.packageFile))
    $parent = Get-Content -LiteralPath $PendingPath -Raw | ConvertFrom-Json
    $parent.rollbackInstallState = New-StateWithDigest -Value $candidate
    $parent.ownedCertificateThumbprints = Join-Ledger -Separator Comma -Values @(
        @(Split-Ledger -Value $parent.ownedCertificateThumbprints -Separator Comma) +
        @(Get-ExplicitlyOwnedCertificateThumbprints -State $child))
    Write-ProtectedJson -Path $PendingPath -Value $parent -ReadSid ([string]$parent.userSid)
    $parent = Get-Content -LiteralPath $PendingPath -Raw | ConvertFrom-Json
    $null = Read-RollbackIdentityJournal -State $parent -InstallRoot $InstallRoot -RequireReady
}

function Copy-ReadyRollbackPackage
{
    param([Parameter(Mandatory = $true)][object]$State,
        [Parameter(Mandatory = $true)][string]$InstallRoot)

    $journal = Read-RollbackIdentityJournal -State $State -InstallRoot $InstallRoot -RequireReady
    if ($null -eq $journal)
    {
        return
    }
    $destination = Join-Path (Join-Path $InstallRoot 'Identity') ([string]$journal.packageFile)
    Copy-VerifiedInstallerFile -SourcePath ([string]$journal.packagePath) `
        -DestinationPath $destination `
        -ExpectedBytes ([Int64](Get-Item -LiteralPath $journal.packagePath).Length) `
        -ExpectedSha256 ([string]$journal.packageSha256)
}

function Complete-RollbackIdentity
{
    param([Parameter(Mandatory = $true)][object]$State,
        [Parameter(Mandatory = $true)][string]$InstallRoot)

    if ([int]$State.schema -lt 3)
    {
        return
    }
    $journal = Read-RollbackIdentityJournal -State $State -InstallRoot $InstallRoot
    if ($null -eq $journal)
    {
        return
    }
    $selected = $null -ne $State.PSObject.Properties['filesCommitted'] -and [bool]$State.filesCommitted
    if ($selected)
    {
        $null = Read-RollbackIdentityJournal -State $State -InstallRoot $InstallRoot -RequireReady
        $candidate = $State.rollbackInstallState
        $registered = @(Get-AppxPackage -User ([string]$State.userSid) `
            -Name ([string]$candidate.packageName) -ErrorAction Stop |
            Where-Object { [string]$_.PackageFullName -eq [string]$candidate.packageFullName })
        if ($registered.Count -ne 1)
        {
            throw 'The re-signed previous package is not registered; rollback evidence was retained.'
        }
        Assert-IdentityIntegrityMaterial -State $candidate -InstallRoot $InstallRoot `
            -PackagePath (Join-Path (Join-Path $InstallRoot 'Identity') ([string]$candidate.packageFile))
        Write-ProtectedInstallState -Path (Join-Path $InstallRoot 'Installer\INSTALL-STATE.json') `
            -Value $candidate -ReadSid ([string]$State.userSid)
        return
    }
    # Before file commit the old registration was never replaced. Recover the
    # independently owned child certificate without changing the old state.
    if ([string]$journal.certificatePhase -eq 'creating')
    {
        Recover-CreatingCertificate -State $journal
    }
    else
    {
        Remove-PreparedCertificateIfUnused -State $journal -InstallRoot $InstallRoot
    }
}
