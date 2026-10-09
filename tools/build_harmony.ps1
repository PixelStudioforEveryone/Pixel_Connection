param(
    [Parameter(Mandatory=$true)][string]$DevEcoHome,
    [string]$LocalSigningProfile,
    [ValidateSet('debug','release')][string]$BuildMode = 'debug',
    [ValidateSet('hap','app')][string]$PackageType = 'hap'
)
$ErrorActionPreference = 'Stop'
$pxcProjectPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\Pixel_Connection_HOS'))
$pxcPublicProfile = Join-Path $pxcProjectPath 'build-profile.json5'
if (-not (Test-Path -LiteralPath $pxcPublicProfile)) {
    Copy-Item -LiteralPath (Join-Path $pxcProjectPath 'build-profile.example.json5') -Destination $pxcPublicProfile
}
$pxcPublicBytes = [IO.File]::ReadAllBytes($pxcPublicProfile)
if ($LocalSigningProfile) {
    $LocalSigningProfile = (Resolve-Path -LiteralPath $LocalSigningProfile).Path
}
$env:DEVECO_SDK_HOME = Join-Path $DevEcoHome 'sdk'
$env:JAVA_HOME = Join-Path $DevEcoHome 'jbr'
$env:PATH = (Join-Path $env:JAVA_HOME 'bin') + ';' + $env:PATH
$pxcNodePath = Join-Path $DevEcoHome 'tools\node\node.exe'
$pxcHvigorPath = Join-Path $DevEcoHome 'tools\hvigor\bin\hvigorw.js'
$pxcOhpmPath = Join-Path $DevEcoHome 'tools\ohpm\bin\ohpm.bat'
Push-Location $pxcProjectPath
try {
    if ($LocalSigningProfile) {
        # Keep the complete private signing profile outside the published source.
        Copy-Item -LiteralPath $LocalSigningProfile -Destination $pxcPublicProfile
    }
    & $pxcOhpmPath install
    if ($LASTEXITCODE -ne 0) { throw 'ohpm install failed' }
    if ($PackageType -eq 'app') {
        & $pxcNodePath $pxcHvigorPath --mode project -p product=default -p "buildMode=$BuildMode" assembleApp --no-daemon
    } else {
        & $pxcNodePath $pxcHvigorPath --mode module -p product=default -p module=pixel_connection@default -p "buildMode=$BuildMode" assembleHap --no-daemon
    }
    if ($LASTEXITCODE -ne 0) { throw 'HarmonyOS build failed' }
} finally {
    [IO.File]::WriteAllBytes($pxcPublicProfile, $pxcPublicBytes)
    Pop-Location
}
