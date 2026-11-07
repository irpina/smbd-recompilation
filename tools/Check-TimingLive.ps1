param(
    [int[]]$Rates = @(60,144,360,0),
    [string]$Game = 'build/windows/Release/smbd.exe',
    [string]$StorageSeed = 'build/checks/hr-ball-states-motion-storage',
    [string]$Output = 'build/checks/timing',
    [string]$Disc = 'Super Monkey Ball Deluxe/Super Monkey Ball Deluxe (USA).xiso.iso'
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $root
try {
    if(!$Rates.Count -or $Rates.Count -gt 12 -or @($Rates | Sort-Object -Unique).Count -ne $Rates.Count) { throw 'Supply 1..12 distinct refresh rates.' }
    $gameHash=(Get-FileHash -LiteralPath $Game -Algorithm SHA256).Hash
    $route=Get-Content knowledge/timing_route.json -Raw | ConvertFrom-Json
    $source=(Get-FileHash data/local/default.xbe -Algorithm SHA256).Hash
    if ($source -ne $route.source_sha256) { throw 'Live timing route requires the verified executable.' }
    if (!(Test-Path -LiteralPath $StorageSeed -PathType Container)) { throw "Missing isolated save seed: $StorageSeed" }
    $directory=Join-Path $Output ('live-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-ffff'))
    $seedRoot=[IO.Path]::GetFullPath($StorageSeed).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    if([IO.Path]::GetFullPath($directory).StartsWith($seedRoot,[StringComparison]::OrdinalIgnoreCase)) { throw 'Live output must not be inside its save seed.' }
    New-Item -ItemType Directory -Path $directory | Out-Null
    $route.input | Set-Content -LiteralPath (Join-Path $directory 'input.txt') -Encoding utf8
    $results=[Collections.Generic.List[object]]::new()
    $seedHash=@(Get-ChildItem -LiteralPath $StorageSeed -Recurse -File | Sort-Object FullName | ForEach-Object {
        [pscustomobject]@{path=[IO.Path]::GetRelativePath([IO.Path]::GetFullPath($StorageSeed),$_.FullName);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    })
    foreach ($rate in $Rates) {
        if ($rate -lt 0 -or $rate -gt 1000) { throw 'Refresh rate must be 0..1000.' }
        $prefix=Join-Path $directory "rate-$rate"
        Copy-Item -LiteralPath $StorageSeed -Destination "$prefix-storage" -Recurse
        @('executable=data/local/default.xbe',"disc=$Disc",'muted=1','vsync=0','fullscreen=0','resolution=1','aspect=1','antialiasing=0',"refresh_rate=$rate") |
            Set-Content -LiteralPath "$prefix.cfg" -Encoding utf8
        $count=if($rate -le 60 -and $rate){750}elseif($rate -le 144 -and $rate){1800}else{3000}
        $arguments=@('--hidden','--mute','--seconds','65','--input',"$directory/input.txt",'--settings',"$prefix.cfg",'--storage',"$prefix-storage",
            '--motion-report',"$prefix-motion.json",'--motion-at','2600','--motion-frames',"$count",'--report',"$prefix-boot.json")
        # Use an argument list, not a shell-built command; all output is muted
        # while native audio mixing and the original input timeline still run.
        $start=[Diagnostics.ProcessStartInfo]::new([IO.Path]::GetFullPath($Game))
        $start.WorkingDirectory=$root
        $start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
        $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
        foreach($argument in $arguments){[void]$start.ArgumentList.Add($argument)}
        $process=[Diagnostics.Process]::Start($start)
        $stdout=$process.StandardOutput.ReadToEndAsync(); $stderr=$process.StandardError.ReadToEndAsync()
        $timer=[Diagnostics.Stopwatch]::StartNew(); $failure=''
        try {
            while(!$process.WaitForExit(1000)) {
                if($timer.Elapsed.TotalSeconds -gt 100) { $failure='Live route exceeded its 100-second wall limit.';break }
            }
        } finally {
            if(!$process.HasExited) { $process.Kill($true);$process.WaitForExit() }
            [IO.File]::WriteAllText([IO.Path]::GetFullPath("$prefix.stdout"),$stdout.GetAwaiter().GetResult())
            [IO.File]::WriteAllText([IO.Path]::GetFullPath("$prefix.stderr"),$stderr.GetAwaiter().GetResult())
        }
        $exit=$process.ExitCode; $process.Dispose()
        $goalTicks=$null; $goalSeconds=$null; $fresh=0; $held=0; $drawless=0; $cameraFresh=0; $cameraHeld=0; $goalCameraFresh=0; $goalCameraHeld=0; $samples=@()
        try {
            $run=Get-Content -LiteralPath "$prefix.stdout" -Raw | ConvertFrom-Json
            $motion=Get-Content -LiteralPath "$prefix-motion.json" -Raw | ConvertFrom-Json
            $boot=Get-Content -LiteralPath "$prefix-boot.json" -Raw | ConvertFrom-Json
            $samples=@($motion.samples)
            if($exit -ne 0 -or !$run.controller_complete -or !$run.audio_muted -or !$boot.passed -or $motion.source_sha256 -ne $source) {
                $failure="Incomplete route or native boundary (exit $exit, $($run.boundary))."
            }
            $goal=$samples | Where-Object { $_.mode -eq 51 -and $_.ball_state -eq 6 } | Select-Object -First 1
            $replay=$samples | Where-Object { $_.mode -eq 54 } | Select-Object -First 1
            if(!$goal -or !$replay) { $failure='Motion coverage misses the goal or replay transition.' }
            else {
                $goalTicks=$replay.ticks-$goal.ticks; $goalSeconds=$replay.seconds-$goal.seconds
                if($goalTicks -ne $route.goal_to_replay_ticks) { $failure="Goal-to-replay took $goalTicks ticks, expected $($route.goal_to_replay_ticks)." }
            }
            for($i=1;$i -lt $samples.Count;++$i) {
                $a=$samples[$i-1];$b=$samples[$i]
                # Main-game rolling and an interior replay span avoid motionless
                # setup, pauses, goal-flight holds and authored clip boundaries.
                $rolling=$a.ball_state -eq 32 -and $b.ball_state -eq 32
                $replaying=$a.ball_state -eq 10 -and $b.ball_state -eq 10 -and $a.replay_time -lt 220 -and $b.replay_time -gt 205
                # B00D0 (type 15) follows the active upward goal flight.
                # The old route checked rolling/replay but missed its holds.
                if($a.ball_state -eq 6 -and $b.ball_state -eq 6 -and $a.camera_state -eq 15 -and $b.camera_state -eq 15 -and
                   $a.velocity[1] -gt 0.01 -and $b.velocity[1] -gt 0.01) {
                    if(($a.camera_matrix -join ',') -eq ($b.camera_matrix -join ',')){++$goalCameraHeld}else{++$goalCameraFresh}
                }
                if(!$rolling -and !$replaying){continue}
                if(($a.ball -join ',') -eq ($b.ball -join ',') -or ($a.matrix -join ',') -eq ($b.matrix -join ',')){++$held}else{++$fresh}
                if($rolling) {
                    if($a.camera_matrix.Count -ne 12 -or $b.camera_matrix.Count -ne 12) { throw 'Missing complete camera view matrices.' }
                    if(($a.camera_matrix -join ',') -eq ($b.camera_matrix -join ',')){++$cameraHeld}else{++$cameraFresh}
                }
                if($b.draws -le $a.draws){++$drawless}
            }
            if(!$fresh -or $held -or $drawless) { $failure="Motion/drawing failed: fresh=$fresh, held=$held, drawless=$drawless." }
            if(!$cameraFresh -or $cameraHeld) { $failure="Rolling camera failed: fresh=$cameraFresh, held=$cameraHeld." }
            if(!$goalCameraFresh -or $goalCameraHeld) { $failure="Goal-flight camera failed: fresh=$goalCameraFresh, held=$goalCameraHeld." }
        } catch { $failure=$_.Exception.Message }
        $results.Add([pscustomobject]@{rate=$rate;passed=(!$failure);exit=$exit;failure=$failure;samples=$samples.Count;goal_to_replay_ticks=$goalTicks;
            goal_to_replay_seconds=$goalSeconds;fresh_motion=$fresh;held_motion=$held;drawless_scenes=$drawless;
            fresh_camera_matrices=$cameraFresh;held_camera_matrices=$cameraHeld;
            fresh_goal_camera_matrices=$goalCameraFresh;held_goal_camera_matrices=$goalCameraHeld;motion="$prefix-motion.json"})
        Write-Host "Live timing $rate Hz: $($results[-1].passed), $goalTicks goal/replay ticks, $fresh fresh motion samples."
    }
    if((Get-FileHash -LiteralPath $Game -Algorithm SHA256).Hash -ne $gameHash) { throw 'Game executable changed during live timing checks.' }
    $report=[pscustomobject]@{format='smbd-timing-live-v1';source_sha256=$source;game_sha256=$gameHash;
        route_sha256=(Get-FileHash knowledge/timing_route.json -Algorithm SHA256).Hash;seed=$seedHash;passed=(@($results | Where-Object { !$_.passed }).Count -eq 0);
        exhaustive=$false;scope='First story stage and replay; hidden/muted; visual checking remains manual.';results=$results.ToArray()}
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $directory 'report.json') -Encoding utf8
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Output 'live-report.json') -Encoding utf8
    if(!$report.passed){throw "Live timing failed; see $directory/report.json"}
} finally {Pop-Location}
