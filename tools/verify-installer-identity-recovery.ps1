[CmdletBinding()]
param([string]$RepositoryRoot)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($RepositoryRoot))
{
    $RepositoryRoot = Join-Path $PSScriptRoot '..'
}
$repositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
if ($PSVersionTable.PSEdition -ne 'Desktop')
{
    # Install state is written by Windows PowerShell 5.1. PowerShell 7 can
    # deserialize ISO dates as DateTime, changing the protected JSON digest.
    # Exercise the same serialization and crypto runtime as the installer.
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -RepositoryRoot $repositoryRoot
    if ($LASTEXITCODE -ne 0)
    {
        throw "Identity recovery verification failed in Windows PowerShell: $LASTEXITCODE"
    }
    return
}
Add-Type -AssemblyName System.Security
Add-Type -AssemblyName System.IO.Compression.FileSystem
. (Join-Path $repositoryRoot 'tools/installer/installer-state.ps1')
. (Join-Path $repositoryRoot 'tools/installer/installer-identity.ps1')

function Assert-Probe
{
    param([bool]$Condition, [string]$Message)
    if (-not $Condition)
    {
        throw $Message
    }
}

function Assert-ProbeFailure
{
    param([scriptblock]$Action)
    $failed = $false
    try
    {
        & $Action | Out-Null
    }
    catch
    {
        $failed = $true
    }
    Assert-Probe $failed 'A damaged identity or incomplete rollback was accepted.'
}

function Get-ProbeFunction
{
    param([string]$File, [string[]]$Names)
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $repositoryRoot $File), [ref]$tokens, [ref]$errors)
    Assert-Probe ($errors.Count -eq 0) "Production script parse failed: $File"
    foreach ($name in $Names)
    {
        $node = $ast.Find({
            param($candidate)
            $candidate -is [Management.Automation.Language.FunctionDefinitionAst] -and $candidate.Name -eq $name
        }, $true)
        Assert-Probe ($null -ne $node) "Production function not found: $name"
        $node.Extent.Text
    }
}

if ($null -eq ('BafxRecoveryProbeCertificate' -as [type]))
{
    # CNG keys stay in memory; these fixtures never modify certificate stores.
    Add-Type -TypeDefinition @'
using System;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

public static class BafxRecoveryProbeCertificate
{
    public static X509Certificate2 Create(bool expired)
    {
        using (var key = new RSACng(2048))
        {
            var request = new CertificateRequest("CN=BaClickFx.Local", key,
                HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddYears(-3),
                expired ? DateTimeOffset.UtcNow.AddDays(-1) : DateTimeOffset.UtcNow.AddYears(2));
        }
    }
}
'@
}

