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
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -RepositoryRoot $repositoryRoot
    if ($LASTEXITCODE -ne 0)
    {
        throw "Installer directory ACL verification failed: $LASTEXITCODE"
    }
    return
}

function Assert-Probe
{
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
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

$functions = @(
    'Set-StrictMode -Version Latest'
    Get-ProbeFunction 'tools/installer/protected-paths.ps1' @('Assert-NoReparsePath')
    Get-ProbeFunction 'tools/installer/install-machine.ps1' @('Set-ProtectedStateAcl')
) -join "`n"
$module = New-Module -ScriptBlock ([scriptblock]::Create($functions))
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$temporaryRoot = Join-Path $temporaryParent ('bafx-directory-acl-' + [Guid]::NewGuid().ToString('N'))
$savedAcls = New-Object Collections.Generic.List[object]
try
{
    $rollbackRoot = Join-Path $temporaryRoot 'transaction'
    $identityRoot = Join-Path $rollbackRoot 'identity'
    $installerRoot = Join-Path $identityRoot 'Installer'
    New-Item -ItemType Directory -Path $installerRoot -Force | Out-Null
    $journalPath = Join-Path $installerRoot 'PREPARE-STATE.json'
    [IO.File]::WriteAllText($journalPath, '{"schema":2}')
    foreach ($path in @($rollbackRoot, $identityRoot, $installerRoot, $journalPath))
    {
        $item = Get-Item -LiteralPath $path -Force
        $savedAcls.Add([pscustomobject]@{
            Path = $path; IsDirectory = $item.PSIsContainer; Sddl = $item.GetAccessControl().Sddl
        })
    }
    $userSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $principal = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    $isAdministrator = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    # Existing children can retain explicit administrator access in hosted CI.
    # Reproduce it so the probe also covers an explicit and an inherited ACE
    # for the same SID after the production helper repairs the parent.
    $existingChildAcl = Get-Acl -LiteralPath $identityRoot
    $existingChildAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'), 'FullControl', 'Allow'))
    Set-Acl -LiteralPath $identityRoot -AclObject $existingChildAcl
    if ($isAdministrator)
    {
        # An elevated fixture owner may also retain an explicit user ACE.
        # It must not be confused with the helper's inherited read-only rule.
        $existingOwnerAcl = Get-Acl -LiteralPath $installerRoot
        $existingOwnerAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            $userSid, 'FullControl', 'Allow'))
        Set-Acl -LiteralPath $installerRoot -AclObject $existingOwnerAcl
    }

    # Reproduce the released installer's DACL: access on the root alone removes
    # inherited permissions from its existing identity/Installer directories.
    $legacyAcl = New-Object Security.AccessControl.DirectorySecurity
    $legacyAcl.SetAccessRuleProtection($true, $false)
    foreach ($sidValue in @('S-1-5-18', 'S-1-5-32-544'))
    {
        $legacyAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.SecurityIdentifier]::new($sidValue), 'FullControl', 'Allow'))
    }
    $legacyAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        $userSid, 'ReadAndExecute', 'Allow'))
    Set-Acl -LiteralPath $rollbackRoot -AclObject $legacyAcl
    $legacyRules = @((Get-Acl -LiteralPath $rollbackRoot).GetAccessRules(
        $true, $true, [Security.Principal.SecurityIdentifier]))
    Assert-Probe ($legacyRules.Count -eq 3 -and
        @($legacyRules | Where-Object {
            $_.InheritanceFlags -ne [Security.AccessControl.InheritanceFlags]::None
        }).Count -eq 0) 'The legacy fixture must omit child inheritance from its access rules.'
    $denied = $false
    try
    {
        Get-ChildItem -LiteralPath $rollbackRoot -Recurse -Force -ErrorAction Stop | Out-Null
    }
    catch [UnauthorizedAccessException]
    {
        $denied = $true
    }
    # Hosted Windows CI runs elevated with UAC disabled. Its token can retain
    # access, so verify the legacy ACL itself in every environment and require
    # denied traversal only for the original non-administrator reproduction.
    if (-not $isAdministrator)
    {
        Assert-Probe $denied 'The legacy fixture did not reproduce inaccessible rollback children.'
    }
    Write-Host "ACL fixture: administrator=$isAdministrator; legacy-traversal-denied=$denied."

    & $module { param($Path, $Sid) Set-ProtectedStateAcl -Path $Path -ReadSid $Sid } $rollbackRoot $userSid.Value
    $items = @(Get-ChildItem -LiteralPath $rollbackRoot -Recurse -Force -ErrorAction Stop)
    Assert-Probe ($items.Count -eq 3) 'Directory hardening did not restore traversal to existing children.'
    Assert-Probe ([IO.File]::ReadAllText($journalPath) -eq '{"schema":2}') 'The rollback journal remained unreadable.'
    $inheritance = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
        [Security.AccessControl.InheritanceFlags]::ObjectInherit
    $writeRights = [int]([Security.AccessControl.FileSystemRights]::Write -bor
        [Security.AccessControl.FileSystemRights]::Delete -bor
        [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor
        [Security.AccessControl.FileSystemRights]::ChangePermissions -bor
        [Security.AccessControl.FileSystemRights]::TakeOwnership)
    $readRights = [Security.AccessControl.FileSystemRights]::ReadAndExecute -bor
        [Security.AccessControl.FileSystemRights]::Synchronize
    foreach ($path in @($rollbackRoot, $identityRoot, $installerRoot, $journalPath))
    {
        $acl = Get-Acl -LiteralPath $path
        $rules = @($acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]))
        $expectedInherited = $path -ne $rollbackRoot
        foreach ($sidValue in @('S-1-5-18', 'S-1-5-32-544'))
        {
            $matches = @($rules | Where-Object {
                $_.IdentityReference.Value -eq $sidValue -and
                $_.FileSystemRights -eq [Security.AccessControl.FileSystemRights]::FullControl -and
                $_.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
                $_.IsInherited -eq $expectedInherited
            })
            Assert-Probe ($matches.Count -eq 1) "Administrator/SYSTEM rule mismatch: path=$path; sid=$sidValue; inherited=$expectedInherited; matches=$($matches.Count); sddl=$($acl.Sddl)"
            if ($path -eq $rollbackRoot)
            {
                Assert-Probe ($matches[0].InheritanceFlags -eq $inheritance) 'Directory access is not inheritable.'
            }
        }
        $userRules = @($rules | Where-Object {
            $_.IdentityReference.Value -eq $userSid.Value -and
            $_.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
            $_.IsInherited -eq $expectedInherited -and
            $_.FileSystemRights -eq $readRights
        })
        Assert-Probe ($userRules.Count -eq 1) "Original-user read rule mismatch: path=$path; inherited=$expectedInherited; matches=$($userRules.Count); sddl=$($acl.Sddl)"
        if (-not $isAdministrator)
        {
            $userWriteRules = @($rules | Where-Object {
                $_.IdentityReference.Value -eq $userSid.Value -and
                $_.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
                ([int]$_.FileSystemRights -band $writeRights) -ne 0
            })
            Assert-Probe ($userWriteRules.Count -eq 0) 'The original user gained write access.'
        }
    }

    & $module { param($Path, $Sid) Set-ProtectedStateAcl -Path $Path -ReadSid $Sid } $journalPath $userSid.Value
    $fileAcl = Get-Acl -LiteralPath $journalPath
    Assert-Probe $fileAcl.AreAccessRulesProtected 'State files must remain individually protected.'
    foreach ($rule in $fileAcl.Access)
    {
        Assert-Probe ($rule.InheritanceFlags -eq [Security.AccessControl.InheritanceFlags]::None) 'File ACLs contain directory inheritance flags.'
    }
    Write-Host "Installer directory ACLs verified on NTFS: legacy inheritance defect, restored traversal, inherited administrator access and read-only original-user access; administrator=$isAdministrator; legacy-traversal-denied=$denied."
}
finally
{
    # The fixture owner retains WRITE_DAC, allowing cleanup without elevation.
    # Restore the root first so inherited permissions make its children usable.
    foreach ($saved in $savedAcls)
    {
        if ($saved.IsDirectory)
        {
            $restoredAcl = New-Object Security.AccessControl.DirectorySecurity
            $restoredAcl.SetSecurityDescriptorSddlForm($saved.Sddl, [Security.AccessControl.AccessControlSections]::Access)
            [IO.Directory]::SetAccessControl($saved.Path, $restoredAcl)
        }
        else
        {
            $restoredAcl = New-Object Security.AccessControl.FileSecurity
            $restoredAcl.SetSecurityDescriptorSddlForm($saved.Sddl, [Security.AccessControl.AccessControlSections]::Access)
            [IO.File]::SetAccessControl($saved.Path, $restoredAcl)
        }
    }
    Remove-Module -ModuleInfo $module -Force -ErrorAction SilentlyContinue
    $resolvedRoot = [IO.Path]::GetFullPath($temporaryRoot)
    if ($resolvedRoot.StartsWith($temporaryParent, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolvedRoot).StartsWith('bafx-directory-acl-', [StringComparison]::Ordinal))
    {
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
