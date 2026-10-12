# prosperoai-bench.ps1 - measure ProsperoAI text speed on a PS5 from a Windows PC.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Uses ProsperoAI's Ollama-style /api/generate endpoint, which returns the
# console's own timings (prompt reading and generation, measured on the PS5),
# so network delay does not affect the numbers.
#
#   .\prosperoai-bench.ps1 -Ip 192.168.1.50 -Label base
#   .\prosperoai-bench.ps1 -Ip 192.168.1.50 -Label pro
#
# Each run appends to bench-results.csv next to this script and then prints a
# table comparing every label measured so far.
# Works in Windows PowerShell 5.1 and PowerShell 7.

param(
    [Parameter(Mandatory = $true)][string]$Ip,
    [string]$Label = "run",
    [int]$Runs = 3,
    [int]$Ctx = 4096,
    [int]$Tokens = 256,
    [string]$Model = ""
)

$ErrorActionPreference = "Stop"
$base = "http://${Ip}:11434"
$csv = Join-Path $PSScriptRoot "bench-results.csv"

function Post-Generate([string]$prompt, [int]$predict) {
    $body = @{ model = $script:Model; prompt = $prompt; stream = $false;
               options = @{ num_predict = $predict; num_ctx = $Ctx } } | ConvertTo-Json -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($body)
    $r = Invoke-RestMethod -Uri "$base/api/generate" -Method Post -Body $bytes `
        -ContentType "application/json; charset=utf-8" -TimeoutSec 900
    if ($r.error) { throw "PS5 answered: $($r.error)" }
    return $r
}

function Median([double[]]$values) {
    $sorted = $values | Sort-Object
    $n = $sorted.Count
    if ($n -eq 0) { return 0 }
    if ($n % 2) { return [double]$sorted[($n - 1) / 2] }
    return ([double]$sorted[$n / 2 - 1] + [double]$sorted[$n / 2]) / 2
}

# 1. Which model?
Write-Host "Connecting to ProsperoAI at $base ..."
try { $tags = Invoke-RestMethod -Uri "$base/api/tags" -TimeoutSec 15 }
catch { Write-Host "Could not reach ProsperoAI. Is the app open on the PS5 and the IP right?" -ForegroundColor Red; exit 1 }
$names = @($tags.models | Where-Object { $_.details.family -eq "" -or $_.details.family -match "text" } | ForEach-Object { $_.name })
if (-not $names) { $names = @($tags.models | ForEach-Object { $_.name }) }
if (-not $Model) {
    $Model = ($names | Where-Object { $_ -match "Mistral" } | Select-Object -First 1)
    if (-not $Model) { $Model = $names | Select-Object -First 1 }
}
if (-not $Model) { Write-Host "No text model installed. Download one in ProsperoAI > Models first." -ForegroundColor Red; exit 1 }
Write-Host "Model: $Model   Label: $Label   Context: $Ctx   Runs: $Runs"

# 2. Warm-up: loads the model and builds the context at this size (not measured).
Write-Host "Warm-up (loads the model; can take a minute the first time) ..."
$null = Post-Generate "Say hello." 8

# Prompts. Plain ASCII, no quotes or new lines. Each run starts with its own
# number so ProsperoAI's prompt cache cannot reuse the previous run's work.
$question = "Explain in detail how a GPU executes a matrix multiplication, covering threads, memory tiers and synchronisation."
$paragraph = "A graphics processor runs thousands of small threads at the same time. Threads are grouped into waves that execute the same instruction together, and waves are grouped into workgroups that share a small fast memory. " +
             "Data moves from the large slow memory into caches, then into the shared memory of a workgroup, and finally into registers where the arithmetic happens. " +
             "Matrix multiplication splits the output into tiles. Each workgroup loads a tile of each input, multiplies them, and adds the result into its tile of the output before moving on to the next pair of input tiles. " +
             "Barriers make every thread in the workgroup wait until the shared tile is fully loaded before anyone reads it, and again before it is overwritten. "
$long = ($paragraph * 8) + "Summarise the text above in one sentence."

$rows = @()
$stamp = Get-Date -Format "yyyy-MM-dd HH:mm"

# 3. Generation speed (what you feel while the answer streams).
for ($i = 1; $i -le $Runs; $i++) {
    $r = Post-Generate "Run $i. $question" $Tokens
    $decodeS = $r.eval_duration / 1e9
    $tps = if ($decodeS -gt 0 -and $r.eval_count -gt 1) { ($r.eval_count - 1) / $decodeS } else { 0 }
    $prefillS = $r.prompt_eval_duration / 1e9
    Write-Host ("  generation run {0}: {1} tokens in {2:N2} s = {3:N1} tok/s" -f $i, $r.eval_count, $decodeS, $tps)
    $rows += [pscustomobject]@{ time = $stamp; label = $Label; model = $Model; ctx = $Ctx; test = "generation"; run = $i
        prompt_tokens = $r.prompt_eval_count; prompt_s = [math]::Round($prefillS, 3)
        gen_tokens = $r.eval_count; gen_s = [math]::Round($decodeS, 3); tok_per_s = [math]::Round($tps, 2) }
}

# 4. Prompt reading speed (the wait before the first word, for long prompts).
for ($i = 1; $i -le $Runs; $i++) {
    $r = Post-Generate "Run $i. $long" 16
    $prefillS = $r.prompt_eval_duration / 1e9
    $tps = if ($prefillS -gt 0) { $r.prompt_eval_count / $prefillS } else { 0 }
    Write-Host ("  prompt-reading run {0}: {1} tokens in {2:N2} s = {3:N0} tok/s" -f $i, $r.prompt_eval_count, $prefillS, $tps)
    $rows += [pscustomobject]@{ time = $stamp; label = $Label; model = $Model; ctx = $Ctx; test = "prompt-reading"; run = $i
        prompt_tokens = $r.prompt_eval_count; prompt_s = [math]::Round($prefillS, 3)
        gen_tokens = $r.eval_count; gen_s = [math]::Round($r.eval_duration / 1e9, 3); tok_per_s = [math]::Round($tps, 2) }
}

$rows | Export-Csv -Path $csv -NoTypeInformation -Append
Write-Host "Saved to $csv"

# 5. Compare every label measured so far (median of the runs).
Write-Host ""
Write-Host "Median tok/s per label:"
$all = Import-Csv $csv
$summary = $all | Group-Object label, model | ForEach-Object {
    $g = $_.Group
    [pscustomobject]@{
        label = $g[0].label
        model = $g[0].model
        "generation tok/s" = [math]::Round((Median ($g | Where-Object test -eq "generation" | ForEach-Object { [double]$_.tok_per_s })), 1)
        "prompt-reading tok/s" = [math]::Round((Median ($g | Where-Object test -eq "prompt-reading" | ForEach-Object { [double]$_.tok_per_s })), 0)
        runs = ($g | Where-Object test -eq "generation").Count
    }
}
$summary | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
