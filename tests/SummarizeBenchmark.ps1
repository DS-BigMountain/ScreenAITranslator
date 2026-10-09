param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference='Stop'
function Get-Median([object[]]$Values) {
    $sorted=@($Values | ForEach-Object {[double]$_} | Sort-Object)
    if(!$sorted.Count){return $null}
    $middle=[int][math]::Floor($sorted.Count/2)
    if($sorted.Count%2){return $sorted[$middle]}
    return ($sorted[$middle-1]+$sorted[$middle])/2
}
if((Get-Median @(3,1,2)) -ne 2 -or (Get-Median @(4,1,3,2)) -ne 2.5){throw 'Median self-check failed'}
$metadata=Get-Content -LiteralPath (Join-Path $Directory 'metadata.json') -Raw | ConvertFrom-Json
$rows=@(Get-Content -LiteralPath (Join-Path $Directory 'trials.jsonl') | ForEach-Object {$_ | ConvertFrom-Json})
if($rows.Count -ne $metadata.rounds*$metadata.inputs.Count*2){throw 'Incomplete benchmark'}
foreach($group in ($rows | Group-Object image,round,path)){if($group.Count -ne 1){throw 'Duplicate trial'}}
$summary=@($rows | Group-Object image,path | ForEach-Object {
    $group=$_.Group
    if($group.Count -ne $metadata.rounds){throw 'Unbalanced samples'}
    $valid=@($group | Where-Object success)
    foreach($row in $valid){
        $parts=$row.preprocess_ms+$row.ocr_ms+$row.api_ms+$row.layout_ms
        if($row.total_ms -lt $parts-1 -or $row.total_ms-$parts -gt 100){throw 'Timing components do not reconcile'}
    }
    [pscustomobject]@{
        image=$group[0].image;path=$group[0].path;trials=$group.Count;success=$valid.Count
        median_total_ms=(Get-Median @($valid.total_ms))
        median_api_ms=(Get-Median @($valid.api_ms))
        median_ocr_ms=(Get-Median @($group.ocr_ms))
        min_total_ms=($valid.total_ms | Measure-Object -Minimum).Minimum
        max_total_ms=($valid.total_ms | Measure-Object -Maximum).Maximum
        median_first_response_ms=(Get-Median @($group | ForEach-Object {$_.attempts[0].ms}))
        median_first_completion_tokens=(Get-Median @($group | ForEach-Object {$_.attempts[0].completion_tokens}))
        median_first_prompt_tokens=(Get-Median @($group | ForEach-Object {$_.attempts[0].prompt_tokens}))
        median_first_cache_hit_tokens=(Get-Median @($group | ForEach-Object {$_.attempts[0].prompt_cache_hit_tokens}))
        attempt_count=($group | ForEach-Object {$_.attempts.Count} | Measure-Object -Sum).Sum
        failed_total_ms=@($group | Where-Object {!$_.success} | ForEach-Object {$_.total_ms})
    }
})
$comparisons=@(foreach($inputImage in $metadata.inputs){
    $direct=$summary | Where-Object {$_.image -eq $inputImage.image -and $_.path -eq 'direct_ai'}
    $ocr=$summary | Where-Object {$_.image -eq $inputImage.image -and $_.path -eq 'ocr_image_ai'}
    [pscustomobject]@{image=$inputImage.image;all_trials_succeeded=($direct.success -eq $direct.trials -and $ocr.success -eq $ocr.trials);observed_median_reduction_percent=if($direct.success -eq $direct.trials -and $ocr.success -eq $ocr.trials){100*(1-$ocr.median_total_ms/$direct.median_total_ms)}else{$null}}
})
$result=[pscustomobject]@{groups=$summary;comparisons=$comparisons;trial_count=$rows.Count;http_attempts=($rows | ForEach-Object {$_.attempts.Count} | Measure-Object -Sum).Sum}
$json=$result | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText((Join-Path (Resolve-Path -LiteralPath $Directory).Path 'summary.json'),$json)
$json
