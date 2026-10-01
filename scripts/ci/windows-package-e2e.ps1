<#
.SYNOPSIS
  End-to-end test of the Windows packages produced by `anyar build`.

.DESCRIPTION
  Scaffolds a vanilla app, builds zip + NSIS installer + MSI, then:
    MSI  : silent per-machine install -> files, Start Menu shortcut, ARP entry,
           the installed app serves its frontend -> major upgrade to a newer
           version (one ARP entry left) -> silent uninstall -> everything gone
    NSIS : silent per-user install -> files + ARP -> silent uninstall -> gone
  Needs: an ELEVATED shell (per-machine MSI), Node/npm, NSIS 3 (makensis) and
  WiX v4+ (wix) on PATH, plus a built anyar CLI.  Used by the Windows CI
  workflow; runnable locally the same way.

.EXAMPLE
  scripts\ci\windows-package-e2e.ps1 -Anyar build-win\cli\Release\anyar.exe
#>
param(
    [Parameter(Mandatory = $true)][string]$Anyar,
    [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) "anyar-package-e2e"),
    [string]$Name = "pkge2e"
)
$ErrorActionPreference = "Stop"
$Anyar = (Resolve-Path $Anyar).Path
$failures = New-Object System.Collections.Generic.List[string]

function Check([string]$what, [bool]$ok) {
    if ($ok) { Write-Host "  [ok]   $what" } else { Write-Host "  [FAIL] $what"; $failures.Add($what) }
}
function Invoke-Anyar([string[]]$argv) {
    & $Anyar @argv
    if ($LASTEXITCODE -ne 0) { throw "anyar $($argv -join ' ') failed ($LASTEXITCODE)" }
}
function Find-Arp([string]$displayName) {
    $roots = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall",
             "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall",
             "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall"
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        Get-ChildItem $r | ForEach-Object { Get-ItemProperty $_.PSPath } |
            Where-Object { $_.DisplayName -eq $displayName }
    }
}
# Start the installed exe, wait for its HTTP server, fetch "/", then close it.
function Test-AppServes([string]$exe) {
    if (-not (Test-Path $exe)) { return $false }
    $out = Join-Path $WorkDir "app-out.txt"
    $err = Join-Path $WorkDir "app-err.txt"
    $p = Start-Process $exe -WorkingDirectory $WorkDir -PassThru `
            -RedirectStandardOutput $out -RedirectStandardError $err
    $port = $null
    for ($i = 0; $i -lt 300 -and -not $port; $i++) {
        Start-Sleep -Milliseconds 100
        $m = Select-String -Path $out -Pattern "listening on http://127.0.0.1:(\d+)" -ErrorAction SilentlyContinue
        if ($m) { $port = $m.Matches[0].Groups[1].Value }
    }
    $ok = $false
    if ($port) {
        try {
            $page = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$port/" -TimeoutSec 10
            $ok = $page.StatusCode -eq 200 -and $page.Content -match "<html"
        } catch {}
        try {
            Invoke-WebRequest -UseBasicParsing -Method Post -TimeoutSec 5 `
                -Uri "http://127.0.0.1:$port/__anyar__/invoke" -ContentType "application/json" `
                -Body '{"id":"1","cmd":"window:close-all","args":{}}' | Out-Null
        } catch {}
    }
    if (-not $p.WaitForExit(15000)) { $p.Kill() }
    return $ok
}

# -- Scaffold + build -----------------------------------------------------------
if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
New-Item -ItemType Directory -Force $WorkDir | Out-Null
Push-Location $WorkDir
try {
    Write-Host "== anyar init $Name"
    Invoke-Anyar @("init", $Name, "-t", "vanilla")
    Set-Location $Name

    Write-Host "== anyar build --package all (1.0.0)"
    Invoke-Anyar @("build", "--package", "all", "--version", "1.0.0", "--publisher", "Anyar CI")
    $msi1 = Join-Path (Get-Location) "build\$Name-1.0.0-win64.msi"
    $setup = Join-Path (Get-Location) "build\$Name-1.0.0-setup.exe"
    $zip = Join-Path (Get-Location) "build\$Name-1.0.0-win64.zip"
    Check "zip produced" (Test-Path $zip)
    Check "NSIS installer produced" (Test-Path $setup)
    Check "MSI produced" (Test-Path $msi1)

    Write-Host "== anyar build --package msi (1.0.1, packaging only)"
    Invoke-Anyar @("build", "--no-frontend", "--no-backend", "--package", "msi",
                   "--version", "1.0.1", "--publisher", "Anyar CI")
    $msi2 = Join-Path (Get-Location) "build\$Name-1.0.1-win64.msi"
    Check "MSI 1.0.1 produced" (Test-Path $msi2)

    # -- MSI: install -> upgrade -> uninstall ------------------------------------
    $dir = Join-Path $env:ProgramFiles $Name
    $lnk = Join-Path ([Environment]::GetFolderPath("CommonPrograms")) "$Name.lnk"

    Write-Host "== MSI install 1.0.0"
    $p = Start-Process msiexec -Wait -PassThru -ArgumentList @(
        "/i", "`"$msi1`"", "/qn", "/l*v", "`"$WorkDir\msi-install.log`"")
    Check "msiexec /i exit 0 (got $($p.ExitCode))" ($p.ExitCode -eq 0)
    Check "exe in Program Files" (Test-Path "$dir\$Name.exe")
    Check "dist\index.html installed" (Test-Path "$dir\dist\index.html")
    Check "DLLs installed" ((Get-ChildItem $dir -Filter *.dll -ErrorAction SilentlyContinue).Count -gt 0)
    Check "Start Menu shortcut" (Test-Path $lnk)
    $arp = @(Find-Arp $Name)
    Check "one ARP entry, version 1.0.0" ($arp.Count -eq 1 -and $arp[0].DisplayVersion -eq "1.0.0")
    Check "ARP publisher" ($arp.Count -ge 1 -and $arp[0].Publisher -eq "Anyar CI")
    Check "installed app serves its frontend" (Test-AppServes "$dir\$Name.exe")

    Write-Host "== MSI major upgrade to 1.0.1"
    $p = Start-Process msiexec -Wait -PassThru -ArgumentList @(
        "/i", "`"$msi2`"", "/qn", "/l*v", "`"$WorkDir\msi-upgrade.log`"")
    Check "upgrade exit 0 (got $($p.ExitCode))" ($p.ExitCode -eq 0)
    $arp = @(Find-Arp $Name)
    Check "one ARP entry after upgrade, version 1.0.1" ($arp.Count -eq 1 -and $arp[0].DisplayVersion -eq "1.0.1")
    Check "exe still installed" (Test-Path "$dir\$Name.exe")

    Write-Host "== MSI downgrade is refused"
    $p = Start-Process msiexec -Wait -PassThru -ArgumentList @("/i", "`"$msi1`"", "/qn")
    Check "installing 1.0.0 over 1.0.1 fails (got $($p.ExitCode))" ($p.ExitCode -ne 0)

    Write-Host "== MSI uninstall"
    $p = Start-Process msiexec -Wait -PassThru -ArgumentList @(
        "/x", "`"$msi2`"", "/qn", "/l*v", "`"$WorkDir\msi-uninstall.log`"")
    Check "msiexec /x exit 0 (got $($p.ExitCode))" ($p.ExitCode -eq 0)
    Check "install dir removed" (-not (Test-Path $dir))
    Check "Start Menu shortcut removed" (-not (Test-Path $lnk))
    Check "ARP entry removed" (@(Find-Arp $Name).Count -eq 0)

    # -- NSIS: per-user install -> uninstall -------------------------------------
    $udir = Join-Path $env:LOCALAPPDATA "Programs\$Name"
    Write-Host "== NSIS install (per-user, silent)"
    $p = Start-Process $setup -ArgumentList "/S" -Wait -PassThru
    Check "setup.exe /S exit 0 (got $($p.ExitCode))" ($p.ExitCode -eq 0)
    Check "exe installed per-user" (Test-Path "$udir\$Name.exe")
    Check "NSIS ARP entry" (@(Find-Arp $Name).Count -eq 1)
    Write-Host "== NSIS uninstall"
    Start-Process "$udir\Uninstall.exe" -ArgumentList "/S" -Wait | Out-Null
    for ($i = 0; $i -lt 100 -and (Test-Path $udir); $i++) { Start-Sleep -Milliseconds 200 }
    Check "per-user dir removed" (-not (Test-Path $udir))
    Check "NSIS ARP entry removed" (@(Find-Arp $Name).Count -eq 0)
} finally {
    Pop-Location
}

if ($failures.Count -gt 0) {
    Write-Host "`n$($failures.Count) check(s) failed:"
    $failures | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
Write-Host "`nAll package checks passed."
