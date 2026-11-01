param(
    [string]$Output,
    [ValidateRange(1,3)][int]$Workers=3,
    [switch]$SkipBuild
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(!$Output) {$Output=Join-Path $root 'build/release'}
$Output=[IO.Path]::GetFullPath($Output)
function Invoke-Checked([string]$Program,[string[]]$Arguments) {
    & $Program @Arguments
    if($LASTEXITCODE -ne 0) {throw "$Program failed with exit code $LASTEXITCODE"}
}
Push-Location $root
try {
    if(!$SkipBuild) { & (Join-Path $PSScriptRoot 'Build.ps1') -Stage Game -Workers $Workers }
    . (Join-Path $PSScriptRoot 'Initialize-Toolchain.ps1')
    if(!$SkipBuild) {Invoke-Checked cmake @('--build','--preset','release','--target','smbd','smbd-tool','--parallel',"$Workers")}
    $package=Join-Path $root 'build/package'
    $stage=Join-Path $package ('stage-'+[Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage,$Output -Force | Out-Null
    # CMake's Runtime component is an explicit whitelist: no media, settings,
    # saves, generated source, profiles, debug maps, or build directories.
    Invoke-Checked cmake @('--install','build/windows','--config','Release','--prefix',$stage,'--component','Runtime')
    $files=@(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName)
    $manifest=@('SMBD-PAYLOAD-V1')
    $cabinet=Join-Path $package 'payload.cab'
    $recipe=Join-Path $package 'payload.ddf'
    $directives=@('.OPTION EXPLICIT','.Set CabinetNameTemplate=payload.cab',('.Set DiskDirectoryTemplate="'+$package+'"'),
        '.Set Cabinet=on','.Set Compress=on','.Set CompressionType=LZX','.Set CompressionMemory=18',
        '.Set MaxDiskSize=0','.Set MaxCabinetSize=0','.Set MaxErrors=1',('.Set RptFileName="'+(Join-Path $package 'payload.rpt')+'"'),
        ('.Set InfFileName="'+(Join-Path $package 'payload.inf')+'"'))
    foreach($file in $files) {
        $relative=[IO.Path]::GetRelativePath($stage,$file.FullName)
        if($relative -match '[^\x20-\x7e]' -or $relative -match '[\r\n\t"]' -or $relative -match '^(data|logs|captures|build)[\\/]') {throw "Unexpected release file: $relative"}
        $manifest+=($relative.Replace('\','/')+"`t"+$file.Length+"`t"+(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)
        $directives+=('"'+$file.FullName+'" "'+$relative+'"')
    }
    $manifestPath=Join-Path $package 'payload.tsv'
    $manifestText=($manifest -join "`n")+"`n"
    $reuse=(Test-Path -LiteralPath $manifestPath) -and [IO.File]::ReadAllText($manifestPath) -ceq $manifestText -and (Test-Path -LiteralPath $cabinet)
    $cachePath=Join-Path $package 'payload-cache.json'
    if($reuse -and (Test-Path -LiteralPath $cachePath)) {
        $cached=Get-Content -LiteralPath $cachePath -Raw | ConvertFrom-Json
        $reuse=$cached.sha256 -eq (Get-FileHash -LiteralPath $cabinet -Algorithm SHA256).Hash
    } else {$reuse=$false}
    [IO.File]::WriteAllText($manifestPath,$manifestText,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllLines($recipe,$directives,[Text.Encoding]::Default)
    if(!$reuse) {
        Write-Host 'Packing verified release files (one compression worker).'
        Invoke-Checked makecab.exe @('/V0','/F',$recipe)
        @{sha256=(Get-FileHash -LiteralPath $cabinet -Algorithm SHA256).Hash} | ConvertTo-Json | Set-Content -LiteralPath $cachePath -Encoding utf8
    } else {Write-Host 'Reusing the verified release payload.'}
    Invoke-Checked cmake @('--preset','windows',"-DSMBD_BUILD_WORKERS=$Workers","-DSMBD_INSTALLER_PAYLOAD=$($cabinet.Replace('\','/'))","-DSMBD_INSTALLER_MANIFEST=$($manifestPath.Replace('\','/'))")
    Invoke-Checked cmake @('--build','--preset','release','--target','smbd-setup','--parallel',"$Workers")
    $versionLine=Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') | Where-Object {$_ -match '^project\(smbd VERSION ([0-9.]+) '}
    if($versionLine -notmatch 'VERSION ([0-9.]+)') {throw 'Cannot determine the release version'}
    $version=$Matches[1]
    $installer=Join-Path $Output "smbd-$version-windows-x64-setup.exe"
    Copy-Item -LiteralPath (Join-Path $root 'build/windows/Release/smbd-setup.exe') -Destination $installer
    $checksum=(Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($installer+'.sha256',"$checksum  $([IO.Path]::GetFileName($installer))`n")
    [pscustomobject]@{format='smbd-release-v1';version=$version;platform='windows-x64';installer=[IO.Path]::GetFileName($installer);sha256=$checksum;
        payload_files=$files.Count;payload_bytes=($files | Measure-Object Length -Sum).Sum;source_commit=(& git rev-parse HEAD);
        source_modified=([bool](& git status --porcelain));includes_game_media=$false} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Output 'release.json') -Encoding utf8
    Write-Host "Installer ready: $installer"
    # Only remove the exact staging directory created by this invocation.
    if([IO.Path]::GetFullPath($stage).StartsWith($package+'\',[StringComparison]::OrdinalIgnoreCase) -and
       [IO.Path]::GetFileName($stage) -match '^stage-[0-9a-f]{32}$') {Remove-Item -LiteralPath $stage -Recurse -Force}
} finally {Pop-Location}
