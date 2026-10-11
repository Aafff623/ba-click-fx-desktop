[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceLogPath,

    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 2147483647)]
    [int]$SetupProcessId,

    [ValidateRange(1, 120)]
    [int]$WaitTimeoutSeconds = 60
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'protected-paths.ps1')

function Move-CompletedInstallerLog
{
    param(
        [string]$SourcePath,
        [string]$LogDirectory,
        [int]$WaitSeconds
    )

    $sourcePathFull = [IO.Path]::GetFullPath($SourcePath)
    $logDirectoryFull = [IO.Path]::GetFullPath($LogDirectory)
    Assert-NoReparsePath -Path $sourcePathFull
    Assert-NoReparsePath -Path $logDirectoryFull
    $name = 'ba-click-fx-desktop-install-' +
        [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' +
        [Guid]::NewGuid().ToString('N') + '.log'
    $archivePath = Join-Path $logDirectoryFull $name
    $pendingPath = "$archivePath.pending"
    $source = $null
    $target = $null
    $hash = $null
    $pendingCreated = $false
    try
    {
        $timer = [Diagnostics.Stopwatch]::StartNew()
        while ($null -eq $source)
        {
            try
            {
                # Exclude writers while copying. Delete sharing lets us remove
                # this exact source without reopening it after verification.
                $source = [IO.File]::Open($sourcePathFull, [IO.FileMode]::Open,
                    [IO.FileAccess]::Read, [IO.FileShare]::Read -bor [IO.FileShare]::Delete)
            }
            catch [IO.IOException]
            {
                if ($timer.Elapsed.TotalSeconds -ge $WaitSeconds)
                {
                    throw
                }
                Start-Sleep -Milliseconds 200
            }
        }

        # Creating a new file inherits data's user-readable ACL. A same-volume
        # rename from an administrator's TEMP would retain the old TEMP ACL.
        $target = [IO.File]::Open($pendingPath, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $pendingCreated = $true
        $source.CopyTo($target)
        $target.Flush($true)
        $source.Position = 0
        $target.Position = 0
        $hash = [Security.Cryptography.SHA256]::Create()
        $sourceHash = [BitConverter]::ToString($hash.ComputeHash($source))
        $targetHash = [BitConverter]::ToString($hash.ComputeHash($target))
        if ($source.Length -ne $target.Length -or $sourceHash -ne $targetHash)
        {
            throw 'Archived installer log does not match the original.'
        }
        $target.Dispose()
        $target = $null
        Assert-NoReparsePath -Path $logDirectoryFull
        Assert-NoReparsePath -Path $pendingPath
        Assert-NoReparsePath -Path $archivePath -AllowMissing
        Move-Item -LiteralPath $pendingPath -Destination $archivePath
        $pendingCreated = $false
        Assert-NoReparsePath -Path $sourcePathFull
        Remove-Item -LiteralPath $sourcePathFull
        return $archivePath
    }
    finally
    {
        if ($null -ne $hash) { $hash.Dispose() }
        if ($null -ne $target) { $target.Dispose() }
        if ($null -ne $source) { $source.Dispose() }
        if ($pendingCreated)
        {
            Assert-NoReparsePath -Path $pendingPath -AllowMissing
            Remove-Item -LiteralPath $pendingPath -ErrorAction SilentlyContinue
        }
    }
}

$logDirectory = Join-Path ([IO.Path]::GetFullPath($InstallDirectory)) 'data'
try
{
    # DeinitializeSetup still precedes Inno's final log records and file close.
    # Wait in this detached helper, never inside the installer process itself.
    $setupProcess = $null
    try { $setupProcess = [Diagnostics.Process]::GetProcessById($SetupProcessId) }
    catch [ArgumentException] { } # Setup may have exited before PowerShell starts.
    if ($null -ne $setupProcess)
    {
        try
        {
            if (-not $setupProcess.WaitForExit($WaitTimeoutSeconds * 1000))
            {
                throw 'Timed out waiting for Setup to close its installation log.'
            }
        }
        finally { $setupProcess.Dispose() }
    }
    Move-CompletedInstallerLog -SourcePath $SourceLogPath `
        -LogDirectory $logDirectory -WaitSeconds $WaitTimeoutSeconds
}
catch
{
    $failure = $_
    try
    {
        Assert-NoReparsePath -Path $logDirectory
        $errorPath = Join-Path $logDirectory 'ba-click-fx-desktop-install-archive-error.log'
        Assert-NoReparsePath -Path $errorPath -AllowMissing
        Add-Content -LiteralPath $errorPath -Encoding UTF8 -Value (
            '{0} Source={1}; Error={2}' -f [DateTime]::UtcNow.ToString('o'),
            $SourceLogPath, $failure.Exception.Message)
    }
    catch { } # If data itself is unavailable, the original log still survives.
    [Console]::Error.WriteLine($failure.ToString())
    exit 1
}
