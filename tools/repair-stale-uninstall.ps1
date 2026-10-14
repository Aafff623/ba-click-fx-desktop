[CmdletBinding()]
param(
    [string]$InstallDirectory = (Join-Path $env:ProgramFiles 'ba-click-fx-desktop'),
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop')
{
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
        '-InstallDirectory', $InstallDirectory)
    if ($Apply) { $arguments += '-Apply' }
    & powershell.exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "Uninstall journal repair failed: $LASTEXITCODE" }
    return
}

Add-Type -AssemblyName System.Security
Add-Type -AssemblyName System.IO.Compression.FileSystem
$sources = Join-Path $PSScriptRoot 'installer'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $sources 'install-machine.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'The machine installer source could not be parsed.' }
# Load functions without running any installer phase.
$functions = @($ast.EndBlock.Statements |
    Where-Object { $_ -is [Management.Automation.Language.FunctionDefinitionAst] } |
    ForEach-Object { $_.Extent.Text }) -join "`n"
$module = New-Module -ScriptBlock {
    param($Sources, $Functions)
    Set-StrictMode -Version Latest
    . (Join-Path $Sources 'installer-diagnostics.ps1')
    . (Join-Path $Sources 'protected-paths.ps1')
    . (Join-Path $Sources 'installer-state.ps1')
    . (Join-Path $Sources 'installer-identity.ps1')
    . ([scriptblock]::Create($Functions))
} -ArgumentList $sources, $functions
try
{
    & $module {
        param($Directory, $Apply, $BackupParent)
        $root = Resolve-ProtectedProgramFilesPath -Path $Directory -Description 'install directory'
        Assert-NoReparsePath -Path $root
        foreach ($name in @('PREPARE-STATE.json', 'UNINSTALL-COMPLETE.json'))
        {
            if (Test-Path -LiteralPath (Join-Path $root ('Installer\' + $name)))
            {
                throw "Recovery evidence exists: $name. This repair cannot proceed."
            }
        }
        $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        $state = Read-OldInstallState -InstallRoot $root -UserSid $sid
        if ($null -eq $state -or [int]$state.schema -ne 2)
        {
            throw 'A complete schema 2 installation is required.'
        }
        $registered = @(Get-AppxPackage -Name ([string]$state.packageName) -ErrorAction Stop)
        if ($registered.Count -ne 1 -or
            [string]$registered[0].PackageFullName -ne [string]$state.packageFullName)
        {
            throw 'The current package registration does not match the protected installation.'
        }
        $journal = Read-StartedUninstallJournal -InstallRoot $root
        if ($null -eq $journal)
        {
            Write-Host 'No stale uninstall journal remains. You can retry normal uninstall.'
            return
        }
        if ([string]$journal.transactionId -eq [string]$state.transactionId)
        {
            if ([string]$journal.stateDigest -ne [string]$state.stateDigest)
            {
                throw 'The current uninstall journal has a different state digest.'
            }
            Write-Host 'The journal belongs to the current installation. No stale journal repair is needed.'
            return
        }
        Write-Host "Verified version $($state.productVersion): state pair, Host, signed package, certificate and registration."
        Write-Host "Stale uninstall transaction: $($journal.transactionId); phase=started."
        if (-not $Apply)
        {
            Write-Host 'Validation only. Run repair-uninstall.cmd as administrator to archive the stale journal.'
            return
        }
        Assert-Administrator
        foreach ($name in @('ba-click-fx-desktop.exe', 'BAFX.ControlCenter.exe'))
        {
            $expectedPath = Join-Path $root $name
            $running = @(Get-CimInstance Win32_Process -Filter ("Name='" + $name + "'") -ErrorAction Stop |
                Where-Object { [string]$_.ExecutablePath -eq $expectedPath })
            if ($running.Count -ne 0) { throw "Close $name before repairing the journal." }
        }
        $backup = Join-Path $BackupParent ('uninstall-journal-recovery-' +
            [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N'))
        Assert-NoReparsePath -Path $backup -AllowMissing
        New-Item -ItemType Directory -Path $backup -ErrorAction Stop | Out-Null
        foreach ($name in @('INSTALL-STATE.json', 'INSTALL-STATE.json.bak', 'UNINSTALL-STATE.json'))
        {
            Copy-Item -LiteralPath (Join-Path $root ('Installer\' + $name)) -Destination $backup -ErrorAction Stop
        }
        Move-SupersededUninstallJournal -InstallRoot $root -UserSid $sid
        $verified = Read-OldInstallState -InstallRoot $root -UserSid $sid
        if ([string]$verified.stateDigest -ne [string]$state.stateDigest -or
            (Test-Path -LiteralPath (Join-Path $root 'Installer\UNINSTALL-STATE.json')))
        {
            throw "Repair verification failed. Backup: $backup"
        }
        Write-Host "Repair completed. Backup: $backup"
        Write-Host 'You can now retry normal uninstall from Windows Settings.'
    } $InstallDirectory ([bool]$Apply) ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\artifacts\local')))
}
finally
{
    Remove-Module -ModuleInfo $module -Force -ErrorAction SilentlyContinue
}