function Write-ProbeSignedPackage
{
    param([string]$Template, [string]$Destination,
        [Security.Cryptography.X509Certificates.X509Certificate2]$Certificate)
    Copy-Item -LiteralPath $Template -Destination $Destination -Force
    $cms = New-Object Security.Cryptography.Pkcs.SignedCms (
        [Security.Cryptography.Pkcs.ContentInfo]::new([Text.Encoding]::UTF8.GetBytes('isolated recovery signature fixture')))
    $signer = New-Object Security.Cryptography.Pkcs.CmsSigner $Certificate
    $signer.IncludeOption = [Security.Cryptography.X509Certificates.X509IncludeOption]::EndCertOnly
    $cms.ComputeSignature($signer)
    $signature = [byte[]](@(0x50, 0x4b, 0x43, 0x58) + $cms.Encode())
    $archive = [IO.Compression.ZipFile]::Open($Destination, [IO.Compression.ZipArchiveMode]::Update)
    try
    {
        $stream = $archive.CreateEntry('AppxSignature.p7x').Open()
        try
        {
            $stream.Write($signature, 0, $signature.Length)
        }
        finally
        {
            $stream.Dispose()
        }
    }
    finally
    {
        $archive.Dispose()
    }
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('bafx-identity-recovery-' + [Guid]::NewGuid().ToString('N'))
$expired = $null
$fresh = $null
$module = $null
try
{
    $expired = [BafxRecoveryProbeCertificate]::Create($true)
    $fresh = [BafxRecoveryProbeCertificate]::Create($false)
    New-Item -ItemType Directory -Path (Join-Path $root 'Identity'), (Join-Path $root 'Installer') -Force | Out-Null
    $hostPath = Join-Path $root 'ba-click-fx-desktop.exe'
    [IO.File]::WriteAllText($hostPath, 'unchanged historical Host fixture')
    $content = Join-Path $root 'template-content'
    New-Item -ItemType Directory -Path $content | Out-Null
    Copy-Item -LiteralPath $hostPath -Destination $content
    [IO.File]::WriteAllText((Join-Path $content 'AppxBlockMap.xml'),
        '<BlockMap><File Name="ba-click-fx-desktop.exe" Size="1" LfhSize="1"><Block Hash="fixture"/></File></BlockMap>')
    $base = 'CialloKing.BaClickFxDesktop-0.2.19.0'
    $template = Join-Path (Join-Path $root 'Identity') ($base + '.unsigned.msix')
    [IO.Compression.ZipFile]::CreateFromDirectory($content, $template)
    $package = Join-Path (Join-Path $root 'Identity') ($base + '-old.msix')
    Write-ProbeSignedPackage $template $package $expired
    $old = [pscustomobject][ordered]@{
        schema = 2; transactionId = '1' * 32
        packageName = 'CialloKing.BaClickFxDesktop'; applicationId = 'BaClickFxDesktop'
        publisher = 'CN=BaClickFx.Local'; productVersion = '0.2.19'; packageVersion = '0.2.19.0'
        packageFullName = 'CialloKing.BaClickFxDesktop_0.2.19.0_x64__fixture'
        packageFamilyName = 'CialloKing.BaClickFxDesktop_fixture'
        installedUserSid = 'S-1-5-21-1-2-3-1001'; externalLocation = $root
        hostFile = 'ba-click-fx-desktop.exe'; hostSha256 = (Get-FileHash $hostPath).Hash
        packageFile = [IO.Path]::GetFileName($package); packageSha256 = (Get-FileHash $package).Hash
        templateSha256 = (Get-FileHash $template).Hash
        certificateThumbprint = $expired.Thumbprint; certificateSha256 = Get-IdentityCertificateSha256 $expired
        certificateNotAfterUtc = $expired.NotAfter.ToUniversalTime().ToString('o')
        certificateInstalledBySetup = $true; certificateOwnership = 'installer-owned'
        ownedCertificateThumbprints = $expired.Thumbprint; ownedPackageFiles = [IO.Path]::GetFileName($package)
    }
    Assert-Probe ($expired.NotAfter -lt (Get-Date)) 'The historical certificate did not expire.'
    Assert-RecordedPackageSigner -State $old -PackagePath $package
    $old | Add-Member -NotePropertyName stateDigest -NotePropertyValue (Get-StateDigest $old)
    $oldJson = $old | ConvertTo-Json -Depth 12
    $metadata = [pscustomobject]@{
        schema = 3; identityMode = 'target-machine-self-signed'
        productVersion = $old.productVersion; packageVersion = $old.packageVersion
        packageName = $old.packageName; applicationId = $old.applicationId; publisher = $old.publisher
        hostSha256 = $old.hostSha256; templateSha256 = $old.templateSha256
        templateFile = [IO.Path]::GetFileName($template); signerFile = 'Installer/BAFX.IdentitySigner.exe'
        signerSha256 = 'A' * 64
    }
    [IO.File]::WriteAllText((Join-Path (Join-Path $root 'Identity') ($base + '.identity-template.json')),
        ($metadata | ConvertTo-Json))
    $payload = Join-Path $root '.staging\current'
    New-Item -ItemType Directory -Path (Join-Path $payload 'Identity'), (Join-Path $payload 'Installer') -Force | Out-Null
    $signerPath = Join-Path $payload 'Installer\BAFX.IdentitySigner.exe'
    [IO.File]::WriteAllText($signerPath, 'signing boundary fixture')
    [IO.File]::WriteAllText((Join-Path $payload 'Identity\CialloKing.BaClickFxDesktop-0.2.20.0.identity-template.json'),
        (@{ signerSha256 = (Get-FileHash $signerPath).Hash } | ConvertTo-Json))
    $parent = [pscustomobject]@{
        schema = 3; stateKind = 'prepare'; transactionId = '2' * 32; filesCommitted = $false
        userSid = $old.installedUserSid; packageName = $old.packageName
        applicationId = $old.applicationId; publisher = $old.publisher; packageVersion = '0.2.20.0'
        productVersion = '0.2.20'; preexistingPackageFullNames = @($old.packageFullName)
        packageFile = 'new-candidate.msix'; packagePath = (Join-Path $root 'Identity\new-candidate.msix')
        oldInstallState = $old; rollbackIdentity = $null; rollbackInstallState = $null
        ownedCertificateThumbprints = $fresh.Thumbprint
    }
    $pending = Join-Path $root 'Installer\PREPARE-STATE.json'
    [IO.File]::WriteAllText($pending, ($parent | ConvertTo-Json -Depth 12))
    $functions = @(
        Get-ProbeFunction 'tools/installer/install-machine.ps1' @('New-StateWithDigest', 'Join-Ledger',
            'Get-ExplicitlyOwnedCertificateThumbprints', 'Write-Utf8NoBom', 'Copy-VerifiedInstallerFile',
            'Assert-FileHash', 'Get-IdentityTemplateContentHash', 'Get-CertificateSha256', 'Assert-IdentityIntegrityMaterial',
            'Assert-InstallStatePair', 'Assert-InstallStateObject', 'Read-OldInstallState',
            'Invoke-PendingRollbackCleanup', 'Remove-PendingPackageFiles')
        Get-ProbeFunction 'tools/installer/unregister-machine.ps1' @('Get-UninstallPhaseRank', 'Assert-InstallStateIntegrity')
        Get-ProbeFunction 'tools/installer/register-user-package.ps1' @('Restore-PreviousPackage',
            'Assert-PreviousPackageMaterial', 'Get-InstallerFileHash', 'Remove-PreviousPackageForReplacement',
            'Remove-NewPackages', 'Assert-PendingRollbackMayRemoveNewPackage', 'Get-InstallStatePairStatus')
        (Get-Command Write-ProbeSignedPackage).Definition.Insert(0, "function Write-ProbeSignedPackage {`n") + "`n}"
    ) -join "`n"
    $tokens = $null
    $errors = $null
    $registrationAst = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $repositoryRoot 'tools/installer/register-user-package.ps1'), [ref]$tokens, [ref]$errors)
    $registrationCore = $registrationAst.Find({
        param($node)
        $node -is [Management.Automation.Language.TryStatementAst] -and
            $node.Extent.Text.Contains("`$script:InstallerStep = 'inspect-current-package-registrations'")
    }, $true).Extent.Text
    $stub = @'
