param(
    [string]$Batch = 'build/generated/batch.json',
    [string]$Tool = 'build/windows/Release/smbd-tool.exe',
    [string]$Cases = 'knowledge/timing_cases.json',
    [string]$Output = 'build/checks/timing',
    [string[]]$Only = @(),
    [switch]$ProbesOnly,
    [switch]$InventoryOnly
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $root
try {
    $target = (Get-Content knowledge/supported_target.json -Raw | ConvertFrom-Json).targets[0].file_sha256
    if ((Get-FileHash data/local/default.xbe -Algorithm SHA256).Hash -ne $target) { throw 'Timing audit requires the verified executable.' }
    $casesHash=(Get-FileHash -LiteralPath $Cases -Algorithm SHA256).Hash
    $suite = Get-Content -LiteralPath $Cases -Raw | ConvertFrom-Json
    if ($suite.source_sha256 -ne $target -or !$suite.cases.Count -or $suite.cases.Count -gt 128) { throw 'Invalid timing case manifest.' }
    $reviews=@{}
    foreach($finding in $suite.findings) {
        if(!$finding.evidence -or $finding.status -notin @('verified_fix','not_time_dependent','needs_research')) { throw 'Invalid timing finding.' }
        if($finding.status -eq 'verified_fix' -and !$finding.cases.Count) { throw 'A verified timing fix needs executable probes.' }
        foreach($name in $finding.cases) { if($name -notin $suite.cases.name) { throw "Missing probe for finding $($finding.id): $name" } }
        foreach($site in $finding.sites) {
            if($reviews.ContainsKey($site)) { throw "Duplicate timing review: $site" }
            $reviews[$site]=$finding
        }
    }
    if($Only.Count) {
        if($InventoryOnly) { throw '-Only cannot be combined with -InventoryOnly.' }
        foreach($pattern in $Only) { if(!@($suite.cases | Where-Object name -Like $pattern).Count) { throw "No timing case matches: $pattern" } }
        $suite.cases=@($suite.cases | Where-Object { $name=$_.name; @($Only | Where-Object { $name -like $_ }).Count })
        $ProbesOnly=$true
    }
    if($ProbesOnly -and $InventoryOnly) { throw '-ProbesOnly cannot be combined with -InventoryOnly.' }
    New-Item -ItemType Directory -Force -Path $Output | Out-Null
    if ((Get-Item -LiteralPath $Batch).Length -gt 128MB) { throw 'Timing index exceeds its input bound.' }
    # JsonDocument avoids expanding the full instruction/writer index into
    # PowerShell objects. Keep only this audit's small per-function records.
    $stream = [IO.File]::OpenRead([IO.Path]::GetFullPath($Batch))
    $document = [Text.Json.JsonDocument]::Parse($stream)
    $nodes = @{}; $changes = @{}; $updates = @{}
    try {
        $batchRoot = $document.RootElement
        if ($batchRoot.GetProperty('sha256').GetString() -ne $target) { throw 'Timing index belongs to another executable.' }
        $closed = $batchRoot.GetProperty('game_reachability').GetProperty('closed').GetBoolean()
        $events = @($batchRoot.GetProperty('timing_events').ToString() | ConvertFrom-Json)
        if(!$ProbesOnly) { foreach ($function in $batchRoot.GetProperty('functions').EnumerateArray()) {
            $entry = $function.GetProperty('entry').GetString()
            $entryCode = $function.GetProperty('timing_entry').GetString()
            $sites = @($function.GetProperty('timing_sites').ToString() | ConvertFrom-Json)
            $node = [pscustomobject]@{
                entry=$entry
                calls=@($function.GetProperty('calls').EnumerateArray() | ForEach-Object GetString)
                callbacks=@($function.GetProperty('static_callback_targets').EnumerateArray() | ForEach-Object GetString)
                edges=@($function.GetProperty('call_edges').ToString() | ConvertFrom-Json)
                scene_calls=@($function.GetProperty('scene_calls').EnumerateArray() | ForEach-Object GetString)
                scene_updates=@($function.GetProperty('scene_updates').EnumerateArray() | ForEach-Object GetString)
                entry_lowering=$entryCode
                sites=@($sites | ForEach-Object { $_.address })
                families=[Collections.Generic.HashSet[string]]::new()
            }
            $nodes[$entry] = $node
            if ($entryCode) { $changes["entry:$entry"] = [pscustomobject]@{address=$entry;kind='entry';owners=@($entry);lowering=$entryCode} }
            foreach ($site in $sites) {
                if (!$changes.ContainsKey($site.address)) {
                    $changes[$site.address] = [pscustomobject]@{address=$site.address;kind='instruction';owners=[Collections.Generic.List[string]]::new();bytes=$site.bytes;original=$site.text;lowering=$site.lowering}
                }
                $changes[$site.address].owners.Add($entry)
            }
            foreach ($update in ($function.GetProperty('persistent_updates').ToString() | ConvertFrom-Json)) {
                if (!$updates.ContainsKey($update.address)) {
                    $updates[$update.address] = [pscustomobject]@{address=$update.address;bytes=$update.bytes;original=$update.text;load=$update.load;operation=$update.operation;
                        operation_text=$update.operation_text;base=$update.base;offset=$update.offset;width=$update.width_bytes;owners=[Collections.Generic.List[string]]::new()}
                }
                $updates[$update.address].owners.Add($entry)
            }
        } }
    } finally { $document.Dispose(); $stream.Dispose() }
    $index=[pscustomobject]@{batch_sha256=(Get-FileHash $Batch -Algorithm SHA256).Hash;remaining_research=$null}
    if(!$ProbesOnly) {
    foreach($site in $reviews.Keys) {
        if(!$changes.ContainsKey($site) -and !$changes.ContainsKey("entry:$site") -and !$updates.ContainsKey($site) -and $site -notin $updates.Values.operation) {
            throw "Reviewed timing site is absent from the current index: $site"
        }
    }
    foreach($change in $changes.Values) {
        $review=$reviews[$change.address]
        $change | Add-Member -NotePropertyName review -NotePropertyValue $(if($review){$review.id}else{$null})
    }
    # Follow every original event family, including inactive party modes.
    # Callback registrations are included conservatively: reachability is a
    # coverage obligation, never proof of execution frequency or a timing bug.
    $roots = @($events) + @([pscustomobject]@{name='FRAME_AND_MODES';entry='0x00014D80'})
    $familyReport = foreach ($family in $roots) {
        $queue = [Collections.Generic.Queue[string]]::new(); $queue.Enqueue($family.entry)
        $seen = [Collections.Generic.HashSet[string]]::new(); $missing = [Collections.Generic.HashSet[string]]::new()
        while ($queue.Count) {
            $entry = $queue.Dequeue()
            if (!$seen.Add($entry)) { continue }
            if (!$nodes.ContainsKey($entry)) { [void]$missing.Add($entry); continue }
            $node = $nodes[$entry]; [void]$node.families.Add($family.name)
            foreach ($next in @($node.calls)+@($node.callbacks)) { $queue.Enqueue($next) }
        }
        [pscustomobject]@{name=$family.name;entry=$family.entry;functions=$seen.Count;missing=@($missing | Sort-Object)}
    }
    $scene = [Collections.Generic.HashSet[string]]::new()
    $queue = [Collections.Generic.Queue[string]]::new(); $queue.Enqueue('0x00014D80')
    while ($queue.Count) {
        $entry=$queue.Dequeue()
        if (!$scene.Add($entry) -or !$nodes.ContainsKey($entry)) { continue }
        foreach ($next in $nodes[$entry].scene_calls) { $queue.Enqueue($next) }
    }
    $candidates = foreach ($update in $updates.Values) {
        $families = @($update.owners | ForEach-Object { $nodes[$_].families } | Sort-Object -Unique)
        if (!$families.Count) { continue }
        $lowerings = @($update.owners | ForEach-Object { $nodes[$_].sites } | Sort-Object -Unique)
        $intermediate = @($update.owners | Where-Object { $scene.Contains($_) -and $update.address -in $nodes[$_].scene_updates }).Count -gt 0
        $review=if($reviews.ContainsKey($update.address)){$reviews[$update.address]}else{$reviews[$update.operation]}
        [pscustomobject]@{address=$update.address;bytes=$update.bytes;original=$update.original;load=$update.load;operation=$update.operation;
            operation_text=$update.operation_text;base=$update.base;offset=$update.offset;width=$update.width;owners=@($update.owners);families=$families;
            scene_path=$intermediate;existing_changes=$lowerings;status=$(if($review){$review.status}else{'needs_research'});review=$(if($review){$review.id}else{$null})}
    }
    $drawLeaves=@('0x00031200','0x000313B0','0x00075400','0x00075EE0','0x000691A0','0x000691C0','0x0001FD80')
    $construction=@('0x0001FA50','0x0001FAA0','0x000736C0','0x00073870','0x0002A640')
    $gates=@{}
    foreach ($node in $nodes.Values) {
        if ($node.entry_lowering.Contains('if(!smbd::frame_step.logic) { cpu.eip=')) {
            if (!$gates.ContainsKey($node.entry)) { $gates[$node.entry]=[Collections.Generic.HashSet[string]]::new() }
            [void]$gates[$node.entry].Add($node.entry)
        }
        foreach ($edge in $node.edges) {
            if ($changes.ContainsKey($edge.site) -and $changes[$edge.site].lowering.Contains('if(!smbd::frame_step.logic) ')) {
                if (!$gates.ContainsKey($edge.site)) { $gates[$edge.site]=[Collections.Generic.HashSet[string]]::new() }
                [void]$gates[$edge.site].Add($edge.target)
            }
        }
    }
    $drawRisks=[Collections.Generic.List[object]]::new()
    foreach ($gate in $gates.Keys) {
        $queue=[Collections.Generic.Queue[object]]::new()
        foreach ($entry in $gates[$gate]) { $queue.Enqueue(@($entry)) }
        $seen=[Collections.Generic.HashSet[string]]::new()
        while ($queue.Count) {
            $path=$queue.Dequeue(); $entry=$path[-1]
            if (!$seen.Add($entry) -or !$nodes.ContainsKey($entry)) { continue }
            foreach ($next in $nodes[$entry].calls) {
                if ($next -in $drawLeaves) { $drawRisks.Add([pscustomobject]@{gate=$gate;path=@($path)+@($next);status='verify_recurring_draw_or_construction'}) }
                elseif ($next -notin $construction) { $queue.Enqueue(@($path)+@($next)) }
            }
        }
    }
    $index = [pscustomobject]@{
        format='smbd-timing-inventory-v1';source_sha256=$target;batch_sha256=(Get-FileHash $Batch -Algorithm SHA256).Hash
        call_graph_closed=$closed;exhaustive=$false
        scope='All recovered frame/event call families. Recurrences and gates are candidates, not automatic fixes; cross-block aliases and transient events need research.'
        families=@($familyReport);changes=@($changes.Values | Sort-Object address,kind)
        findings=@($suite.findings);cases_sha256=$casesHash
        candidates=@($candidates | Sort-Object @{Expression='scene_path';Descending=$true},address)
        draws_under_rule_gates=$drawRisks.ToArray()
        remaining_research=[pscustomobject]@{
            scene_candidates=@($candidates | Where-Object { $_.scene_path -and $_.status -eq 'needs_research' }).Count
            changes=@($changes.Values | Where-Object { !$_.review }).Count
            scope='Counts are research obligations, not a count of confirmed bugs.'
        }
    }
    $index | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath (Join-Path $Output 'inventory.json') -Encoding utf8
    # Make the exact caller/callee paths and emitted timing boundaries reusable.
    @($nodes.Values | Where-Object { $_.families.Count } | Sort-Object entry | ForEach-Object {
        [pscustomobject]@{entry=$_.entry;calls=$_.calls;callbacks=$_.callbacks;edges=$_.edges;scene_calls=$_.scene_calls;families=@($_.families);entry_lowering=$_.entry_lowering;sites=$_.sites}
    }) | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Output 'calls.json') -Encoding utf8
    Write-Host "Timing inventory: $($familyReport.Count) roots, $($changes.Count) existing changes, $(@($candidates | Where-Object scene_path).Count)/$(@($candidates).Count) recurrence candidates on possible intermediate-scene paths."
    Write-Host "Research remains open: $($index.remaining_research.scene_candidates) scene candidates and $($index.remaining_research.changes) timing changes lack review. Passing probes does not close this queue."
    if ($InventoryOnly) { return }
    }

    $toolHash=(Get-FileHash -LiteralPath $Tool -Algorithm SHA256).Hash
    $runDirectory = Join-Path $Output ('run-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-ffff'))
    New-Item -ItemType Directory -Path $runDirectory | Out-Null
    $suite | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $runDirectory 'cases.json') -Encoding utf8
    function Word([double]$value) { '0x'+[BitConverter]::SingleToUInt32Bits([float]$value).ToString('X8') }
    function Values($observation,[string]$bytes) {
        $data = [Convert]::FromHexString($bytes)
        $width=if($observation.type -in @('i16','u16')){2}else{4}
        for ($offset=0; $offset -lt $data.Length; $offset+=$width) {
            switch ($observation.type) {
                i16 { [double][BitConverter]::ToInt16($data,$offset) }
                u16 { [double][BitConverter]::ToUInt16($data,$offset) }
                f32 { [double][BitConverter]::ToSingle($data,$offset) }
                i32 { [double][BitConverter]::ToInt32($data,$offset) }
                u32 { [double][BitConverter]::ToUInt32($data,$offset) }
                default { throw "Unknown observation type $($observation.type)" }
            }
        }
    }
    $schedules = @(
        @{name='60';intervals=@(1.0)}, @{name='30-cap';intervals=@(1.0)},
        @{name='75';intervals=@(60.0/75)}, @{name='120';intervals=@(0.5)},
        @{name='144';intervals=@(60.0/144)}, @{name='165';intervals=@(60.0/165)},
        @{name='240';intervals=@(0.25)}, @{name='360';intervals=@(60.0/360)},
        @{name='1000';intervals=@(0.06)},
        @{name='uncapped-jitter';intervals=@(0.07,0.31,0.13,0.49,0.11,0.17,0.29,0.43)},
        @{name='hitch-recovery';intervals=@(0.2,0.3,1.0,1.0,1.0,0.4,0.1)}
    )
    $results = [Collections.Generic.List[object]]::new()
    foreach ($case in $suite.cases) {
        if ($case.name -notmatch '^[a-z0-9-]+$' -or $case.ticks -le 0 -or $case.ticks -gt 60) { throw 'Invalid timing case bounds.' }
        foreach ($observation in $case.observe) {
            if ($observation.rule -eq 'tick-sequence' -and ($observation.count -ne 1 -or $observation.expected.Count -ne $case.ticks+1)) {
                throw "Tick sequence for $($case.name) must give one value per tick, including tick zero."
            }
            if ($null -ne $observation.phase_speeds -and ($observation.rule -ne 'tick-sequence' -or $observation.phase_speeds.Count -ne $case.ticks)) {
                throw "Phase speeds for $($case.name) must give one speed per interval between tick samples."
            }
        }
        $actions=@($case.actions | Where-Object { $null -ne $_ } | Sort-Object tick)
        foreach($action in $actions) { if($action.tick -lt 0 -or $action.tick -ge $case.ticks) { throw 'Timing action is outside the case.' } }
        $baseline = $null
        foreach ($schedule in $schedules) {
            $lines = [Collections.Generic.List[string]]::new()
            foreach ($line in $case.setup) { $lines.Add($line) }
            $elapsed = 0.0; $samples = [Collections.Generic.List[double]]::new(); $indexStep = 0; $actionIndex=0
            while ($elapsed -lt $case.ticks-1e-9) {
                while($actionIndex -lt $actions.Count -and $actions[$actionIndex].tick -le $elapsed+1e-9) {
                    foreach($line in $actions[$actionIndex].commands) { $lines.Add($line) }; ++$actionIndex
                }
                $delta = [Math]::Min($schedule.intervals[$indexStep%$schedule.intervals.Count],$case.ticks-$elapsed)
                if($actionIndex -lt $actions.Count) { $delta=[Math]::Min($delta,$actions[$actionIndex].tick-$elapsed) }
                $before = [Math]::Floor($elapsed+1e-9); $elapsed += $delta; $whole = [Math]::Floor($elapsed+1e-9)
                if ([Math]::Abs($elapsed-$whole) -lt 1e-9) { $elapsed=$whole }
                $phase = [Math]::Max(0.0,$elapsed-$whole)
                $lines.Add(('step {0} {1} {2}' -f (Word $delta),(Word $phase),[int]($whole -gt $before)))
                foreach ($line in $case.update) { $lines.Add($line) }
                foreach ($observation in $case.observe) {
                    $width=if($observation.type -in @('i16','u16')){2}else{4}
                    $lines.Add("read $($observation.address) $($observation.count*$width)")
                }
                $samples.Add($elapsed); ++$indexStep
            }
            $prefix = Join-Path $runDirectory ($case.name+'-'+$schedule.name)
            $lines | Set-Content -LiteralPath "$prefix.txt" -Encoding utf8
            $failure = ''; $maximum = 0.0; $last = [Collections.Generic.List[double]]::new()
            & $Tool probe-cpu data/local/default.xbe $case.capture "$prefix.txt" "$prefix.json" > "$prefix.stdout"
            $exit = $LASTEXITCODE
            if (!(Test-Path -LiteralPath "$prefix.json")) { $failure="Probe failed without a report (exit $exit)." }
            else {
                $probe = Get-Content -LiteralPath "$prefix.json" -Raw | ConvertFrom-Json
                if ($exit -ne 0 -or !$probe.passed) { $failure="Probe failed: $($probe.error)" }
                elseif ($case.snapshot_sha256 -and $probe.snapshot_sha256 -ne $case.snapshot_sha256) { $failure='Snapshot hash does not match the researched fixture.' }
                else {
                    $reads = @($probe.records | Where-Object bytes)
                    if ($reads.Count -ne $samples.Count*$case.observe.Count) { $failure='Missing observations.' }
                    else {
                        for ($sample=0; $sample -lt $samples.Count; ++$sample) {
                            $time = $samples[$sample]
                            for ($o=0; $o -lt $case.observe.Count; ++$o) {
                                $observation = $case.observe[$o]
                                $time=$samples[$sample]
                                if($observation.quantize -eq 'truncate') {
                                    $whole=[Math]::Floor($time+1e-9)
                                    $time=$whole+[double][float]($time-$whole)
                                }
                                $values = @(Values $observation $reads[$sample*$case.observe.Count+$o].bytes)
                                for ($component=0; $component -lt $values.Count; ++$component) {
                                    $value = $values[$component]; $expected = $null
                                    switch ($observation.rule) {
                                        countdown { $expected=$observation.initial-[Math]::Floor($time+1e-9) }
                                        counter { $expected=[Math]::Clamp($observation.initial+$observation.speed*[Math]::Floor($time+1e-9),[double]$observation.minimum,[double]$observation.maximum) }
                                        constant { $expected=$observation.expected[$component] }
                                        linear { $expected=$observation.initial[$component]+$observation.speed[$component]*$time }
                                        clamped-linear { $expected=[Math]::Clamp($observation.initial+$observation.speed*($time-[double]$observation.start),[double]$observation.minimum,[double]$observation.maximum) }
                                        tick-sequence {
                                            $tick=[int][Math]::Floor($time+1e-9)
                                            $expected=$observation.expected[$tick]
                                            if ($tick -lt $case.ticks -and $null -ne $observation.phase_speeds) {
                                                # The recipe sends a float phase. An integer sample can
                                                # straddle a truncation boundary if the oracle uses the
                                                # double schedule time instead of that actual input.
                                                $phase=if($observation.quantize -eq 'truncate'){[double][float]($time-$tick)}else{$time-$tick}
                                                $expected+=$observation.phase_speeds[$tick]*$phase
                                                if($observation.quantize -eq 'truncate'){$expected=[Math]::Truncate($expected)}
                                            }
                                        }
                                        euler { $expected=$observation.initial[$component]+$observation.speed[$component]*$time+$observation.acceleration[$component]*$time*($time+1)/2 }
                                        response { $expected=$observation.target+($observation.initial-$observation.target)*[Math]::Pow($observation.retention,$time) }
                                        response-stages {
                                            $expected=$observation.initial; $start=0.0
                                            foreach ($stage in $observation.stages) {
                                                if($time -le $start){break}
                                                if($null -ne $stage.initial){$expected=$stage.initial}
                                                $duration=[Math]::Clamp($time-$start,0.0,$stage.until-$start)
                                                $expected=$stage.target+($expected-$stage.target)*[Math]::Pow($stage.retention,$duration)
                                                $start=$stage.until
                                            }
                                        }
                                        rotation-y {
                                            $angle=[Math]::Truncate($observation.speed*$time+1e-7)*[Math]::PI/32768/2
                                            $expected=switch($component) { 1 { [Math]::Sin($angle) } 3 { [Math]::Cos($angle) } default { 0.0 } }
                                        }
                                        endpoint { if ($sample -eq $samples.Count-1 -and $null -ne $baseline) { $expected=$baseline[$last.Count] } }
                                        default { throw "Unknown timing rule $($observation.rule)" }
                                    }
                                    if($null -ne $expected -and $observation.quantize -eq 'truncate') {
                                        if($observation.rule -eq 'linear') {
                                            $whole=[Math]::Floor($time)
                                            $expected=$observation.initial[$component]+$observation.speed[$component]*$whole+
                                                [Math]::Truncate([double][float](($time-$whole)*$observation.speed[$component]))
                                        }
                                        $expected=[Math]::Truncate($expected)
                                    }
                                    if (![double]::IsFinite($value)) { $failure="Non-finite $($observation.name) at $time ticks." }
                                    if ($null -ne $expected) {
                                        $error = [Math]::Abs($value-$expected); $maximum=[Math]::Max($maximum,$error)
                                        if ($error -gt $observation.tolerance -and !$failure) { $failure="$($observation.name) at $time ticks: $value, expected $expected (tolerance $($observation.tolerance))." }
                                    }
                                    if ($sample -eq $samples.Count-1) { $last.Add($value) }
                                }
                                if ($schedule.name -eq '60' -and $sample -eq $samples.Count-1 -and $observation.baseline_bytes -and
                                    $reads[$sample*$case.observe.Count+$o].bytes -ne $observation.baseline_bytes -and !$failure) {
                                    $failure="60 Hz bytes changed for $($observation.name)."
                                }
                            }
                        }
                    }
                }
            }
            if ($schedule.name -eq '60' -and !$failure) { $baseline=$last.ToArray() }
            if ($schedule.name -ne '60' -and $null -eq $baseline -and !$failure) { $failure='Missing valid 60 Hz reference.' }
            $results.Add([pscustomobject]@{case=$case.name;schedule=$schedule.name;passed=(!$failure);samples=$samples.Count;maximum_error=$maximum;failure=$failure;report="$prefix.json"})
        }
    }
    $failed = @($results | Where-Object { !$_.passed })
    if((Get-FileHash -LiteralPath $Cases -Algorithm SHA256).Hash -ne $casesHash -or
       (Get-FileHash -LiteralPath $Tool -Algorithm SHA256).Hash -ne $toolHash) { throw 'Timing inputs changed during the run; results are not an acceptance record.' }
    $findings=foreach($finding in $suite.findings) {
        $checks=@($results | Where-Object { $_.case -in $finding.cases })
        [pscustomobject]@{id=$finding.id;status=$finding.status;checks=$checks.Count;
            passed=$(if($checks.Count){@($checks | Where-Object { !$_.passed }).Count -eq 0}else{$null});sites=$finding.sites;evidence=$finding.evidence}
    }
    $report = [pscustomobject]@{
        format='smbd-timing-audit-v1';source_sha256=$target;tool_sha256=$toolHash;batch_sha256=$index.batch_sha256
        cases_sha256=$casesHash;passed=($failed.Count -eq 0 -and $closed);exhaustive=$false
        scope=$(if($ProbesOnly){'Selected AOT numeric probes; original-code inventory was not scanned. Not complete gameplay or visual acceptance.'}else{'AOT numeric probes and original-code inventory; not complete gameplay or visual acceptance.'})
        remaining_research=$index.remaining_research
        checks=$results.Count;failures=$failed.Count;findings=@($findings);results=$results.ToArray()
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $runDirectory 'report.json') -Encoding utf8
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Output 'report.json') -Encoding utf8
    Write-Host "Timing probes: $($results.Count-$failed.Count)/$($results.Count) passed. Report: $Output/report.json"
    if (!$report.passed) { throw 'Timing audit failed; see the structured report.' }
} finally { Pop-Location }