param($Root, $Functions, $Fresh)
. (Join-Path $Root 'tools/installer/installer-state.ps1')
. (Join-Path $Root 'tools/installer/installer-identity.ps1')
. ([scriptblock]::Create($Functions))
$script:Fresh = $Fresh
$script:Registered = @()
$script:Events = New-Object Collections.Generic.List[string]
$script:FailNextAdd = $false
$script:TrustMissing = $false
$script:FailCleanup = $false
function Assert-NoReparsePath { param($Path, [switch]$AllowMissing) }
function Assert-ProtectedStateAcl { param($Path) }
function Write-ProtectedJson { param($Path, $Value, $ReadSid)
    [IO.File]::WriteAllText($Path, ((New-StateWithDigest $Value) | ConvertTo-Json -Depth 12)) }
function Write-ProtectedInstallState { param($Path, $Value, $ReadSid)
    Write-ProtectedJson $Path $Value $ReadSid
    Copy-Item -LiteralPath $Path -Destination "$Path.bak" -Force }
function Get-AuthenticodeSignature { param($LiteralPath)
    [pscustomobject]@{ Status = [Management.Automation.SignatureStatus]::Valid; SignerCertificate = $script:Fresh } }
function Test-Path { param($LiteralPath, $PathType)
    if ($LiteralPath.StartsWith('Cert:\')) {
        return -not $script:TrustMissing -and -not $LiteralPath.StartsWith('Cert:\LocalMachine\My\') }
    Microsoft.PowerShell.Management\Test-Path @PSBoundParameters }
function Get-ChildItem { param($Path, $LiteralPath, [switch]$Recurse, [switch]$Force)
    if ($Path -eq 'Cert:\LocalMachine\TrustedPeople') {
        if (-not $script:TrustMissing) { $script:Fresh }; return }
    Microsoft.PowerShell.Management\Get-ChildItem @PSBoundParameters }
function Get-Item { param($LiteralPath, [switch]$Force)
    if ($LiteralPath.StartsWith('Cert:\')) { return $script:Fresh }
    Microsoft.PowerShell.Management\Get-Item @PSBoundParameters }
function Get-AppxPackage { [CmdletBinding()]param($User, $Name)
    foreach ($fullName in $script:Registered) { [pscustomobject]@{ PackageFullName = $fullName } } }
function Remove-AppxPackage { [CmdletBinding()]param($Package, $User)
    $script:Events.Add('remove:' + $Package)
    $script:Registered = @($script:Registered | Where-Object { $_ -ne $Package }) }
function Add-AppxPackage { param($Path, $ExternalLocation, [switch]$ForceApplicationShutdown,
    [switch]$ForceUpdateFromAnyVersion)
    $script:Events.Add('add:' + [IO.Path]::GetFileName($Path))
    if ($script:FailNextAdd) { $script:FailNextAdd = $false; throw 'Injected new-package registration failure.' }
    $script:Registered = @($script:ExpectedFullName) }
function Write-Result { param($Succeeded, $Package, $State, $ErrorMessage)
    $script:RegistrationSucceeded = $Succeeded }
function Stop-RegistrationWithFailure { param($ErrorRecord, $Step, $ResultErrorMessage)
    $script:FailureStep = $Step }
function Remove-PreparedCertificateIfUnused { param($State, $InstallRoot) }
function Remove-EmptyRollbackDirectories { param($InstallRoot) }
function Remove-CompletedRollbackEvidence { param($InstallRoot, $TransactionId)
    $evidence = Join-Path $InstallRoot ('.rollback\' + $TransactionId)
    if (Microsoft.PowerShell.Management\Test-Path -LiteralPath $evidence) {
        Remove-Item -LiteralPath $evidence -Recurse -Force }
    if ($script:FailCleanup) { $script:FailCleanup = $false; throw 'Injected interruption after evidence deletion.' } }
function Assert-IdentityPayload { param($InstallRoot, $PendingStatePath, $PendingStateSeed,
    $IdentityProductVersion, $IdentityPackageVersion)
    $journal = Get-StatePropertiesWithoutDigest $PendingStateSeed
    $journal.certificatePhase = 'ready'
    $journal.certificateSanUri = 'urn:bafx:installer:' + $journal.transactionId
    $journal.certificateThumbprint = $script:Fresh.Thumbprint
    $journal.certificateSha256 = Get-IdentityCertificateSha256 $script:Fresh
    $journal.certificateNotAfterUtc = $script:Fresh.NotAfter.ToUniversalTime().ToString('o')
    $journal.certificateWasPresent = $false; $journal.certificateOwnership = 'installer-owned'
    $journal.packageFile = 'CialloKing.BaClickFxDesktop-' + $IdentityPackageVersion + '-' + $script:Fresh.Thumbprint + '.msix'
    $journal.packagePath = Join-Path (Join-Path $InstallRoot 'Identity') $journal.packageFile
    $unsigned = Join-Path (Join-Path $InstallRoot 'Identity') ('CialloKing.BaClickFxDesktop-' + $IdentityPackageVersion + '.unsigned.msix')
    Write-ProbeSignedPackage $unsigned $journal.packagePath $script:Fresh
    $journal.packageSha256 = (Get-FileHash $journal.packagePath).Hash
    $journal.hostFile = 'ba-click-fx-desktop.exe'
    $journal.hostSha256 = (Get-FileHash (Join-Path $InstallRoot $journal.hostFile)).Hash
    $journal.ownedCertificateThumbprints = $script:Fresh.Thumbprint
    $journal.ownedPackageFiles = $journal.packageFile
    Write-ProtectedJson $PendingStatePath $journal $journal.userSid }
Export-ModuleMember -Function @()
'@
    $module = New-Module -ScriptBlock ([scriptblock]::Create($stub)) -ArgumentList $repositoryRoot, $functions, $fresh
    $statePath = Join-Path $root 'Installer\INSTALL-STATE.json'
    [IO.File]::WriteAllText($statePath, $oldJson)
    [IO.File]::WriteAllText("$statePath.bak", $oldJson)
    & $module { $script:TrustMissing = $true }
    Assert-ProbeFailure { & $module { param($Root, $Sid)
        Read-OldInstallState -InstallRoot $Root -UserSid $Sid
    } $root $old.installedUserSid }
    & $module { param($Root, $Old)
        $null = Read-OldInstallState -InstallRoot $Root -UserSid $Old.installedUserSid -AllowCertificateRepair
        Assert-InstallStateIntegrity -State $Old -InstallRoot $Root
    } $root $old
    $damagedState = $oldJson | ConvertFrom-Json
    $damagedState.hostSha256 = '0' * 64
    $damagedJson = $damagedState | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText($statePath, $damagedJson)
    [IO.File]::WriteAllText("$statePath.bak", $damagedJson)
    Assert-ProbeFailure { & $module { param($Root, $Sid)
        Read-OldInstallState -InstallRoot $Root -UserSid $Sid -AllowCertificateRepair
    } $root $old.installedUserSid }
    [IO.File]::WriteAllText($statePath, $oldJson)
    [IO.File]::WriteAllText("$statePath.bak", $oldJson)
    [IO.File]::AppendAllText($hostPath, 'tamper')
    Assert-ProbeFailure { & $module { param($Root, $Sid)
        Read-OldInstallState -InstallRoot $Root -UserSid $Sid -AllowCertificateRepair
    } $root $old.installedUserSid }
    [IO.File]::WriteAllText($hostPath, 'unchanged historical Host fixture')
    & $module { $script:TrustMissing = $false }
    & $module { param($Root, $Payload, $Pending)
        Prepare-RollbackIdentity -InstallRoot $Root -PayloadRoot $Payload -PendingPath $Pending
    } $root $payload $pending
    $parent = Get-Content $pending -Raw | ConvertFrom-Json
    Assert-Probe (($parent.oldInstallState | ConvertTo-Json -Depth 12) -ceq $oldJson) 'The immutable old state changed.'
    Assert-Probe ($parent.rollbackInstallState.certificateThumbprint -ne $old.certificateThumbprint) 'Rollback reused the expired certificate.'
    & $module { param($Parent, $Root, $Core)
        $script:ExpectedFullName = $Parent.oldInstallState.packageFullName
        $installRoot = $Root; $packageName = $Parent.packageName
        $packagePath = Join-Path $Root 'new-candidate.msix'; $registeredPackage = $null
        foreach ($version in @($Parent.oldInstallState.packageVersion, $Parent.packageVersion))
        {
            $script:Registered = @($Parent.oldInstallState.packageFullName)
            $script:FailNextAdd = $true
            $machineState = $Parent.PSObject.Copy()
            $machineState.packageVersion = $version
            $machineState.productVersion = $version.Substring(0, $version.LastIndexOf('.'))
            . ([scriptblock]::Create($Core))
            if ($script:Registered.Count -ne 0 -or $script:RegistrationSucceeded -or
                $script:FailureStep -ne 'register-identity-package')
            { throw "Production registration failure did not enter rollback for $version." }
            Copy-ReadyRollbackPackage -State $Parent -InstallRoot $Root
            Restore-PreviousPackage -State $Parent -InstallRoot $Root
        }
        Copy-ReadyRollbackPackage -State $Parent -InstallRoot $Root
        Restore-PreviousPackage -State $Parent -InstallRoot $Root
        $Parent.filesCommitted = $true
        Complete-RollbackIdentity -State $Parent -InstallRoot $Root
        Restore-PreviousPackage -State $Parent -InstallRoot $Root
        Complete-RollbackIdentity -State $Parent -InstallRoot $Root
        if ($script:Registered.Count -ne 1 -or $script:Events[-1] -ne ('add:' + $Parent.rollbackInstallState.packageFile))
        { throw 'Recovery or retry used an incorrect package.' }
    } $parent $root $registrationCore
    $committed = Get-Content (Join-Path $root 'Installer\INSTALL-STATE.json') -Raw | ConvertFrom-Json
    Assert-Probe ($committed.transactionId -eq $parent.rollbackInstallState.transactionId) 'Rollback state was not committed.'
    Assert-Probe ((Get-FileHash $hostPath).Hash -eq $old.hostSha256) 'Recovery changed historical Host bytes.'
    $incomplete = Get-Content $pending -Raw | ConvertFrom-Json
    $incomplete.rollbackInstallState = $null
    Assert-ProbeFailure { & $module { param($Parent, $Root)
        $script:Events.Clear()
        Remove-PreviousPackageForReplacement -State $Parent -InstallRoot $Root
    } $incomplete $root }
    Assert-Probe ((& $module { $script:Events.Count }) -eq 0) 'Incomplete rollback changed registrations.'
    $damagedRollback = Get-Content $pending -Raw | ConvertFrom-Json
    $damagedRollback.rollbackInstallState.certificateSha256 = '0' * 64
    Assert-ProbeFailure { & $module { param($Parent, $Root)
        Read-RollbackIdentityJournal -State $Parent -InstallRoot $Root -RequireReady
    } $damagedRollback $root }
    # Exercise the deletion window after a fully restored state. Retry must
    # use the protected completion marker even when the child journal is gone.
    & $module { $script:FailCleanup = $true }
    $cleanupError = ''
    try
    {
        & $module { param($Parent, $Root, $Payload, $Pending)
            Invoke-PendingRollbackCleanup -State $Parent -InstallRoot $Root -PayloadRoot $Payload -PendingPath $Pending
        } $parent $root $payload $pending
    }
    catch
    {
        $cleanupError = $_.Exception.Message
    }
    Assert-Probe ($cleanupError -eq 'Injected interruption after evidence deletion.') "Unexpected cleanup failure: $cleanupError"
    $completed = Get-Content $pending -Raw | ConvertFrom-Json
    Assert-Probe ([bool]$completed.rollbackComplete) 'Cleanup did not persist rollback completion.'
    & $module { param($Parent, $Root, $Payload, $Pending)
        $script:Events.Clear()
        if (Assert-PendingRollbackMayRemoveNewPackage -State $Parent -InstallRoot $Root)
        { throw 'Completed rollback was allowed to remove its restored registration.' }
        Restore-PreviousPackage -State $Parent -InstallRoot $Root
        Invoke-PendingRollbackCleanup -State $Parent -InstallRoot $Root -PayloadRoot $Payload -PendingPath $Pending
        if ($script:Events.Count -ne 0) { throw 'Cleanup retry changed the restored registration.' }
    } $completed $root $payload $pending
    Assert-Probe (-not (Test-Path -LiteralPath $pending)) 'Cleanup retry left its pending journal.'
    $damaged = $oldJson | ConvertFrom-Json
    $damaged.certificateSha256 = '0' * 64
    Assert-ProbeFailure { Assert-RecordedPackageSigner -State $damaged -PackagePath $package }
    [IO.File]::AppendAllText($package, 'tamper')
    Assert-ProbeFailure { Assert-RecordedPackageSigner -State $old -PackagePath $package }
    Write-Host 'Identity recovery verified: missing trust repair/uninstall, expired CMS signer, fresh rollback, same-version/upgrade registration failure, cleanup interruption retry and tamper rejection.'
}
finally
{
    if ($null -ne $module) { Remove-Module -ModuleInfo $module -Force -ErrorAction SilentlyContinue }
    if ($null -ne $expired) { $expired.Dispose() }
    if ($null -ne $fresh) { $fresh.Dispose() }
    $resolved = [IO.Path]::GetFullPath($root)
    $parentRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolved.StartsWith($parentRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved).StartsWith('bafx-identity-recovery-', [StringComparison]::Ordinal))
    {
        Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
    }
}
